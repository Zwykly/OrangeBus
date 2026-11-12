#include "eq_state.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"

#define TAG "EQ_STATE"
#define STATE_NAMESPACE  "eq_state"
#define STATE_KEY        "active"
#define PRESET_NAMESPACE "eq_preset"   /* owned by eq_processor.c */
#define PRESET_PREFIX    "p_"
#define STATE_VERSION    1
#define CHECK_PERIOD_US  (2 * 1000 * 1000)

/* Stored as one NVS blob. Always memset before filling so padding bytes
 * compare equal in memcmp(). */
typedef struct {
    uint8_t version;
    uint8_t enabled;
    char name[EQ_STATE_NAME_MAX + 1];
    float bands[EQ_BANDS * 3];  /* freq, q, gain per band */
} eq_state_blob_t;

struct eq_state_t {
    eq_processor_t *eq;
    portMUX_TYPE lock;                  /* guards name */
    char name[EQ_STATE_NAME_MAX + 1];
    eq_state_blob_t saved;              /* what NVS currently holds */
    eq_state_blob_t pending;            /* snapshot from the previous check */
    bool havePending;
    esp_timer_handle_t timer;
};

eq_state_t *eq_state_create(eq_processor_t *eq)
{
    if (!eq) return NULL;
    eq_state_t *st = calloc(1, sizeof(eq_state_t));
    if (!st) return NULL;
    st->eq = eq;
    portMUX_INITIALIZE(&st->lock);
    return st;
}

static void snapshot(eq_state_t *st, eq_state_blob_t *out)
{
    memset(out, 0, sizeof(*out));
    out->version = STATE_VERSION;
    out->enabled = eq_processor_is_enabled(st->eq) ? 1 : 0;
    taskENTER_CRITICAL(&st->lock);
    memcpy(out->name, st->name, sizeof(out->name));
    taskEXIT_CRITICAL(&st->lock);
    for (int i = 0; i < EQ_BANDS; i++) {
        const eq_band_params_t *b = eq_processor_get_band(st->eq, i);
        if (!b) continue;
        out->bands[i * 3 + 0] = b->freq;
        out->bands[i * 3 + 1] = b->q;
        out->bands[i * 3 + 2] = b->gain_db;
    }
}

