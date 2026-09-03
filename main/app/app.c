#include "app.h"
#include "orangebus.h"
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

#define TAG "ORANGEBUS"

/* Kontekst aplikacji - agreguje wszystkie moduly i stan komunikacji */
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
    orangebus_a2dp_state_t lastA2dpState;
    orangebus_hfp_state_t lastHfpState;
    char lastTitle[81];
    char lastArtist[81];
    volatile bool uiModeChanged;
} ibus_ctx_t;

/* TODO: Globalna instancja kontekstu - wzorzec singleton wymagany przez model
 * callbackow ESP-IDF. Do refaktoryzacji gdyby zaszla potrzeba wielu instancji. */
static ibus_ctx_t *s_ibus_ctx = NULL;

static void set_active_ui(ibus_ctx_t *ic);

/* Callback: zadanie statusu CDC od radia */
static void on_cdc_status_req(uint8_t *data, uint8_t len)
{
    ibus_ctx_t *ic = s_ibus_ctx;
    cdc_on_request(ic->cdc, data, len);
}

/* Callback: nacisniecie przycisku CDC (play/stop/next/prev z radia lub MFL) */
static void on_cdc_button_press(uint8_t *data, uint8_t len)
{
    ibus_ctx_t *ic = s_ibus_ctx;
    if (len < 1) return;
    uint8_t cmd = data[0];
    switch (cmd) {
    case ORANGEBUS_IBUS_CDC_CMD_START_PLAYING:
        if (a2dp_sink_get_state(ic->a2dp) >= ORANGEBUS_A2DP_CONNECTED) {
            avrcp_controller_send_passthrough(ic->avrcp, ESP_AVRC_PT_CMD_PLAY);
        }
        cdc_set_playing(ic->cdc, true);
        ui_mid_on_cdc_start(ic->uiMid);
        break;
    case ORANGEBUS_IBUS_CDC_CMD_STOP_PLAYING:
        if (a2dp_sink_get_state(ic->a2dp) == ORANGEBUS_A2DP_PLAYING) {
            avrcp_controller_send_passthrough(ic->avrcp, ESP_AVRC_PT_CMD_PAUSE);
        }
        cdc_set_playing(ic->cdc, false);
        ui_mid_on_cdc_stop(ic->uiMid);
        break;
    case ORANGEBUS_IBUS_CDC_CMD_CHANGE_TRACK:
        avrcp_controller_send_passthrough(ic->avrcp, ESP_AVRC_PT_CMD_FORWARD);
        break;
    case ORANGEBUS_IBUS_CDC_CMD_CHANGE_TRACK_BL:
        avrcp_controller_send_passthrough(ic->avrcp, ESP_AVRC_PT_CMD_BACKWARD);
        break;
    default:
        cdc_on_button_press(ic->cdc, data, len);
        break;
    }
}

/* Callback: przycisk MFL (kierownica multifunkcyjna) - speak, vol+/- */
static void on_mfl_button(uint8_t *data, uint8_t len)
{
    ibus_ctx_t *ic = s_ibus_ctx;
    if (len < 1) return;
    uint8_t btn = data[0];
    if (btn == ORANGEBUS_IBUS_MFL_BTN_SPEAK) {
        orangebus_hfp_state_t hfpState = hfp_client_get_state(ic->hfp);
        if (hfpState == ORANGEBUS_HFP_INCOMING) {
            hfp_client_answer(ic->hfp);
        } else if (hfpState == ORANGEBUS_HFP_ACTIVE) {
            hfp_client_reject(ic->hfp);
        } else if (hfpState == ORANGEBUS_HFP_CONNECTED) {
            hfp_client_redial(ic->hfp);
        } else if (a2dp_sink_get_state(ic->a2dp) >= ORANGEBUS_A2DP_CONNECTED && hfpState < ORANGEBUS_HFP_CONNECTED) {
            avrcp_controller_send_passthrough(ic->avrcp, ESP_AVRC_PT_CMD_PLAY);
        }
    } else if (btn == ORANGEBUS_IBUS_MFL_BTN_VOL_UP) {
        audio_output_adjust_volume(ic->audio, 5);
    } else if (btn == ORANGEBUS_IBUS_MFL_BTN_VOL_DOWN) {
        audio_output_adjust_volume(ic->audio, -5);
    }
}

