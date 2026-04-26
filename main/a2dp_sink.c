#include "a2dp_sink.h"
#include <stdlib.h>
#include "audio_output.h"
#include "esp_log.h"
#include "esp_a2dp_api.h"
#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"

#define TAG "A2DP_SINK"

struct a2dp_sink_t {
    bluebus_a2dp_state_t state;
    audio_output_t *audio;
    uint8_t led_pin;
    bluebus_hfp_state_t *hfp_state_ref;
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
            sink->state = BLUEBUS_A2DP_CONNECTED;
            gpio_set_level(sink->led_pin, 1);
            ESP_LOGI(TAG, "A2DP Connected");
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            sink->state = BLUEBUS_A2DP_IDLE;
            if (!sink->hfp_state_ref || *sink->hfp_state_ref < BLUEBUS_HFP_CONNECTED) {
                gpio_set_level(sink->led_pin, 0);
            }
            ESP_LOGI(TAG, "A2DP Disconnected");
        }
        break;
    case ESP_A2D_AUDIO_STATE_EVT:
        if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
            sink->state = BLUEBUS_A2DP_PLAYING;
            audio_output_switch_a2dp(sink->audio);
            audio_output_set_mute(sink->audio, false);
            ESP_LOGI(TAG, "A2DP Playing");
        } else if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_SUSPEND) {
            sink->state = BLUEBUS_A2DP_PAUSED;
            ESP_LOGI(TAG, "A2DP Suspended");
        }
        break;
    default:
        break;
    }
}

a2dp_sink_t *a2dp_sink_create(audio_output_t *audio, uint8_t led_pin)
{
    a2dp_sink_t *sink = calloc(1, sizeof(a2dp_sink_t));
    if (!sink) return NULL;
    sink->audio = audio;
    sink->led_pin = led_pin;
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

bluebus_a2dp_state_t a2dp_sink_get_state(const a2dp_sink_t *sink)
{
    return sink ? sink->state : BLUEBUS_A2DP_IDLE;
}

bluebus_a2dp_state_t *a2dp_sink_get_state_ptr(a2dp_sink_t *sink)
{
    return sink ? &sink->state : NULL;
}

const char *a2dp_sink_state_str(bluebus_a2dp_state_t state)
{
    switch (state) {
    case BLUEBUS_A2DP_PLAYING:    return "PLAYING";
    case BLUEBUS_A2DP_CONNECTED:  return "CONNECTED";
    case BLUEBUS_A2DP_PAUSED:     return "PAUSED";
    case BLUEBUS_A2DP_CONNECTING: return "CONNECTING";
    default:                      return "IDLE";
    }
}

void a2dp_sink_set_hfp_state_ref(a2dp_sink_t *sink, bluebus_hfp_state_t *ref)
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
