#include "eq_processor.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"
#include "nvs.h"

#define TAG "EQ"
#define NVS_NAMESPACE "eq_preset"

/* Parameter smoothing: live filter parameters glide towards the target in
 * sub-blocks of EQ_SUBBLOCK_FRAMES, moving EQ_SMOOTH_COEF of the remaining
 * distance each step (~10 ms time constant at 44.1 kHz). Coefficients are
 * recomputed per sub-block only while a band is still moving. */
#define EQ_SUBBLOCK_FRAMES   32
#define EQ_SMOOTH_COEF       0.07f
#define EQ_EPS_GAIN_DB       0.01f
#define EQ_EPS_LOG_FREQ      0.0005f
#define EQ_EPS_Q             0.001f

/* After gliding back to flat, keep filtering this long so the filter
 * history settles before bypassing (avoids a step at the switch-over). */
#define EQ_BYPASS_HOLD_FRAMES 4410

#define EQ_FREQ_MIN   20.0f
#define EQ_FREQ_MAX_RATIO 0.45f   /* of sample rate, stay clear of Nyquist */
#define EQ_Q_MIN      0.1f
#define EQ_Q_MAX      10.0f
#define EQ_GAIN_MAX_DB 24.0f

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

/* Live (audio-task owned) band: current smoothed parameters + filter. */
typedef struct {
    float log_freq;
    float q;
    float gain_db;
    eq_coeff_t coeff;
    eq_state_t state_l;
    eq_state_t state_r;
} eq_band_t;

typedef struct {
    float log_freq;
    float q;
    float gain_db;
} eq_dest_t;

struct eq_processor_t {
    /* Control side: written by SPP/CLI under `lock`. */
    portMUX_TYPE lock;
    eq_band_params_t target[EQ_BANDS];
    bool enabled;
    bool flat;
    volatile uint32_t target_gen;

    /* Audio side: touched only by the audio task. */
    eq_band_t bands[EQ_BANDS];
    eq_dest_t dest[EQ_BANDS];
    uint32_t seen_gen;
    bool dest_flat;
    bool active;
    bool ramping;
    uint32_t hold_frames;

    uint32_t sample_rate;
};

/* Peaking EQ biquad coefficients (RBJ Audio EQ Cookbook). */
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
    calc_biquad_peaking(&band->coeff, expf(band->log_freq), band->q, band->gain_db, sr);
}

static bool gain_is_flat(float g)
{
    return g <= 0.001f && g >= -0.001f;
}

static bool params_flat(const eq_band_params_t *p)
{
    for (int i = 0; i < EQ_BANDS; i++) {
        if (!gain_is_flat(p[i].gain_db)) return false;
    }
    return true;
}

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Reject NaN/inf and clamp to a range where the biquad stays well-behaved
 * (e.g. freq above Nyquist would produce a garbage, unstable filter). */
static bool sanitize_params(eq_band_params_t *p, uint32_t sr)
{
    if (!isfinite(p->freq) || !isfinite(p->q) || !isfinite(p->gain_db)) return false;
    p->freq = clampf(p->freq, EQ_FREQ_MIN, EQ_FREQ_MAX_RATIO * (float)sr);
    p->q = clampf(p->q, EQ_Q_MIN, EQ_Q_MAX);
    p->gain_db = clampf(p->gain_db, -EQ_GAIN_MAX_DB, EQ_GAIN_MAX_DB);
    return true;
}

static void reset_state(eq_state_t *s)
{
    s->x1 = 0.0f;
    s->x2 = 0.0f;
    s->y1 = 0.0f;
    s->y2 = 0.0f;
}

static inline float biquad_process(const eq_coeff_t *c, eq_state_t *s, float x)
{
    float y = c->b0 * x + c->b1 * s->x1 + c->b2 * s->x2
              - c->a1 * s->y1 - c->a2 * s->y2;
    s->x2 = s->x1;
    s->x1 = x;
    s->y2 = s->y1;
    s->y1 = y;
    return y;
}

/* Publish a control-side change to the audio task. Caller holds the lock. */
static void publish_locked(eq_processor_t *eq)
{
    eq->flat = params_flat(eq->target);
    eq->target_gen++;
}

