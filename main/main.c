#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_hf_client_api.h"
#include "driver/i2s_std.h"
#include "driver/i2s_common.h"
#include "driver/gpio.h"

#define TAG "BT_TEST"

#define I2S_BCK 26
#define I2S_WS 25
#define I2S_DATA 22
#define BT_LED 2
#define TEL_MUTE 4

typedef enum {
    BT_A2DP_IDLE = 0,
    BT_A2DP_CONNECTING,
    BT_A2DP_CONNECTED,
    BT_A2DP_PLAYING,
    BT_A2DP_PAUSED
} bt_a2dp_state_t;

typedef enum {
    BT_HFP_IDLE = 0,
    BT_HFP_CONNECTED,
    BT_HFP_AUDIO_OPEN,
    BT_HFP_INCOMING,
    BT_HFP_OUTGOING,
    BT_HFP_ACTIVE
} bt_hfp_state_t;

static bt_a2dp_state_t s_a2dp_state = BT_A2DP_IDLE;
static bt_hfp_state_t s_hfp_state = BT_HFP_IDLE;
static uint8_t s_volume = 70;
static bool s_muted = false;
static bool s_is_a2dp_mode = true;
static bool s_i2s_initialized = false;
static uint32_t s_i2s_rate = 0;
static char s_meta_title[81] = "";
static char s_meta_artist[81] = "";
static char s_meta_album[81] = "";
static uint8_t s_meta_field_count = 0;
static uint32_t s_txn_count = 0;
static char s_caller_id[33] = "";
static bool s_sco_open = false;
static bool s_a2dp_was_playing = false;
static TimerHandle_t s_meta_timer = NULL;
static bool s_meta_requesting = false;
static bool s_sco_is_msbc = false;
static esp_bd_addr_t s_peer_bda = {0};

static i2s_chan_handle_t s_tx_handle = NULL;
static SemaphoreHandle_t s_i2s_mutex = NULL;

static SemaphoreHandle_t s_avrcp_cmd_sem = NULL;
static TickType_t s_last_cmd_tick = 0;
#define AVRCP_CMD_MIN_INTERVAL_MS 100
#define AVRCP_PRESSED_RELEASED_GAP_MS 50

static bool i2s_init(uint32_t rate)
{
    if (s_i2s_mutex) xSemaphoreTake(s_i2s_mutex, portMAX_DELAY);

    if (s_tx_handle != NULL) {
        i2s_channel_disable(s_tx_handle);
        i2s_del_channel(s_tx_handle);
        s_tx_handle = NULL;
    }
    s_i2s_initialized = false;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear_after_cb = true;
    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S new channel failed: %s", esp_err_to_name(ret));
        if (s_i2s_mutex) xSemaphoreGive(s_i2s_mutex);
        return false;
    }

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    i2s_std_gpio_config_t gpio_cfg = {
        .mclk = I2S_GPIO_UNUSED,
        .bclk = I2S_BCK,
        .ws = I2S_WS,
        .dout = I2S_DATA,
        .din = I2S_GPIO_UNUSED,
        .invert_flags = {0},
    };

    i2s_std_config_t std_cfg = {
        .clk_cfg = clk_cfg,
        .slot_cfg = slot_cfg,
        .gpio_cfg = gpio_cfg,
    };

    ret = i2s_channel_init_std_mode(s_tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S init std mode failed: %s", esp_err_to_name(ret));
        i2s_del_channel(s_tx_handle);
        s_tx_handle = NULL;
        if (s_i2s_mutex) xSemaphoreGive(s_i2s_mutex);
        return false;
    }

    ret = i2s_channel_enable(s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S enable failed: %s", esp_err_to_name(ret));
        i2s_del_channel(s_tx_handle);
        s_tx_handle = NULL;
        if (s_i2s_mutex) xSemaphoreGive(s_i2s_mutex);
        return false;
    }

    s_is_a2dp_mode = (rate > 16000);
    s_i2s_rate = rate;
    s_i2s_initialized = true;
    ESP_LOGI(TAG, "I2S configured: %luHz stereo", rate);

    if (s_i2s_mutex) xSemaphoreGive(s_i2s_mutex);
    return true;
}

