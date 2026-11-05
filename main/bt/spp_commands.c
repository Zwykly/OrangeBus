#include "spp_private.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_avrc_api.h"
#include "eq_processor.h"
#include "ibus.h"
#include "cdc.h"
#include "tel.h"
#include "ibus_config.h"
#include "comfort.h"
#include "avrcp_controller.h"

#define TAG "SPP"

/* Rozwiazanie numeru trybu UI na nazwe do odpowiedzi SPP */
static const char *ui_mode_str(uint8_t mode)
{
    switch (mode) {
    case ORANGEBUS_UI_MODE_CD53: return "CD53";
    case ORANGEBUS_UI_MODE_BMBT: return "BMBT";
    case ORANGEBUS_UI_MODE_MID:  return "MID";
    case ORANGEBUS_UI_MODE_MIR:  return "MIR";
    default: return "UNKNOWN";
    }
}

/* Obsluga komend EQ: ON/OFF, SAVE/LOAD preset, ustawienie pasa, zapytanie */
static void handle_eq_cmd(spp_server_t *spp, const char *cmd)
{
    if (strncmp(cmd, "EQ:ON", 5) == 0) {
        eq_processor_set_enabled(spp->eq, true);
        spp_send_response(spp, "OK\r\n");
        ESP_LOGI(TAG, "EQ enabled via SPP");
    } else if (strncmp(cmd, "EQ:OFF", 6) == 0) {
        eq_processor_set_enabled(spp->eq, false);
        spp_send_response(spp, "OK\r\n");
        ESP_LOGI(TAG, "EQ disabled via SPP");
    } else if (strncmp(cmd, "EQ:SAVE:", 8) == 0) {
        const char *name = cmd + 8;
        esp_err_t ret = eq_processor_save_preset(spp->eq, name);
        spp_send_response(spp, (ret == ESP_OK) ? "OK\r\n" : "ERR\r\n");
    } else if (strncmp(cmd, "EQ:LOAD:", 8) == 0) {
        const char *name = cmd + 8;
        esp_err_t ret = eq_processor_load_preset(spp->eq, name);
        spp_send_response(spp, (ret == ESP_OK) ? "OK\r\n" : "ERR\r\n");
    } else if (strncmp(cmd, "EQ:", 3) == 0 && cmd[3] != '?') {
        int idx;
        float freq, q, gain;
        if (sscanf(cmd, "EQ:%d:%f:%f:%f", &idx, &freq, &q, &gain) == 4) {
            eq_processor_set_band(spp->eq, idx, freq, q, gain);
            spp_send_response(spp, "OK\r\n");
            ESP_LOGI(TAG, "Band %d: %.0fHz Q%.1f %+.1fdB", idx, freq, q, gain);
        } else {
            spp_send_response(spp, "ERR\r\n");
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
        spp_send_response(spp, resp);
    } else {
        spp_send_response(spp, "ERR:UNKNOWN\r\n");
    }
}

/* Obsluga komend IBUS: debug, TEL, CDC, CONFIG, SEND hex, zapytanie statusu */
static void handle_ibus_cmd(spp_server_t *spp, const char *cmd)
{
    if (strncmp(cmd, "IBUS:DEBUG:ON", 13) == 0) {
        ibus_set_debug_mode(spp->ibus, true);
        spp_send_response(spp, "OK:DEBUG_ON\r\n");
    } else if (strncmp(cmd, "IBUS:DEBUG:OFF", 14) == 0) {
        ibus_set_debug_mode(spp->ibus, false);
        spp_send_response(spp, "OK:DEBUG_OFF\r\n");
    } else if (strncmp(cmd, "IBUS:TEL:CONNECT", 16) == 0) {
        tel_set_connected(spp->tel, true);
        spp_send_response(spp, "OK:TEL_CONN\r\n");
    } else if (strncmp(cmd, "IBUS:TEL:DISCONNECT", 19) == 0) {
        tel_set_connected(spp->tel, false);
        spp_send_response(spp, "OK:TEL_DISC\r\n");
    } else if (strncmp(cmd, "IBUS:TEL:CALL:ACTIVE", 20) == 0) {
        tel_set_call_active(spp->tel, true);
        spp_send_response(spp, "OK:CALL_ACTIVE\r\n");
    } else if (strncmp(cmd, "IBUS:TEL:CALL:END", 17) == 0) {
        tel_set_call_active(spp->tel, false);
        spp_send_response(spp, "OK:CALL_END\r\n");
    } else if (strncmp(cmd, "IBUS:TEL:CALL:INCOMING:", 23) == 0) {
        tel_set_call_incoming(spp->tel, true);
        if (strlen(cmd) > 23) tel_set_caller_id(spp->tel, cmd + 23);
        spp_send_response(spp, "OK:CALL_INCOMING\r\n");
    } else if (strncmp(cmd, "IBUS:TEL:CALLER:", 16) == 0) {
        tel_set_caller_id(spp->tel, cmd + 16);
        spp_send_response(spp, "OK:CALLER_ID\r\n");
    } else if (strncmp(cmd, "IBUS:CDC:PLAY", 13) == 0) {
        cdc_set_playing(spp->cdc, true);
        if (spp->avrcp) {
            avrcp_controller_send_passthrough(spp->avrcp, ESP_AVRC_PT_CMD_PLAY);
        }
        spp_send_response(spp, "OK:CDC_PLAY\r\n");
    } else if (strncmp(cmd, "IBUS:CDC:STOP", 13) == 0) {
        cdc_set_playing(spp->cdc, false);
        if (spp->avrcp) {
            avrcp_controller_send_passthrough(spp->avrcp, ESP_AVRC_PT_CMD_PAUSE);
        }
        spp_send_response(spp, "OK:CDC_STOP\r\n");
    } else if (strncmp(cmd, "IBUS:CONFIG:UI:", 15) == 0) {
        uint8_t mode = (uint8_t)atoi(cmd + 15);
        if (mode != ORANGEBUS_UI_MODE_CD53 && mode != ORANGEBUS_UI_MODE_BMBT
            && mode != ORANGEBUS_UI_MODE_MID && mode != ORANGEBUS_UI_MODE_MIR) {
            spp_send_response(spp, "ERR\r\n");
        } else {
            ibus_config_set(spp->config, "ui_mode", mode);
            char resp[32];
            snprintf(resp, sizeof(resp), "OK:UI=%s\r\n", ui_mode_str(mode));
            spp_send_response(spp, resp);
            if (spp->uiModeChanged) {
                *spp->uiModeChanged = true;
            }
        }
    } else if (strncmp(cmd, "IBUS:CONFIG:COMFORT:BLINK:", 26) == 0) {
        uint8_t val = (uint8_t)atoi(cmd + 26);
        ibus_config_set(spp->config, "comfort_blink", val);
        char resp[32];
        snprintf(resp, sizeof(resp), "OK:BLINK:%d\r\n", val);
        spp_send_response(spp, resp);
        if (val && spp->comfort) {
            comfort_send_test_blink(spp->comfort);
        }
    } else if (strncmp(cmd, "IBUS:CONFIG:COMFORT:LOCKS:", 26) == 0) {
        uint8_t val = (uint8_t)atoi(cmd + 26);
        ibus_config_set(spp->config, "comfort_locks", val);
        char resp[32];
        snprintf(resp, sizeof(resp), "OK:LOCKS:%d\r\n", val);
        spp_send_response(spp, resp);
    } else if (strncmp(cmd, "IBUS:CONFIG:COMFORT:MIRRORS:", 28) == 0) {
        uint8_t val = (uint8_t)atoi(cmd + 28);
        ibus_config_set(spp->config, "comfort_mirrors", val);
        char resp[32];
        snprintf(resp, sizeof(resp), "OK:MIRRORS:%d\r\n", val);
        spp_send_response(spp, resp);
    } else if (strncmp(cmd, "IBUS:CONFIG:AUTOPLAY:", 21) == 0) {
        uint8_t val = (uint8_t)atoi(cmd + 21);
        ibus_config_set(spp->config, "autoplay", val);
        char resp[32];
        snprintf(resp, sizeof(resp), "OK:AUTOPLAY:%d\r\n", val);
        spp_send_response(spp, resp);
    } else if (strncmp(cmd, "IBUS:CONFIG:META:", 17) == 0) {
        uint8_t val = (uint8_t)atoi(cmd + 17);
        if (val > 3) {
            spp_send_response(spp, "ERR\r\n");
        } else {
            ibus_config_set(spp->config, "meta_mode", val);
            char resp[32];
            snprintf(resp, sizeof(resp), "OK:META:%d\r\n", val);
            spp_send_response(spp, resp);
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
            spp_send_response(spp, "OK:SENT\r\n");
        } else {
            spp_send_response(spp, "ERR:FORMAT\r\n");
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
        spp_send_response(spp, resp);
    } else {
        spp_send_response(spp, "ERR:UNKNOWN_IBUS\r\n");
    }
}

/* Punkt wejsciowy parsera komend - rozdziela linie EQ i IBUS */
void spp_process_line(spp_server_t *spp, const char *line)
{
    if (strncmp(line, "EQ", 2) == 0) {
        handle_eq_cmd(spp, line);
    } else if (strncmp(line, "IBUS", 4) == 0) {
        handle_ibus_cmd(spp, line);
    } else {
        spp_send_response(spp, "ERR:UNKNOWN\r\n");
    }
}