/* Callback: zmiana stanu zaplonu z IKE - powiadamia wszystkie moduly UI i komfort */
static void on_ignition_status(uint8_t *data, uint8_t len)
{
    ibus_ctx_t *ic = s_ibus_ctx;
    if (len < 1) return;
    bool on = (data[0] != ORANGEBUS_IBUS_IGNITION_OFF);
    cdc_on_ignition(ic->cdc, data, len);
    ui_cd53_on_ignition(ic->uiCd53, on);
    ui_mir_on_ignition(ic->uiMir, on);
    ui_mid_on_ignition(ic->uiMid, on);
    ui_bmbt_on_ignition(ic->uiBmbt, on);
    comfort_on_ignition(ic->comfort, on);
    if (on) {
        set_active_ui(ic);
    }
}

/* Callback: zmiana glosnosci z radia */
static void on_volume_change(uint8_t *data, uint8_t len)
{
    ibus_ctx_t *ic = s_ibus_ctx;
    if (len < 1) return;
    uint8_t dir = data[0];
    if (dir == ORANGEBUS_IBUS_RAD_VOL_UP) {
        audio_output_adjust_volume(ic->audio, 2);
    } else {
        audio_output_adjust_volume(ic->audio, -2);
    }
}

/* Aktywuje interfejs UI zgodnie z konfiguracja (ui_mode), deaktywuje pozostale */
static void set_active_ui(ibus_ctx_t *ic)
{
    uint8_t uiMode = ibus_config_get(ic->config, "ui_mode");
    ui_cd53_set_active(ic->uiCd53, uiMode == ORANGEBUS_UI_MODE_CD53);
    ui_bmbt_set_active(ic->uiBmbt, uiMode == ORANGEBUS_UI_MODE_BMBT);
    ui_mid_set_active(ic->uiMid, uiMode == ORANGEBUS_UI_MODE_MID);
    ui_mir_set_active(ic->uiMir, uiMode == ORANGEBUS_UI_MODE_MIR);
}

/* Formatuje metadane utworu i wysyla do aktywnego interfejsu UI.
 * metaMode steruje formatem wyswietlania: 0=tytul-artysta, 1=artysta-tytul,
 * 2=tytul, 3=tytul|artysta */
