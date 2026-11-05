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
    uint32_t lastMetadataVersion;   /* avrcp metadata version last shown */
    uint32_t lastConfigRevision;    /* config revision last applied */
    bool mflVoiceHeld;   /* R/T button was held: suppress the release action */
} ibus_ctx_t;

/* TODO: Globalna instancja kontekstu - wzorzec singleton wymagany przez model
 * callbackow ESP-IDF. Do refaktoryzacji gdyby zaszla potrzeba wielu instancji. */
static ibus_ctx_t *s_ibus_ctx = NULL;

static void set_active_ui(ibus_ctx_t *ic);

/* Callback: first valid frame from another module since boot - the bus is
 * alive. Announce the CD changer and ask the IKE for the ignition state (we may
 * have missed its broadcast while booting). Never sent into a silent bus, so it
 * cannot wake a sleeping car. Same sequence as BlueBus. */
static void on_first_frame(uint8_t *data, uint8_t len)
{
    (void)data;
    (void)len;
    ibus_ctx_t *ic = s_ibus_ctx;
    cdc_announce(ic->cdc);
    ibus_send_ike_ignition_request(ic->ibus);
}

/* Callback: the radio pinged the CD changer (the driver already answered). */
static void on_cdc_ping(uint8_t *data, uint8_t len)
{
    (void)data;
    (void)len;
    cdc_on_ping(s_ibus_ctx->cdc);
}

/* Callback: radio -> CDC command (status poll, play, stop, next/prev...).
 * The CDC reply goes out first - the radio drops a changer that answers late -
 * then the matching Bluetooth action is queued. */
static void on_cdc_request(uint8_t *data, uint8_t len)
{
    ibus_ctx_t *ic = s_ibus_ctx;
    if (len < 1) return;
    cdc_on_request(ic->cdc, data, len);

    uint8_t command = data[0];
    orangebus_a2dp_state_t a2dpState = a2dp_sink_get_state(ic->a2dp);
    switch (command) {
    case ORANGEBUS_IBUS_CDC_CMD_START_PLAYING:
        if (a2dpState >= ORANGEBUS_A2DP_CONNECTED && a2dpState != ORANGEBUS_A2DP_PLAYING) {
            avrcp_controller_send_passthrough(ic->avrcp, ESP_AVRC_PT_CMD_PLAY);
        }
        ui_mid_on_cdc_start(ic->uiMid);
        break;
    case ORANGEBUS_IBUS_CDC_CMD_STOP_PLAYING:
    case ORANGEBUS_IBUS_CDC_CMD_PAUSE_PLAYING:
        if (a2dpState == ORANGEBUS_A2DP_PLAYING) {
            avrcp_controller_send_passthrough(ic->avrcp, ESP_AVRC_PT_CMD_PAUSE);
        }
        if (command == ORANGEBUS_IBUS_CDC_CMD_STOP_PLAYING) {
            ui_mid_on_cdc_stop(ic->uiMid);
        }
        break;
    case ORANGEBUS_IBUS_CDC_CMD_CHANGE_TRACK:
    case ORANGEBUS_IBUS_CDC_CMD_CHANGE_TRACK_BL: {
        /* Direction is in the second byte for both variants: 0x00 = next. */
        bool next = (len < 2) || (data[1] == ORANGEBUS_IBUS_CDC_TRACK_NEXT);
        avrcp_controller_send_passthrough(ic->avrcp,
            next ? ESP_AVRC_PT_CMD_FORWARD : ESP_AVRC_PT_CMD_BACKWARD);
        break;
    }
    default:
        break;
    }
}

/* Callback: steering wheel R/T (voice) button. Mirrors BlueBus: a short press
 * answers / ends calls (or toggles playback when we are the active source),
 * a long hold toggles the phone's voice assistant. Next/Prev are ignored here:
 * the radio turns them into CDC track-change commands. */