eq_processor_t *eq_processor_create(void)
{
    eq_processor_t *eq = calloc(1, sizeof(eq_processor_t));
    if (!eq) return NULL;
    portMUX_INITIALIZE(&eq->lock);
    eq->enabled = true;
    eq->flat = true;
    eq->sample_rate = 44100;
    memcpy(eq->target, default_bands, sizeof(eq->target));
    return eq;
}

void eq_processor_destroy(eq_processor_t *eq)
{
    if (eq) free(eq);
}

esp_err_t eq_processor_init(eq_processor_t *eq, uint32_t sample_rate)
{
    if (!eq) return ESP_ERR_INVALID_ARG;

    taskENTER_CRITICAL(&eq->lock);
    eq->sample_rate = sample_rate;
    eq->enabled = true;
    memcpy(eq->target, default_bands, sizeof(eq->target));
    publish_locked(eq);
    taskEXIT_CRITICAL(&eq->lock);

    /* Live side starts bypassed with clean filters; the first processed
     * block picks up the target and glides in from flat. */
    eq->active = false;
    eq->ramping = false;
    eq->seen_gen = eq->target_gen - 1;
    eq_processor_reset(eq);

    ESP_LOGI(TAG, "Init: %luHz, %d bands, enabled=%d", sample_rate, EQ_BANDS, eq->enabled);
    return ESP_OK;
}

void eq_processor_set_band(eq_processor_t *eq, int index, float freq, float q, float gain_db)
{
    if (!eq || index < 0 || index >= EQ_BANDS) return;
    eq_band_params_t p = { .freq = freq, .q = q, .gain_db = gain_db };
    if (!sanitize_params(&p, eq->sample_rate)) {
        ESP_LOGW(TAG, "Band %d: invalid parameters ignored", index);
        return;
    }
    taskENTER_CRITICAL(&eq->lock);
    eq->target[index] = p;
    publish_locked(eq);
    taskEXIT_CRITICAL(&eq->lock);
}

const eq_band_params_t *eq_processor_get_band(const eq_processor_t *eq, int index)
{
    if (!eq || index < 0 || index >= EQ_BANDS) return NULL;
    return &eq->target[index];
}

void eq_processor_set_enabled(eq_processor_t *eq, bool enabled)
{
    if (!eq) return;
    taskENTER_CRITICAL(&eq->lock);
    eq->enabled = enabled;
    publish_locked(eq);
    taskEXIT_CRITICAL(&eq->lock);
}

bool eq_processor_is_enabled(const eq_processor_t *eq)
{
    return eq ? eq->enabled : false;
}

bool eq_processor_is_flat(const eq_processor_t *eq)
{
    return eq ? eq->flat : true;
}

/* Audio task: take a snapshot of the latest target if it changed. */
static void sync_targets(eq_processor_t *eq)
{
    if (eq->seen_gen == eq->target_gen) return;

    eq_band_params_t snap[EQ_BANDS];
    bool enabled;
    taskENTER_CRITICAL(&eq->lock);
    memcpy(snap, eq->target, sizeof(snap));
    enabled = eq->enabled;
    eq->seen_gen = eq->target_gen;
    taskEXIT_CRITICAL(&eq->lock);

    /* Disabled == glide every band to 0 dB, then bypass. */
    bool flat = true;
    for (int i = 0; i < EQ_BANDS; i++) {
        eq->dest[i].log_freq = logf(snap[i].freq);
        eq->dest[i].q = snap[i].q;
        eq->dest[i].gain_db = enabled ? snap[i].gain_db : 0.0f;
        if (!gain_is_flat(eq->dest[i].gain_db)) flat = false;
    }
    eq->dest_flat = flat;

    if (!eq->active) {
        if (flat) return;
        /* Enter from bypass: start at 0 dB with zeroed history. A 0 dB
         * peaking biquad with zero state is an exact identity, so the
         * switch-over itself is seamless; the gain then glides in. */
        for (int i = 0; i < EQ_BANDS; i++) {
            eq_band_t *b = &eq->bands[i];
            b->log_freq = eq->dest[i].log_freq;
            b->q = eq->dest[i].q;
            b->gain_db = 0.0f;
            update_band_coeff(b, eq->sample_rate);
            reset_state(&b->state_l);
            reset_state(&b->state_r);
        }
        eq->active = true;
    }
    eq->ramping = true;
    eq->hold_frames = 0;
}

