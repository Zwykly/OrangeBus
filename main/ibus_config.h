#ifndef IBUS_CONFIG_H
#define IBUS_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "orangebus.h"

ibus_config_t *ibus_config_create(void);
void ibus_config_destroy(ibus_config_t *cfg);
esp_err_t ibus_config_init(ibus_config_t *cfg);
uint8_t ibus_config_get(ibus_config_t *cfg, const char *key);
void ibus_config_set(ibus_config_t *cfg, const char *key, uint8_t val);

#endif
