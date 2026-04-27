#include "spp_server.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_spp_api.h"
#include "eq_processor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "SPP"
#define SPP_SERVER_NAME "BMW_BlueBus_SPP"
#define SPP_MAX_CMD 256

struct spp_server_t {
    bool connected;
    uint32_t handle;
    eq_processor_t *eq;
    char cmd_buf[SPP_MAX_CMD];
    int cmd_len;
};

static spp_server_t *s_instance = NULL;

static void send_response(spp_server_t *spp, const char *msg)
{
if (!spp || !spp->connected || spp->handle == 0) return;
esp_spp_write(spp->handle, (int)strlen(msg), (uint8_t *)msg);
}

static void handle_eq_cmd(spp_server_t *spp, const char *cmd)
{
    if (strncmp(cmd, "EQ:ON", 5) == 0) {
        eq_processor_set_enabled(spp->eq, true);
        send_response(spp, "OK\r\n");
        ESP_LOGI(TAG, "EQ enabled via SPP");
    } else if (strncmp(cmd, "EQ:OFF", 6) == 0) {
        eq_processor_set_enabled(spp->eq, false);
        send_response(spp, "OK\r\n");
        ESP_LOGI(TAG, "EQ disabled via SPP");
    } else if (strncmp(cmd, "EQ:SAVE:", 8) == 0) {
        const char *name = cmd + 8;
        esp_err_t ret = eq_processor_save_preset(spp->eq, name);
        send_response(spp, (ret == ESP_OK) ? "OK\r\n" : "ERR\r\n");
    } else if (strncmp(cmd, "EQ:LOAD:", 8) == 0) {
        const char *name = cmd + 8;
        esp_err_t ret = eq_processor_load_preset(spp->eq, name);
        send_response(spp, (ret == ESP_OK) ? "OK\r\n" : "ERR\r\n");
    } else if (strncmp(cmd, "EQ:", 3) == 0 && cmd[3] != '?') {
        int idx;
        float freq, q, gain;
        if (sscanf(cmd, "EQ:%d:%f:%f:%f", &idx, &freq, &q, &gain) == 4) {
            eq_processor_set_band(spp->eq, idx, freq, q, gain);
            send_response(spp, "OK\r\n");
            ESP_LOGI(TAG, "Band %d: %.0fHz Q%.1f %+.1fdB", idx, freq, q, gain);
        } else {
            send_response(spp, "ERR\r\n");
        }
    } else if (strncmp(cmd, "EQ?", 3) == 0) {
        char resp[256];
        int pos = 0;
        for (int i = 0; i < EQ_BANDS; i++) {
            const eq_band_params_t *b = eq_processor_get_band(spp->eq, i);
            if (!b) continue;
            pos += snprintf(resp + pos, sizeof(resp) - pos, "%s%d:%.0f:%.1f:%+.1f",
                            (i > 0) ? ";" : "", i, b->freq, b->q, b->gain_db);
        }
        pos += snprintf(resp + pos, sizeof(resp) - pos, "\r\n");
        send_response(spp, resp);
    } else {
        send_response(spp, "ERR:UNKNOWN\r\n");
    }
}

static void process_line(spp_server_t *spp, const char *line)
{
    if (strncmp(line, "EQ", 2) == 0) {
        handle_eq_cmd(spp, line);
    } else {
        send_response(spp, "ERR:UNKNOWN\r\n");
    }
}

static void spp_cmd_task(void *arg)
{
    spp_server_t *spp = (spp_server_t *)arg;
    while (1) {
        if (spp->cmd_len > 0) {
            spp->cmd_buf[spp->cmd_len] = '\0';

            char *start = spp->cmd_buf;
            while (*start) {
                char *cr = strchr(start, '\r');
                char *lf = strchr(start, '\n');
                char *end = cr ? cr : lf;
                if (!end) break;

                *end = '\0';
                if (end > start) {
                    process_line(spp, start);
                }
                start = end + 1;
                while (*start == '\r' || *start == '\n') start++;
            }

            int remaining = spp->cmd_len - (int)(start - spp->cmd_buf);
            if (remaining > 0 && start != spp->cmd_buf) {
                memmove(spp->cmd_buf, start, remaining);
            }
            spp->cmd_len = remaining;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

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
            int space = SPP_MAX_CMD - 1 - spp->cmd_len;
            int copy = (param->data_ind.len < space) ? param->data_ind.len : space;
            if (copy > 0) {
                memcpy(spp->cmd_buf + spp->cmd_len, param->data_ind.data, copy);
                spp->cmd_len += copy;
            }
        }
        break;

    default:
        break;
    }
}

spp_server_t *spp_server_create(eq_processor_t *eq)
{
    spp_server_t *spp = calloc(1, sizeof(spp_server_t));
    if (!spp) return NULL;
    spp->eq = eq;
    return spp;
}

void spp_server_destroy(spp_server_t *spp)
{
    if (spp) free(spp);
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

    xTaskCreate(spp_cmd_task, "spp_cmd", 4096, spp, 5, NULL);
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
