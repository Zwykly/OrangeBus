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

#define AVRCP_TL_MASK      0x0F
#define AVRCP_CMD_QUEUE_LEN 8

typedef struct {
    uint8_t cmd;
} avrcp_cmd_t;

struct avrcp_controller_t {
    QueueHandle_t cmd_queue;
    TaskHandle_t worker_task;
    TickType_t last_cmd_tick;
    uint32_t txn_count;
    SemaphoreHandle_t metaMutex;
    orangebus_metadata_t metadata;
    uint8_t meta_field_count;
    TimerHandle_t meta_timer;
    bool meta_requesting;
    struct a2dp_sink_t *a2dp_sink;
    orangebus_a2dp_state_t *a2dp_state_ref;
};

static avrcp_controller_t *s_instance = NULL;

static uint8_t next_txn_label(avrcp_controller_t *ac)
{
    uint8_t tl = (uint8_t)(ac->txn_count & AVRCP_TL_MASK);
    ac->txn_count++;
    return tl;
}

static void notify_metadata(avrcp_controller_t *ac)
{
    /* Snapshot under the mutex: writers in on_avrcp_meta run in BTC context. */
    orangebus_metadata_t snap;
    if (ac->metaMutex) xSemaphoreTake(ac->metaMutex, portMAX_DELAY);
    memcpy(&snap, &ac->metadata, sizeof(snap));
    if (ac->metaMutex) xSemaphoreGive(ac->metaMutex);
    ESP_LOGI(TAG, "Metadata: \"%s\" by %s (%s)",
             snap.title[0] ? snap.title : "-",
             snap.artist[0] ? snap.artist : "-",
             snap.album[0] ? snap.album : "-");
}

static void request_metadata_impl(avrcp_controller_t *ac)
{
    if (ac->metaMutex) xSemaphoreTake(ac->metaMutex, portMAX_DELAY);
    ac->meta_field_count = 0;
    ac->metadata.title[0]  = '\0';
    ac->metadata.artist[0] = '\0';
    ac->metadata.album[0]  = '\0';
    ac->meta_requesting = true;
    if (ac->metaMutex) xSemaphoreGive(ac->metaMutex);
    esp_avrc_ct_send_metadata_cmd(next_txn_label(ac), ESP_AVRC_MD_ATTR_TITLE);
    esp_avrc_ct_send_metadata_cmd(next_txn_label(ac), ESP_AVRC_MD_ATTR_ARTIST);
    esp_avrc_ct_send_metadata_cmd(next_txn_label(ac), ESP_AVRC_MD_ATTR_ALBUM);
}

static void meta_timer_cb(TimerHandle_t timer)
{
    avrcp_controller_t *ac = s_instance;
    if (!ac) return;
    bool pending = false;
    if (ac->metaMutex) xSemaphoreTake(ac->metaMutex, portMAX_DELAY);
    pending = ac->meta_requesting;
    ac->meta_requesting = false;
    if (ac->metaMutex) xSemaphoreGive(ac->metaMutex);
    if (pending) notify_metadata(ac);
}

static void on_avrcp_meta(avrcp_controller_t *ac, uint8_t attr_id, const uint8_t *val, uint8_t len)
{
    char buf[81] = "";
    if (len > 80) len = 80;
    memcpy(buf, val, len);
    buf[len] = '\0';

    if (ac->metaMutex) xSemaphoreTake(ac->metaMutex, portMAX_DELAY);
    switch (attr_id) {
    case ESP_AVRC_MD_ATTR_TITLE:
        strncpy(ac->metadata.title, buf, 80);
        ac->metadata.title[80] = '\0';
        ac->meta_field_count |= 0x01;
        break;
    case ESP_AVRC_MD_ATTR_ARTIST:
        strncpy(ac->metadata.artist, buf, 80);
        ac->metadata.artist[80] = '\0';
        ac->meta_field_count |= 0x02;
        break;
    case ESP_AVRC_MD_ATTR_ALBUM:
        strncpy(ac->metadata.album, buf, 80);
        ac->metadata.album[80] = '\0';
        ac->meta_field_count |= 0x04;
        break;
    default:
        if (ac->metaMutex) xSemaphoreGive(ac->metaMutex);
        return;
    }

    bool complete = (ac->meta_field_count == 0x07);
    if (ac->metaMutex) xSemaphoreGive(ac->metaMutex);

    if (complete) {
        notify_metadata(ac);
        if (ac->metaMutex) xSemaphoreTake(ac->metaMutex, portMAX_DELAY);
        ac->meta_requesting = false;
        if (ac->metaMutex) xSemaphoreGive(ac->metaMutex);
        if (ac->meta_timer) xTimerStop(ac->meta_timer, 0);
    } else if (ac->meta_timer) {
        xTimerReset(ac->meta_timer, 0);
    }
}