static void switch_to_a2dp(void)
{
    if (!s_is_a2dp_mode || !s_i2s_initialized || s_i2s_rate != 44100) {
        i2s_init(44100);
    }
}

static void switch_to_sco(void)
{
    uint32_t rate = s_sco_is_msbc ? 16000 : 8000;
    if (s_is_a2dp_mode || !s_i2s_initialized || s_i2s_rate != rate) {
        i2s_init(rate);
    }
}

static void a2dp_data_cb(const uint8_t *data, uint32_t len)
{
    if (s_muted || !s_i2s_initialized || s_tx_handle == NULL) return;
    if (s_i2s_mutex && !xSemaphoreTake(s_i2s_mutex, pdMS_TO_TICKS(10))) return;

    if (s_volume >= 100) {
        i2s_channel_write(s_tx_handle, data, len, NULL, portMAX_DELAY);
    } else {
        int16_t *samples = (int16_t *)data;
        uint32_t sample_count = len / 2;
        float scale = s_volume / 100.0f;
        for (uint32_t i = 0; i < sample_count; i++) {
            samples[i] = (int16_t)(samples[i] * scale);
        }
        i2s_channel_write(s_tx_handle, data, len, NULL, portMAX_DELAY);
    }

    if (s_i2s_mutex) xSemaphoreGive(s_i2s_mutex);
}

static int16_t s_sco_stereo_buf[480];

static void hfp_audio_recv_cb(const uint8_t *data, uint32_t len)
{
    if (s_muted || !s_i2s_initialized || s_tx_handle == NULL) return;
    if (s_i2s_mutex && !xSemaphoreTake(s_i2s_mutex, pdMS_TO_TICKS(10))) return;

    uint32_t mono_samples = len / 2;
    if (mono_samples > 240) mono_samples = 240;
    uint32_t stereo_len = mono_samples * 4;

    const int16_t *mono = (const int16_t *)data;
    float scale = s_volume / 100.0f;
    for (uint32_t i = 0; i < mono_samples; i++) {
        int16_t scaled = (int16_t)(mono[i] * scale);
        s_sco_stereo_buf[i * 2] = scaled;
        s_sco_stereo_buf[i * 2 + 1] = scaled;
    }
    i2s_channel_write(s_tx_handle, s_sco_stereo_buf, stereo_len, NULL, portMAX_DELAY);

    if (s_i2s_mutex) xSemaphoreGive(s_i2s_mutex);
}

static uint32_t hfp_audio_send_cb(uint8_t *data, uint32_t len)
{
    if (data == NULL || len == 0) return 0;
    memset(data, 0, len);
    return len;
}

static void notify_metadata(void)
{
    ESP_LOGI(TAG, "Metadata: \"%s\" by %s (%s)",
             s_meta_title[0] ? s_meta_title : "-",
             s_meta_artist[0] ? s_meta_artist : "-",
             s_meta_album[0] ? s_meta_album : "-");
}

static void request_metadata(void)
{
    s_meta_field_count = 0;
    s_meta_title[0] = '\0';
    s_meta_artist[0] = '\0';
    s_meta_album[0] = '\0';
    s_meta_requesting = true;
    esp_avrc_ct_send_metadata_cmd(s_txn_count++, ESP_AVRC_MD_ATTR_TITLE);
    esp_avrc_ct_send_metadata_cmd(s_txn_count++, ESP_AVRC_MD_ATTR_ARTIST);
    esp_avrc_ct_send_metadata_cmd(s_txn_count++, ESP_AVRC_MD_ATTR_ALBUM);
}

static void meta_timer_cb(TimerHandle_t timer)
{
    if (s_meta_requesting) {
        notify_metadata();
        s_meta_requesting = false;
    }
}