static esp_err_t write_blob(const eq_state_blob_t *blob)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(STATE_NAMESPACE, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_blob(h, STATE_KEY, blob, sizeof(*blob));
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

/* esp_timer task: save once the state differs from NVS and has not changed
 * since the previous check (i.e. the user stopped moving the slider). */
static void check_cb(void *arg)
{
    eq_state_t *st = (eq_state_t *)arg;
    eq_state_blob_t now;
    snapshot(st, &now);

    if (memcmp(&now, &st->saved, sizeof(now)) == 0) {
        st->havePending = false;
        return;
    }
    if (st->havePending && memcmp(&now, &st->pending, sizeof(now)) == 0) {
        esp_err_t ret = write_blob(&now);
        if (ret == ESP_OK) {
            st->saved = now;
            st->havePending = false;
            ESP_LOGI(TAG, "Saved EQ state (%s, preset '%s')",
                     now.enabled ? "on" : "off", now.name[0] ? now.name : "custom");
        } else {
            ESP_LOGE(TAG, "Save failed: %s", esp_err_to_name(ret));
        }
        return;
    }
    st->pending = now;
    st->havePending = true;
}

static bool blob_valid(const eq_state_blob_t *b)
{
    if (b->version != STATE_VERSION) return false;
    for (int i = 0; i < EQ_BANDS * 3; i++) {
        if (!isfinite(b->bands[i])) return false;
    }
    for (int i = 0; i < EQ_BANDS; i++) {
        if (b->bands[i * 3 + 0] <= 0.0f || b->bands[i * 3 + 1] <= 0.0f) return false;
    }
    return memchr(b->name, '\0', sizeof(b->name)) != NULL;
}

esp_err_t eq_state_restore_and_start(eq_state_t *st)
{
    if (!st) return ESP_ERR_INVALID_ARG;

    eq_state_blob_t blob;
    size_t size = sizeof(blob);
    nvs_handle_t h;
    esp_err_t ret = nvs_open(STATE_NAMESPACE, NVS_READONLY, &h);
    if (ret == ESP_OK) {
        ret = nvs_get_blob(h, STATE_KEY, &blob, &size);
        nvs_close(h);
    }

    if (ret == ESP_OK && size == sizeof(blob) && blob_valid(&blob)) {
        for (int i = 0; i < EQ_BANDS; i++) {
            eq_processor_set_band(st->eq, i, blob.bands[i * 3 + 0],
                                  blob.bands[i * 3 + 1], blob.bands[i * 3 + 2]);
        }
        eq_processor_set_enabled(st->eq, blob.enabled != 0);
        eq_state_set_name(st, blob.name);
        ESP_LOGI(TAG, "Restored EQ state (%s, preset '%s')",
                 blob.enabled ? "on" : "off", blob.name[0] ? blob.name : "custom");
    } else {
        eq_state_set_name(st, "Flat");
        ESP_LOGI(TAG, "No stored EQ state, using defaults");
    }
    /* Whatever is live now counts as saved; only later changes are written. */
    snapshot(st, &st->saved);

    const esp_timer_create_args_t args = {
        .callback = check_cb,
        .arg = st,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "eq_state",
    };
    ret = esp_timer_create(&args, &st->timer);
    if (ret == ESP_OK) ret = esp_timer_start_periodic(st->timer, CHECK_PERIOD_US);
    if (ret != ESP_OK) ESP_LOGE(TAG, "Timer start failed: %s", esp_err_to_name(ret));
    return ret;
}

void eq_state_get_name(eq_state_t *st, char *out, size_t outSize)
{
    if (!out || outSize == 0) return;
    out[0] = '\0';
    if (!st) return;
    taskENTER_CRITICAL(&st->lock);
    strncpy(out, st->name, outSize - 1);
    out[outSize - 1] = '\0';
    taskEXIT_CRITICAL(&st->lock);
}

void eq_state_set_name(eq_state_t *st, const char *name)
{
    if (!st) return;
    char tmp[EQ_STATE_NAME_MAX + 1] = {0};
    if (name) strncpy(tmp, name, EQ_STATE_NAME_MAX);
    taskENTER_CRITICAL(&st->lock);
    memcpy(st->name, tmp, sizeof(st->name));
    taskEXIT_CRITICAL(&st->lock);
}

bool eq_state_name_valid(const char *name)
{
    if (!name) return false;
    size_t len = strlen(name);
    if (len == 0 || len > EQ_STATE_NAME_MAX) return false;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                  || c == ' ' || c == '_' || c == '-' || c == '+' || c == '.';
        if (!ok) return false;
    }
    return true;
}

void eq_state_list_presets(char *out, size_t outSize)
{
    if (!out || outSize == 0) return;
    out[0] = '\0';
    size_t pos = 0;

    nvs_iterator_t it = NULL;
    esp_err_t ret = nvs_entry_find(NVS_DEFAULT_PART_NAME, PRESET_NAMESPACE, NVS_TYPE_BLOB, &it);
    while (ret == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        size_t prefixLen = strlen(PRESET_PREFIX);
        if (strncmp(info.key, PRESET_PREFIX, prefixLen) == 0) {
            const char *name = info.key + prefixLen;
            size_t need = strlen(name) + (pos > 0 ? 1 : 0);
            if (pos + need < outSize) {
                pos += snprintf(out + pos, outSize - pos, "%s%s", pos > 0 ? "," : "", name);
            }
        }
        ret = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
}

esp_err_t eq_state_delete_preset(const char *name)
{
    if (!eq_state_name_valid(name)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t ret = nvs_open(PRESET_NAMESPACE, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;
    char key[NVS_KEY_NAME_MAX_SIZE];
    snprintf(key, sizeof(key), PRESET_PREFIX "%s", name);
    ret = nvs_erase_key(h, key);
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}
