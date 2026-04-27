#include "cli.h"
#include <stdlib.h>
#include "audio_output.h"
#include "avrcp_controller.h"
#include "a2dp_sink.h"
#include "hfp_client.h"
#include "eq_processor.h"
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
            if (a2dp_sink_get_state(cli->a2dp) >= BLUEBUS_A2DP_CONNECTED) {
                avrcp_controller_send_passthrough(cli->avrcp, ESP_AVRC_PT_CMD_PLAY);
                ESP_LOGI(TAG, "CMD: Play");
            }
            break;
        case 's':
            if (a2dp_sink_get_state(cli->a2dp) == BLUEBUS_A2DP_PLAYING) {
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
            if (hfp_client_get_state(cli->hfp) == BLUEBUS_HFP_INCOMING) {
                hfp_client_answer(cli->hfp);
                ESP_LOGI(TAG, "CMD: Answer");
            }
            break;
        case 'r':
            if (hfp_client_get_state(cli->hfp) == BLUEBUS_HFP_INCOMING) {
                hfp_client_reject(cli->hfp);
                ESP_LOGI(TAG, "CMD: Reject");
            } else if (hfp_client_get_state(cli->hfp) == BLUEBUS_HFP_ACTIVE ||
                       hfp_client_get_state(cli->hfp) == BLUEBUS_HFP_OUTGOING) {
                hfp_client_reject(cli->hfp);
                ESP_LOGI(TAG, "CMD: End call");
            }
            break;
        case 'd':
            if (hfp_client_get_state(cli->hfp) >= BLUEBUS_HFP_CONNECTED) {
                hfp_client_redial(cli->hfp);
                ESP_LOGI(TAG, "CMD: Redial");
            }
            break;
        case 'v':
            if (a2dp_sink_get_state(cli->a2dp) >= BLUEBUS_A2DP_CONNECTED) {
                avrcp_controller_send_passthrough(cli->avrcp, AVRCP_PT_CMD_VOICE_RECOG);
                ESP_LOGI(TAG, "CMD: Voice Assistant (AVRCP)");
            }
            break;
        case 'V':
            if (hfp_client_get_state(cli->hfp) >= BLUEBUS_HFP_CONNECTED) {
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
const bluebus_metadata_t *meta = avrcp_controller_get_metadata(cli->avrcp);
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
        default:
            break;
        }
    }
}

cli_t *cli_create(audio_output_t *audio, avrcp_controller_t *avrcp, a2dp_sink_t *a2dp, hfp_client_t *hfp, eq_processor_t *eq)
{
cli_t *cli = calloc(1, sizeof(cli_t));
if (!cli) return NULL;
cli->audio = audio;
cli->avrcp = avrcp;
cli->a2dp = a2dp;
cli->hfp = hfp;
cli->eq = eq;
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
