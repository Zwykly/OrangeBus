#include "audio_output.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "eq_processor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "driver/i2s_common.h"

#define TAG "AUDIO_OUT"

struct audio_output_t {
i2s_chan_handle_t tx_handle;
i2s_chan_handle_t rx_handle;
SemaphoreHandle_t mutex;
uint32_t rate;
bool is_a2dp_mode;
bool initialized;
uint8_t volume;
bool muted;
int16_t sco_stereo_buf[480];
eq_processor_t *eq;
};

static bool i2s_configure(audio_output_t *ao, uint32_t rate)
{
    if (ao->mutex) xSemaphoreTake(ao->mutex, portMAX_DELAY);

    if (ao->tx_handle != NULL) {
        i2s_channel_disable(ao->tx_handle);
        i2s_del_channel(ao->tx_handle);
        ao->tx_handle = NULL;
    }
    if (ao->rx_handle != NULL) {
        i2s_channel_disable(ao->rx_handle);
        i2s_del_channel(ao->rx_handle);
        ao->rx_handle = NULL;
    }
    ao->initialized = false;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear_after_cb = true;
    i2s_chan_handle_t *rx_handle_ptr = (rate <= 16000) ? &ao->rx_handle : NULL;
    esp_err_t ret = i2s_new_channel(&chan_cfg, &ao->tx_handle, rx_handle_ptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S new channel failed: %s", esp_err_to_name(ret));
        if (ao->mutex) xSemaphoreGive(ao->mutex);
        return false;
    }

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    i2s_std_gpio_config_t gpio_cfg = {
        .mclk = BLUEBUS_I2S_MCLK,
        .bclk = BLUEBUS_I2S_BCK,
        .ws   = BLUEBUS_I2S_WS,
        .dout = BLUEBUS_I2S_DATA,
        .din  = BLUEBUS_I2S_MIC_DATA,
        .invert_flags = {0},
    };

    i2s_std_config_t std_cfg = {
        .clk_cfg  = clk_cfg,
        .slot_cfg = slot_cfg,
        .gpio_cfg = gpio_cfg,
    };

    ret = i2s_channel_init_std_mode(ao->tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S init std mode failed: %s", esp_err_to_name(ret));
        i2s_del_channel(ao->tx_handle);
        ao->tx_handle = NULL;
        if (ao->mutex) xSemaphoreGive(ao->mutex);
        return false;
    }

    ret = i2s_channel_enable(ao->tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S TX enable failed: %s", esp_err_to_name(ret));
        i2s_del_channel(ao->tx_handle);
        if (ao->rx_handle) {
            i2s_del_channel(ao->rx_handle);
            ao->rx_handle = NULL;
        }
        ao->tx_handle = NULL;
        if (ao->mutex) xSemaphoreGive(ao->mutex);
        return false;
    }

    if (ao->rx_handle != NULL && rate <= 16000) {
        ret = i2s_channel_init_std_mode(ao->rx_handle, &std_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S RX init failed: %s", esp_err_to_name(ret));
            i2s_channel_disable(ao->tx_handle);
            i2s_del_channel(ao->tx_handle);
            i2s_del_channel(ao->rx_handle);
            ao->tx_handle = NULL;
            ao->rx_handle = NULL;
            if (ao->mutex) xSemaphoreGive(ao->mutex);
            return false;
        }
        ret = i2s_channel_enable(ao->rx_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S RX enable failed: %s", esp_err_to_name(ret));
            i2s_channel_disable(ao->tx_handle);
            i2s_del_channel(ao->tx_handle);
            i2s_channel_disable(ao->rx_handle);
            i2s_del_channel(ao->rx_handle);
            ao->tx_handle = NULL;
            ao->rx_handle = NULL;
            if (ao->mutex) xSemaphoreGive(ao->mutex);
            return false;
        }
    }

    ao->is_a2dp_mode = (rate > 16000);
    ao->rate = rate;
    ao->initialized = true;
    ESP_LOGI(TAG, "I2S configured: %luHz %s", rate, (ao->rx_handle != NULL) ? "stereo (TX+RX)" : "stereo (TX only)");

    if (ao->mutex) xSemaphoreGive(ao->mutex);
    return true;
}

audio_output_t *audio_output_create(void)
{
    audio_output_t *ao = calloc(1, sizeof(audio_output_t));
    if (!ao) return NULL;
    ao->mutex = xSemaphoreCreateMutex();
    ao->volume = 70;
    ao->is_a2dp_mode = true;
    return ao;
}

void audio_output_destroy(audio_output_t *ao)
{
    if (!ao) return;
    if (ao->tx_handle) {
        i2s_channel_disable(ao->tx_handle);
        i2s_del_channel(ao->tx_handle);
    }
    if (ao->rx_handle) {
        i2s_channel_disable(ao->rx_handle);
        i2s_del_channel(ao->rx_handle);
    }
    if (ao->mutex) vSemaphoreDelete(ao->mutex);
    free(ao);
}