static esp_err_t send_avrcp_pt_cmd(uint8_t cmd)
{
    if (s_avrcp_cmd_sem) xSemaphoreTake(s_avrcp_cmd_sem, portMAX_DELAY);

    TickType_t now = xTaskGetTickCount();
    TickType_t min_interval = pdMS_TO_TICKS(AVRCP_CMD_MIN_INTERVAL_MS);
    if ((now - s_last_cmd_tick) < min_interval) {
        vTaskDelay(min_interval - (now - s_last_cmd_tick));
    }

    esp_err_t ret = esp_avrc_ct_send_passthrough_cmd(s_txn_count++, cmd, ESP_AVRC_PT_CMD_STATE_PRESSED);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AVRCP PRESSED cmd 0x%02X failed: %s", cmd, esp_err_to_name(ret));
        s_last_cmd_tick = xTaskGetTickCount();
        if (s_avrcp_cmd_sem) xSemaphoreGive(s_avrcp_cmd_sem);
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(AVRCP_PRESSED_RELEASED_GAP_MS));

    ret = esp_avrc_ct_send_passthrough_cmd(s_txn_count++, cmd, ESP_AVRC_PT_CMD_STATE_RELEASED);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AVRCP RELEASED cmd 0x%02X failed: %s", cmd, esp_err_to_name(ret));
    }

    s_last_cmd_tick = xTaskGetTickCount();
    if (s_avrcp_cmd_sem) xSemaphoreGive(s_avrcp_cmd_sem);
    return ret;
}

static void on_avrcp_meta(uint8_t attr_id, const uint8_t *val, uint8_t len)
{
    char buf[81] = "";
    if (len > 80) len = 80;
    memcpy(buf, val, len);
    buf[len] = '\0';

    switch (attr_id) {
    case ESP_AVRC_MD_ATTR_TITLE:
        strncpy(s_meta_title, buf, 80);
        s_meta_title[80] = '\0';
        s_meta_field_count |= 0x01;
        break;
    case ESP_AVRC_MD_ATTR_ARTIST:
        strncpy(s_meta_artist, buf, 80);
        s_meta_artist[80] = '\0';
        s_meta_field_count |= 0x02;
        break;
    case ESP_AVRC_MD_ATTR_ALBUM:
        strncpy(s_meta_album, buf, 80);
        s_meta_album[80] = '\0';
        s_meta_field_count |= 0x04;
        break;
    default:
        return;
    }

    if (s_meta_field_count == 0x07) {
        notify_metadata();
        s_meta_requesting = false;
        if (s_meta_timer) xTimerStop(s_meta_timer, 0);
    } else if (s_meta_timer) {
        xTimerReset(s_meta_timer, 0);
    }
}

static void a2dp_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            s_a2dp_state = BT_A2DP_CONNECTED;
            gpio_set_level(BT_LED, 1);
            ESP_LOGI(TAG, "A2DP Connected");
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            s_a2dp_state = BT_A2DP_IDLE;
            if (s_hfp_state < BT_HFP_CONNECTED) {
                gpio_set_level(BT_LED, 0);
            }
            ESP_LOGI(TAG, "A2DP Disconnected");
        }
        break;
    case ESP_A2D_AUDIO_STATE_EVT:
        if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
            s_a2dp_state = BT_A2DP_PLAYING;
            switch_to_a2dp();
            s_muted = false;
            ESP_LOGI(TAG, "A2DP Playing");
        } else if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_SUSPEND) {
            s_a2dp_state = BT_A2DP_PAUSED;
            ESP_LOGI(TAG, "A2DP Suspended");
        }
        break;
    default:
        break;
    }
}

