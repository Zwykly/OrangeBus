#include "bluebus.h"
#include "audio_output.h"
#include "avrcp_controller.h"
#include "a2dp_sink.h"
#include "hfp_client.h"
#include "bt_manager.h"
#include "cli.h"
#include "eq_processor.h"
#include "spp_server.h"
#include "ibus.h"
#include "ibus_config.h"
#include "cdc.h"
#include "tel.h"
#include "ui_cd53.h"
#include "ui_mir.h"
#include "ui_mid.h"
#include "ui_bmbt.h"
#include "comfort.h"
#include "esp_log.h"
#include "esp_gap_bt_api.h"
#include "esp_avrc_api.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "BLUEBUS"

typedef struct {
    ibus_t *ibus;
    ibus_config_t *config;
    cdc_t *cdc;
    tel_t *tel;
    ui_cd53_t *uiCd53;
    ui_mir_t *uiMir;
    ui_mid_t *uiMid;
    ui_bmbt_t *uiBmbt;
    comfort_t *comfort;
    avrcp_controller_t *avrcp;
    a2dp_sink_t *a2dp;
    hfp_client_t *hfp;
    audio_output_t *audio;
} ibus_ctx_t;

static ibus_ctx_t *s_ibus_ctx = NULL;

static void on_cdc_status_req(uint8_t *data, uint8_t len)
{
    ibus_ctx_t *ic = s_ibus_ctx;
    cdc_on_request(ic->cdc, data, len);
}

static void on_cdc_button_press(uint8_t *data, uint8_t len)
{
    ibus_ctx_t *ic = s_ibus_ctx;
    if (len < 1) return;
    uint8_t cmd = data[0];
    switch (cmd) {
    case BLUEBUS_IBUS_CDC_CMD_START_PLAYING:
        if (a2dp_sink_get_state(ic->a2dp) >= BLUEBUS_A2DP_CONNECTED) {
            avrcp_controller_send_passthrough(ic->avrcp, ESP_AVRC_PT_CMD_PLAY);
        }
        cdc_set_playing(ic->cdc, true);
        ui_mid_on_cdc_start(ic->uiMid);
        break;
    case BLUEBUS_IBUS_CDC_CMD_STOP_PLAYING:
        if (a2dp_sink_get_state(ic->a2dp) == BLUEBUS_A2DP_PLAYING) {
            avrcp_controller_send_passthrough(ic->avrcp, ESP_AVRC_PT_CMD_PAUSE);
        }
        cdc_set_playing(ic->cdc, false);
        ui_mid_on_cdc_stop(ic->uiMid);
        break;
    case BLUEBUS_IBUS_CDC_CMD_CHANGE_TRACK:
        avrcp_controller_send_passthrough(ic->avrcp, ESP_AVRC_PT_CMD_FORWARD);
        break;
    case BLUEBUS_IBUS_CDC_CMD_CHANGE_TRACK_BL:
        avrcp_controller_send_passthrough(ic->avrcp, ESP_AVRC_PT_CMD_BACKWARD);
        break;
    default:
        cdc_on_button_press(ic->cdc, data, len);
        break;
    }
}

static void on_mfl_button(uint8_t *data, uint8_t len)
{
    ibus_ctx_t *ic = s_ibus_ctx;
    if (len < 1) return;
    uint8_t btn = data[0];
    if (btn == BLUEBUS_IBUS_MFL_BTN_SPEAK) {
        bluebus_hfp_state_t hfpState = hfp_client_get_state(ic->hfp);
        if (hfpState == BLUEBUS_HFP_INCOMING) {
            hfp_client_answer(ic->hfp);
        } else if (hfpState == BLUEBUS_HFP_ACTIVE) {
            hfp_client_reject(ic->hfp);
        } else if (hfpState == BLUEBUS_HFP_CONNECTED) {
            hfp_client_redial(ic->hfp);
        } else if (a2dp_sink_get_state(ic->a2dp) >= BLUEBUS_A2DP_CONNECTED && hfpState < BLUEBUS_HFP_CONNECTED) {
            avrcp_controller_send_passthrough(ic->avrcp, ESP_AVRC_PT_CMD_PLAY);
        }
    } else if (btn == BLUEBUS_IBUS_MFL_BTN_VOL_UP) {
        audio_output_adjust_volume(ic->audio, 5);
    } else if (btn == BLUEBUS_IBUS_MFL_BTN_VOL_DOWN) {
        audio_output_adjust_volume(ic->audio, -5);
    }
}