static bool approach(float *cur, float target, float eps)
{
    float d = target - *cur;
    if (d <= eps && d >= -eps) {
        if (*cur == target) return false;
        *cur = target;
        return true;
    }
    *cur += d * EQ_SMOOTH_COEF;
    return true;
}

/* One smoothing step; returns false once every band has reached its target. */
static bool ramp_step(eq_processor_t *eq)
{
    bool moving = false;
    for (int i = 0; i < EQ_BANDS; i++) {
        eq_band_t *b = &eq->bands[i];
        const eq_dest_t *d = &eq->dest[i];
        bool changed = false;
        changed |= approach(&b->gain_db, d->gain_db, EQ_EPS_GAIN_DB);
        changed |= approach(&b->log_freq, d->log_freq, EQ_EPS_LOG_FREQ);
        changed |= approach(&b->q, d->q, EQ_EPS_Q);
        if (changed) {
            update_band_coeff(b, eq->sample_rate);
            moving = true;
        }
    }
    return moving;
}

static void run_filters(eq_processor_t *eq, int16_t *samples, uint32_t frames)
{
    for (uint32_t n = 0; n < frames; n++) {
        float l = (float)samples[2 * n] / 32768.0f;
        float r = (float)samples[2 * n + 1] / 32768.0f;

        for (int i = 0; i < EQ_BANDS; i++) {
            eq_band_t *b = &eq->bands[i];
            l = biquad_process(&b->coeff, &b->state_l, l);
            r = biquad_process(&b->coeff, &b->state_r, r);
        }

        l = clampf(l, -1.0f, 1.0f);
        r = clampf(r, -1.0f, 1.0f);
        samples[2 * n] = (int16_t)(l * 32767.0f);
        samples[2 * n + 1] = (int16_t)(r * 32767.0f);
    }
}

void eq_processor_process(eq_processor_t *eq, int16_t *samples, uint32_t frames)
{
    if (!eq || !samples || frames == 0) return;

    sync_targets(eq);
    if (!eq->active) return;

    uint32_t pos = 0;
    while (pos < frames) {
        uint32_t n = frames - pos;
        if (n > EQ_SUBBLOCK_FRAMES) n = EQ_SUBBLOCK_FRAMES;

        if (eq->ramping) eq->ramping = ramp_step(eq);
        run_filters(eq, samples + 2 * pos, n);
        pos += n;

        if (!eq->ramping && eq->dest_flat) {
            eq->hold_frames += n;
            if (eq->hold_frames >= EQ_BYPASS_HOLD_FRAMES) {
                /* Every band is at exactly 0 dB and the history has
                 * settled: output == input, safe to bypass. */
                eq->active = false;
                eq_processor_reset(eq);
                return;
            }
        }
    }
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

    float blob[EQ_BANDS * 3];
    eq_processor_t *mut = (eq_processor_t *)eq;   /* lock only */
    taskENTER_CRITICAL(&mut->lock);
    for (int i = 0; i < EQ_BANDS; i++) {
        blob[i * 3 + 0] = eq->target[i].freq;
        blob[i * 3 + 1] = eq->target[i].q;
        blob[i * 3 + 2] = eq->target[i].gain_db;
    }
    taskEXIT_CRITICAL(&mut->lock);

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(ret));
        return ret;
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

    eq_band_params_t loaded[EQ_BANDS];
    for (int i = 0; i < EQ_BANDS; i++) {
        loaded[i].freq = blob[i * 3 + 0];
        loaded[i].q = blob[i * 3 + 1];
        loaded[i].gain_db = blob[i * 3 + 2];
        if (!sanitize_params(&loaded[i], eq->sample_rate)) {
            ESP_LOGE(TAG, "Preset '%s' has invalid band %d", name, i);
            return ESP_ERR_INVALID_STATE;
        }
    }

    taskENTER_CRITICAL(&eq->lock);
    memcpy(eq->target, loaded, sizeof(eq->target));
    publish_locked(eq);
    taskEXIT_CRITICAL(&eq->lock);

    ESP_LOGI(TAG, "Preset '%s' loaded", name);
    return ESP_OK;
}
