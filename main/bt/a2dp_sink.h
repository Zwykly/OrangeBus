#ifndef A2DP_SINK_H
#define A2DP_SINK_H

/* A2DP sink: receives music from the phone and tracks the stream state. */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "orangebus.h"

a2dp_sink_t *a2dp_sink_create(audio_output_t *audio);
void a2dp_sink_destroy(a2dp_sink_t *sink);

esp_err_t a2dp_sink_init(a2dp_sink_t *sink);

orangebus_a2dp_state_t a2dp_sink_get_state(const a2dp_sink_t *sink);
void a2dp_sink_set_state(a2dp_sink_t *sink, orangebus_a2dp_state_t state);
const char *a2dp_sink_state_str(orangebus_a2dp_state_t state);

esp_err_t a2dp_sink_register_callbacks(a2dp_sink_t *sink);

#endif