static void avrcp_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        if (param->conn_stat.connected) {
            ESP_LOGI(TAG, "AVRCP Connected");
        } else {
            ESP_LOGI(TAG, "AVRCP Disconnected");
            s_meta_title[0] = s_meta_artist[0] = s_meta_album[0] = '\0';
            s_meta_field_count = 0;
        }
        break;
    case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
        if (param->rmt_feats.feat_mask & ESP_AVRC_FEAT_META_DATA) {
            esp_avrc_ct_send_get_rn_capabilities_cmd(s_txn_count++);
            request_metadata();
        }
        ESP_LOGI(TAG, "AVRCP Remote features received");
        break;
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT: {
        esp_avrc_rn_evt_cap_mask_t evt_mask = param->get_rn_caps_rsp.evt_set;
        if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &evt_mask, ESP_AVRC_RN_TRACK_CHANGE)) {
            esp_avrc_ct_send_register_notification_cmd(s_txn_count++, ESP_AVRC_RN_TRACK_CHANGE, 0);
        }
        if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &evt_mask, ESP_AVRC_RN_PLAY_STATUS_CHANGE)) {
            esp_avrc_ct_send_register_notification_cmd(s_txn_count++, ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
        }
        break;
    }
    case ESP_AVRC_CT_METADATA_RSP_EVT: {
        const char *txt = (const char *)param->meta_rsp.attr_text;
        if (txt) {
            on_avrcp_meta(param->meta_rsp.attr_id, (const uint8_t *)txt, param->meta_rsp.attr_length);
        }
        break;
    }
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
        if (param->change_ntf.event_id == ESP_AVRC_RN_TRACK_CHANGE) {
            request_metadata();
            esp_avrc_ct_send_register_notification_cmd(s_txn_count++, ESP_AVRC_RN_TRACK_CHANGE, 0);
    } else if (param->change_ntf.event_id == ESP_AVRC_RN_PLAY_STATUS_CHANGE) {
        esp_avrc_playback_stat_t play_status = param->change_ntf.event_parameter.playback;
        if (play_status == ESP_AVRC_PLAYBACK_PLAYING) {
            s_a2dp_state = BT_A2DP_PLAYING;
        } else if (play_status == ESP_AVRC_PLAYBACK_PAUSED) {
            s_a2dp_state = BT_A2DP_PAUSED;
        }
        ESP_LOGI(TAG, "Play status: %d", play_status);
        esp_avrc_ct_send_register_notification_cmd(s_txn_count++, ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
    }
        break;
    default:
        break;
    }
}

