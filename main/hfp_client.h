#ifndef HFP_CLIENT_H
#define HFP_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "bluebus.h"

hfp_client_t *hfp_client_create(audio_output_t *audio, avrcp_controller_t *avrcp, a2dp_sink_t *a2dp);
void hfp_client_destroy(hfp_client_t *hf);

esp_err_t hfp_client_init(hfp_client_t *hf);

bluebus_hfp_state_t hfp_client_get_state(const hfp_client_t *hf);
bluebus_hfp_state_t *hfp_client_get_state_ptr(hfp_client_t *hf);
const char *hfp_client_state_str(bluebus_hfp_state_t state);
const char *hfp_client_get_caller_id(const hfp_client_t *hf);
bool hfp_client_is_vra_active(const hfp_client_t *hf);

void hfp_client_answer(hfp_client_t *hf);
void hfp_client_reject(hfp_client_t *hf);
void hfp_client_redial(hfp_client_t *hf);
void hfp_client_toggle_voice_recognition(hfp_client_t *hf);

esp_err_t hfp_client_register_callbacks(hfp_client_t *hf);

#endif
