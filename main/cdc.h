#ifndef CDC_H
#define CDC_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "bluebus.h"

cdc_t *cdc_create(ibus_t *ibus, ibus_config_t *config);
void cdc_destroy(cdc_t *cdc);
esp_err_t cdc_init(cdc_t *cdc);
void cdc_on_request(cdc_t *cdc, uint8_t *data, uint8_t len);
void cdc_on_ignition(cdc_t *cdc, uint8_t *data, uint8_t len);
void cdc_on_button_press(cdc_t *cdc, uint8_t *data, uint8_t len);
void cdc_tick(cdc_t *cdc);
bool cdc_is_playing(const cdc_t *cdc);
void cdc_set_playing(cdc_t *cdc, bool playing);
bool cdc_is_ignition_on(const cdc_t *cdc);

#endif