static void hfp_client_cb(esp_hf_client_cb_event_t event, esp_hf_client_cb_param_t *param)
{
    switch (event) {
    case ESP_HF_CLIENT_CONNECTION_STATE_EVT: {
        esp_hf_client_connection_state_t state = param->conn_stat.state;
        if (state == ESP_HF_CLIENT_CONNECTION_STATE_CONNECTED ||
            state == ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED) {
            s_hfp_state = BT_HFP_CONNECTED;
            memcpy(s_peer_bda, param->conn_stat.remote_bda, sizeof(esp_bd_addr_t));
            gpio_set_level(BT_LED, 1);
            ESP_LOGI(TAG, "HFP Connected");
        } else if (state == ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED) {
            s_sco_open = false;
            s_hfp_state = BT_HFP_IDLE;
            if (s_a2dp_state == BT_A2DP_IDLE) {
                gpio_set_level(BT_LED, 0);
            }
            ESP_LOGI(TAG, "HFP Disconnected");
        }
        break;
    }
    case ESP_HF_CLIENT_AUDIO_STATE_EVT: {
        esp_hf_client_audio_state_t audio_state = param->audio_stat.state;
    if (audio_state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC) {
        s_sco_open = true;
        s_sco_is_msbc = true;
        s_hfp_state = BT_HFP_AUDIO_OPEN;
        gpio_set_level(TEL_MUTE, 1);
        switch_to_sco();
        ESP_LOGI(TAG, "SCO Audio Open (mSBC 16kHz)");
    } else if (audio_state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED) {
        s_sco_open = true;
        s_sco_is_msbc = false;
        s_hfp_state = BT_HFP_AUDIO_OPEN;
        gpio_set_level(TEL_MUTE, 1);
        switch_to_sco();
        ESP_LOGI(TAG, "SCO Audio Open (CVSD 8kHz)");
    } else if (audio_state == ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED) {
        s_sco_open = false;
        gpio_set_level(TEL_MUTE, 0);
        if (s_hfp_state == BT_HFP_ACTIVE || s_hfp_state == BT_HFP_AUDIO_OPEN) {
            s_hfp_state = BT_HFP_CONNECTED;
        }
        ESP_LOGI(TAG, "SCO Audio Closed");
    }
        break;
    }
    case ESP_HF_CLIENT_RING_IND_EVT:
        ESP_LOGI(TAG, "HFP Ring!");
        if (s_hfp_state == BT_HFP_CONNECTED || s_hfp_state == BT_HFP_AUDIO_OPEN) {
            s_hfp_state = BT_HFP_INCOMING;
            esp_hf_client_connect_audio(s_peer_bda);
            if (s_a2dp_state == BT_A2DP_PLAYING) {
                s_a2dp_was_playing = true;
                send_avrcp_pt_cmd(ESP_AVRC_PT_CMD_PAUSE);
            } else {
                s_a2dp_was_playing = false;
            }
        }
        break;
    case ESP_HF_CLIENT_CLIP_EVT: {
        const char *num = param->clip.number;
            if (num) {
                strncpy(s_caller_id, num, sizeof(s_caller_id) - 1);
                s_caller_id[sizeof(s_caller_id) - 1] = '\0';
                ESP_LOGI(TAG, "Caller ID: %s", num);
        }
        break;
    }
    case ESP_HF_CLIENT_CIND_CALL_SETUP_EVT: {
        esp_hf_call_setup_status_t cs = param->call_setup.status;
        if (cs == ESP_HF_CALL_SETUP_STATUS_INCOMING) {
            s_hfp_state = BT_HFP_INCOMING;
        } else if (cs == ESP_HF_CALL_SETUP_STATUS_OUTGOING_DIALING ||
            cs == ESP_HF_CALL_SETUP_STATUS_OUTGOING_ALERTING) {
            s_hfp_state = BT_HFP_OUTGOING;
            esp_hf_client_connect_audio(s_peer_bda);
            if (s_a2dp_state == BT_A2DP_PLAYING) {
                    s_a2dp_was_playing = true;
                    send_avrcp_pt_cmd(ESP_AVRC_PT_CMD_PAUSE);
                }
        } else if (cs == ESP_HF_CALL_SETUP_STATUS_IDLE) {
            if (s_hfp_state == BT_HFP_INCOMING || s_hfp_state == BT_HFP_OUTGOING) {
                s_hfp_state = BT_HFP_CONNECTED;
            }
        }
        break;
    }
    case ESP_HF_CLIENT_CIND_CALL_EVT:
    if (param->call.status == ESP_HF_CALL_STATUS_NO_CALLS) {
        s_sco_open = false;
        s_caller_id[0] = '\0';
        if (s_hfp_state == BT_HFP_ACTIVE || s_hfp_state == BT_HFP_AUDIO_OPEN) {
            esp_hf_client_disconnect_audio(s_peer_bda);
            gpio_set_level(TEL_MUTE, 0);
            s_hfp_state = BT_HFP_CONNECTED;
            switch_to_a2dp();
        }
            if (s_a2dp_was_playing && s_a2dp_state >= BT_A2DP_CONNECTED) {
                switch_to_a2dp();
                send_avrcp_pt_cmd(ESP_AVRC_PT_CMD_PLAY);
                s_a2dp_was_playing = false;
            }
            ESP_LOGI(TAG, "Call ended");
        } else if (param->call.status == ESP_HF_CALL_STATUS_CALL_IN_PROGRESS) {
            if (s_hfp_state == BT_HFP_INCOMING || s_hfp_state == BT_HFP_OUTGOING) {
                s_hfp_state = BT_HFP_ACTIVE;
                if (!s_sco_open) {
                    esp_hf_client_connect_audio(s_peer_bda);
                }
            }
            ESP_LOGI(TAG, "Call active");
        }
        break;
    default:
        break;
    }
}

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Auth success: %s", param->auth_cmpl.device_name);
        } else {
            ESP_LOGE(TAG, "Auth failed, status: %d", param->auth_cmpl.stat);
        }
        break;
    case ESP_BT_GAP_PIN_REQ_EVT:
        ESP_LOGI(TAG, "PIN req, min_16_digit: %d", param->pin_req.min_16_digit);
        if (param->pin_req.min_16_digit) {
            esp_bt_pin_code_t pin_code = {0};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin_code);
        } else {
            esp_bt_pin_code_t pin_code;
            pin_code[0] = '1'; pin_code[1] = '2'; pin_code[2] = '3'; pin_code[3] = '4';
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        }
        break;
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "SSP confirm: %06"PRIu32, param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(TAG, "Mode change: %d", param->mode_chg.mode);
        break;
    default:
        break;
    }
}