static void send_metadata_to_ui(ibus_ctx_t *ic, const orangebus_metadata_t *meta, bool playing)
{
    if (!meta) return;
    uint8_t uiMode = ibus_config_get(ic->config, "ui_mode");
    uint8_t metaMode = ibus_config_get(ic->config, "meta_mode");

    char metaText[ORANGEBUS_IBUS_MID_MAX_CHARS + 1];
    switch (metaMode) {
    case 1:
        if (meta->artist[0] && meta->title[0]) {
            snprintf(metaText, sizeof(metaText), "%.8s - %.8s", meta->artist, meta->title);
        } else if (meta->title[0]) {
            snprintf(metaText, sizeof(metaText), "%.24s", meta->title);
        } else {
            strncpy(metaText, playing ? "Streaming" : "Bluetooth", sizeof(metaText) - 1);
            metaText[sizeof(metaText) - 1] = '\0';
        }
        break;
    case 2:
        if (meta->title[0]) {
            snprintf(metaText, sizeof(metaText), "%.24s", meta->title);
        } else {
            strncpy(metaText, playing ? "Streaming" : "Bluetooth", sizeof(metaText) - 1);
            metaText[sizeof(metaText) - 1] = '\0';
        }
        break;
    case 3:
        if (meta->title[0] && meta->artist[0]) {
            snprintf(metaText, sizeof(metaText), "%.12s|%.11s", meta->title, meta->artist);
        } else if (meta->title[0]) {
            snprintf(metaText, sizeof(metaText), "%.24s", meta->title);
        } else {
            strncpy(metaText, playing ? "Streaming" : "Bluetooth", sizeof(metaText) - 1);
            metaText[sizeof(metaText) - 1] = '\0';
        }
        break;
    default:
        if (meta->title[0] && meta->artist[0]) {
            snprintf(metaText, sizeof(metaText), "%.10s - %.10s", meta->title, meta->artist);
        } else if (meta->title[0]) {
            snprintf(metaText, sizeof(metaText), "%.24s", meta->title);
        } else {
            strncpy(metaText, playing ? "Streaming" : "Bluetooth", sizeof(metaText) - 1);
            metaText[sizeof(metaText) - 1] = '\0';
        }
        break;
    }

    switch (uiMode) {
    case ORANGEBUS_UI_MODE_CD53:
        ui_cd53_show_title(ic->uiCd53, metaText);
        break;
    case ORANGEBUS_UI_MODE_MIR:
        ui_mir_show_title(ic->uiMir, metaText);
        break;
    case ORANGEBUS_UI_MODE_MID:
        ui_mid_show_title(ic->uiMid, metaText);
        break;
    case ORANGEBUS_UI_MODE_BMBT:
        ui_bmbt_on_metadata(ic->uiBmbt, meta->title, meta->artist, meta->album);
        break;
    }
}

/* Glowne zadanie I-BUS: odbiera pakiety, co 100ms wywoluje tick() wszystkich
 * modulow i synchronizuje stany A2DP/HFP z emulatorami I-BUS */
