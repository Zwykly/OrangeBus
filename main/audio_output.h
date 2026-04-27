#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "bluebus.h"

audio_output_t *audio_output_create(void);
void audio_output_destroy(audio_output_t *ao);

esp_err_t audio_output_init(audio_output_t *ao, uint32_t rate);
void audio_output_set_eq(audio_output_t *ao, eq_processor_t *eq);

void audio_output_switch_a2dp(audio_output_t *ao);
void audio_output_switch_sco(audio_output_t *ao, bool msbc);

void audio_output_set_volume(audio_output_t *ao, uint8_t vol);
void audio_output_adjust_volume(audio_output_t *ao, int8_t delta);
void audio_output_set_mute(audio_output_t *ao, bool mute);
void audio_output_toggle_mute(audio_output_t *ao);

uint8_t audio_output_get_volume(const audio_output_t *ao);
bool audio_output_is_muted(const audio_output_t *ao);
bool audio_output_is_a2dp_mode(const audio_output_t *ao);

void audio_output_a2dp_data_cb(audio_output_t *ao, const uint8_t *data, uint32_t len);
void audio_output_hfp_recv_cb(audio_output_t *ao, const uint8_t *data, uint32_t len);
uint32_t audio_output_hfp_send_cb(audio_output_t *ao, uint8_t *data, uint32_t len);

#endif
