#include "avrcp_controller.h"
#include "a2dp_sink.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_avrc_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#define TAG "AVRCP_CTL"

#define AVRCP_TL_MASK              0x0F   /* AVRCP transaction labels are 4 bits */
#define AVRCP_CMD_QUEUE_LEN        8
#define AVRCP_META_TIMEOUT_MS      2000   /* publish partial metadata after this */
#define AVRCP_TEXT_MAX             (sizeof(((orangebus_metadata_t *)0)->title) - 1)

/* Bits in metaFieldsReceived */
#define META_FIELD_TITLE   0x01
#define META_FIELD_ARTIST  0x02
#define META_FIELD_ALBUM   0x04
#define META_FIELDS_ALL    (META_FIELD_TITLE | META_FIELD_ARTIST | META_FIELD_ALBUM)

typedef struct {
    uint8_t passthroughCmd;
} avrcp_cmd_t;

struct avrcp_controller_t {
    /* Passthrough commands are queued and sent by a worker task so callers
     * (I-BUS callbacks, CLI, SPP) never block on the Bluetooth stack. */
    QueueHandle_t cmdQueue;
    TaskHandle_t workerTask;
    TickType_t lastCmdTick;
    uint32_t transactionCount;

    /* Track metadata. Responses arrive one attribute at a time into `pending`;
     * once all are in (or the timeout fires) they are copied to `published`
     * and `metadataVersion` is bumped. Readers only copy on a version change. */
    SemaphoreHandle_t metaMutex;
    orangebus_metadata_t pending;
    orangebus_metadata_t published;
    uint8_t metaFieldsReceived;
    bool metaRequestActive;
    volatile uint32_t metadataVersion;
    TimerHandle_t metaTimer;

    a2dp_sink_t *a2dpSink;
};

/* Bluedroid callbacks carry no user pointer, so the instance is kept here. */
static avrcp_controller_t *s_instance = NULL;

static uint8_t next_transaction_label(avrcp_controller_t *ac)
{
    uint8_t label = (uint8_t)(ac->transactionCount & AVRCP_TL_MASK);
    ac->transactionCount++;
    return label;
}

/* ---------- Metadata ---------- */

/* Copies pending -> published and notifies readers. Caller holds metaMutex. */
static void publish_metadata_locked(avrcp_controller_t *ac)
{
    memcpy(&ac->published, &ac->pending, sizeof(ac->published));
    ac->metaRequestActive = false;
    ac->metadataVersion++;
}

static void log_published_metadata(avrcp_controller_t *ac)
{
    orangebus_metadata_t snapshot;
    avrcp_controller_copy_metadata(ac, &snapshot);
    ESP_LOGI(TAG, "Metadata: \"%s\" by %s (%s)",
             snapshot.title[0] ? snapshot.title : "-",
             snapshot.artist[0] ? snapshot.artist : "-",
             snapshot.album[0] ? snapshot.album : "-");
}

/* Asks the phone for title, artist and album of the current track. */
static void request_metadata(avrcp_controller_t *ac)
{
    xSemaphoreTake(ac->metaMutex, portMAX_DELAY);
    memset(&ac->pending, 0, sizeof(ac->pending));
    ac->metaFieldsReceived = 0;
    ac->metaRequestActive = true;
    xSemaphoreGive(ac->metaMutex);
    esp_avrc_ct_send_metadata_cmd(next_transaction_label(ac),
        ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST | ESP_AVRC_MD_ATTR_ALBUM);
    if (ac->metaTimer) xTimerReset(ac->metaTimer, 0);
}

/* Some phones never send every attribute - publish what we have. */
static void metadata_timeout_cb(TimerHandle_t timer)
{
    (void)timer;
    avrcp_controller_t *ac = s_instance;
    if (!ac) return;
    bool published = false;
    xSemaphoreTake(ac->metaMutex, portMAX_DELAY);
    if (ac->metaRequestActive) {
        publish_metadata_locked(ac);
        published = true;
    }
    xSemaphoreGive(ac->metaMutex);
    if (published) log_published_metadata(ac);
}

