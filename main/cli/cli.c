#include "cli.h"
#include <stdlib.h>
#include "audio_output.h"
#include "avrcp_controller.h"
#include "a2dp_sink.h"
#include "hfp_client.h"
#include "eq_processor.h"
#include "ibus.h"
#include "cdc.h"
#include "tel.h"
#include "ibus_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_avrc_api.h"
#include <stdio.h>

#define TAG "CLI"

struct cli_t {
    audio_output_t *audio;
    avrcp_controller_t *avrcp;
    a2dp_sink_t *a2dp;
    hfp_client_t *hfp;
    eq_processor_t *eq;
    ibus_t *ibus;
    cdc_t *cdc;
    tel_t *tel;
    ibus_config_t *config;
};

static void serial_cmd_task(void *arg)
{
    cli_t *cli = (cli_t *)arg;
    char buf[1];
    while (1) {
        int len = fread(buf, 1, 1, stdin);
        if (len <= 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        char c = buf[0];
        switch (c) {
        case '+':
            audio_output_adjust_volume(cli->audio, 5);
            ESP_LOGI(TAG, "Vol: %d%%", audio_output_get_volume(cli->audio));
            break;
        case '-':
            audio_output_adjust_volume(cli->audio, -5);
            ESP_LOGI(TAG, "Vol: %d%%", audio_output_get_volume(cli->audio));
            break;
        case 'm':
            audio_output_toggle_mute(cli->audio);
            ESP_LOGI(TAG, "%s", audio_output_is_muted(cli->audio) ? "MUTED" : "UNMUTED");
            break;
        case 'p':
            if (a2dp_sink_get_state(cli->a2dp) >= ORANGEBUS_A2DP_CONNECTED) {
                avrcp_controller_send_passthrough(cli->avrcp, ESP_AVRC_PT_CMD_PLAY);
                ESP_LOGI(TAG, "CMD: Play");
            }
            break;
        case 's':
            if (a2dp_sink_get_state(cli->a2dp) == ORANGEBUS_A2DP_PLAYING) {
                avrcp_controller_send_passthrough(cli->avrcp, ESP_AVRC_PT_CMD_PAUSE);
                ESP_LOGI(TAG, "CMD: Pause");
            }
            break;
        case 'n':
            avrcp_controller_send_passthrough(cli->avrcp, ESP_AVRC_PT_CMD_FORWARD);
            ESP_LOGI(TAG, "CMD: Next track");
            break;
        case 'b':
            avrcp_controller_send_passthrough(cli->avrcp, ESP_AVRC_PT_CMD_BACKWARD);
            ESP_LOGI(TAG, "CMD: Previous track");
            break;
        case 'a':
            if (hfp_client_get_state(cli->hfp) == ORANGEBUS_HFP_INCOMING) {
                hfp_client_answer(cli->hfp);
                ESP_LOGI(TAG, "CMD: Answer");
            }
            break;
        case 'r':
            if (hfp_client_get_state(cli->hfp) == ORANGEBUS_HFP_INCOMING) {
                hfp_client_reject(cli->hfp);
                ESP_LOGI(TAG, "CMD: Reject");
            } else if (hfp_client_get_state(cli->hfp) == ORANGEBUS_HFP_ACTIVE ||
                       hfp_client_get_state(cli->hfp) == ORANGEBUS_HFP_OUTGOING) {
                hfp_client_reject(cli->hfp);
                ESP_LOGI(TAG, "CMD: End call");
            }
            break;
        case 'd':
            if (hfp_client_get_state(cli->hfp) >= ORANGEBUS_HFP_CONNECTED) {
                hfp_client_redial(cli->hfp);
                ESP_LOGI(TAG, "CMD: Redial");
            }
            break;
        case 'v':
            if (a2dp_sink_get_state(cli->a2dp) >= ORANGEBUS_A2DP_CONNECTED) {
                avrcp_controller_send_passthrough(cli->avrcp, AVRCP_PT_CMD_VOICE_RECOG);
                ESP_LOGI(TAG, "CMD: Voice Assistant (AVRCP)");
            }
            break;
        case 'V':
            if (hfp_client_get_state(cli->hfp) >= ORANGEBUS_HFP_CONNECTED) {
                hfp_client_toggle_voice_recognition(cli->hfp);
                ESP_LOGI(TAG, "CMD: Voice Recognition %s (HFP)",
                         hfp_client_is_vra_active(cli->hfp) ? "OFF" : "ON");
            }
            break;
case 'h':
case '?':
ESP_LOGI(TAG, "=== Status ===");
ESP_LOGI(TAG, " A2DP: %s", a2dp_sink_state_str(a2dp_sink_get_state(cli->a2dp)));
ESP_LOGI(TAG, " HFP: %s", hfp_client_state_str(hfp_client_get_state(cli->hfp)));
ESP_LOGI(TAG, " Vol: %d%% %s", audio_output_get_volume(cli->audio),
audio_output_is_muted(cli->audio) ? "(MUTED)" : "");
ESP_LOGI(TAG, " EQ: %s", eq_processor_is_enabled(cli->eq) ? "ON" : "OFF");
{
const orangebus_metadata_t *meta = avrcp_controller_get_metadata(cli->avrcp);
if (meta) {
ESP_LOGI(TAG, " Meta: %s - %s (%s)",
meta->title[0] ? meta->title : "-",
meta->artist[0] ? meta->artist : "-",
meta->album[0] ? meta->album : "-");
}
}
if (hfp_client_get_caller_id(cli->hfp)[0]) {
ESP_LOGI(TAG, " Caller: %s", hfp_client_get_caller_id(cli->hfp));
}
if (hfp_client_is_vra_active(cli->hfp)) {
ESP_LOGI(TAG, " Voice: ACTIVE");
}
break;
case 'e':
eq_processor_set_enabled(cli->eq, !eq_processor_is_enabled(cli->eq));
ESP_LOGI(TAG, "EQ: %s", eq_processor_is_enabled(cli->eq) ? "ON" : "OFF");
break;
case 'E':
ESP_LOGI(TAG, "=== EQ Bands ===");
for (int i = 0; i < EQ_BANDS; i++) {
const eq_band_params_t *b = eq_processor_get_band(cli->eq, i);
if (b) {
ESP_LOGI(TAG, " B%d: %.0fHz Q%.1f %+.1fdB", i, b->freq, b->q, b->gain_db);
}
}
        break;
    case 'i':
        if (cli->ibus) {
            ibus_set_debug_mode(cli->ibus, !ibus_is_debug_mode(cli->ibus));
            ESP_LOGI(TAG, "IBUS Debug: %s", ibus_is_debug_mode(cli->ibus) ? "ON" : "OFF");
        }
        break;
    case 'I':
        ESP_LOGI(TAG, "=== IBUS Status ===");
        if (cli->ibus) {
            ESP_LOGI(TAG, " Debug: %s", ibus_is_debug_mode(cli->ibus) ? "ON" : "OFF");
        }
        if (cli->cdc) {
            ESP_LOGI(TAG, " CDC: %s, Ign: %s",
                cdc_is_playing(cli->cdc) ? "PLAYING" : "STOPPED",
                cdc_is_ignition_on(cli->cdc) ? "ON" : "OFF");
        }
        if (cli->tel) {
            ESP_LOGI(TAG, " TEL: %s, Call: %s",
                tel_is_connected(cli->tel) ? "CONNECTED" : "DISCONNECTED",
                tel_is_call_active(cli->tel) ? "ACTIVE" : "INACTIVE");
        }
        if (cli->config) {
            uint8_t uiMode = ibus_config_get(cli->config, "ui_mode");
            const char *uiStr = "UNKNOWN";
            switch (uiMode) {
            case ORANGEBUS_UI_MODE_CD53: uiStr = "CD53"; break;
            case ORANGEBUS_UI_MODE_BMBT: uiStr = "BMBT"; break;
            case ORANGEBUS_UI_MODE_MID:  uiStr = "MID"; break;
            case ORANGEBUS_UI_MODE_MIR:  uiStr = "MIR"; break;
            }
            ESP_LOGI(TAG, " UI: %s, Autoplay: %d, Blink: %d, Locks: %d, Mirrors: %d",
                uiStr,
                ibus_config_get(cli->config, "autoplay"),
                ibus_config_get(cli->config, "comfort_blink"),
                ibus_config_get(cli->config, "comfort_locks"),
                ibus_config_get(cli->config, "comfort_mirrors"));
        }
        break;
    case 'u':
        if (cli->config) {
            uint8_t modes[] = {ORANGEBUS_UI_MODE_CD53, ORANGEBUS_UI_MODE_BMBT, ORANGEBUS_UI_MODE_MID, ORANGEBUS_UI_MODE_MIR};
            uint8_t cur = ibus_config_get(cli->config, "ui_mode");
            uint8_t next = 0;
            for (int j = 0; j < 4; j++) {
                if (modes[j] == cur) { next = modes[(j + 1) % 4]; break; }
            }
            ibus_config_set(cli->config, "ui_mode", next);
            const char *str = "UNKNOWN";
            switch (next) {
            case ORANGEBUS_UI_MODE_CD53: str = "CD53"; break;
            case ORANGEBUS_UI_MODE_BMBT: str = "BMBT"; break;
            case ORANGEBUS_UI_MODE_MID:  str = "MID"; break;
            case ORANGEBUS_UI_MODE_MIR:  str = "MIR"; break;
            }
            ESP_LOGI(TAG, "UI Mode: %s", str);
        }
        break;
    case 'U':
        if (cli->config) {
            ibus_config_set(cli->config, "autoplay",
                ibus_config_get(cli->config, "autoplay") ? 0 : 1);
            ESP_LOGI(TAG, "Autoplay: %s", ibus_config_get(cli->config, "autoplay") ? "ON" : "OFF");
        }
        break;
    case 'c':
        if (cli->config) {
            ibus_config_set(cli->config, "comfort_blink",
                ibus_config_get(cli->config, "comfort_blink") ? 0 : 1);
            ESP_LOGI(TAG, "Comfort Blink: %s",
                ibus_config_get(cli->config, "comfort_blink") ? "ON" : "OFF");
        }
        break;
    case 'C':
        if (cli->config) {
            uint8_t locks = ibus_config_get(cli->config, "comfort_locks") ? 0 : 1;
            uint8_t mirrors = ibus_config_get(cli->config, "comfort_mirrors") ? 0 : 1;
            ibus_config_set(cli->config, "comfort_locks", locks);
            ibus_config_set(cli->config, "comfort_mirrors", mirrors);
            ESP_LOGI(TAG, "Locks: %s, Mirrors: %s",
                locks ? "ON" : "OFF", mirrors ? "ON" : "OFF");
        }
        break;
    default:
            break;
        }
    }
}

cli_t *cli_create(audio_output_t *audio, avrcp_controller_t *avrcp, a2dp_sink_t *a2dp, hfp_client_t *hfp, eq_processor_t *eq, ibus_t *ibus, cdc_t *cdc, tel_t *tel, ibus_config_t *config)
{
    cli_t *cli = calloc(1, sizeof(cli_t));
    if (!cli) return NULL;
    cli->audio = audio;
    cli->avrcp = avrcp;
    cli->a2dp = a2dp;
    cli->hfp = hfp;
    cli->eq = eq;
    cli->ibus = ibus;
    cli->cdc = cdc;
    cli->tel = tel;
    cli->config = config;
    return cli;
}

void cli_destroy(cli_t *cli)
{
    if (cli) free(cli);
}

void cli_start(cli_t *cli)
{
    if (!cli) return;
    xTaskCreate(serial_cmd_task, "serial_cmd", 3072, cli, 5, NULL);
}
