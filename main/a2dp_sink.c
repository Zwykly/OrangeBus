#include "a2dp_sink.h"
#include <stdlib.h>
#include "audio_output.h"
#include "esp_log.h"
#include "esp_a2dp_api.h"
#include "freertos/FreeRTOS.h"

#define TAG "A2DP_SINK"

struct a2dp_sink_t {
    orangebus_a2dp_state_t state;
    audio_output_t *audio;
    orangebus_hfp_state_t *hfp_state_ref;
};

static a2dp_sink_t *s_instance = NULL;

static void a2dp_data_cb(const uint8_t *data, uint32_t len)
{
    if (s_instance) {
        audio_output_a2dp_data_cb(s_instance->audio, data, len);
    }
}

static void a2dp_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    a2dp_sink_t *sink = s_instance;
    if (!sink) return;

    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            sink->state = ORANGEBUS_A2DP_CONNECTED;
            ESP_LOGI(TAG, "A2DP Connected");
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            sink->state = ORANGEBUS_A2DP_IDLE;
            ESP_LOGI(TAG, "A2DP Disconnected");
        }
        break;
    case ESP_A2D_AUDIO_STATE_EVT:
        if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
            sink->state = ORANGEBUS_A2DP_PLAYING;
            audio_output_switch_a2dp(sink->audio);
            audio_output_set_mute(sink->audio, false);
            ESP_LOGI(TAG, "A2DP Playing");
        } else if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_SUSPEND) {
            sink->state = ORANGEBUS_A2DP_PAUSED;
            ESP_LOGI(TAG, "A2DP Suspended");
        }
        break;
    default:
        break;
    }
}

a2dp_sink_t *a2dp_sink_create(audio_output_t *audio)
{
    a2dp_sink_t *sink = calloc(1, sizeof(a2dp_sink_t));
    if (!sink) return NULL;
    sink->audio = audio;
    return sink;
}

void a2dp_sink_destroy(a2dp_sink_t *sink)
{
    if (sink) free(sink);
}

esp_err_t a2dp_sink_init(a2dp_sink_t *sink)
{
    if (!sink) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

orangebus_a2dp_state_t a2dp_sink_get_state(const a2dp_sink_t *sink)
{
    return sink ? sink->state : ORANGEBUS_A2DP_IDLE;
}

orangebus_a2dp_state_t *a2dp_sink_get_state_ptr(a2dp_sink_t *sink)
{
    return sink ? &sink->state : NULL;
}

const char *a2dp_sink_state_str(orangebus_a2dp_state_t state)
{
    switch (state) {
    case ORANGEBUS_A2DP_PLAYING:    return "PLAYING";
    case ORANGEBUS_A2DP_CONNECTED:  return "CONNECTED";
    case ORANGEBUS_A2DP_PAUSED:     return "PAUSED";
    case ORANGEBUS_A2DP_CONNECTING: return "CONNECTING";
    default:                      return "IDLE";
    }
}

void a2dp_sink_set_hfp_state_ref(a2dp_sink_t *sink, orangebus_hfp_state_t *ref)
{
    if (sink) sink->hfp_state_ref = ref;
}

esp_err_t a2dp_sink_register_callbacks(a2dp_sink_t *sink)
{
    if (!sink) return ESP_ERR_INVALID_ARG;
    s_instance = sink;
    esp_a2d_register_callback(a2dp_cb);
    esp_err_t ret = esp_a2d_sink_init();
    esp_a2d_sink_register_data_callback(a2dp_data_cb);
    return ret;
}
