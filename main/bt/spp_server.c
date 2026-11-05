#include "spp_server.h"
#include "spp_private.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_spp_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define TAG "SPP"
#define SPP_SERVER_NAME "BMW_OrangeBus_SPP"

/* Bluedroid callbacks carry no user pointer, so the instance is kept here. */
static spp_server_t *s_instance = NULL;

/* Sends a reply to the connected SPP client (no-op when disconnected). */
void spp_send_response(spp_server_t *spp, const char *msg)
{
    if (!spp || !spp->connected || spp->handle == 0) return;
    esp_spp_write(spp->handle, (int)strlen(msg), (uint8_t *)msg);
}

/* Removes the first complete line ("...\r" or "...\n") from the receive
 * buffer and copies it to `line`. Returns false when no full line is buffered.
 * Caller holds bufMutex. */
static bool take_line_locked(spp_server_t *spp, char *line, size_t lineSize)
{
    /* Skip leftover CR/LF from the previous line ending. */
    int skip = 0;
    while (skip < spp->cmd_len && (spp->cmd_buf[skip] == '\r' || spp->cmd_buf[skip] == '\n')) skip++;

    int end = skip;
    while (end < spp->cmd_len && spp->cmd_buf[end] != '\r' && spp->cmd_buf[end] != '\n') end++;
    if (end >= spp->cmd_len) {
        /* No terminator yet: just drop the skipped separators. */
        if (skip > 0) {
            memmove(spp->cmd_buf, spp->cmd_buf + skip, spp->cmd_len - skip);
            spp->cmd_len -= skip;
        }
        return false;
    }

    size_t lineLen = (size_t)(end - skip);
    if (lineLen >= lineSize) lineLen = lineSize - 1;
    memcpy(line, spp->cmd_buf + skip, lineLen);
    line[lineLen] = '\0';

    int consumed = end + 1;
    memmove(spp->cmd_buf, spp->cmd_buf + consumed, spp->cmd_len - consumed);
    spp->cmd_len -= consumed;
    return true;
}

/* Command task: sleeps until the SPP callback signals new data, then runs
 * every complete line. Commands execute OUTSIDE the buffer lock, so a slow
 * command (e.g. an I-BUS send) never makes the BT callback drop data. */
static void spp_cmd_task(void *arg)
{
    spp_server_t *spp = (spp_server_t *)arg;
    char line[SPP_MAX_CMD];
    while (1) {
        xSemaphoreTake(spp->dataReady, portMAX_DELAY);
        while (1) {
            xSemaphoreTake(spp->bufMutex, portMAX_DELAY);
            bool haveLine = take_line_locked(spp, line, sizeof(line));
            /* A full buffer without any line ending can never complete: reset. */
            if (!haveLine && spp->cmd_len >= SPP_MAX_CMD - 1) spp->cmd_len = 0;
            xSemaphoreGive(spp->bufMutex);
            if (!haveLine) break;
            if (line[0] != '\0') spp_process_line(spp, line);
        }
    }
}

/* SPP event callback (BT task): server start, connect/disconnect, data.
 * Incoming bytes are only buffered here; spp_cmd_task parses them. */
static void spp_callback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    spp_server_t *spp = s_instance;
    if (!spp) return;

    switch (event) {
    case ESP_SPP_INIT_EVT:
        ESP_LOGI(TAG, "SPP init, starting server...");
        esp_spp_start_srv(ESP_SPP_SEC_AUTHENTICATE, ESP_SPP_ROLE_MASTER,
                          0, SPP_SERVER_NAME);
        break;

    case ESP_SPP_START_EVT:
        ESP_LOGI(TAG, "SPP server started, listening...");
        break;

    case ESP_SPP_SRV_OPEN_EVT:
        spp->handle = param->srv_open.handle;
        spp->connected = true;
        ESP_LOGI(TAG, "SPP client connected (handle %lu)", spp->handle);
        break;

    case ESP_SPP_CLOSE_EVT:
        spp->connected = false;
        spp->handle = 0;
        ESP_LOGI(TAG, "SPP client disconnected");
        break;

    case ESP_SPP_DATA_IND_EVT:
        if (param->data_ind.len > 0) {
            /* BTC thread: never block; drop on contention (task holds the
             * mutex only for a short parse/memmove). */
            bool taken = spp->bufMutex
                ? (xSemaphoreTake(spp->bufMutex, 0) == pdTRUE)
                : true;
            if (taken) {
                int space = SPP_MAX_CMD - 1 - spp->cmd_len;
                int copy = (param->data_ind.len < space) ? param->data_ind.len : space;
                if (copy > 0) {
                    memcpy(spp->cmd_buf + spp->cmd_len, param->data_ind.data, copy);
                    spp->cmd_len += copy;
                }
                if (spp->bufMutex) xSemaphoreGive(spp->bufMutex);
            } else {
                ESP_LOGW(TAG, "SPP data dropped: cmd buffer busy");
            }
            if (spp->dataReady) xSemaphoreGive(spp->dataReady);
        }
        break;

    default:
        break;
    }
}

spp_server_t *spp_server_create(eq_processor_t *eq, ibus_t *ibus, cdc_t *cdc, tel_t *tel, ibus_config_t *config, comfort_t *comfort, avrcp_controller_t *avrcp)
{
    spp_server_t *spp = calloc(1, sizeof(spp_server_t));
    if (!spp) return NULL;
    spp->eq = eq;
    spp->ibus = ibus;
    spp->cdc = cdc;
    spp->tel = tel;
    spp->config = config;
    spp->comfort = comfort;
    spp->avrcp = avrcp;
    spp->dataReady = xSemaphoreCreateBinary();
    spp->bufMutex = xSemaphoreCreateMutex();
    if (!spp->dataReady || !spp->bufMutex) {
        if (spp->dataReady) vSemaphoreDelete(spp->dataReady);
        if (spp->bufMutex) vSemaphoreDelete(spp->bufMutex);
        free(spp);
        return NULL;
    }
    return spp;
}

void spp_server_destroy(spp_server_t *spp)
{
    if (!spp) return;
    if (s_instance == spp) s_instance = NULL;
    if (spp->dataReady) vSemaphoreDelete(spp->dataReady);
    if (spp->bufMutex) vSemaphoreDelete(spp->bufMutex);
    free(spp);
}

esp_err_t spp_server_init(spp_server_t *spp)
{
    if (!spp) return ESP_ERR_INVALID_ARG;
    s_instance = spp;

    esp_err_t ret = esp_spp_register_callback(spp_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPP register callback failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_spp_cfg_t spp_cfg = BT_SPP_DEFAULT_CONFIG();
    spp_cfg.mode = ESP_SPP_MODE_CB;
    ret = esp_spp_enhanced_init(&spp_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPP init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Below ibus_task so bulk SPP parsing never delays radio polls. */
    xTaskCreate(spp_cmd_task, "spp_cmd", 4096, spp, 4, NULL);
    ESP_LOGI(TAG, "SPP server initialized");
    return ESP_OK;
}

bool spp_server_is_connected(const spp_server_t *spp)
{
    return spp ? spp->connected : false;
}

esp_err_t spp_server_send(spp_server_t *spp, const char *msg)
{
    if (!spp || !spp->connected || !msg) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = esp_spp_write(spp->handle, (int)strlen(msg), (uint8_t *)msg);
    return ret;
}