static void on_mfl_button(uint8_t *data, uint8_t len)
{
    ibus_ctx_t *ic = s_ibus_ctx;
    if (len < 1) return;
    uint8_t button = data[0];

    if (button == ORANGEBUS_IBUS_MFL_BTN_VOICE_PRESS) {
        ic->mflVoiceHeld = false;
    } else if (button == ORANGEBUS_IBUS_MFL_BTN_VOICE_HOLD) {
        ic->mflVoiceHeld = true;
        hfp_client_toggle_voice_recognition(ic->hfp);
    } else if (button == ORANGEBUS_IBUS_MFL_BTN_VOICE_REL && !ic->mflVoiceHeld) {
        orangebus_hfp_state_t hfpState = hfp_client_get_state(ic->hfp);
        if (hfpState == ORANGEBUS_HFP_INCOMING) {
            hfp_client_answer(ic->hfp);
        } else if (hfpState == ORANGEBUS_HFP_ACTIVE || hfpState == ORANGEBUS_HFP_OUTGOING
                   || hfpState == ORANGEBUS_HFP_AUDIO_OPEN) {
            hfp_client_reject(ic->hfp);
        } else if (cdc_is_playing(ic->cdc)) {
            orangebus_a2dp_state_t a2dpState = a2dp_sink_get_state(ic->a2dp);
            if (a2dpState == ORANGEBUS_A2DP_PLAYING) {
                avrcp_controller_send_passthrough(ic->avrcp, ESP_AVRC_PT_CMD_PAUSE);
            } else if (a2dpState >= ORANGEBUS_A2DP_CONNECTED) {
                avrcp_controller_send_passthrough(ic->avrcp, ESP_AVRC_PT_CMD_PLAY);
            }
        }
    }
}

/* Converts a volume data byte (bit0 = up, high nibble = steps) to a signed
 * percentage change. */
static int8_t volume_delta(uint8_t volumeByte, int8_t percentPerStep)
{
    uint8_t steps = ORANGEBUS_IBUS_VOL_STEPS(volumeByte);
    if (steps == 0) steps = 1;
    int16_t delta = (int16_t)steps * percentPerStep;
    if (delta > 100) delta = 100;
    return (volumeByte & ORANGEBUS_IBUS_VOL_DIR_UP_MASK) ? (int8_t)delta : (int8_t)-delta;
}

/* Callback: steering wheel volume keys. Outside a call the radio already
 * applies them to its own output, so we only follow them while call audio
 * (which we generate) is open - otherwise the volume would be applied twice. */
static void on_mfl_volume(uint8_t *data, uint8_t len)
{
    ibus_ctx_t *ic = s_ibus_ctx;
    if (len < 1) return;
    orangebus_hfp_state_t hfpState = hfp_client_get_state(ic->hfp);
    if (hfpState == ORANGEBUS_HFP_AUDIO_OPEN || hfpState == ORANGEBUS_HFP_ACTIVE) {
        audio_output_adjust_volume(ic->audio, volume_delta(data[0], 5));
    }
}

/* Callback: ignition status from the IKE - notifies every module. */
static void on_ignition_status(uint8_t *data, uint8_t len)
{
    ibus_ctx_t *ic = s_ibus_ctx;
    if (len < 1) return;
    bool on = (data[0] != ORANGEBUS_IBUS_IGNITION_OFF);
    cdc_on_ignition(ic->cdc, on);
    tel_on_ignition(ic->tel, on);
    ui_cd53_on_ignition(ic->uiCd53, on);
    ui_mir_on_ignition(ic->uiMir, on);
    ui_mid_on_ignition(ic->uiMid, on);
    ui_bmbt_on_ignition(ic->uiBmbt, on);
    comfort_on_ignition(ic->comfort, on);
    if (on) {
        set_active_ui(ic);
    }
}

/* Callback: radio volume step (bit0 = up, high nibble = number of steps). */
static void on_rad_volume(uint8_t *data, uint8_t len)
{
    ibus_ctx_t *ic = s_ibus_ctx;
    if (len < 1) return;
    audio_output_adjust_volume(ic->audio, volume_delta(data[0], 2));
}

/* Callback: status GM (wariant ZKE) - wykrywanie sprzetu dla komfortu */
static void on_gm_status(uint8_t *data, uint8_t len)
{
    ibus_ctx_t *ic = s_ibus_ctx;
    comfort_on_gm_status(ic->comfort, data, len);
}

/* Callback: status LCM (wariant swiatel) - wykrywanie sprzetu dla komfortu */
static void on_lm_status(uint8_t *data, uint8_t len)
{
    ibus_ctx_t *ic = s_ibus_ctx;
    comfort_on_lm_status(ic->comfort, data, len);
}

/* Callback: key-fob frame from the GM (00 04 BF 72 xx). The action is the high
 * nibble of xx: 1 = lock, 2 = unlock (low nibble = key number). */