static void on_ignition_status(uint8_t *data, uint8_t len)
{
    ibus_ctx_t *ic = s_ibus_ctx;
    if (len < 1) return;
    bool on = (data[0] != BLUEBUS_IBUS_IGNITION_OFF);
    cdc_on_ignition(ic->cdc, data, len);
    ui_cd53_on_ignition(ic->uiCd53, on);
    ui_mir_on_ignition(ic->uiMir, on);
    ui_mid_on_ignition(ic->uiMid, on);
    ui_bmbt_on_ignition(ic->uiBmbt, on);
    comfort_on_ignition(ic->comfort, on);
}

static void on_volume_change(uint8_t *data, uint8_t len)
{
    ibus_ctx_t *ic = s_ibus_ctx;
    if (len < 1) return;
    uint8_t dir = data[0];
    if (dir == BLUEBUS_IBUS_RAD_VOL_UP) {
        audio_output_adjust_volume(ic->audio, 2);
    } else {
        audio_output_adjust_volume(ic->audio, -2);
    }
}

static void send_metadata_to_ui(ibus_ctx_t *ic, const bluebus_metadata_t *meta, bool playing)
{
    if (!meta) return;
    uint8_t uiMode = ibus_config_get(ic->config, "ui_mode");

    char metaText[BLUEBUS_IBUS_MID_MAX_CHARS + 1];
    if (meta->title[0] && meta->artist[0]) {
        snprintf(metaText, sizeof(metaText), "%.10s - %.10s", meta->title, meta->artist);
    } else if (meta->title[0]) {
        snprintf(metaText, sizeof(metaText), "%.24s", meta->title);
    } else {
        strncpy(metaText, playing ? "Streaming" : "Bluetooth", sizeof(metaText) - 1);
        metaText[sizeof(metaText) - 1] = '\0';
    }

    switch (uiMode) {
    case BLUEBUS_UI_MODE_CD53:
        ui_cd53_show_title(ic->uiCd53, metaText);
        break;
    case BLUEBUS_UI_MODE_MIR:
        ui_mir_show_title(ic->uiMir, metaText);
        break;
    case BLUEBUS_UI_MODE_MID:
        ui_mid_show_title(ic->uiMid, metaText);
        break;
    case BLUEBUS_UI_MODE_BMBT:
        ui_bmbt_on_metadata(ic->uiBmbt, meta->title, meta->artist, meta->album);
        break;
    }
}

