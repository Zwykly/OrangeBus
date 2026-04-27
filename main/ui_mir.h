#ifndef UI_MIR_H
#define UI_MIR_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "bluebus.h"

ui_mir_t *ui_mir_create(ibus_t *ibus, ibus_config_t *config);
void ui_mir_destroy(ui_mir_t *ui);
esp_err_t ui_mir_init(ui_mir_t *ui);
void ui_mir_tick(ui_mir_t *ui);
void ui_mir_show_title(ui_mir_t *ui, const char *text);
void ui_mir_clear(ui_mir_t *ui);
void ui_mir_on_ignition(ui_mir_t *ui, bool on);

#endif
