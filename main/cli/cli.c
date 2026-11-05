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
#include "power_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_avrc_api.h"
#include <stdio.h>
#include "esp_system.h"

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
    power_manager_t *power;
};

/* Serial console task: one character = one command.
 *   +/- volume, m mute, p/s play/pause, n/b next/prev,
 *   a/r/d answer/reject/redial, v/V voice assistant (AVRCP/HFP),
 *   h status, e/E EQ toggle/bands, i/I I-BUS debug/status,
 *   u/U/c/C UI mode / autoplay / comfort settings,
 *   P power-gating status, z force a power-off attempt. */
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
        /* --- Volume --- */
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
        /* --- Playback (AVRCP) --- */
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
        /* --- Phone (HFP) --- */
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
        /* --- System status --- */
        case 'h':
        case '?':
            ESP_LOGI(TAG, "=== Status ===");
            ESP_LOGI(TAG, " A2DP: %s", a2dp_sink_state_str(a2dp_sink_get_state(cli->a2dp)));
            ESP_LOGI(TAG, " HFP: %s", hfp_client_state_str(hfp_client_get_state(cli->hfp)));
            ESP_LOGI(TAG, " Vol: %d%% %s", audio_output_get_volume(cli->audio),
                     audio_output_is_muted(cli->audio) ? "(MUTED)" : "");
            ESP_LOGI(TAG, " EQ: %s", eq_processor_is_enabled(cli->eq) ? "ON" : "OFF");
            {
                /* Snapshot copy: metadata is written by the BT task. */
                orangebus_metadata_t meta;
                avrcp_controller_copy_metadata(cli->avrcp, &meta);
                ESP_LOGI(TAG, " Meta: %s - %s (%s)",
                         meta.title[0] ? meta.title : "-",
                         meta.artist[0] ? meta.artist : "-",
                         meta.album[0] ? meta.album : "-");
            }
            ESP_LOGI(TAG, " Heap: free=%lu min=%lu",
                     (unsigned long)esp_get_free_heap_size(),
                     (unsigned long)esp_get_minimum_free_heap_size());
            if (hfp_client_get_caller_id(cli->hfp)[0]) {
                ESP_LOGI(TAG, " Caller: %s", hfp_client_get_caller_id(cli->hfp));
            }
            if (hfp_client_is_vra_active(cli->hfp)) {
                ESP_LOGI(TAG, " Voice: ACTIVE");
            }
            break;
        /* --- Equalizer --- */
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
        /* --- I-BUS debug and status --- */
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
                ESP_LOGI(TAG, " UI: %s, Autoplay: %d, Blink: %d, Locks: %d, Mirrors: %d",
                         ibus_config_ui_mode_name(ibus_config_get(cli->config, IBUS_CFG_UI_MODE)),
                         ibus_config_get(cli->config, IBUS_CFG_AUTOPLAY),
                         ibus_config_get(cli->config, IBUS_CFG_COMFORT_BLINK),
                         ibus_config_get(cli->config, IBUS_CFG_COMFORT_LOCKS),
                         ibus_config_get(cli->config, IBUS_CFG_COMFORT_MIRRORS));
            }
            break;
        /* --- Settings: UI / autoplay / comfort --- */
        case 'u':
            if (cli->config) {
                /* Cycle CD53 -> BMBT -> MID -> MIR -> CD53. An unknown stored
                 * value restarts at CD53. ibus_task applies the change. */
                static const uint8_t modes[] = {ORANGEBUS_UI_MODE_CD53, ORANGEBUS_UI_MODE_BMBT,
                                                ORANGEBUS_UI_MODE_MID, ORANGEBUS_UI_MODE_MIR};
                const int modeCount = sizeof(modes) / sizeof(modes[0]);
                uint8_t current = ibus_config_get(cli->config, IBUS_CFG_UI_MODE);
                uint8_t next = ORANGEBUS_UI_MODE_CD53;
                for (int j = 0; j < modeCount; j++) {
                    if (modes[j] == current) { next = modes[(j + 1) % modeCount]; break; }
                }
                ibus_config_set(cli->config, IBUS_CFG_UI_MODE, next);
                ESP_LOGI(TAG, "UI Mode: %s", ibus_config_ui_mode_name(next));
            }
            break;
        case 'U':
            if (cli->config) {
                ibus_config_set(cli->config, IBUS_CFG_AUTOPLAY,
                    ibus_config_get(cli->config, IBUS_CFG_AUTOPLAY) ? 0 : 1);
                ESP_LOGI(TAG, "Autoplay: %s", ibus_config_get(cli->config, IBUS_CFG_AUTOPLAY) ? "ON" : "OFF");
            }
            break;
        case 'c':
            if (cli->config) {
                ibus_config_set(cli->config, IBUS_CFG_COMFORT_BLINK,
                    ibus_config_get(cli->config, IBUS_CFG_COMFORT_BLINK) ? 0 : 1);
                ESP_LOGI(TAG, "Comfort Blink: %s",
                    ibus_config_get(cli->config, IBUS_CFG_COMFORT_BLINK) ? "ON" : "OFF");
            }
            break;
        case 'C':
            if (cli->config) {
                uint8_t locks = ibus_config_get(cli->config, IBUS_CFG_COMFORT_LOCKS) ? 0 : 1;
                uint8_t mirrors = ibus_config_get(cli->config, IBUS_CFG_COMFORT_MIRRORS) ? 0 : 1;
                ibus_config_set(cli->config, IBUS_CFG_COMFORT_LOCKS, locks);
                ibus_config_set(cli->config, IBUS_CFG_COMFORT_MIRRORS, mirrors);
                ESP_LOGI(TAG, "Locks: %s, Mirrors: %s",
                    locks ? "ON" : "OFF", mirrors ? "ON" : "OFF");
            }
            break;
        /* --- Power gating --- */
        case 'P':
            power_manager_print_status(cli->power);
            break;
        case 'z':
            /* Bypasses the policy - the board switches off right away. */
            ESP_LOGW(TAG, "Forcing power-off attempt");
            power_manager_request_off(cli->power);
            break;
        default:
            break;
        }
    }
}

/* Creates the CLI with references to the modules it controls. */
cli_t *cli_create(audio_output_t *audio, avrcp_controller_t *avrcp, a2dp_sink_t *a2dp, hfp_client_t *hfp, eq_processor_t *eq, ibus_t *ibus, cdc_t *cdc, tel_t *tel, ibus_config_t *config, power_manager_t *power)
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
    cli->power = power;
    return cli;
}

void cli_destroy(cli_t *cli)
{
    if (cli) free(cli);
}

/* Starts the serial console task.
 * Lower priority than ibus_task so CLI parsing never delays radio polls. */
void cli_start(cli_t *cli)
{
    if (!cli) return;
    xTaskCreate(serial_cmd_task, "serial_cmd", 3072, cli, 4, NULL);
}
