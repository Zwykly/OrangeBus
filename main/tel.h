#ifndef TEL_H
#define TEL_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "bluebus.h"

#define TEL_CALLER_ID_MAX 32

tel_t *tel_create(ibus_t *ibus, ibus_config_t *config);
void tel_destroy(tel_t *tel);
esp_err_t tel_init(tel_t *tel);
void tel_set_connected(tel_t *tel, bool connected);
void tel_set_call_active(tel_t *tel, bool active);
void tel_set_call_incoming(tel_t *tel, bool incoming);
void tel_set_caller_id(tel_t *tel, const char *id);
void tel_tick(tel_t *tel);
bool tel_is_connected(const tel_t *tel);
bool tel_is_call_active(const tel_t *tel);

#endif
