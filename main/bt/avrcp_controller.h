#ifndef AVRCP_CONTROLLER_H
#define AVRCP_CONTROLLER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "orangebus.h"

#define AVRCP_CMD_MIN_INTERVAL_MS  100
#define AVRCP_PRESSED_RELEASED_GAP_MS 50
#define AVRCP_PT_CMD_VOICE_RECOG   0x30

avrcp_controller_t *avrcp_controller_create(void);
void avrcp_controller_destroy(avrcp_controller_t *ac);

esp_err_t avrcp_controller_init(avrcp_controller_t *ac);

void avrcp_controller_send_passthrough(avrcp_controller_t *ac, uint8_t cmd);
void avrcp_controller_request_metadata(avrcp_controller_t *ac);
const orangebus_metadata_t *avrcp_controller_get_metadata(const avrcp_controller_t *ac);
void avrcp_controller_copy_metadata(const avrcp_controller_t *ac, orangebus_metadata_t *out);

void avrcp_controller_set_a2dp_state_ref(avrcp_controller_t *ac, orangebus_a2dp_state_t *ref);
void avrcp_controller_set_a2dp_sink(avrcp_controller_t *ac, struct a2dp_sink_t *sink);
orangebus_a2dp_state_t *avrcp_controller_get_a2dp_state_ref(avrcp_controller_t *ac);

esp_err_t avrcp_controller_register_callbacks(avrcp_controller_t *ac);

#endif
