#include "cdc.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ibus.h"
#include "ibus_config.h"

#define TAG "CDC"
#define CDC_STATUS_INTERVAL 2000
#define CDC_POLL_TIMEOUT 20000

struct cdc_t {
    ibus_t *ibus;
    ibus_config_t *config;
    bool playing;
    bool ignitionOn;
    uint32_t lastStatusTime;
    uint32_t lastPollTime;
};

static void cdc_send_status(cdc_t *cdc)
{
    uint8_t status = cdc->playing ? BLUEBUS_IBUS_CDC_STAT_PLAYING : BLUEBUS_IBUS_CDC_STAT_STOP;
    uint8_t function = cdc->playing ? BLUEBUS_IBUS_CDC_FUNC_PLAYING : BLUEBUS_IBUS_CDC_FUNC_NOT_PLAY;
    ibus_send_cdc_status(cdc->ibus, status, function);
    cdc->lastStatusTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
}

cdc_t *cdc_create(ibus_t *ibus, ibus_config_t *config)
{
    cdc_t *cdc = calloc(1, sizeof(cdc_t));
    if (!cdc) return NULL;
    cdc->ibus = ibus;
    cdc->config = config;
    return cdc;
}

void cdc_destroy(cdc_t *cdc)
{
    free(cdc);
}

esp_err_t cdc_init(cdc_t *cdc)
{
    if (!cdc) return ESP_ERR_INVALID_ARG;
    cdc->playing = false;
    cdc->ignitionOn = false;
    cdc->lastStatusTime = 0;
    cdc->lastPollTime = 0;
    ESP_LOGI(TAG, "CDC emulator initialized");
    return ESP_OK;
}

void cdc_on_request(cdc_t *cdc, uint8_t *data, uint8_t len)
{
    if (!cdc || len < 1) return;
    cdc->lastPollTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint8_t cmd = data[0];
    switch (cmd) {
    case BLUEBUS_IBUS_CDC_CMD_GET_STATUS:
        cdc_send_status(cdc);
        break;
    case BLUEBUS_IBUS_CDC_CMD_STOP_PLAYING:
        cdc->playing = false;
        cdc_send_status(cdc);
        break;
    case BLUEBUS_IBUS_CDC_CMD_START_PLAYING:
        cdc->playing = true;
        cdc_send_status(cdc);
        break;
    case BLUEBUS_IBUS_CDC_CMD_PAUSE_PLAYING:
        cdc->playing = false;
        cdc_send_status(cdc);
        break;
    default:
        cdc_send_status(cdc);
        break;
    }
}

void cdc_on_ignition(cdc_t *cdc, uint8_t *data, uint8_t len)
{
    if (!cdc || len < 1) return;
    bool wasOn = cdc->ignitionOn;
    cdc->ignitionOn = (data[0] != BLUEBUS_IBUS_IGNITION_OFF);
    if (cdc->ignitionOn && !wasOn) {
        ESP_LOGI(TAG, "Ignition ON");
        cdc->lastStatusTime = 0;
        cdc->lastPollTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
    } else if (!cdc->ignitionOn && wasOn) {
        ESP_LOGI(TAG, "Ignition OFF");
        cdc->playing = false;
    }
}

void cdc_on_button_press(cdc_t *cdc, uint8_t *data, uint8_t len)
{
    if (!cdc || len < 1) return;
    ESP_LOGI(TAG, "CDC button: 0x%02X", data[0]);
}

void cdc_tick(cdc_t *cdc)
{
    if (!cdc) return;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (!cdc->ignitionOn) return;
    if (cdc->lastPollTime == 0 || (now - cdc->lastPollTime) > CDC_POLL_TIMEOUT) {
        cdc->lastPollTime = now;
    }
    if ((now - cdc->lastStatusTime) >= CDC_STATUS_INTERVAL) {
        cdc_send_status(cdc);
    }
}

bool cdc_is_playing(const cdc_t *cdc)
{
    return cdc ? cdc->playing : false;
}

void cdc_set_playing(cdc_t *cdc, bool playing)
{
    if (!cdc) return;
    cdc->playing = playing;
    cdc_send_status(cdc);
}

bool cdc_is_ignition_on(const cdc_t *cdc)
{
    return cdc ? cdc->ignitionOn : false;
}
