#include "spp_server.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_spp_api.h"
#include "esp_avrc_api.h"
#include "eq_processor.h"
#include "ibus.h"
#include "cdc.h"
#include "tel.h"
#include "ibus_config.h"
#include "comfort.h"
#include "avrcp_controller.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "SPP"
#define SPP_SERVER_NAME "BMW_BlueBus_SPP"
#define SPP_MAX_CMD 256

struct spp_server_t {
bool connected;
uint32_t handle;
eq_processor_t *eq;
ibus_t *ibus;
cdc_t *cdc;
tel_t *tel;
ibus_config_t *config;
comfort_t *comfort;
avrcp_controller_t *avrcp;
volatile bool *uiModeChanged;
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

static const char *ui_mode_str(uint8_t mode)
{
    switch (mode) {
    case BLUEBUS_UI_MODE_CD53: return "CD53";
    case BLUEBUS_UI_MODE_BMBT: return "BMBT";
    case BLUEBUS_UI_MODE_MID:  return "MID";
    case BLUEBUS_UI_MODE_MIR:  return "MIR";
    default: return "UNKNOWN";
    }
}

static void handle_ibus_cmd(spp_server_t *spp, const char *cmd)
{
    if (strncmp(cmd, "IBUS:DEBUG:ON", 13) == 0) {
        ibus_set_debug_mode(spp->ibus, true);
        send_response(spp, "OK:DEBUG_ON\r\n");
    } else if (strncmp(cmd, "IBUS:DEBUG:OFF", 14) == 0) {
        ibus_set_debug_mode(spp->ibus, false);
        send_response(spp, "OK:DEBUG_OFF\r\n");
    } else if (strncmp(cmd, "IBUS:TEL:CONNECT", 16) == 0) {
        tel_set_connected(spp->tel, true);
        send_response(spp, "OK:TEL_CONN\r\n");
    } else if (strncmp(cmd, "IBUS:TEL:DISCONNECT", 19) == 0) {
        tel_set_connected(spp->tel, false);
        send_response(spp, "OK:TEL_DISC\r\n");
    } else if (strncmp(cmd, "IBUS:TEL:CALL:ACTIVE", 20) == 0) {
        tel_set_call_active(spp->tel, true);
        send_response(spp, "OK:CALL_ACTIVE\r\n");
    } else if (strncmp(cmd, "IBUS:TEL:CALL:END", 17) == 0) {
        tel_set_call_active(spp->tel, false);
        send_response(spp, "OK:CALL_END\r\n");
    } else if (strncmp(cmd, "IBUS:TEL:CALL:INCOMING:", 23) == 0) {
        tel_set_call_incoming(spp->tel, true);
        if (strlen(cmd) > 23) tel_set_caller_id(spp->tel, cmd + 23);
        send_response(spp, "OK:CALL_INCOMING\r\n");
    } else if (strncmp(cmd, "IBUS:TEL:CALLER:", 16) == 0) {
        tel_set_caller_id(spp->tel, cmd + 16);
        send_response(spp, "OK:CALLER_ID\r\n");
	} else if (strncmp(cmd, "IBUS:CDC:PLAY", 13) == 0) {
		cdc_set_playing(spp->cdc, true);
		if (spp->avrcp) {
			avrcp_controller_send_passthrough(spp->avrcp, ESP_AVRC_PT_CMD_PLAY);
		}
		send_response(spp, "OK:CDC_PLAY\r\n");
	} else if (strncmp(cmd, "IBUS:CDC:STOP", 13) == 0) {
		cdc_set_playing(spp->cdc, false);
		if (spp->avrcp) {
			avrcp_controller_send_passthrough(spp->avrcp, ESP_AVRC_PT_CMD_PAUSE);
		}
		send_response(spp, "OK:CDC_STOP\r\n");
	} else if (strncmp(cmd, "IBUS:CONFIG:UI:", 15) == 0) {
		uint8_t mode = (uint8_t)atoi(cmd + 15);
		if (mode != BLUEBUS_UI_MODE_CD53 && mode != BLUEBUS_UI_MODE_BMBT
			&& mode != BLUEBUS_UI_MODE_MID && mode != BLUEBUS_UI_MODE_MIR) {
			send_response(spp, "ERR\r\n");
		} else {
			ibus_config_set(spp->config, "ui_mode", mode);
			char resp[32];
			snprintf(resp, sizeof(resp), "OK:UI=%s\r\n", ui_mode_str(mode));
			send_response(spp, resp);
			if (spp->uiModeChanged) {
				*spp->uiModeChanged = true;
			}
		}
	} else if (strncmp(cmd, "IBUS:CONFIG:COMFORT:BLINK:", 26) == 0) {
		uint8_t val = (uint8_t)atoi(cmd + 26);
		ibus_config_set(spp->config, "comfort_blink", val);
		char resp[32];
		snprintf(resp, sizeof(resp), "OK:BLINK:%d\r\n", val);
		send_response(spp, resp);
		if (val && spp->comfort) {
			comfort_send_test_blink(spp->comfort);
		}
	} else if (strncmp(cmd, "IBUS:CONFIG:COMFORT:LOCKS:", 26) == 0) {
		uint8_t val = (uint8_t)atoi(cmd + 26);
		ibus_config_set(spp->config, "comfort_locks", val);
		char resp[32];
		snprintf(resp, sizeof(resp), "OK:LOCKS:%d\r\n", val);
		send_response(spp, resp);
	} else if (strncmp(cmd, "IBUS:CONFIG:COMFORT:MIRRORS:", 28) == 0) {
		uint8_t val = (uint8_t)atoi(cmd + 28);
		ibus_config_set(spp->config, "comfort_mirrors", val);
		char resp[32];
		snprintf(resp, sizeof(resp), "OK:MIRRORS:%d\r\n", val);
		send_response(spp, resp);
	} else if (strncmp(cmd, "IBUS:CONFIG:AUTOPLAY:", 21) == 0) {
		uint8_t val = (uint8_t)atoi(cmd + 21);
		ibus_config_set(spp->config, "autoplay", val);
		char resp[32];
		snprintf(resp, sizeof(resp), "OK:AUTOPLAY:%d\r\n", val);
		send_response(spp, resp);
	} else if (strncmp(cmd, "IBUS:CONFIG:META:", 17) == 0) {
		uint8_t val = (uint8_t)atoi(cmd + 17);
		if (val > 3) {
			send_response(spp, "ERR\r\n");
		} else {
			ibus_config_set(spp->config, "meta_mode", val);
			char resp[32];
			snprintf(resp, sizeof(resp), "OK:META:%d\r\n", val);
			send_response(spp, resp);
		}
    } else if (strncmp(cmd, "IBUS:SEND:", 10) == 0) {
        uint8_t buf[32];
        uint8_t idx = 0;
        const char *hex = cmd + 10;
        while (*hex && *(hex + 1) && idx < sizeof(buf)) {
            unsigned int byte;
            if (sscanf(hex, "%2x", &byte) == 1) {
                buf[idx++] = (uint8_t)byte;
            }
            hex += 2;
            while (*hex == ' ') hex++;
        }
        if (idx >= 4) {
            ibus_send_packet(spp->ibus, buf[0], buf[2], buf[3], &buf[4], idx - 4);
            send_response(spp, "OK:SENT\r\n");
        } else {
            send_response(spp, "ERR:FORMAT\r\n");
        }
    } else if (strncmp(cmd, "IBUS?", 5) == 0) {
        char resp[128];
        snprintf(resp, sizeof(resp),
            "IBUS:DEBUG=%s,UI=%s,AUTOPLAY=%d,META=%d,BLINK=%d,LOCKS=%d,MIRRORS=%d\r\n",
            ibus_is_debug_mode(spp->ibus) ? "ON" : "OFF",
            ui_mode_str(ibus_config_get(spp->config, "ui_mode")),
            ibus_config_get(spp->config, "autoplay"),
            ibus_config_get(spp->config, "meta_mode"),
            ibus_config_get(spp->config, "comfort_blink"),
            ibus_config_get(spp->config, "comfort_locks"),
            ibus_config_get(spp->config, "comfort_mirrors"));
        send_response(spp, resp);
    } else {
        send_response(spp, "ERR:UNKNOWN_IBUS\r\n");
    }
}

static void process_line(spp_server_t *spp, const char *line)
{
    if (strncmp(line, "EQ", 2) == 0) {
        handle_eq_cmd(spp, line);
    } else if (strncmp(line, "IBUS", 4) == 0) {
        handle_ibus_cmd(spp, line);
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

spp_server_t *spp_server_create(eq_processor_t *eq, ibus_t *ibus, cdc_t *cdc, tel_t *tel, ibus_config_t *config, comfort_t *comfort, avrcp_controller_t *avrcp, volatile bool *uiModeChanged)
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
	spp->uiModeChanged = uiModeChanged;
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
