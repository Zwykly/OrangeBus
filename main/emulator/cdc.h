#ifndef CDC_H
#define CDC_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "orangebus.h"

/* CD changer (CDC, I-BUS address 0x18) emulator.
 * Makes the radio believe a CD changer is fitted so it switches its audio input
 * to us, and translates the radio's CD commands into playback state. */

cdc_t *cdc_create(ibus_t *ibus, ibus_config_t *config);
void cdc_destroy(cdc_t *cdc);
esp_err_t cdc_init(cdc_t *cdc);

/* Radio -> CDC command (0x38 xx): updates state and sends the status reply. */
void cdc_on_request(cdc_t *cdc, const uint8_t *data, uint8_t len);
/* Radio pinged the CDC (the driver already answered); restarts the announce timer. */
void cdc_on_ping(cdc_t *cdc);
void cdc_on_ignition(cdc_t *cdc, bool ignitionOn);
/* Broadcasts "CD changer ready after reset" so the radio (re)discovers us. */
void cdc_announce(cdc_t *cdc);
/* Call every 100 ms: re-announces / refreshes status while the ignition is on. */
void cdc_tick(cdc_t *cdc);

bool cdc_is_playing(const cdc_t *cdc);
/* Local play/stop (autoplay, SPP): updates state and tells the radio. */
void cdc_set_playing(cdc_t *cdc, bool playing);
bool cdc_is_ignition_on(const cdc_t *cdc);

#endif