static void ibus_task(void *arg)
{
    ibus_ctx_t *ic = (ibus_ctx_t *)arg;
    uint32_t lastTick = xTaskGetTickCount() * portTICK_PERIOD_MS;

    while (1) {
        if (ibus_is_debug_mode(ic->ibus)) {
            vTaskDelay(pdMS_TO_TICKS(100));
        } else {
            ibus_process(ic->ibus);
        }

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

            if (ic->uiModeChanged) {
                ic->uiModeChanged = false;
                set_active_ui(ic);
            }

            orangebus_a2dp_state_t a2dpState = a2dp_sink_get_state(ic->a2dp);
            if (a2dpState != ic->lastA2dpState) {
                orangebus_a2dp_state_t prev = ic->lastA2dpState;
                ic->lastA2dpState = a2dpState;
                if (a2dpState == ORANGEBUS_A2DP_PLAYING) {
                    const orangebus_metadata_t *meta = avrcp_controller_get_metadata(ic->avrcp);
                    send_metadata_to_ui(ic, meta, true);
                    ui_bmbt_on_playback(ic->uiBmbt, true);
                } else if (a2dpState == ORANGEBUS_A2DP_PAUSED || a2dpState == ORANGEBUS_A2DP_CONNECTED) {
                    ui_bmbt_on_playback(ic->uiBmbt, false);
                }
                if (a2dpState >= ORANGEBUS_A2DP_CONNECTED
                    && prev < ORANGEBUS_A2DP_CONNECTED
                    && ibus_config_get(ic->config, "autoplay") != 0
                    && !cdc_is_playing(ic->cdc)) {
                    cdc_set_playing(ic->cdc, true);
                    avrcp_controller_send_passthrough(ic->avrcp, ESP_AVRC_PT_CMD_PLAY);
                }
            }

            const orangebus_metadata_t *meta = avrcp_controller_get_metadata(ic->avrcp);
            if (meta && (strcmp(meta->title, ic->lastTitle) != 0 || strcmp(meta->artist, ic->lastArtist) != 0)) {
                strncpy(ic->lastTitle, meta->title, sizeof(ic->lastTitle) - 1);
                ic->lastTitle[sizeof(ic->lastTitle) - 1] = '\0';
                strncpy(ic->lastArtist, meta->artist, sizeof(ic->lastArtist) - 1);
                ic->lastArtist[sizeof(ic->lastArtist) - 1] = '\0';
                send_metadata_to_ui(ic, meta, a2dpState == ORANGEBUS_A2DP_PLAYING);
            }

            orangebus_hfp_state_t hfpState = hfp_client_get_state(ic->hfp);
            if (hfpState != ic->lastHfpState) {
                ic->lastHfpState = hfpState;
                if (hfpState == ORANGEBUS_HFP_CONNECTED) {
                    tel_set_connected(ic->tel, true);
                } else if (hfpState == ORANGEBUS_HFP_IDLE) {
                    tel_set_connected(ic->tel, false);
                    tel_set_call_active(ic->tel, false);
                } else if (hfpState == ORANGEBUS_HFP_INCOMING) {
                    tel_set_call_incoming(ic->tel, true);
                    const char *callerId = hfp_client_get_caller_id(ic->hfp);
                    if (callerId && callerId[0]) tel_set_caller_id(ic->tel, callerId);
                } else if (hfpState == ORANGEBUS_HFP_ACTIVE || hfpState == ORANGEBUS_HFP_AUDIO_OPEN) {
                    tel_set_call_active(ic->tel, true);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* TODO: Callbacki comfort_on_door_lock/gm_status/lm_status sa zdefiniowane
 * w module comfort ale nigdzie nie zarejestrowane w ibus_register_callback.
 * Funkcje komfortu (auto-lock, skladanie lusterek) sa obecnie martwym kodem. */

void app_run(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " ESP32 OrangeBus BT Test (ESP-IDF v6.0)");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Pair phone with 'BMW-OrangeBus'");
    ESP_LOGI(TAG, "Commands: +/- vol, m mute, p play, s pause");
    ESP_LOGI(TAG, " n next, b prev, a answer, r reject, d redial");
    ESP_LOGI(TAG, " v voice (AVRCP), V voice (HFP), h status");
    ESP_LOGI(TAG, " e toggle EQ, E show EQ bands");
    ESP_LOGI(TAG, " i ibus debug, I ibus status, u cycle UI mode");
    ESP_LOGI(TAG, " U autoplay, c blink, C locks+mirrors");

    bt_manager_init();

    audio_output_t *audio = audio_output_create();
    audio_output_init(audio, 44100);

    eq_processor_t *eq = eq_processor_create();
    eq_processor_init(eq, 44100);
    audio_output_set_eq(audio, eq);

    avrcp_controller_t *avrcp = avrcp_controller_create();
    avrcp_controller_init(avrcp);

    a2dp_sink_t *a2dp = a2dp_sink_create(audio);
    a2dp_sink_init(a2dp);

    avrcp_controller_set_a2dp_state_ref(avrcp, a2dp_sink_get_state_ptr(a2dp));

    hfp_client_t *hfp = hfp_client_create(audio, avrcp, a2dp);
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
    if (!ibus) {
        ESP_LOGE(TAG, "I-BUS create failed (low heap), aborting init");
        return;
    }
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

    ibus_register_callback(ibus, ORANGEBUS_IBUS_EVT_CDC_STATUS_REQ, on_cdc_status_req);
    ibus_register_callback(ibus, ORANGEBUS_IBUS_EVT_CDC_BUTTON_PRESS, on_cdc_button_press);
    ibus_register_callback(ibus, ORANGEBUS_IBUS_EVT_MFL_BUTTON_PRESS, on_mfl_button);
    ibus_register_callback(ibus, ORANGEBUS_IBUS_EVT_IGNITION_STATUS, on_ignition_status);
    ibus_register_callback(ibus, ORANGEBUS_IBUS_EVT_VOLUME_CHANGE, on_volume_change);

    set_active_ui(ic);

    spp_server_t *spp = spp_server_create(eq, ibus, cdc, tel, ibusConfig, comfort, avrcp, &ic->uiModeChanged);
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
