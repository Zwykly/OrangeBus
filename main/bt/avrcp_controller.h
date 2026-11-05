#ifndef AVRCP_CONTROLLER_H
#define AVRCP_CONTROLLER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "orangebus.h"

/* AVRCP controller: sends play/pause/next/prev to the phone and collects the
 * current track's title / artist / album. */

#define AVRCP_CMD_MIN_INTERVAL_MS     100   /* gap between two passthrough commands */
#define AVRCP_PRESSED_RELEASED_GAP_MS 50    /* "button down" time of one command */
#define AVRCP_PT_CMD_VOICE_RECOG      0x30  /* vendor passthrough: voice assistant */

avrcp_controller_t *avrcp_controller_create(void);
void avrcp_controller_destroy(avrcp_controller_t *ac);
esp_err_t avrcp_controller_init(avrcp_controller_t *ac);
void avrcp_controller_set_a2dp_sink(avrcp_controller_t *ac, a2dp_sink_t *sink);
esp_err_t avrcp_controller_register_callbacks(avrcp_controller_t *ac);

/* Queues a passthrough command (ESP_AVRC_PT_CMD_*); never blocks on the stack. */
void avrcp_controller_send_passthrough(avrcp_controller_t *ac, uint8_t cmd);
void avrcp_controller_request_metadata(avrcp_controller_t *ac);

/* Changes whenever new metadata is published - compare it before copying. */
uint32_t avrcp_controller_get_metadata_version(const avrcp_controller_t *ac);
/* Thread-safe snapshot of the last complete metadata. */
void avrcp_controller_copy_metadata(const avrcp_controller_t *ac, orangebus_metadata_t *out);

#endif
