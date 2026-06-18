#include "hfp_client.h"
#include <stdlib.h>
#include <string.h>
#include "audio_output.h"
#include "avrcp_controller.h"
#include "a2dp_sink.h"
#include "esp_log.h"
#include "esp_hf_client_api.h"
#include "esp_avrc_api.h"
#include "freertos/FreeRTOS.h"

#define TAG "HFP_CLIENT"

struct hfp_client_t {
    orangebus_hfp_state_t state;
    esp_bd_addr_t peer_bda;
    bool sco_open;
    bool sco_is_msbc;
    bool a2dp_was_playing;
    bool vra_active;
    char caller_id[33];
    audio_output_t *audio;
    avrcp_controller_t *avrcp;
    a2dp_sink_t *a2dp;
};

static hfp_client_t *s_instance = NULL;

/* Callback przekazujacy przychodzace dane audio SCO do wyjscia audio */
static void hfp_audio_recv_cb(const uint8_t *data, uint32_t len)
{
    if (s_instance) {
        audio_output_hfp_recv_cb(s_instance->audio, data, len);
    }
}

/* Callback pobierajacy dane audio SCO do wyslania ze zrodla audio */
static uint32_t hfp_audio_send_cb(uint8_t *data, uint32_t len)
{
    if (s_instance) {
        return audio_output_hfp_send_cb(s_instance->audio, data, len);
    }
    if (data && len) memset(data, 0, len);
    return len;
}

/* Glowny callback zdarzen klienta HFP (polaczenie, SCO, rozmowy, CLIP, BVRA) */
static void hfp_client_cb(esp_hf_client_cb_event_t event, esp_hf_client_cb_param_t *param)
{
    hfp_client_t *hf = s_instance;
    if (!hf) return;

    switch (event) {
    case ESP_HF_CLIENT_CONNECTION_STATE_EVT: {
        esp_hf_client_connection_state_t state = param->conn_stat.state;
        if (state == ESP_HF_CLIENT_CONNECTION_STATE_CONNECTED ||
            state == ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED) {
            hf->state = ORANGEBUS_HFP_CONNECTED;
            memcpy(hf->peer_bda, param->conn_stat.remote_bda, sizeof(esp_bd_addr_t));
            ESP_LOGI(TAG, "HFP Connected");
        } else if (state == ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED) {
            hf->sco_open = false;
            hf->state = ORANGEBUS_HFP_IDLE;
            ESP_LOGI(TAG, "HFP Disconnected");
        }
        break;
    }
    case ESP_HF_CLIENT_AUDIO_STATE_EVT: {
        esp_hf_client_audio_state_t audio_state = param->audio_stat.state;
        if (audio_state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC) {
            hf->sco_open = true;
            hf->sco_is_msbc = true;
            hf->state = ORANGEBUS_HFP_AUDIO_OPEN;
            audio_output_switch_sco(hf->audio, true);
            ESP_LOGI(TAG, "SCO Audio Open (mSBC 16kHz)");
        } else if (audio_state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED) {
            hf->sco_open = true;
            hf->sco_is_msbc = false;
            hf->state = ORANGEBUS_HFP_AUDIO_OPEN;
            audio_output_switch_sco(hf->audio, false);
            ESP_LOGI(TAG, "SCO Audio Open (CVSD 8kHz)");
        } else if (audio_state == ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED) {
            hf->sco_open = false;
            if (hf->state == ORANGEBUS_HFP_ACTIVE || hf->state == ORANGEBUS_HFP_AUDIO_OPEN) {
                hf->state = ORANGEBUS_HFP_CONNECTED;
            }
            ESP_LOGI(TAG, "SCO Audio Closed");
        }
        break;
    }
    case ESP_HF_CLIENT_RING_IND_EVT:
        ESP_LOGI(TAG, "HFP Ring!");
        if (hf->state == ORANGEBUS_HFP_CONNECTED || hf->state == ORANGEBUS_HFP_AUDIO_OPEN) {
            hf->state = ORANGEBUS_HFP_INCOMING;
            esp_hf_client_connect_audio(hf->peer_bda);
            if (a2dp_sink_get_state(hf->a2dp) == ORANGEBUS_A2DP_PLAYING) {
                hf->a2dp_was_playing = true;
                avrcp_controller_send_passthrough(hf->avrcp, ESP_AVRC_PT_CMD_PAUSE);
            } else {
                hf->a2dp_was_playing = false;
            }
        }
        break;
    case ESP_HF_CLIENT_CLIP_EVT: {
        const char *num = param->clip.number;
        if (num) {
            strncpy(hf->caller_id, num, sizeof(hf->caller_id) - 1);
            hf->caller_id[sizeof(hf->caller_id) - 1] = '\0';
            ESP_LOGI(TAG, "Caller ID: %s", num);
        }
        break;
    }
    case ESP_HF_CLIENT_CIND_CALL_SETUP_EVT: {
        esp_hf_call_setup_status_t cs = param->call_setup.status;
        if (cs == ESP_HF_CALL_SETUP_STATUS_INCOMING) {
            hf->state = ORANGEBUS_HFP_INCOMING;
        } else if (cs == ESP_HF_CALL_SETUP_STATUS_OUTGOING_DIALING ||
                   cs == ESP_HF_CALL_SETUP_STATUS_OUTGOING_ALERTING) {
            hf->state = ORANGEBUS_HFP_OUTGOING;
            esp_hf_client_connect_audio(hf->peer_bda);
            if (a2dp_sink_get_state(hf->a2dp) == ORANGEBUS_A2DP_PLAYING) {
                hf->a2dp_was_playing = true;
                avrcp_controller_send_passthrough(hf->avrcp, ESP_AVRC_PT_CMD_PAUSE);
            }
        } else if (cs == ESP_HF_CALL_SETUP_STATUS_IDLE) {
            if (hf->state == ORANGEBUS_HFP_INCOMING || hf->state == ORANGEBUS_HFP_OUTGOING) {
                hf->state = ORANGEBUS_HFP_CONNECTED;
            }
        }
        break;
    }
    case ESP_HF_CLIENT_CIND_CALL_EVT:
        if (param->call.status == ESP_HF_CALL_STATUS_NO_CALLS) {
            hf->sco_open = false;
            hf->caller_id[0] = '\0';
            if (hf->state == ORANGEBUS_HFP_ACTIVE || hf->state == ORANGEBUS_HFP_AUDIO_OPEN) {
                esp_hf_client_disconnect_audio(hf->peer_bda);
                hf->state = ORANGEBUS_HFP_CONNECTED;
                audio_output_switch_a2dp(hf->audio);
            }
            if (hf->a2dp_was_playing && a2dp_sink_get_state(hf->a2dp) >= ORANGEBUS_A2DP_CONNECTED) {
                audio_output_switch_a2dp(hf->audio);
                avrcp_controller_send_passthrough(hf->avrcp, ESP_AVRC_PT_CMD_PLAY);
                hf->a2dp_was_playing = false;
            }
            ESP_LOGI(TAG, "Call ended");
        } else if (param->call.status == ESP_HF_CALL_STATUS_CALL_IN_PROGRESS) {
            if (hf->state == ORANGEBUS_HFP_INCOMING || hf->state == ORANGEBUS_HFP_OUTGOING) {
                hf->state = ORANGEBUS_HFP_ACTIVE;
                if (!hf->sco_open) {
                    esp_hf_client_connect_audio(hf->peer_bda);
                }
            }
            ESP_LOGI(TAG, "Call active");
        }
        break;
    case ESP_HF_CLIENT_BVRA_EVT:
        hf->vra_active = (param->bvra.value == ESP_HF_VR_STATE_ENABLED);
        ESP_LOGI(TAG, "Voice recognition %s", hf->vra_active ? "ENABLED" : "DISABLED");
        break;
    default:
        break;
    }
}

