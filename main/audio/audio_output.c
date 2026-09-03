#include "audio_output.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "esp_log.h"
#include "eq_processor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "driver/i2s_std.h"
#include "driver/i2s_common.h"

#define TAG "AUDIO_OUT"

/* Dedicated audio pipeline (fixes 1.1 + 1.2):
 * BT callbacks only enqueue raw frames; DSP + I2S write happen here. */
#define AUDIO_RINGBUF_BYTES (16 * 1024)
#define AUDIO_SCRATCH_BYTES 4096
#define AUDIO_TASK_STACK 4096
#define AUDIO_TASK_PRIO 10
#define AUDIO_I2S_WRITE_TIMEOUT_MS 50

struct audio_output_t {
i2s_chan_handle_t tx_handle;
i2s_chan_handle_t rx_handle;
SemaphoreHandle_t mutex;
RingbufHandle_t a2dp_ring;
TaskHandle_t audio_task;
int16_t *a2dp_scratch;
size_t scratch_bytes;
volatile bool task_running;
_Atomic uint32_t processed_count;
_Atomic uint32_t drop_count;
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
        .mclk = ORANGEBUS_I2S_MCLK,
        .bclk = ORANGEBUS_I2S_BCK,
        .ws   = ORANGEBUS_I2S_WS,
        .dout = ORANGEBUS_I2S_DATA,
        .din  = ORANGEBUS_I2S_MIC_DATA,
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
    ao->scratch_bytes = AUDIO_SCRATCH_BYTES;
    ao->a2dp_scratch = calloc(1, AUDIO_SCRATCH_BYTES);
    if (!ao->a2dp_scratch) {
        if (ao->mutex) vSemaphoreDelete(ao->mutex);
        free(ao);
        return NULL;
    }
    return ao;
}

void audio_output_destroy(audio_output_t *ao)
{
    if (!ao) return;
    ao->task_running = false;
    if (ao->audio_task) {
        vTaskDelete(ao->audio_task);
        ao->audio_task = NULL;
    }
    if (ao->a2dp_ring) {
        vRingbufferDelete(ao->a2dp_ring);
        ao->a2dp_ring = NULL;
    }
    if (ao->tx_handle) {
        i2s_channel_disable(ao->tx_handle);
        i2s_del_channel(ao->tx_handle);
    }
    if (ao->rx_handle) {
        i2s_channel_disable(ao->rx_handle);
        i2s_del_channel(ao->rx_handle);
    }
    if (ao->a2dp_scratch) free(ao->a2dp_scratch);
    if (ao->mutex) vSemaphoreDelete(ao->mutex);
    free(ao);
}

/* Dedicated audio task: owns EQ/volume DSP + blocking I2S write.
 * Never runs in BT stack context, so BTC stalls/WDT resets are avoided. */
static void audio_out_task(void *arg)
{
    audio_output_t *ao = (audio_output_t *)arg;
    while (ao->task_running) {
        size_t item_len = 0;
        uint8_t *item = (uint8_t *)xRingbufferReceive(ao->a2dp_ring, &item_len,
                                                      pdMS_TO_TICKS(100));
        if (!item) continue;
        uint32_t frame_len = (uint32_t)item_len;
        if (frame_len == 0 || frame_len > ao->scratch_bytes) {
            vRingbufferReturnItem(ao->a2dp_ring, item);
            atomic_fetch_add(&ao->drop_count, 1);
            continue;
        }
        memcpy(ao->a2dp_scratch, item, frame_len);
        vRingbufferReturnItem(ao->a2dp_ring, item);

        /* Snapshot config under the mutex, then release it for DSP:
         * holding the mutex across EQ + write would stall BT event callbacks
         * in i2s_configure (portMAX_DELAY) during mode switches. */
        eq_processor_t *eq = NULL;
        uint8_t volume = 100;
        if (ao->mutex) xSemaphoreTake(ao->mutex, portMAX_DELAY);
        /* Re-check mode under the lock: I2S may have switched to SCO rates
         * after the frame was enqueued (CODE_REVIEW 1.7). */
        bool ready = ao->initialized && ao->tx_handle != NULL && !ao->muted
            && ao->is_a2dp_mode;
        if (ready) {
            eq = ao->eq;
            volume = ao->volume;
        }
        if (ao->mutex) xSemaphoreGive(ao->mutex);
        if (!ready) continue;

        int16_t *samples = ao->a2dp_scratch;
        uint32_t sample_count = frame_len / 2;

        if (eq && eq_processor_is_enabled(eq)) {
            for (uint32_t i = 0; i + 1 < sample_count; i += 2) {
                eq_processor_process_frame(eq, &samples[i], &samples[i + 1]);
            }
        }

        if (volume < 100) {
            float scale = volume / 100.0f;
            for (uint32_t i = 0; i < sample_count; i++) {
                samples[i] = (int16_t)(samples[i] * scale);
            }
        }

        /* Re-take only around the bounded I2S write so reconfiguration in
         * i2s_configure waits at most one write timeout, not the DSP cost. */
        size_t written = 0;
        esp_err_t ret;
        if (ao->mutex) xSemaphoreTake(ao->mutex, portMAX_DELAY);
        if (ao->initialized && ao->tx_handle != NULL) {
            ret = i2s_channel_write(ao->tx_handle, samples, frame_len,
                                    &written, pdMS_TO_TICKS(AUDIO_I2S_WRITE_TIMEOUT_MS));
        } else {
            ret = ESP_ERR_INVALID_STATE;
        }
        if (ao->mutex) xSemaphoreGive(ao->mutex);
        atomic_fetch_add(&ao->processed_count, 1);
        uint32_t done = atomic_load(&ao->processed_count);
        if (done % 500 == 0) {
            ESP_LOGD(TAG, "A2DP frames processed=%lu dropped=%lu last_len=%lu",
                     done, atomic_load(&ao->drop_count), frame_len);
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S write failed: %s (written=%u/%lu)",
                     esp_err_to_name(ret), written, frame_len);
        }
    }
    vTaskDelete(NULL);
}

