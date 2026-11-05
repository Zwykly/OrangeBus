#ifndef UI_CD53_H
#define UI_CD53_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "orangebus.h"

ui_cd53_t *ui_cd53_create(ibus_t *ibus, ibus_config_t *config);
void ui_cd53_destroy(ui_cd53_t *ui);
esp_err_t ui_cd53_init(ui_cd53_t *ui);
void ui_cd53_tick(ui_cd53_t *ui);
void ui_cd53_show_title(ui_cd53_t *ui, const char *text);
void ui_cd53_clear(ui_cd53_t *ui);
void ui_cd53_on_ignition(ui_cd53_t *ui, bool on);
void ui_cd53_set_active(ui_cd53_t *ui, bool active);

#endif