/* Konstruktor klienta HFP - alokuje strukture i wiaze z audio, AVRCP i A2DP */
hfp_client_t *hfp_client_create(audio_output_t *audio, avrcp_controller_t *avrcp, a2dp_sink_t *a2dp)
{
    hfp_client_t *hf = calloc(1, sizeof(hfp_client_t));
    if (!hf) return NULL;
    hf->audio = audio;
    hf->avrcp = avrcp;
    hf->a2dp = a2dp;
    return hf;
}

void hfp_client_destroy(hfp_client_t *hf)
{
    if (hf) free(hf);
}

esp_err_t hfp_client_init(hfp_client_t *hf)
{
    if (!hf) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

orangebus_hfp_state_t hfp_client_get_state(const hfp_client_t *hf)
{
    return hf ? hf->state : ORANGEBUS_HFP_IDLE;
}

orangebus_hfp_state_t *hfp_client_get_state_ptr(hfp_client_t *hf)
{
    return hf ? &hf->state : NULL;
}

const char *hfp_client_state_str(orangebus_hfp_state_t state)
{
    switch (state) {
    case ORANGEBUS_HFP_ACTIVE:     return "ACTIVE";
    case ORANGEBUS_HFP_INCOMING:   return "INCOMING";
    case ORANGEBUS_HFP_OUTGOING:   return "OUTGOING";
    case ORANGEBUS_HFP_AUDIO_OPEN: return "SCO OPEN";
    case ORANGEBUS_HFP_CONNECTED:  return "CONNECTED";
    default:                     return "IDLE";
    }
}

const char *hfp_client_get_caller_id(const hfp_client_t *hf)
{
    return hf ? hf->caller_id : "";
}

bool hfp_client_is_vra_active(const hfp_client_t *hf)
{
    return hf ? hf->vra_active : false;
}

void hfp_client_answer(hfp_client_t *hf)
{
    if (hf && hf->state == ORANGEBUS_HFP_INCOMING) {
        esp_hf_client_answer_call();
    }
}

void hfp_client_reject(hfp_client_t *hf)
{
    if (!hf) return;
    if (hf->state == ORANGEBUS_HFP_INCOMING) {
        esp_hf_client_reject_call();
    } else if (hf->state == ORANGEBUS_HFP_ACTIVE || hf->state == ORANGEBUS_HFP_OUTGOING) {
        esp_hf_client_reject_call();
    }
}

void hfp_client_redial(hfp_client_t *hf)
{
    if (hf && hf->state >= ORANGEBUS_HFP_CONNECTED) {
        esp_hf_client_dial(NULL);
    }
}

void hfp_client_toggle_voice_recognition(hfp_client_t *hf)
{
    if (!hf || hf->state < ORANGEBUS_HFP_CONNECTED) return;
    if (hf->vra_active) {
        esp_hf_client_stop_voice_recognition();
    } else {
        esp_hf_client_start_voice_recognition();
    }
}

/* Rejestruje callbacki HFP i inicjalizuje klienta - data callback tylko przy sukcesie init */
esp_err_t hfp_client_register_callbacks(hfp_client_t *hf)
{
    if (!hf) return ESP_ERR_INVALID_ARG;
    s_instance = hf;
    esp_hf_client_register_callback(hfp_client_cb);
    esp_err_t ret = esp_hf_client_init();
    if (ret == ESP_OK) {
        esp_hf_client_register_data_callback(hfp_audio_recv_cb, hfp_audio_send_cb);
    }
    return ret;
}
