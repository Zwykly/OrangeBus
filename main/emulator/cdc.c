#include "cdc.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ibus.h"
#include "ibus_config.h"

#define TAG "CDC"

/* No ping/poll from the radio for this long (ignition on) -> announce again.
 * Matches BlueBus HANDLER_CDC_ANOUNCE_TIMEOUT. */
#define CDC_REANNOUNCE_TIMEOUT_MS 21000
/* BMBT (navigation) radios expect a CDC status at least every ~20 s
 * (they poll every 19.945 s); send one ourselves if they did not ask. */
#define CDC_STATUS_REFRESH_MS 20000

struct cdc_t {
    ibus_t *ibus;
    ibus_config_t *config;
    uint8_t function;        /* CDC function byte last ordered by the radio */
    bool ignitionOn;
    uint32_t lastStatusMs;   /* last status frame we sent */
    uint32_t lastPollMs;     /* last ping / command from the radio (or our announce) */
};

static uint32_t now_ms(void)
{
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

/* Status byte that belongs to a stored function byte. */
static uint8_t status_for_function(uint8_t function)
{
    if (function == ORANGEBUS_IBUS_CDC_FUNC_PLAYING) return ORANGEBUS_IBUS_CDC_STAT_PLAYING;
    if (function == ORANGEBUS_IBUS_CDC_FUNC_PAUSE) return ORANGEBUS_IBUS_CDC_STAT_PAUSE;
    return ORANGEBUS_IBUS_CDC_STAT_STOP;
}

/* Sends a status frame. Navigation (BMBT) radios get "1 disc, disc 1"; all
 * other radios get "6 discs, disc 7" so every disc button press reaches us as
 * a CDC command (there is no disc 7 to switch to). */
static void cdc_send_status(cdc_t *cdc, uint8_t status, uint8_t function)
{
    bool navigationUi = ibus_config_get(cdc->config, IBUS_CFG_UI_MODE) == ORANGEBUS_UI_MODE_BMBT;
    uint8_t discsLoaded = navigationUi ? ORANGEBUS_IBUS_CDC_DISC_LOADED_1 : ORANGEBUS_IBUS_CDC_DISC_ALL;
    uint8_t discNumber = navigationUi ? 0x01 : ORANGEBUS_IBUS_CDC_DISC_NUMBER_VIRTUAL;
    ibus_send_cdc_status(cdc->ibus, status, function, discsLoaded, discNumber);
    cdc->lastStatusMs = now_ms();
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
    cdc->function = ORANGEBUS_IBUS_CDC_FUNC_NOT_PLAY;
    cdc->ignitionOn = false;
    cdc->lastStatusMs = now_ms();
    cdc->lastPollMs = now_ms();
    ESP_LOGI(TAG, "CDC emulator initialized");
    return ESP_OK;
}

/* Answers a radio command with exactly the state the radio asked for, even if
 * Bluetooth has not caught up yet - otherwise the radio keeps repeating the
 * command (same approach as BlueBus HandlerIBusCDCStatus). */
void cdc_on_request(cdc_t *cdc, const uint8_t *data, uint8_t len)
{
    if (!cdc || !data || len < 1) return;
    cdc->lastPollMs = now_ms();

    uint8_t command = data[0];
    uint8_t argument = (len >= 2) ? data[1] : 0x00;
    uint8_t status = ORANGEBUS_IBUS_CDC_STAT_STOP;
    uint8_t function = ORANGEBUS_IBUS_CDC_FUNC_NOT_PLAY;

    switch (command) {
    case ORANGEBUS_IBUS_CDC_CMD_GET_STATUS:
        function = cdc->function;
        status = status_for_function(function);
        break;
    case ORANGEBUS_IBUS_CDC_CMD_STOP_PLAYING:
        cdc->function = ORANGEBUS_IBUS_CDC_FUNC_NOT_PLAY;
        break;
    case ORANGEBUS_IBUS_CDC_CMD_PAUSE_PLAYING:
        cdc->function = ORANGEBUS_IBUS_CDC_FUNC_PAUSE;
        status = ORANGEBUS_IBUS_CDC_STAT_PAUSE;
        function = ORANGEBUS_IBUS_CDC_FUNC_PAUSE;
        break;
    case ORANGEBUS_IBUS_CDC_CMD_START_PLAYING:
        cdc->function = ORANGEBUS_IBUS_CDC_FUNC_PLAYING;
        status = ORANGEBUS_IBUS_CDC_STAT_PLAYING;
        function = ORANGEBUS_IBUS_CDC_FUNC_PLAYING;
        break;
    case ORANGEBUS_IBUS_CDC_CMD_CHANGE_TRACK:
    case ORANGEBUS_IBUS_CDC_CMD_CHANGE_TRACK_BL:
        status = ORANGEBUS_IBUS_CDC_STAT_PLAYING;
        function = cdc->function;
        break;
    case ORANGEBUS_IBUS_CDC_CMD_SEEK:
        status = ORANGEBUS_IBUS_CDC_STAT_FAST_REV;
        break;
    case ORANGEBUS_IBUS_CDC_CMD_CD_CHANGE:
        status = ORANGEBUS_IBUS_CDC_STAT_PLAYING;
        function = ORANGEBUS_IBUS_CDC_FUNC_PLAYING;
        break;
    case ORANGEBUS_IBUS_CDC_CMD_SCAN:
    case ORANGEBUS_IBUS_CDC_CMD_RANDOM_MODE:
        /* Second byte 0x01 = mode on, anything else = off. */
        status = ORANGEBUS_IBUS_CDC_STAT_PLAYING;
        function = cdc->function;
        if (argument == 0x01) {
            function = (command == ORANGEBUS_IBUS_CDC_CMD_SCAN)
                ? ORANGEBUS_IBUS_CDC_FUNC_SCAN_MODE
                : ORANGEBUS_IBUS_CDC_FUNC_RANDOM_MODE;
        }
        break;
    default:
        /* Unknown command: echo it back as the status, like BlueBus. */
        status = command;
        break;
    }
    cdc_send_status(cdc, status, function);
}

void cdc_on_ping(cdc_t *cdc)
{
    if (cdc) cdc->lastPollMs = now_ms();
}

void cdc_announce(cdc_t *cdc)
{
    if (!cdc) return;
    ibus_send_module_status(cdc->ibus, ORANGEBUS_IBUS_DEV_CDC, ORANGEBUS_IBUS_DEV_LOC,
        ORANGEBUS_IBUS_MOD_STATUS_ANNOUNCE);
    cdc->lastPollMs = now_ms();
    ESP_LOGI(TAG, "CDC announce sent (%lu ms after boot)", (unsigned long)esp_log_timestamp());
}

void cdc_on_ignition(cdc_t *cdc, bool ignitionOn)
{
    if (!cdc) return;
    bool wasOn = cdc->ignitionOn;
    cdc->ignitionOn = ignitionOn;
    if (ignitionOn && !wasOn) {
        ESP_LOGI(TAG, "Ignition ON");
        /* A radio that just powered up with the key learns about us at once. */
        cdc_announce(cdc);
    } else if (!ignitionOn && wasOn) {
        ESP_LOGI(TAG, "Ignition OFF");
        cdc->function = ORANGEBUS_IBUS_CDC_FUNC_NOT_PLAY;
    }
}

/* Never transmits with the ignition off, so the car's bus can go to sleep. */
void cdc_tick(cdc_t *cdc)
{
    if (!cdc || !cdc->ignitionOn) return;
    uint32_t now = now_ms();
    if ((now - cdc->lastPollMs) >= CDC_REANNOUNCE_TIMEOUT_MS) {
        cdc_announce(cdc);
    }
    if (ibus_config_get(cdc->config, IBUS_CFG_UI_MODE) == ORANGEBUS_UI_MODE_BMBT
        && (now - cdc->lastStatusMs) >= CDC_STATUS_REFRESH_MS) {
        cdc_send_status(cdc, status_for_function(cdc->function), cdc->function);
    }
}

bool cdc_is_playing(const cdc_t *cdc)
{
    return cdc ? cdc->function == ORANGEBUS_IBUS_CDC_FUNC_PLAYING : false;
}

void cdc_set_playing(cdc_t *cdc, bool playing)
{
    if (!cdc) return;
    cdc->function = playing ? ORANGEBUS_IBUS_CDC_FUNC_PLAYING : ORANGEBUS_IBUS_CDC_FUNC_NOT_PLAY;
    if (cdc->ignitionOn) {
        cdc_send_status(cdc, status_for_function(cdc->function), cdc->function);
    }
}

bool cdc_is_ignition_on(const cdc_t *cdc)
{
    return cdc ? cdc->ignitionOn : false;
}