static void serial_cmd_task(void *arg)
{
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
            if (s_volume < 95) { s_volume += 5; ESP_LOGI(TAG, "Vol: %d%%", s_volume); }
            break;
        case '-':
            if (s_volume > 5) { s_volume -= 5; ESP_LOGI(TAG, "Vol: %d%%", s_volume); }
            break;
        case 'm':
            s_muted = !s_muted;
            ESP_LOGI(TAG, "%s", s_muted ? "MUTED" : "UNMUTED");
            break;
        case 'p':
            if (s_a2dp_state >= BT_A2DP_CONNECTED) {
                send_avrcp_pt_cmd(ESP_AVRC_PT_CMD_PLAY);
                ESP_LOGI(TAG, "CMD: Play");
            }
            break;
        case 's':
            if (s_a2dp_state == BT_A2DP_PLAYING) {
                send_avrcp_pt_cmd(ESP_AVRC_PT_CMD_PAUSE);
                ESP_LOGI(TAG, "CMD: Pause");
            }
            break;
        case 'n':
            send_avrcp_pt_cmd(ESP_AVRC_PT_CMD_FORWARD);
            ESP_LOGI(TAG, "CMD: Next track");
            break;
        case 'b':
            send_avrcp_pt_cmd(ESP_AVRC_PT_CMD_BACKWARD);
            ESP_LOGI(TAG, "CMD: Previous track");
            break;
        case 'a':
            if (s_hfp_state == BT_HFP_INCOMING) {
                esp_hf_client_answer_call();
                ESP_LOGI(TAG, "CMD: Answer");
            }
            break;
        case 'r':
            if (s_hfp_state == BT_HFP_INCOMING) {
                esp_hf_client_reject_call();
                ESP_LOGI(TAG, "CMD: Reject");
            } else if (s_hfp_state == BT_HFP_ACTIVE || s_hfp_state == BT_HFP_OUTGOING) {
                esp_hf_client_reject_call();
                ESP_LOGI(TAG, "CMD: End call");
            }
            break;
        case 'd':
            if (s_hfp_state >= BT_HFP_CONNECTED) {
                esp_hf_client_dial(NULL);
                ESP_LOGI(TAG, "CMD: Redial");
            }
            break;
        case 'h':
        case '?':
            ESP_LOGI(TAG, "=== Status ===");
            ESP_LOGI(TAG, " A2DP: %s",
                s_a2dp_state == BT_A2DP_PLAYING ? "PLAYING" :
                s_a2dp_state == BT_A2DP_CONNECTED ? "CONNECTED" :
                s_a2dp_state == BT_A2DP_PAUSED ? "PAUSED" : "IDLE");
            ESP_LOGI(TAG, " HFP: %s",
                s_hfp_state == BT_HFP_ACTIVE ? "ACTIVE" :
                s_hfp_state == BT_HFP_INCOMING ? "INCOMING" :
                s_hfp_state == BT_HFP_OUTGOING ? "OUTGOING" :
                s_hfp_state == BT_HFP_AUDIO_OPEN ? "SCO OPEN" :
                s_hfp_state == BT_HFP_CONNECTED ? "CONNECTED" : "IDLE");
            ESP_LOGI(TAG, " Vol: %d%% %s", s_volume, s_muted ? "(MUTED)" : "");
            ESP_LOGI(TAG, " Meta: %s - %s (%s)",
                s_meta_title[0] ? s_meta_title : "-",
                s_meta_artist[0] ? s_meta_artist : "-",
                s_meta_album[0] ? s_meta_album : "-");
            if (s_caller_id[0]) ESP_LOGI(TAG, " Caller: %s", s_caller_id);
            break;
        default:
            break;
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " ESP32 BlueBus BT Test (ESP-IDF v6.0)");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Pair phone with 'BMW-BlueBus'");
    ESP_LOGI(TAG, "Commands: +/- vol, m mute, p play, s pause");
    ESP_LOGI(TAG, " n next, b prev, a answer, r reject, d redial, h status");

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BT_LED) | (1ULL << TEL_MUTE),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(BT_LED, 0);
    gpio_set_level(TEL_MUTE, 0);

    ESP_LOGI(TAG, "[1/6] NVS init...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "[2/6] Release BLE memory...");
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    ESP_LOGI(TAG, "[3/6] BT controller init...");
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Controller init failed: %s", esp_err_to_name(ret));
        return;
    }

    esp_bt_sleep_disable();
    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Controller enable failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "[4/6] Bluedroid init...");
    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ret = esp_bluedroid_init_with_cfg(&bluedroid_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "[5/6] Set device name + security...");
    esp_bt_gap_set_device_name("BMW-BlueBus");
    esp_bt_gap_register_callback(gap_cb);

    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(uint8_t));

    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_FIXED;
    esp_bt_pin_code_t pin_code;
    pin_code[0] = '1'; pin_code[1] = '2'; pin_code[2] = '3'; pin_code[3] = '4';
    esp_bt_gap_set_pin(pin_type, 4, pin_code);

    ESP_LOGI(TAG, "[6/6] Profile init (AVRCP, A2DP, HFP)...");
    esp_avrc_ct_register_callback(avrcp_ct_cb);
    ret = esp_avrc_ct_init();
    ESP_LOGI(TAG, "AVRCP CT init: %s", esp_err_to_name(ret));

    esp_a2d_register_callback(a2dp_cb);
    ret = esp_a2d_sink_init();
    ESP_LOGI(TAG, "A2DP sink init: %s", esp_err_to_name(ret));

    esp_a2d_sink_register_data_callback(a2dp_data_cb);

    esp_hf_client_register_callback(hfp_client_cb);
    ret = esp_hf_client_init();
    ESP_LOGI(TAG, "HFP client init: %s", esp_err_to_name(ret));

    esp_hf_client_register_data_callback(hfp_audio_recv_cb, hfp_audio_send_cb);

    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    ESP_LOGI(TAG, "I2S init...");
    s_i2s_mutex = xSemaphoreCreateMutex();
    if (!i2s_init(44100)) {
        ESP_LOGE(TAG, "I2S init failed (continuing without audio)");
    }

    s_meta_timer = xTimerCreate("meta_tmr", pdMS_TO_TICKS(2000), pdFALSE, NULL, meta_timer_cb);
    s_avrcp_cmd_sem = xSemaphoreCreateMutex();

    xTaskCreate(serial_cmd_task, "serial_cmd", 3072, NULL, 5, NULL);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " INIT COMPLETE - BT discoverable!");
    ESP_LOGI(TAG, "========================================");
}
