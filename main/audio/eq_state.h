#ifndef EQ_STATE_H
#define EQ_STATE_H

/* Persistent "live" EQ state: the 5 band parameters, the on/off switch and
 * the name of the active preset survive a power cycle.
 *
 * The module only uses the public eq_processor API. A low-rate periodic
 * check snapshots the processor and writes NVS once a change has been stable
 * for one period, so dragging a slider in the app (dozens of band updates a
 * second) costs a single flash write instead of one per update.
 *
 * Named presets keep living in the "eq_preset" namespace owned by
 * eq_processor; this module only lists / deletes them. */

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "eq_processor.h"

/* Longest preset name accepted over SPP (NVS key "p_<name>" must fit 15 chars). */
#define EQ_STATE_NAME_MAX 12

typedef struct eq_state_t eq_state_t;

eq_state_t *eq_state_create(eq_processor_t *eq);

/* Applies the stored state to the processor (call once after
 * eq_processor_init) and starts the background save check. */
esp_err_t eq_state_restore_and_start(eq_state_t *st);

/* Name of the active preset; "" means the bands were edited by hand. */
void eq_state_get_name(eq_state_t *st, char *out, size_t outSize);
void eq_state_set_name(eq_state_t *st, const char *name);

/* 1..EQ_STATE_NAME_MAX chars of [A-Za-z0-9 _+.-] (no ',' '=' ':' so names
 * can be listed safely in a single SPP line). */
bool eq_state_name_valid(const char *name);

/* Writes "name1,name2,..." of every preset stored on the device. */
void eq_state_list_presets(char *out, size_t outSize);
esp_err_t eq_state_delete_preset(const char *name);

#endif
