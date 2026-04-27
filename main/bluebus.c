#include "bluebus.h"
#include "audio_output.h"
#include "avrcp_controller.h"
#include "a2dp_sink.h"
#include "hfp_client.h"
#include "bt_manager.h"
#include "cli.h"
#include "eq_processor.h"
#include "spp_server.h"
#include "esp_log.h"
#include "esp_gap_bt_api.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"

#define TAG "BLUEBUS"

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " ESP32 BlueBus BT Test (ESP-IDF v6.0)");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Pair phone with 'BMW-BlueBus'");
ESP_LOGI(TAG, "Commands: +/- vol, m mute, p play, s pause");
ESP_LOGI(TAG, " n next, b prev, a answer, r reject, d redial");
ESP_LOGI(TAG, " v voice (AVRCP), V voice (HFP), h status");
ESP_LOGI(TAG, " e toggle EQ, E show EQ bands");

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BLUEBUS_BT_LED) | (1ULL << BLUEBUS_TEL_MUTE),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(BLUEBUS_BT_LED, 0);
    gpio_set_level(BLUEBUS_TEL_MUTE, 0);

    bt_manager_init();

audio_output_t *audio = audio_output_create();
audio_output_init(audio, 44100);

eq_processor_t *eq = eq_processor_create();
eq_processor_init(eq, 44100);
audio_output_set_eq(audio, eq);

    avrcp_controller_t *avrcp = avrcp_controller_create();
    avrcp_controller_init(avrcp);

    a2dp_sink_t *a2dp = a2dp_sink_create(audio, BLUEBUS_BT_LED);
    a2dp_sink_init(a2dp);

    avrcp_controller_set_a2dp_state_ref(avrcp, a2dp_sink_get_state_ptr(a2dp));

    hfp_client_t *hfp = hfp_client_create(audio, avrcp, a2dp, BLUEBUS_TEL_MUTE, BLUEBUS_BT_LED);
    hfp_client_init(hfp);

    a2dp_sink_set_hfp_state_ref(a2dp, hfp_client_get_state_ptr(hfp));

    esp_err_t ret = avrcp_controller_register_callbacks(avrcp);
    ESP_LOGI(TAG, "AVRCP CT init: %s", esp_err_to_name(ret));

    ret = a2dp_sink_register_callbacks(a2dp);
    ESP_LOGI(TAG, "A2DP sink init: %s", esp_err_to_name(ret));

    ret = hfp_client_register_callbacks(hfp);
    ESP_LOGI(TAG, "HFP client init: %s", esp_err_to_name(ret));

esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

spp_server_t *spp = spp_server_create(eq);
ret = spp_server_init(spp);
ESP_LOGI(TAG, "SPP server init: %s", esp_err_to_name(ret));

cli_t *cli = cli_create(audio, avrcp, a2dp, hfp, eq);
    cli_start(cli);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " INIT COMPLETE - BT discoverable!");
    ESP_LOGI(TAG, "========================================");
}
