#ifndef UI_MID_H
#define UI_MID_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "bluebus.h"

ui_mid_t *ui_mid_create(ibus_t *ibus, ibus_config_t *config);
void ui_mid_destroy(ui_mid_t *ui);
esp_err_t ui_mid_init(ui_mid_t *ui);
void ui_mid_tick(ui_mid_t *ui);
void ui_mid_show_title(ui_mid_t *ui, const char *text);
void ui_mid_clear(ui_mid_t *ui);
void ui_mid_on_ignition(ui_mid_t *ui, bool on);
void ui_mid_on_cdc_start(ui_mid_t *ui);
void ui_mid_on_cdc_stop(ui_mid_t *ui);
void ui_mid_set_active(ui_mid_t *ui, bool active);

#endif
