#include "tel.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ibus.h"
#include "ibus_config.h"

#define TAG "TEL"
#define TEL_STATUS_INTERVAL 5000
#define TEL_LED_INTERVAL 10000

struct tel_t {
    ibus_t *ibus;
    ibus_config_t *config;
    bool connected;
    bool callActive;
    bool callIncoming;
    char callerId[TEL_CALLER_ID_MAX];
    uint32_t lastStatusTime;
    uint32_t lastLEDTime;
};

static void tel_send_status(tel_t *tel)
{
    uint8_t status;
    if (tel->callActive) {
        status = ORANGEBUS_IBUS_TEL_STATUS_POWER_CALL;
    } else if (tel->connected) {
        status = ORANGEBUS_IBUS_TEL_STATUS_POWER_HF;
    } else {
        status = ORANGEBUS_IBUS_TEL_STATUS_NONE;
    }
    ibus_send_tel_status(tel->ibus, status);
    tel->lastStatusTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static void tel_send_led(tel_t *tel)
{
    uint8_t led;
    if (tel->callActive) {
        led = ORANGEBUS_IBUS_TEL_LED_GREEN_ON;
    } else if (tel->callIncoming) {
        led = ORANGEBUS_IBUS_TEL_LED_GREEN_BLINK;
    } else if (tel->connected) {
        led = ORANGEBUS_IBUS_TEL_LED_GREEN_ON;
    } else {
        led = ORANGEBUS_IBUS_TEL_LED_RED_ON;
    }
    ibus_send_tel_led_status(tel->ibus, led);
    tel->lastLEDTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static void tel_send_caller_id(tel_t *tel)
{
    if (strlen(tel->callerId) == 0) return;
    if (tel->callActive) {
        ibus_send_tel_title_text(tel->ibus, ORANGEBUS_IBUS_TEL_TITLE_ON_CALL,
            tel->callerId, ORANGEBUS_IBUS_TEL_TITLE_OPT_SET);
    } else if (tel->callIncoming) {
        ibus_send_tel_title_text(tel->ibus, ORANGEBUS_IBUS_TEL_TITLE_DEFAULT,
            tel->callerId, ORANGEBUS_IBUS_TEL_TITLE_OPT_SET);
    }
}

tel_t *tel_create(ibus_t *ibus, ibus_config_t *config)
{
    tel_t *tel = calloc(1, sizeof(tel_t));
    if (!tel) return NULL;
    tel->ibus = ibus;
    tel->config = config;
    return tel;
}

void tel_destroy(tel_t *tel)
{
    free(tel);
}

esp_err_t tel_init(tel_t *tel)
{
    if (!tel) return ESP_ERR_INVALID_ARG;
    memset(tel->callerId, 0, sizeof(tel->callerId));
    tel->lastStatusTime = 0;
    tel->lastLEDTime = 0;
    ESP_LOGI(TAG, "TEL emulator initialized");
    return ESP_OK;
}

void tel_set_connected(tel_t *tel, bool connected)
{
    if (!tel) return;
    tel->connected = connected;
    tel_send_led(tel);
    tel_send_status(tel);
}

void tel_set_call_active(tel_t *tel, bool active)
{
    if (!tel) return;
    bool wasActive = tel->callActive;
    tel->callActive = active;
    if (active && !wasActive) {
        tel_send_status(tel);
        tel_send_led(tel);
        if (strlen(tel->callerId) > 0) {
            tel_send_caller_id(tel);
        }
    } else if (!active && wasActive) {
        tel->callIncoming = false;
        memset(tel->callerId, 0, sizeof(tel->callerId));
        tel_send_status(tel);
        tel_send_led(tel);
    }
}

void tel_set_call_incoming(tel_t *tel, bool incoming)
{
    if (!tel) return;
    tel->callIncoming = incoming;
    if (incoming) {
        tel_send_status(tel);
        tel_send_led(tel);
    }
}

void tel_set_caller_id(tel_t *tel, const char *id)
{
    if (!tel || !id) return;
    strncpy(tel->callerId, id, sizeof(tel->callerId) - 1);
    tel->callerId[sizeof(tel->callerId) - 1] = '\0';
    if (tel->callActive || tel->callIncoming) {
        tel_send_caller_id(tel);
    }
}

void tel_tick(tel_t *tel)
{
    if (!tel) return;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (tel->connected && (now - tel->lastStatusTime) >= TEL_STATUS_INTERVAL) {
        tel_send_status(tel);
    }
    if ((now - tel->lastLEDTime) >= TEL_LED_INTERVAL) {
        tel_send_led(tel);
    }
}

bool tel_is_connected(const tel_t *tel)
{
    return tel ? tel->connected : false;
}

bool tel_is_call_active(const tel_t *tel)
{
    return tel ? tel->callActive : false;
}