/* Stores one metadata attribute from the phone (BTC task context). */
static void on_metadata_attribute(avrcp_controller_t *ac, uint8_t attrId, const uint8_t *text, uint16_t textLen)
{
    char *target = NULL;
    uint8_t fieldBit = 0;
    switch (attrId) {
    case ESP_AVRC_MD_ATTR_TITLE:  target = ac->pending.title;  fieldBit = META_FIELD_TITLE;  break;
    case ESP_AVRC_MD_ATTR_ARTIST: target = ac->pending.artist; fieldBit = META_FIELD_ARTIST; break;
    case ESP_AVRC_MD_ATTR_ALBUM:  target = ac->pending.album;  fieldBit = META_FIELD_ALBUM;  break;
    default: return;
    }
    if (textLen > AVRCP_TEXT_MAX) textLen = AVRCP_TEXT_MAX;

    bool complete;
    xSemaphoreTake(ac->metaMutex, portMAX_DELAY);
    memcpy(target, text, textLen);
    target[textLen] = '\0';
    ac->metaFieldsReceived |= fieldBit;
    complete = (ac->metaFieldsReceived == META_FIELDS_ALL);
    if (complete) publish_metadata_locked(ac);
    xSemaphoreGive(ac->metaMutex);

    if (complete) {
        if (ac->metaTimer) xTimerStop(ac->metaTimer, 0);
        log_published_metadata(ac);
    }
}

static void clear_metadata(avrcp_controller_t *ac)
{
    xSemaphoreTake(ac->metaMutex, portMAX_DELAY);
    memset(&ac->pending, 0, sizeof(ac->pending));
    ac->metaFieldsReceived = 0;
    publish_metadata_locked(ac);
    xSemaphoreGive(ac->metaMutex);
}

/* ---------- Passthrough worker ---------- */

/* Sends queued passthrough commands as PRESSED + RELEASED pairs, keeping a
 * minimum gap between commands (some phones drop commands sent too fast). */
static void avrcp_cmd_worker(void *arg)
{
    avrcp_controller_t *ac = (avrcp_controller_t *)arg;
    avrcp_cmd_t item;
    const TickType_t minInterval = pdMS_TO_TICKS(AVRCP_CMD_MIN_INTERVAL_MS);
    while (1) {
        if (xQueueReceive(ac->cmdQueue, &item, portMAX_DELAY) != pdTRUE) continue;

        TickType_t sinceLast = xTaskGetTickCount() - ac->lastCmdTick;
        if (sinceLast < minInterval) {
            vTaskDelay(minInterval - sinceLast);
        }

        esp_err_t ret = esp_avrc_ct_send_passthrough_cmd(next_transaction_label(ac),
            item.passthroughCmd, ESP_AVRC_PT_CMD_STATE_PRESSED);
        if (ret == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(AVRCP_PRESSED_RELEASED_GAP_MS));
            ret = esp_avrc_ct_send_passthrough_cmd(next_transaction_label(ac),
                item.passthroughCmd, ESP_AVRC_PT_CMD_STATE_RELEASED);
        }
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "AVRCP cmd 0x%02X failed: %s", item.passthroughCmd, esp_err_to_name(ret));
        }
        ac->lastCmdTick = xTaskGetTickCount();
    }
}

/* ---------- Bluedroid callback (BTC task) ---------- */

static void avrcp_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    avrcp_controller_t *ac = s_instance;
    if (!ac) return;

    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        ESP_LOGI(TAG, "AVRCP %s", param->conn_stat.connected ? "Connected" : "Disconnected");
        if (!param->conn_stat.connected) clear_metadata(ac);
        break;

    case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
        /* Phone supports metadata: learn which notifications it offers and
         * fetch the current track. */
        if (param->rmt_feats.feat_mask & ESP_AVRC_FEAT_META_DATA) {
            esp_avrc_ct_send_get_rn_capabilities_cmd(next_transaction_label(ac));
            request_metadata(ac);
        }
        break;

    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT: {
        /* Subscribe to track-change and play-status notifications. */
        esp_avrc_rn_evt_cap_mask_t events = param->get_rn_caps_rsp.evt_set;
        if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &events, ESP_AVRC_RN_TRACK_CHANGE)) {
            esp_avrc_ct_send_register_notification_cmd(next_transaction_label(ac), ESP_AVRC_RN_TRACK_CHANGE, 0);
        }
        if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &events, ESP_AVRC_RN_PLAY_STATUS_CHANGE)) {
            esp_avrc_ct_send_register_notification_cmd(next_transaction_label(ac), ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
        }
        break;
    }

    case ESP_AVRC_CT_METADATA_RSP_EVT:
        if (param->meta_rsp.attr_text) {
            on_metadata_attribute(ac, param->meta_rsp.attr_id,
                param->meta_rsp.attr_text, param->meta_rsp.attr_length);
        }
        break;

    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
        /* Notifications are one-shot: re-register after each one. */
        if (param->change_ntf.event_id == ESP_AVRC_RN_TRACK_CHANGE) {
            request_metadata(ac);
            esp_avrc_ct_send_register_notification_cmd(next_transaction_label(ac), ESP_AVRC_RN_TRACK_CHANGE, 0);
        } else if (param->change_ntf.event_id == ESP_AVRC_RN_PLAY_STATUS_CHANGE) {
            esp_avrc_playback_stat_t playStatus = param->change_ntf.event_parameter.playback;
            if (playStatus == ESP_AVRC_PLAYBACK_PLAYING) {
                a2dp_sink_set_state(ac->a2dpSink, ORANGEBUS_A2DP_PLAYING);
            } else if (playStatus == ESP_AVRC_PLAYBACK_PAUSED) {
                a2dp_sink_set_state(ac->a2dpSink, ORANGEBUS_A2DP_PAUSED);
            }
            ESP_LOGI(TAG, "Play status: %d", playStatus);
            esp_avrc_ct_send_register_notification_cmd(next_transaction_label(ac), ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
        }
        break;

    default:
        break;
    }
}