static void on_door_lock(uint8_t *data, uint8_t len)
{
    ibus_ctx_t *ic = s_ibus_ctx;
    if (len < 1) return;
    uint8_t keyAction = ORANGEBUS_IBUS_GM_REMOTE_ACTION(data[0]);
    if (keyAction == ORANGEBUS_IBUS_GM_REMOTE_LOCK) {
        comfort_on_door_lock(ic->comfort, true);
    } else if (keyAction == ORANGEBUS_IBUS_GM_REMOTE_UNLOCK) {
        comfort_on_door_lock(ic->comfort, false);
    }
}

/* Aktywuje interfejs UI zgodnie z konfiguracja (ui_mode), deaktywuje pozostale */
static void set_active_ui(ibus_ctx_t *ic)
{
    uint8_t uiMode = ibus_config_get(ic->config, IBUS_CFG_UI_MODE);
    ui_cd53_set_active(ic->uiCd53, uiMode == ORANGEBUS_UI_MODE_CD53);
    ui_bmbt_set_active(ic->uiBmbt, uiMode == ORANGEBUS_UI_MODE_BMBT);
    ui_mid_set_active(ic->uiMid, uiMode == ORANGEBUS_UI_MODE_MID);
    ui_mir_set_active(ic->uiMir, uiMode == ORANGEBUS_UI_MODE_MIR);
}

/* Builds the one-line track text for small displays. metaMode selects the
 * layout: 0 = "title - artist", 1 = "artist - title", 2 = title only,
 * 3 = "title|artist". Falls back to "Streaming"/"Bluetooth" with no title. */
static void format_track_text(char *out, size_t outSize, const orangebus_metadata_t *meta,
                              uint8_t metaMode, bool playing)
{
    bool hasTitle = meta->title[0] != '\0';
    bool hasArtist = meta->artist[0] != '\0';
    if (!hasTitle) {
        snprintf(out, outSize, "%s", playing ? "Streaming" : "Bluetooth");
    } else if (!hasArtist || metaMode == 2) {
        snprintf(out, outSize, "%.24s", meta->title);
    } else if (metaMode == 1) {
        snprintf(out, outSize, "%.8s - %.8s", meta->artist, meta->title);
    } else if (metaMode == 3) {
        snprintf(out, outSize, "%.12s|%.11s", meta->title, meta->artist);
    } else {
        snprintf(out, outSize, "%.10s - %.10s", meta->title, meta->artist);
    }
}

