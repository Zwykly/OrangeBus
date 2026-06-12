#ifndef A2DP_SINK_H
#define A2DP_SINK_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "bluebus.h"

a2dp_sink_t *a2dp_sink_create(audio_output_t *audio);
void a2dp_sink_destroy(a2dp_sink_t *sink);

esp_err_t a2dp_sink_init(a2dp_sink_t *sink);

bluebus_a2dp_state_t a2dp_sink_get_state(const a2dp_sink_t *sink);
bluebus_a2dp_state_t *a2dp_sink_get_state_ptr(a2dp_sink_t *sink);
const char *a2dp_sink_state_str(bluebus_a2dp_state_t state);

void a2dp_sink_set_hfp_state_ref(a2dp_sink_t *sink, bluebus_hfp_state_t *ref);
esp_err_t a2dp_sink_register_callbacks(a2dp_sink_t *sink);

#endif
