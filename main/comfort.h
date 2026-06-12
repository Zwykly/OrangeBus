#ifndef COMFORT_H
#define COMFORT_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "bluebus.h"

comfort_t *comfort_create(ibus_t *ibus, ibus_config_t *config);
void comfort_destroy(comfort_t *c);
esp_err_t comfort_init(comfort_t *c);
void comfort_tick(comfort_t *c);
void comfort_on_ignition(comfort_t *c, bool on);
void comfort_on_door_lock(comfort_t *c, bool locked);
void comfort_on_gm_status(comfort_t *c, uint8_t *data, uint8_t len);
void comfort_on_lm_status(comfort_t *c, uint8_t *data, uint8_t len);
void comfort_send_test_blink(comfort_t *c);
bluebus_comfort_gm_variant_t comfort_get_gm_variant(const comfort_t *c);
bluebus_comfort_lm_variant_t comfort_get_lm_variant(const comfort_t *c);

#endif
