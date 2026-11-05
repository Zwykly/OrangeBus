#ifndef IBUS_CONFIG_H
#define IBUS_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "orangebus.h"

/* Persistent user settings (one byte each, stored in NVS namespace "ibus_cfg").
 * Values are cached in RAM, so reads are cheap enough for hot paths. */
typedef enum {
    IBUS_CFG_UI_MODE = 0,       /* orangebus_ui_mode_t: which display the car has */
    IBUS_CFG_AUTOPLAY,          /* 1 = start playback when the phone connects */
    IBUS_CFG_COMFORT_BLINK,     /* 1 = flash indicators on lock/unlock */
    IBUS_CFG_COMFORT_LOCKS,     /* 1 = extra central-lock pulse on lock */
    IBUS_CFG_COMFORT_MIRRORS,   /* 1 = unfold mirrors on unlock */
    IBUS_CFG_META_MODE,         /* 0..3: track text layout, see send_metadata_to_ui() */
    IBUS_CFG_PWR_GATE,          /* 1 = power gating enabled, 0 = never switch off (bench) */
    IBUS_CFG_PWR_IDLE_S,        /* seconds of bus silence (ignition off) before power-off */
    IBUS_CFG_KEY_COUNT,
} ibus_config_key_t;

ibus_config_t *ibus_config_create(void);
void ibus_config_destroy(ibus_config_t *cfg);
esp_err_t ibus_config_init(ibus_config_t *cfg);

uint8_t ibus_config_get(const ibus_config_t *cfg, ibus_config_key_t key);
/* Writes to NVS only when the value actually changes (saves flash wear). */
void ibus_config_set(ibus_config_t *cfg, ibus_config_key_t key, uint8_t value);
/* Incremented on every successful change - lets other tasks notice updates
 * (e.g. a new UI mode) without a dedicated flag per setting. */
uint32_t ibus_config_get_revision(const ibus_config_t *cfg);

/* Human-readable UI mode name for logs / SPP replies. */
const char *ibus_config_ui_mode_name(uint8_t uiMode);

#endif