static bool audio_pipeline_start(audio_output_t *ao)
{
    if (!ao || ao->audio_task) return true;
    if (!ao->a2dp_ring) {
        ao->a2dp_ring = xRingbufferCreate(AUDIO_RINGBUF_BYTES, RINGBUF_TYPE_BYTEBUF);
        if (!ao->a2dp_ring) {
            ESP_LOGE(TAG, "A2DP ringbuffer create failed");
            return false;
        }
    }
    ao->task_running = true;
    BaseType_t ok = xTaskCreate(audio_out_task, "audio_out", AUDIO_TASK_STACK,
                                ao, AUDIO_TASK_PRIO, &ao->audio_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "audio_out task create failed");
        ao->task_running = false;
        ao->audio_task = NULL;
        return false;
    }
    return true;
}

esp_err_t audio_output_init(audio_output_t *ao, uint32_t rate)
{
    if (!ao) return ESP_ERR_INVALID_ARG;
    if (!i2s_configure(ao, rate)) return ESP_FAIL;
    if (!audio_pipeline_start(ao)) return ESP_FAIL;
    return ESP_OK;
}

void audio_output_set_eq(audio_output_t *ao, eq_processor_t *eq)
{
    if (ao) ao->eq = eq;
}

void audio_output_switch_a2dp(audio_output_t *ao)
{
    if (!ao) return;
    if (!ao->is_a2dp_mode || !ao->initialized || ao->rate != 44100) {
        i2s_configure(ao, 44100);
    }
}

void audio_output_switch_sco(audio_output_t *ao, bool msbc)
{
    if (!ao) return;
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

uint32_t audio_output_get_drop_count(const audio_output_t *ao)
{
    return ao ? atomic_load(&ao->drop_count) : 0;
}

uint32_t audio_output_get_processed_count(const audio_output_t *ao)
{
    return ao ? atomic_load(&ao->processed_count) : 0;
}

/* A2DP data callback (Bluedroid BTC thread context).
 * Must never modify `data` in place nor block: copy the frame into the
 * ringbuffer and return immediately. DSP + I2S write happen in audio_out_task. */
void audio_output_a2dp_data_cb(audio_output_t *ao, const uint8_t *data, uint32_t len)
{
    if (!ao || !data || len == 0) return;
    if (!ao->a2dp_ring || !ao->task_running) return;
    if (ao->muted) return;
    /* Drop 44.1 kHz A2DP frames while I2S runs at 8/16 kHz SCO rates;
     * otherwise call audio is severely distorted (CODE_REVIEW 1.7). */
    if (!ao->is_a2dp_mode) return;
    if (len > ao->scratch_bytes) {
        atomic_fetch_add(&ao->drop_count, 1);
        return;
    }
    /* Zero-block send: never stall the BT stack; drop + count on overflow. */
    if (xRingbufferSend(ao->a2dp_ring, data, len, 0) != pdTRUE) {
        atomic_fetch_add(&ao->drop_count, 1);
    }
}

void audio_output_hfp_recv_cb(audio_output_t *ao, const uint8_t *data, uint32_t len)
{
    if (!ao || !data || len == 0) return;
    if (ao->mutex && !xSemaphoreTake(ao->mutex, pdMS_TO_TICKS(10))) return;
    /* Mode gate under the mutex so a concurrent switch cannot slip an SCO
     * frame into an A2DP-configured channel (CODE_REVIEW 1.7). */
    if (ao->muted || !ao->initialized || ao->tx_handle == NULL || ao->is_a2dp_mode) {
        if (ao->mutex) xSemaphoreGive(ao->mutex);
        return;
    }

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
