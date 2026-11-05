#ifndef ORANGEBUS_TYPES_H
#define ORANGEBUS_TYPES_H

#include <stdint.h>
#include <stdbool.h>

/* --- A2DP (Bluetooth music stream) state --- */
typedef enum {
    ORANGEBUS_A2DP_IDLE = 0,
    ORANGEBUS_A2DP_CONNECTING,
    ORANGEBUS_A2DP_CONNECTED,
    ORANGEBUS_A2DP_PLAYING,
    ORANGEBUS_A2DP_PAUSED,
} orangebus_a2dp_state_t;

/* --- HFP (hands-free phone) state --- */
typedef enum {
    ORANGEBUS_HFP_IDLE = 0,
    ORANGEBUS_HFP_CONNECTED,
    ORANGEBUS_HFP_AUDIO_OPEN,
    ORANGEBUS_HFP_INCOMING,
    ORANGEBUS_HFP_OUTGOING,
    ORANGEBUS_HFP_ACTIVE,
} orangebus_hfp_state_t;

/* --- I-BUS events delivered to the application --- */
typedef enum {
    ORANGEBUS_IBUS_EVT_FIRST_FRAME = 0,   /* first valid foreign frame since boot */
    ORANGEBUS_IBUS_EVT_CDC_REQUEST,       /* radio -> CDC command (0x38), incl. status poll */
    ORANGEBUS_IBUS_EVT_CDC_PING,          /* module ping to the CDC (0x01), already answered */
    ORANGEBUS_IBUS_EVT_MFL_BUTTON,        /* steering wheel button (0x3B) */
    ORANGEBUS_IBUS_EVT_MFL_VOLUME,        /* steering wheel volume keys (0x32) */
    ORANGEBUS_IBUS_EVT_IGNITION_STATUS,   /* IKE ignition status (0x11) */
    ORANGEBUS_IBUS_EVT_RAD_VOLUME,        /* radio volume step (0x32) */
    ORANGEBUS_IBUS_EVT_MID_BUTTON_PRESS,
    ORANGEBUS_IBUS_EVT_MID_MODE_CHANGE,
    ORANGEBUS_IBUS_EVT_BMBT_BUTTON_PRESS,
    ORANGEBUS_IBUS_EVT_GT_MENU_SELECT,
    ORANGEBUS_IBUS_EVT_GT_CHANGE_UI_REQ,
    ORANGEBUS_IBUS_EVT_PDC_STATUS,
    ORANGEBUS_IBUS_EVT_GM_STATUS,
    ORANGEBUS_IBUS_EVT_LM_STATUS,
    ORANGEBUS_IBUS_EVT_DOOR_LOCK,
    ORANGEBUS_IBUS_EVT_COUNT,
} orangebus_ibus_event_t;

/* I-BUS event callback: data = frame payload (after CMD), len = payload length. */
typedef void (*orangebus_ibus_cb_t)(uint8_t *data, uint8_t len);

/* --- Display UI mode (depends on the radio / display in the car) --- */
typedef enum {
    ORANGEBUS_UI_MODE_CD53 = 1,
    ORANGEBUS_UI_MODE_BMBT = 2,
    ORANGEBUS_UI_MODE_MID = 3,
    ORANGEBUS_UI_MODE_MIR = 5,
} orangebus_ui_mode_t;

/* --- GM (ZKE body module) variants --- */
typedef enum {
    ORANGEBUS_COMFORT_GM_UNKNOWN = 0,
    ORANGEBUS_COMFORT_GM_ZKE3_GM1,
    ORANGEBUS_COMFORT_GM_ZKE3_GM5,
    ORANGEBUS_COMFORT_GM_ZKE5,
} orangebus_comfort_gm_variant_t;

/* --- Light module (LCM/LSZ) variants --- */
typedef enum {
    ORANGEBUS_COMFORT_LM_UNKNOWN = 0,
    ORANGEBUS_COMFORT_LM_LME38,
    ORANGEBUS_COMFORT_LM_LCM,
    ORANGEBUS_COMFORT_LM_LCM_II,
    ORANGEBUS_COMFORT_LM_LSZ,
} orangebus_comfort_lm_variant_t;

/* Track metadata from AVRCP (title, artist, album). */
typedef struct {
    char title[81];
    char artist[81];
    char album[81];
} orangebus_metadata_t;

/* Opaque module handles (defined inside each module). */
typedef struct audio_output_t audio_output_t;
typedef struct avrcp_controller_t avrcp_controller_t;
typedef struct a2dp_sink_t a2dp_sink_t;
typedef struct hfp_client_t hfp_client_t;
typedef struct cli_t cli_t;
typedef struct eq_processor_t eq_processor_t;
typedef struct spp_server_t spp_server_t;
typedef struct ibus_t ibus_t;
typedef struct ibus_config_t ibus_config_t;
typedef struct cdc_t cdc_t;
typedef struct tel_t tel_t;
typedef struct ui_cd53_t ui_cd53_t;
typedef struct ui_mir_t ui_mir_t;
typedef struct ui_mid_t ui_mid_t;
typedef struct ui_bmbt_t ui_bmbt_t;
typedef struct comfort_t comfort_t;

#endif
