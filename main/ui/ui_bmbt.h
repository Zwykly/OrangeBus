#ifndef UI_BMBT_H
#define UI_BMBT_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "orangebus.h"

#define UI_BMBT_META_MAX 41

ui_bmbt_t *ui_bmbt_create(ibus_t *ibus, ibus_config_t *config);
void ui_bmbt_destroy(ui_bmbt_t *ui);
esp_err_t ui_bmbt_init(ui_bmbt_t *ui);
void ui_bmbt_tick(ui_bmbt_t *ui);
void ui_bmbt_on_metadata(ui_bmbt_t *ui, const char *title, const char *artist, const char *album);
void ui_bmbt_on_playback(ui_bmbt_t *ui, bool playing);
void ui_bmbt_on_ignition(ui_bmbt_t *ui, bool on);
void ui_bmbt_clear(ui_bmbt_t *ui);
void ui_bmbt_set_active(ui_bmbt_t *ui, bool active);

#endif