/* Zadanie worker przetwarzajace komendy passthrough z kolejki */
static void avrcp_cmd_worker(void *arg)
{
    avrcp_controller_t *ac = (avrcp_controller_t *)arg;
    avrcp_cmd_t item;
    while (1) {
        if (xQueueReceive(ac->cmd_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        TickType_t now = xTaskGetTickCount();
        TickType_t min_interval = pdMS_TO_TICKS(AVRCP_CMD_MIN_INTERVAL_MS);
        if ((now - ac->last_cmd_tick) < min_interval) {
            vTaskDelay(min_interval - (now - ac->last_cmd_tick));
        }

        esp_err_t ret = esp_avrc_ct_send_passthrough_cmd(next_txn_label(ac), item.cmd, ESP_AVRC_PT_CMD_STATE_PRESSED);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "AVRCP PRESSED cmd 0x%02X failed: %s", item.cmd, esp_err_to_name(ret));
            ac->last_cmd_tick = xTaskGetTickCount();
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(AVRCP_PRESSED_RELEASED_GAP_MS));

        ret = esp_avrc_ct_send_passthrough_cmd(next_txn_label(ac), item.cmd, ESP_AVRC_PT_CMD_STATE_RELEASED);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "AVRCP RELEASED cmd 0x%02X failed: %s", item.cmd, esp_err_to_name(ret));
        }

        ac->last_cmd_tick = xTaskGetTickCount();
    }
}

/* Glowny callback zdarzen AVRCP CT (polaczenie, metadane, powiadomienia) */
static void avrcp_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    avrcp_controller_t *ac = s_instance;
    if (!ac) return;

    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        if (param->conn_stat.connected) {
            ESP_LOGI(TAG, "AVRCP Connected");
        } else {
            ESP_LOGI(TAG, "AVRCP Disconnected");
            if (ac->metaMutex) xSemaphoreTake(ac->metaMutex, portMAX_DELAY);
            ac->metadata.title[0] = ac->metadata.artist[0] = ac->metadata.album[0] = '\0';
            ac->meta_field_count = 0;
            if (ac->metaMutex) xSemaphoreGive(ac->metaMutex);
        }
        break;
    case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
        if (param->rmt_feats.feat_mask & ESP_AVRC_FEAT_META_DATA) {
            esp_avrc_ct_send_get_rn_capabilities_cmd(next_txn_label(ac));
            request_metadata_impl(ac);
        }
        ESP_LOGI(TAG, "AVRCP Remote features received");
        break;
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT: {
        esp_avrc_rn_evt_cap_mask_t evt_mask = param->get_rn_caps_rsp.evt_set;
        if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &evt_mask, ESP_AVRC_RN_TRACK_CHANGE)) {
            esp_avrc_ct_send_register_notification_cmd(next_txn_label(ac), ESP_AVRC_RN_TRACK_CHANGE, 0);
        }
        if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &evt_mask, ESP_AVRC_RN_PLAY_STATUS_CHANGE)) {
            esp_avrc_ct_send_register_notification_cmd(next_txn_label(ac), ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
        }
        break;
    }
    case ESP_AVRC_CT_METADATA_RSP_EVT: {
        const char *txt = (const char *)param->meta_rsp.attr_text;
        if (txt) {
            on_avrcp_meta(ac, param->meta_rsp.attr_id, (const uint8_t *)txt, param->meta_rsp.attr_length);
        }
        break;
    }
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
        if (param->change_ntf.event_id == ESP_AVRC_RN_TRACK_CHANGE) {
            request_metadata_impl(ac);
            esp_avrc_ct_send_register_notification_cmd(next_txn_label(ac), ESP_AVRC_RN_TRACK_CHANGE, 0);
        } else if (param->change_ntf.event_id == ESP_AVRC_RN_PLAY_STATUS_CHANGE) {
            esp_avrc_playback_stat_t play_status = param->change_ntf.event_parameter.playback;
            if (play_status == ESP_AVRC_PLAYBACK_PLAYING) {
                if (ac->a2dp_sink) {
                    a2dp_sink_set_state(ac->a2dp_sink, ORANGEBUS_A2DP_PLAYING);
                } else if (ac->a2dp_state_ref) {
                    *ac->a2dp_state_ref = ORANGEBUS_A2DP_PLAYING;
                }
            } else if (play_status == ESP_AVRC_PLAYBACK_PAUSED) {
                if (ac->a2dp_sink) {
                    a2dp_sink_set_state(ac->a2dp_sink, ORANGEBUS_A2DP_PAUSED);
                } else if (ac->a2dp_state_ref) {
                    *ac->a2dp_state_ref = ORANGEBUS_A2DP_PAUSED;
                }
            }
            ESP_LOGI(TAG, "Play status: %d", play_status);
            esp_avrc_ct_send_register_notification_cmd(next_txn_label(ac), ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
        }
        break;
    default:
        break;
    }
}

