#include "ibus_config.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define TAG "IBUS_CFG"
#define NVS_NAMESPACE "ibus_cfg"

/* NVS key name (max 15 chars) and factory default for every setting.
 * Indexed by ibus_config_key_t - keep both lists in the same order. */
typedef struct {
    const char *nvsKey;
    uint8_t defaultValue;
} config_entry_t;

static const config_entry_t s_entries[IBUS_CFG_KEY_COUNT] = {
    [IBUS_CFG_UI_MODE]          = {"ui_mode", ORANGEBUS_UI_MODE_CD53},
    [IBUS_CFG_AUTOPLAY]         = {"autoplay", 1},
    [IBUS_CFG_COMFORT_BLINK]    = {"comfort_blink", 0},
    [IBUS_CFG_COMFORT_LOCKS]    = {"comfort_locks", 0},
    [IBUS_CFG_COMFORT_MIRRORS]  = {"comfort_mirrors", 0},
    [IBUS_CFG_META_MODE]        = {"meta_mode", 0},
    [IBUS_CFG_PWR_GATE]         = {"pwr_gate", 1},
    [IBUS_CFG_PWR_IDLE_S]       = {"pwr_idle_s", 60},
};

struct ibus_config_t {
    nvs_handle_t nvsHandle;
    bool nvsOpen;
    uint8_t values[IBUS_CFG_KEY_COUNT];   /* RAM cache, always valid after create */
    volatile uint32_t revision;
};

static bool key_valid(ibus_config_key_t key)
{
    return (unsigned)key < IBUS_CFG_KEY_COUNT;
}

ibus_config_t *ibus_config_create(void)
{
    ibus_config_t *cfg = calloc(1, sizeof(ibus_config_t));
    if (!cfg) return NULL;
    /* Defaults until init() loads NVS, so get() is valid even if NVS fails. */
    for (int i = 0; i < IBUS_CFG_KEY_COUNT; i++) {
        cfg->values[i] = s_entries[i].defaultValue;
    }
    return cfg;
}

void ibus_config_destroy(ibus_config_t *cfg)
{
    if (!cfg) return;
    if (cfg->nvsOpen) nvs_close(cfg->nvsHandle);
    free(cfg);
}

/* Loads every setting from NVS; missing keys are written with their default. */
esp_err_t ibus_config_init(ibus_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &cfg->nvsHandle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s (using defaults)", esp_err_to_name(ret));
        return ret;
    }
    cfg->nvsOpen = true;

    bool wroteDefaults = false;
    for (int i = 0; i < IBUS_CFG_KEY_COUNT; i++) {
        uint8_t value;
        ret = nvs_get_u8(cfg->nvsHandle, s_entries[i].nvsKey, &value);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            value = s_entries[i].defaultValue;
            nvs_set_u8(cfg->nvsHandle, s_entries[i].nvsKey, value);
            wroteDefaults = true;
        } else if (ret != ESP_OK) {
            value = s_entries[i].defaultValue;
        }
        cfg->values[i] = value;
    }
    if (wroteDefaults) nvs_commit(cfg->nvsHandle);

    ESP_LOGI(TAG, "I-BUS config initialized");
    return ESP_OK;
}

uint8_t ibus_config_get(const ibus_config_t *cfg, ibus_config_key_t key)
{
    if (!key_valid(key)) return 0;
    return cfg ? cfg->values[key] : s_entries[key].defaultValue;
}

void ibus_config_set(ibus_config_t *cfg, ibus_config_key_t key, uint8_t value)
{
    if (!cfg || !key_valid(key)) return;
    if (cfg->values[key] == value) return;
    if (cfg->nvsOpen) {
        if (nvs_set_u8(cfg->nvsHandle, s_entries[key].nvsKey, value) != ESP_OK) return;
        if (nvs_commit(cfg->nvsHandle) != ESP_OK) return;
    }
    cfg->values[key] = value;
    cfg->revision++;
}

uint32_t ibus_config_get_revision(const ibus_config_t *cfg)
{
    return cfg ? cfg->revision : 0;
}

const char *ibus_config_ui_mode_name(uint8_t uiMode)
{
    switch (uiMode) {
    case ORANGEBUS_UI_MODE_CD53: return "CD53";
    case ORANGEBUS_UI_MODE_BMBT: return "BMBT";
    case ORANGEBUS_UI_MODE_MID:  return "MID";
    case ORANGEBUS_UI_MODE_MIR:  return "MIR";
    default:                     return "UNKNOWN";
    }
}