esp_err_t audio_output_init(audio_output_t *ao, uint32_t rate)
{
if (!ao) return ESP_ERR_INVALID_ARG;
if (!i2s_configure(ao, rate)) return ESP_FAIL;
return ESP_OK;
}

void audio_output_set_eq(audio_output_t *ao, eq_processor_t *eq)
{
if (ao) ao->eq = eq;
}

void audio_output_switch_a2dp(audio_output_t *ao)
{
    if (!ao->is_a2dp_mode || !ao->initialized || ao->rate != 44100) {
        i2s_configure(ao, 44100);
    }
}

void audio_output_switch_sco(audio_output_t *ao, bool msbc)
{
    uint32_t rate = msbc ? 16000 : 8000;
    if (!ao->is_a2dp_mode && ao->initialized && ao->rate == rate) return;
    i2s_configure(ao, rate);
}

void audio_output_set_volume(audio_output_t *ao, uint8_t vol)
{
    if (!ao) return;
    if (vol > 100) vol = 100;
    ao->volume = vol;
}

void audio_output_adjust_volume(audio_output_t *ao, int8_t delta)
{
    if (!ao) return;
    int16_t v = (int16_t)ao->volume + delta;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    ao->volume = (uint8_t)v;
}

void audio_output_set_mute(audio_output_t *ao, bool mute)
{
    if (ao) ao->muted = mute;
}

void audio_output_toggle_mute(audio_output_t *ao)
{
    if (ao) ao->muted = !ao->muted;
}

uint8_t audio_output_get_volume(const audio_output_t *ao)
{
    return ao ? ao->volume : 0;
}

bool audio_output_is_muted(const audio_output_t *ao)
{
    return ao ? ao->muted : true;
}

bool audio_output_is_a2dp_mode(const audio_output_t *ao)
{
    return ao ? ao->is_a2dp_mode : true;
}

void audio_output_a2dp_data_cb(audio_output_t *ao, const uint8_t *data, uint32_t len)
{
if (!ao || ao->muted || !ao->initialized || ao->tx_handle == NULL) return;
if (ao->mutex && !xSemaphoreTake(ao->mutex, pdMS_TO_TICKS(10))) return;

int16_t *samples = (int16_t *)data;
uint32_t sample_count = len / 2;

if (ao->eq && eq_processor_is_enabled(ao->eq)) {
    for (uint32_t i = 0; i < sample_count; i += 2) {
        eq_processor_process_frame(ao->eq, &samples[i], &samples[i + 1]);
    }
}

if (ao->volume < 100) {
    float scale = ao->volume / 100.0f;
    for (uint32_t i = 0; i < sample_count; i++) {
        samples[i] = (int16_t)(samples[i] * scale);
    }
}

i2s_channel_write(ao->tx_handle, data, len, NULL, portMAX_DELAY);

if (ao->mutex) xSemaphoreGive(ao->mutex);
}

void audio_output_hfp_recv_cb(audio_output_t *ao, const uint8_t *data, uint32_t len)
{
    if (!ao || ao->muted || !ao->initialized || ao->tx_handle == NULL) return;
    if (ao->mutex && !xSemaphoreTake(ao->mutex, pdMS_TO_TICKS(10))) return;

    uint32_t mono_samples = len / 2;
    if (mono_samples > 240) mono_samples = 240;
    uint32_t stereo_len = mono_samples * 4;

    const int16_t *mono = (const int16_t *)data;
    float scale = ao->volume / 100.0f;
    for (uint32_t i = 0; i < mono_samples; i++) {
        int16_t scaled = (int16_t)(mono[i] * scale);
        ao->sco_stereo_buf[i * 2]     = scaled;
        ao->sco_stereo_buf[i * 2 + 1] = scaled;
    }
    i2s_channel_write(ao->tx_handle, ao->sco_stereo_buf, stereo_len, NULL, portMAX_DELAY);

    if (ao->mutex) xSemaphoreGive(ao->mutex);
}

uint32_t audio_output_hfp_send_cb(audio_output_t *ao, uint8_t *data, uint32_t len)
{
    if (!data || len == 0) return 0;
    if (!ao || !ao->initialized || ao->rx_handle == NULL) {
        memset(data, 0, len);
        return len;
    }

    uint32_t mono_samples = len / 2;
    if (mono_samples == 0) {
        memset(data, 0, len);
        return len;
    }

    size_t stereo_bytes = mono_samples * 4;
    if (stereo_bytes > 960) {
        stereo_bytes = 960;
        mono_samples = 240;
    }

    int16_t stereo_buf[240 * 2];
    size_t bytes_read = 0;
    esp_err_t ret = i2s_channel_read(ao->rx_handle, stereo_buf, stereo_bytes, &bytes_read, pdMS_TO_TICKS(20));
    if (ret != ESP_OK || bytes_read < stereo_bytes) {
        memset(data, 0, len);
        return len;
    }

    int16_t *mono = (int16_t *)data;
    for (uint32_t i = 0; i < mono_samples; i++) {
        mono[i] = stereo_buf[i * 2];
    }

    return len;
}