/* Sends track metadata to whichever display UI is configured. */
static void send_metadata_to_ui(ibus_ctx_t *ic, const orangebus_metadata_t *meta, bool playing)
{
    if (!meta) return;
    uint8_t uiMode = ibus_config_get(ic->config, IBUS_CFG_UI_MODE);
    if (uiMode == ORANGEBUS_UI_MODE_BMBT) {
        /* The navigation screen has room for separate lines. */
        ui_bmbt_on_metadata(ic->uiBmbt, meta->title, meta->artist, meta->album);
        return;
    }

    char trackText[ORANGEBUS_IBUS_MID_MAX_CHARS + 1];
    format_track_text(trackText, sizeof(trackText), meta,
        ibus_config_get(ic->config, IBUS_CFG_META_MODE), playing);
    switch (uiMode) {
    case ORANGEBUS_UI_MODE_CD53: ui_cd53_show_title(ic->uiCd53, trackText); break;
    case ORANGEBUS_UI_MODE_MIR:  ui_mir_show_title(ic->uiMir, trackText); break;
    case ORANGEBUS_UI_MODE_MID:  ui_mid_show_title(ic->uiMid, trackText); break;
    default: break;
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

            /* A setting changed (SPP or CLI): re-apply the UI mode. */
            uint32_t configRevision = ibus_config_get_revision(ic->config);
            if (configRevision != ic->lastConfigRevision) {
                ic->lastConfigRevision = configRevision;
                set_active_ui(ic);
            }

            /* --- A2DP state -> displays, autoplay --- */
            orangebus_a2dp_state_t a2dpState = a2dp_sink_get_state(ic->a2dp);
            if (a2dpState != ic->lastA2dpState) {
                orangebus_a2dp_state_t previousState = ic->lastA2dpState;
                ic->lastA2dpState = a2dpState;
                if (a2dpState == ORANGEBUS_A2DP_PLAYING) {
                    ic->lastMetadataVersion--;   /* force a metadata refresh below */
                    ui_bmbt_on_playback(ic->uiBmbt, true);
                } else if (a2dpState == ORANGEBUS_A2DP_PAUSED || a2dpState == ORANGEBUS_A2DP_CONNECTED) {
                    ui_bmbt_on_playback(ic->uiBmbt, false);
                }
                bool justConnected = a2dpState >= ORANGEBUS_A2DP_CONNECTED
                    && previousState < ORANGEBUS_A2DP_CONNECTED;
                if (justConnected && ibus_config_get(ic->config, IBUS_CFG_AUTOPLAY)
                    && !cdc_is_playing(ic->cdc)) {
                    cdc_set_playing(ic->cdc, true);
                    avrcp_controller_send_passthrough(ic->avrcp, ESP_AVRC_PT_CMD_PLAY);
                }
            }

            /* --- New track metadata -> display (only copied when it changed) --- */
            uint32_t metadataVersion = avrcp_controller_get_metadata_version(ic->avrcp);
            if (metadataVersion != ic->lastMetadataVersion) {
                ic->lastMetadataVersion = metadataVersion;
                orangebus_metadata_t meta;
                avrcp_controller_copy_metadata(ic->avrcp, &meta);
                send_metadata_to_ui(ic, &meta, a2dpState == ORANGEBUS_A2DP_PLAYING);
            }

            /* --- HFP state -> telephone emulator --- */
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

        /* Event-driven idle: wake early on UART RX, else re-check 100 ms
         * ticks at 10 ms granularity (CODE_REVIEW 3.3). */
        if (!ibus_is_debug_mode(ic->ibus)) {
            ibus_wait_for_data(ic->ibus, 10);
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

/* Comfort callbacks (door lock, GM/LCM variant detection) are wired to the
 * I-BUS dispatcher below; variant pings are polled in comfort_tick. */

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

    avrcp_controller_set_a2dp_sink(avrcp, a2dp);

    hfp_client_t *hfp = hfp_client_create(audio, avrcp, a2dp);
    hfp_client_init(hfp);

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
    ic->lastConfigRevision = ibus_config_get_revision(ibusConfig);

    ibus_register_callback(ibus, ORANGEBUS_IBUS_EVT_FIRST_FRAME, on_first_frame);
    ibus_register_callback(ibus, ORANGEBUS_IBUS_EVT_CDC_REQUEST, on_cdc_request);
    ibus_register_callback(ibus, ORANGEBUS_IBUS_EVT_CDC_PING, on_cdc_ping);
    ibus_register_callback(ibus, ORANGEBUS_IBUS_EVT_MFL_BUTTON, on_mfl_button);
    ibus_register_callback(ibus, ORANGEBUS_IBUS_EVT_MFL_VOLUME, on_mfl_volume);
    ibus_register_callback(ibus, ORANGEBUS_IBUS_EVT_IGNITION_STATUS, on_ignition_status);
    ibus_register_callback(ibus, ORANGEBUS_IBUS_EVT_RAD_VOLUME, on_rad_volume);
    ibus_register_callback(ibus, ORANGEBUS_IBUS_EVT_GM_STATUS, on_gm_status);
    ibus_register_callback(ibus, ORANGEBUS_IBUS_EVT_LM_STATUS, on_lm_status);
    ibus_register_callback(ibus, ORANGEBUS_IBUS_EVT_DOOR_LOCK, on_door_lock);

    set_active_ui(ic);

    spp_server_t *spp = spp_server_create(eq, ibus, cdc, tel, ibusConfig, comfort, avrcp);
    ret = spp_server_init(spp);
    ESP_LOGI(TAG, "SPP server init: %s", esp_err_to_name(ret));

    cli_t *cli_inst = cli_create(audio, avrcp, a2dp, hfp, eq, ibus, cdc, tel, ibusConfig);
    cli_start(cli_inst);

    /* Time-critical bus task runs above CLI/SPP so radio polls are not
     * delayed by command parsing (CODE_REVIEW 3.5). */
    xTaskCreate(ibus_task, "ibus_task", 4096, ic, 6, NULL);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " INIT COMPLETE - BT discoverable!");
    ESP_LOGI(TAG, " I-BUS active on UART2 (9600 8E1)");
    ESP_LOGI(TAG, "========================================");
}