static void ibus_task(void *arg)
{
    ibus_ctx_t *ic = (ibus_ctx_t *)arg;
    uint32_t lastTick = xTaskGetTickCount() * portTICK_PERIOD_MS;

    while (1) {
        ibus_process(ic->ibus);

        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - lastTick >= 100) {
            lastTick = now;
            cdc_tick(ic->cdc);
            tel_tick(ic->tel);
            ui_cd53_tick(ic->uiCd53);
            ui_mir_tick(ic->uiMir);
            ui_mid_tick(ic->uiMid);
            ui_bmbt_tick(ic->uiBmbt);
            comfort_tick(ic->comfort);

            bluebus_a2dp_state_t a2dpState = a2dp_sink_get_state(ic->a2dp);
            if (a2dpState == BLUEBUS_A2DP_PLAYING) {
                const bluebus_metadata_t *meta = avrcp_controller_get_metadata(ic->avrcp);
                send_metadata_to_ui(ic, meta, true);
                ui_bmbt_on_playback(ic->uiBmbt, true);
            } else if (a2dpState == BLUEBUS_A2DP_PAUSED || a2dpState == BLUEBUS_A2DP_CONNECTED) {
                ui_bmbt_on_playback(ic->uiBmbt, false);
            }

            bluebus_hfp_state_t hfpState = hfp_client_get_state(ic->hfp);
            if (hfpState == BLUEBUS_HFP_CONNECTED) {
                tel_set_connected(ic->tel, true);
            } else if (hfpState == BLUEBUS_HFP_IDLE) {
                tel_set_connected(ic->tel, false);
                tel_set_call_active(ic->tel, false);
            } else if (hfpState == BLUEBUS_HFP_INCOMING) {
                tel_set_call_incoming(ic->tel, true);
                const char *callerId = hfp_client_get_caller_id(ic->hfp);
                if (callerId && callerId[0]) tel_set_caller_id(ic->tel, callerId);
            } else if (hfpState == BLUEBUS_HFP_ACTIVE || hfpState == BLUEBUS_HFP_AUDIO_OPEN) {
                tel_set_call_active(ic->tel, true);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

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
ESP_LOGI(TAG, " i ibus debug, I ibus status, u cycle UI mode");
ESP_LOGI(TAG, " U autoplay, c blink, C locks+mirrors");

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

ibus_config_t *ibusConfig = ibus_config_create();
ibus_config_init(ibusConfig);

ibus_t *ibus = ibus_create(ibusConfig);
ibus_init(ibus);

cdc_t *cdc = cdc_create(ibus, ibusConfig);
cdc_init(cdc);

tel_t *tel = tel_create(ibus, ibusConfig);
tel_init(tel);

ui_cd53_t *uiCd53 = ui_cd53_create(ibus, ibusConfig);
ui_cd53_init(uiCd53);

ui_mir_t *uiMir = ui_mir_create(ibus, ibusConfig);
ui_mir_init(uiMir);

ui_mid_t *uiMid = ui_mid_create(ibus, ibusConfig);
ui_mid_init(uiMid);

ui_bmbt_t *uiBmbt = ui_bmbt_create(ibus, ibusConfig);
ui_bmbt_init(uiBmbt);

comfort_t *comfort = comfort_create(ibus, ibusConfig);
comfort_init(comfort);

ibus_ctx_t *ic = calloc(1, sizeof(ibus_ctx_t));
s_ibus_ctx = ic;
ic->ibus = ibus;
ic->config = ibusConfig;
ic->cdc = cdc;
ic->tel = tel;
ic->uiCd53 = uiCd53;
ic->uiMir = uiMir;
ic->uiMid = uiMid;
ic->uiBmbt = uiBmbt;
ic->comfort = comfort;
ic->avrcp = avrcp;
ic->a2dp = a2dp;
ic->hfp = hfp;
ic->audio = audio;

ibus_register_callback(ibus, BLUEBUS_IBUS_EVT_CDC_STATUS_REQ, on_cdc_status_req);
ibus_register_callback(ibus, BLUEBUS_IBUS_EVT_CDC_BUTTON_PRESS, on_cdc_button_press);
ibus_register_callback(ibus, BLUEBUS_IBUS_EVT_MFL_BUTTON_PRESS, on_mfl_button);
ibus_register_callback(ibus, BLUEBUS_IBUS_EVT_IGNITION_STATUS, on_ignition_status);
ibus_register_callback(ibus, BLUEBUS_IBUS_EVT_VOLUME_CHANGE, on_volume_change);

spp_server_t *spp = spp_server_create(eq, ibus, cdc, tel, ibusConfig, comfort);
ret = spp_server_init(spp);
ESP_LOGI(TAG, "SPP server init: %s", esp_err_to_name(ret));

cli_t *cli_inst = cli_create(audio, avrcp, a2dp, hfp, eq, ibus, cdc, tel, ibusConfig);
cli_start(cli_inst);

xTaskCreate(ibus_task, "ibus_task", 4096, ic, 5, NULL);

ESP_LOGI(TAG, "========================================");
ESP_LOGI(TAG, " INIT COMPLETE - BT discoverable!");
ESP_LOGI(TAG, " I-BUS active on UART2 (9600 8E1)");
ESP_LOGI(TAG, "========================================");
}
