#include "eq_processor.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#define TAG "EQ"
#define NVS_NAMESPACE "eq_preset"

static const eq_band_params_t default_bands[EQ_BANDS] = {
    {60.0f,   1.0f, 0.0f},
    {250.0f,  1.0f, 0.0f},
    {1000.0f, 1.0f, 0.0f},
    {4000.0f, 1.0f, 0.0f},
    {12000.0f, 1.0f, 0.0f},
};

typedef struct {
    float b0, b1, b2, a1, a2;
} eq_coeff_t;

typedef struct {
    float x1, x2, y1, y2;
} eq_state_t;

typedef struct {
    eq_band_params_t params;
    eq_coeff_t coeff;
    eq_state_t state_l;
    eq_state_t state_r;
} eq_band_t;

struct eq_processor_t {
    eq_band_t bands[EQ_BANDS];
    bool enabled;
    uint32_t sample_rate;
};

static void calc_biquad_peaking(eq_coeff_t *c, float freq, float q, float gain_db, uint32_t sr)
{
    float w0 = 2.0f * (float)M_PI * freq / (float)sr;
    float cosw0 = cosf(w0);
    float sinw0 = sinf(w0);
    float A = powf(10.0f, gain_db / 40.0f);
    float alpha = sinw0 / (2.0f * q);

    float a0 = 1.0f + alpha / A;
    c->b0 = (1.0f + alpha * A) / a0;
    c->b1 = (-2.0f * cosw0) / a0;
    c->b2 = (1.0f - alpha * A) / a0;
    c->a1 = (-2.0f * cosw0) / a0;
    c->a2 = (1.0f - alpha / A) / a0;
}

static void update_band_coeff(eq_band_t *band, uint32_t sr)
{
    calc_biquad_peaking(&band->coeff, band->params.freq, band->params.q, band->params.gain_db, sr);
}

static void reset_state(eq_state_t *s)
{
    s->x1 = 0.0f;
    s->x2 = 0.0f;
    s->y1 = 0.0f;
    s->y2 = 0.0f;
}

static float biquad_process(eq_coeff_t *c, eq_state_t *s, float x)
{
    float y = c->b0 * x + c->b1 * s->x1 + c->b2 * s->x2
              - c->a1 * s->y1 - c->a2 * s->y2;
    s->x2 = s->x1;
    s->x1 = x;
    s->y2 = s->y1;
    s->y1 = y;
    return y;
}

eq_processor_t *eq_processor_create(void)
{
    eq_processor_t *eq = calloc(1, sizeof(eq_processor_t));
    if (!eq) return NULL;
    eq->enabled = true;
    eq->sample_rate = 44100;
    return eq;
}

void eq_processor_destroy(eq_processor_t *eq)
{
    if (eq) free(eq);
}

esp_err_t eq_processor_init(eq_processor_t *eq, uint32_t sample_rate)
{
    if (!eq) return ESP_ERR_INVALID_ARG;
    eq->sample_rate = sample_rate;
    eq->enabled = true;

    for (int i = 0; i < EQ_BANDS; i++) {
        eq->bands[i].params = default_bands[i];
        update_band_coeff(&eq->bands[i], sample_rate);
        reset_state(&eq->bands[i].state_l);
        reset_state(&eq->bands[i].state_r);
    }

    ESP_LOGI(TAG, "Init: %luHz, %d bands, enabled=%d", sample_rate, EQ_BANDS, eq->enabled);
    return ESP_OK;
}

void eq_processor_set_band(eq_processor_t *eq, int index, float freq, float q, float gain_db)
{
    if (!eq || index < 0 || index >= EQ_BANDS) return;
    eq->bands[index].params.freq = freq;
    eq->bands[index].params.q = q;
    eq->bands[index].params.gain_db = gain_db;
    update_band_coeff(&eq->bands[index], eq->sample_rate);
    reset_state(&eq->bands[index].state_l);
    reset_state(&eq->bands[index].state_r);
}

const eq_band_params_t *eq_processor_get_band(const eq_processor_t *eq, int index)
{
    if (!eq || index < 0 || index >= EQ_BANDS) return NULL;
    return &eq->bands[index].params;
}

void eq_processor_set_enabled(eq_processor_t *eq, bool enabled)
{
    if (eq) eq->enabled = enabled;
}

bool eq_processor_is_enabled(const eq_processor_t *eq)
{
    return eq ? eq->enabled : false;
}

void eq_processor_process_frame(eq_processor_t *eq, int16_t *left, int16_t *right)
{
    if (!eq || !eq->enabled) return;

    float in_l = (float)*left / 32768.0f;
    float in_r = (float)*right / 32768.0f;

    for (int i = 0; i < EQ_BANDS; i++) {
        eq_coeff_t *c = &eq->bands[i].coeff;
        in_l = biquad_process(c, &eq->bands[i].state_l, in_l);
        in_r = biquad_process(c, &eq->bands[i].state_r, in_r);
    }

    if (in_l > 1.0f) in_l = 1.0f;
    if (in_l < -1.0f) in_l = -1.0f;
    if (in_r > 1.0f) in_r = 1.0f;
    if (in_r < -1.0f) in_r = -1.0f;

    *left = (int16_t)(in_l * 32767.0f);
    *right = (int16_t)(in_r * 32767.0f);
}

void eq_processor_reset(eq_processor_t *eq)
{
    if (!eq) return;
    for (int i = 0; i < EQ_BANDS; i++) {
        reset_state(&eq->bands[i].state_l);
        reset_state(&eq->bands[i].state_r);
    }
}

esp_err_t eq_processor_save_preset(const eq_processor_t *eq, const char *name)
{
    if (!eq || !name) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(ret));
        return ret;
    }

    float blob[EQ_BANDS * 3];
    for (int i = 0; i < EQ_BANDS; i++) {
        blob[i * 3 + 0] = eq->bands[i].params.freq;
        blob[i * 3 + 1] = eq->bands[i].params.q;
        blob[i * 3 + 2] = eq->bands[i].params.gain_db;
    }

    char key[NVS_KEY_NAME_MAX_SIZE];
    snprintf(key, sizeof(key), "p_%s", name);
    ret = nvs_set_blob(handle, key, blob, sizeof(blob));
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
        ESP_LOGI(TAG, "Preset '%s' saved", name);
    } else {
        ESP_LOGE(TAG, "Preset save failed: %s", esp_err_to_name(ret));
    }

    nvs_close(handle);
    return ret;
}

esp_err_t eq_processor_load_preset(eq_processor_t *eq, const char *name)
{
    if (!eq || !name) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(ret));
        return ret;
    }

    char key[NVS_KEY_NAME_MAX_SIZE];
    snprintf(key, sizeof(key), "p_%s", name);

    float blob[EQ_BANDS * 3];
    size_t required_size = sizeof(blob);
    ret = nvs_get_blob(handle, key, blob, &required_size);
    nvs_close(handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Preset '%s' not found: %s", name, esp_err_to_name(ret));
        return ret;
    }

    for (int i = 0; i < EQ_BANDS; i++) {
        eq->bands[i].params.freq = blob[i * 3 + 0];
        eq->bands[i].params.q = blob[i * 3 + 1];
        eq->bands[i].params.gain_db = blob[i * 3 + 2];
        update_band_coeff(&eq->bands[i], eq->sample_rate);
        reset_state(&eq->bands[i].state_l);
        reset_state(&eq->bands[i].state_r);
    }

    ESP_LOGI(TAG, "Preset '%s' loaded", name);
    return ESP_OK;
}