/* Konstruktor kontrolera AVRCP - alokuje strukture */
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
    if (ac->meta_timer) xTimerDelete(ac->meta_timer, 0);
    if (ac->cmd_queue) vQueueDelete(ac->cmd_queue);
    if (ac->worker_task) vTaskDelete(ac->worker_task);
    if (ac->metaMutex) vSemaphoreDelete(ac->metaMutex);
    free(ac);
}

/* Inicjalizuje kontroler AVRCP: timer metadanych, kolejke komend i zadanie worker */
esp_err_t avrcp_controller_init(avrcp_controller_t *ac)
{
    if (!ac) return ESP_ERR_INVALID_ARG;

    ac->meta_timer = xTimerCreate("meta_tmr", pdMS_TO_TICKS(2000), pdFALSE, NULL, meta_timer_cb);
    ac->cmd_queue = xQueueCreate(AVRCP_CMD_QUEUE_LEN, sizeof(avrcp_cmd_t));
    if (!ac->cmd_queue) return ESP_ERR_NO_MEM;

    /* Kept at 5 (above CLI/SPP, below the T1 audio task at 10): AVRCP
     * command pacing is BT-adjacent but must not starve ibus_task. */
    BaseType_t ok = xTaskCreate(avrcp_cmd_worker, "avrcp_cmd", 3072, ac, 5, &ac->worker_task);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

    return ESP_OK;
}

void avrcp_controller_send_passthrough(avrcp_controller_t *ac, uint8_t cmd)
{
    if (!ac || !ac->cmd_queue) return;
    avrcp_cmd_t item = { .cmd = cmd };
    if (xQueueSend(ac->cmd_queue, &item, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "AVRCP cmd queue full, dropping cmd 0x%02X", cmd);
    }
}

void avrcp_controller_request_metadata(avrcp_controller_t *ac)
{
    if (ac) request_metadata_impl(ac);
}

const orangebus_metadata_t *avrcp_controller_get_metadata(const avrcp_controller_t *ac)
{
    return ac ? &ac->metadata : NULL;
}

/* Snapshot copy under the metadata mutex: use this from non-BTC tasks
 * (e.g. ibus_task) instead of strcmp on the live pointer (CODE_REVIEW 1.6). */
void avrcp_controller_copy_metadata(const avrcp_controller_t *ac, orangebus_metadata_t *out)
{
    if (!ac || !out) return;
    avrcp_controller_t *mut = (avrcp_controller_t *)ac;
    if (mut->metaMutex) xSemaphoreTake(mut->metaMutex, portMAX_DELAY);
    memcpy(out, &ac->metadata, sizeof(*out));
    if (mut->metaMutex) xSemaphoreGive(mut->metaMutex);
}

void avrcp_controller_set_a2dp_state_ref(avrcp_controller_t *ac, orangebus_a2dp_state_t *ref)
{
    if (ac) ac->a2dp_state_ref = ref;
}

/* Preferred wiring: pass the sink object so play-status updates go through
 * the locked a2dp_sink_set_state() setter instead of a raw pointer. */
void avrcp_controller_set_a2dp_sink(avrcp_controller_t *ac, struct a2dp_sink_t *sink)
{
    if (ac) ac->a2dp_sink = sink;
}

orangebus_a2dp_state_t *avrcp_controller_get_a2dp_state_ref(avrcp_controller_t *ac)
{
    return ac ? ac->a2dp_state_ref : NULL;
}

/* Rejestruje callback AVRCP CT w stosie Bluetooth i inicjalizuje profil */
esp_err_t avrcp_controller_register_callbacks(avrcp_controller_t *ac)
{
    if (!ac) return ESP_ERR_INVALID_ARG;
    s_instance = ac;
    esp_avrc_ct_register_callback(avrcp_ct_cb);
    return esp_avrc_ct_init();
}
