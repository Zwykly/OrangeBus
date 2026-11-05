#include "ibus_config.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define TAG "IBUS_CFG"
#define NVS_NAMESPACE "ibus_cfg"

typedef struct {
    char key[16];
    uint8_t default_val;
} config_entry_t;

static const config_entry_t s_defaults[] = {
{"ui_mode", ORANGEBUS_UI_MODE_CD53},
{"autoplay", 1},
{"comfort_blink",0},
{"comfort_locks",0},
{"comfort_mirrors",0},
{"meta_mode", 0},
};

#define NUM_DEFAULTS (sizeof(s_defaults) / sizeof(s_defaults[0]))

struct ibus_config_t {
    nvs_handle_t nvs_handle;
    bool initialized;
    uint8_t cache[NUM_DEFAULTS];
    bool cache_valid;
};

ibus_config_t *ibus_config_create(void)
{
    ibus_config_t *cfg = calloc(1, sizeof(ibus_config_t));
    return cfg;
}

void ibus_config_destroy(ibus_config_t *cfg)
{
    if (cfg) {
        if (cfg->initialized) nvs_close(cfg->nvs_handle);
        free(cfg);
    }
}

esp_err_t ibus_config_init(ibus_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &cfg->nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(ret));
        return ret;
    }
    cfg->initialized = true;

    for (int i = 0; i < (int)NUM_DEFAULTS; i++) {
        uint8_t val;
        ret = nvs_get_u8(cfg->nvs_handle, s_defaults[i].key, &val);
        if (ret == ESP_ERR_NOT_FOUND) {
            val = s_defaults[i].default_val;
            nvs_set_u8(cfg->nvs_handle, s_defaults[i].key, val);
        } else if (ret != ESP_OK) {
            val = s_defaults[i].default_val;
        }
        cfg->cache[i] = val;
    }
    cfg->cache_valid = true;
    nvs_commit(cfg->nvs_handle);

    ESP_LOGI(TAG, "I-BUS config initialized");
    return ESP_OK;
}

static int find_index(const char *key)
{
    for (int i = 0; i < (int)NUM_DEFAULTS; i++) {
        if (strcmp(s_defaults[i].key, key) == 0) return i;
    }
    return -1;
}

static uint8_t find_default(const char *key)
{
    int idx = find_index(key);
    return idx >= 0 ? s_defaults[idx].default_val : 0;
}

uint8_t ibus_config_get(ibus_config_t *cfg, const char *key)
{
    if (!cfg || !key) return find_default(key ? key : "");
    int idx = find_index(key);
    if (idx < 0) return 0;
    if (cfg->cache_valid) return cfg->cache[idx];
    if (!cfg->initialized) return s_defaults[idx].default_val;
    uint8_t val;
    esp_err_t ret = nvs_get_u8(cfg->nvs_handle, key, &val);
    if (ret != ESP_OK) return s_defaults[idx].default_val;
    return val;
}

void ibus_config_set(ibus_config_t *cfg, const char *key, uint8_t val)
{
    if (!cfg || !cfg->initialized || !key) return;
    int idx = find_index(key);
    if (idx >= 0 && cfg->cache_valid && cfg->cache[idx] == val) return;
    if (nvs_set_u8(cfg->nvs_handle, key, val) != ESP_OK) return;
    if (nvs_commit(cfg->nvs_handle) != ESP_OK) return;
    if (idx >= 0) cfg->cache[idx] = val;
}
