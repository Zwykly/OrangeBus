#ifndef EQ_PROCESSOR_H
#define EQ_PROCESSOR_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define EQ_BANDS 5

typedef struct {
    float freq;
    float q;
    float gain_db;
} eq_band_params_t;

typedef struct eq_processor_t eq_processor_t;

eq_processor_t *eq_processor_create(void);
void eq_processor_destroy(eq_processor_t *eq);

esp_err_t eq_processor_init(eq_processor_t *eq, uint32_t sample_rate);

void eq_processor_set_band(eq_processor_t *eq, int index, float freq, float q, float gain_db);
const eq_band_params_t *eq_processor_get_band(const eq_processor_t *eq, int index);

void eq_processor_set_enabled(eq_processor_t *eq, bool enabled);
bool eq_processor_is_enabled(const eq_processor_t *eq);

void eq_processor_process_frame(eq_processor_t *eq, int16_t *left, int16_t *right);
void eq_processor_reset(eq_processor_t *eq);

esp_err_t eq_processor_save_preset(const eq_processor_t *eq, const char *name);
esp_err_t eq_processor_load_preset(eq_processor_t *eq, const char *name);

#endif
