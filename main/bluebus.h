#ifndef BLUEBUS_H
#define BLUEBUS_H

#include "esp_err.h"

#define BLUEBUS_I2S_BCK     26
#define BLUEBUS_I2S_WS      25
#define BLUEBUS_I2S_DATA    22
#define BLUEBUS_BT_LED      2
#define BLUEBUS_TEL_MUTE    4

typedef enum {
    BLUEBUS_A2DP_IDLE = 0,
    BLUEBUS_A2DP_CONNECTING,
    BLUEBUS_A2DP_CONNECTED,
    BLUEBUS_A2DP_PLAYING,
    BLUEBUS_A2DP_PAUSED,
} bluebus_a2dp_state_t;

typedef enum {
    BLUEBUS_HFP_IDLE = 0,
    BLUEBUS_HFP_CONNECTED,
    BLUEBUS_HFP_AUDIO_OPEN,
    BLUEBUS_HFP_INCOMING,
    BLUEBUS_HFP_OUTGOING,
    BLUEBUS_HFP_ACTIVE,
} bluebus_hfp_state_t;

typedef struct {
    char title[81];
    char artist[81];
    char album[81];
} bluebus_metadata_t;

typedef struct audio_output_t audio_output_t;
typedef struct avrcp_controller_t avrcp_controller_t;
typedef struct a2dp_sink_t a2dp_sink_t;
typedef struct hfp_client_t hfp_client_t;
typedef struct cli_t cli_t;

#endif