/* ---------- Public API ---------- */

avrcp_controller_t *avrcp_controller_create(void)
{
    avrcp_controller_t *ac = calloc(1, sizeof(avrcp_controller_t));
    if (!ac) return NULL;
    ac->metaMutex = xSemaphoreCreateMutex();
    if (!ac->metaMutex) {
        free(ac);
        return NULL;
    }
    return ac;
}

void avrcp_controller_destroy(avrcp_controller_t *ac)
{
    if (!ac) return;
    if (ac->metaTimer) xTimerDelete(ac->metaTimer, 0);
    if (ac->workerTask) vTaskDelete(ac->workerTask);
    if (ac->cmdQueue) vQueueDelete(ac->cmdQueue);
    if (ac->metaMutex) vSemaphoreDelete(ac->metaMutex);
    free(ac);
}

esp_err_t avrcp_controller_init(avrcp_controller_t *ac)
{
    if (!ac) return ESP_ERR_INVALID_ARG;
    ac->metaTimer = xTimerCreate("meta_tmr", pdMS_TO_TICKS(AVRCP_META_TIMEOUT_MS), pdFALSE, NULL, metadata_timeout_cb);
    ac->cmdQueue = xQueueCreate(AVRCP_CMD_QUEUE_LEN, sizeof(avrcp_cmd_t));
    if (!ac->cmdQueue) return ESP_ERR_NO_MEM;

    /* Priority 5: above CLI/SPP (4), below ibus_task (6) and audio (10). */
    BaseType_t ok = xTaskCreate(avrcp_cmd_worker, "avrcp_cmd", 3072, ac, 5, &ac->workerTask);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

void avrcp_controller_send_passthrough(avrcp_controller_t *ac, uint8_t cmd)
{
    if (!ac || !ac->cmdQueue) return;
    avrcp_cmd_t item = { .passthroughCmd = cmd };
    if (xQueueSend(ac->cmdQueue, &item, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "AVRCP cmd queue full, dropping cmd 0x%02X", cmd);
    }
}

void avrcp_controller_request_metadata(avrcp_controller_t *ac)
{
    if (ac) request_metadata(ac);
}

uint32_t avrcp_controller_get_metadata_version(const avrcp_controller_t *ac)
{
    return ac ? ac->metadataVersion : 0;
}

void avrcp_controller_copy_metadata(const avrcp_controller_t *ac, orangebus_metadata_t *out)
{
    if (!ac || !out) return;
    xSemaphoreTake(ac->metaMutex, portMAX_DELAY);
    memcpy(out, &ac->published, sizeof(*out));
    xSemaphoreGive(ac->metaMutex);
}

void avrcp_controller_set_a2dp_sink(avrcp_controller_t *ac, a2dp_sink_t *sink)
{
    if (ac) ac->a2dpSink = sink;
}

esp_err_t avrcp_controller_register_callbacks(avrcp_controller_t *ac)
{
    if (!ac) return ESP_ERR_INVALID_ARG;
    s_instance = ac;
    esp_avrc_ct_register_callback(avrcp_ct_cb);
    return esp_avrc_ct_init();
}
