#include "comfort.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ibus.h"
#include "ibus_config.h"

#define TAG "COMFORT"
#define BLINK_ON_MS 500
#define BLINK_OFF_MS 300
#define PING_INTERVAL 15000

struct comfort_t {
    ibus_t *ibus;
    ibus_config_t *config;
    orangebus_comfort_gm_variant_t gmVariant;
    orangebus_comfort_lm_variant_t lmVariant;
    bool ignitionOn;
    bool lastLockState;
    bool blinkActive;
    uint32_t blinkOffTime;
    uint32_t lastLcmPing;
    uint32_t lastGmPing;
};

static void send_lock_command(comfort_t *c, bool lock)
{
    uint8_t data[] = { lock ? 0x00 : 0x01 };
    ibus_send_packet(c->ibus, ORANGEBUS_IBUS_DEV_GLO, ORANGEBUS_IBUS_DEV_GM,
        lock ? ORANGEBUS_IBUS_GLO_CMD_LOCK : ORANGEBUS_IBUS_GLO_CMD_UNLOCK, data, sizeof(data));
}

static void send_blink_command(comfort_t *c, bool on)
{
    if (c->lmVariant == ORANGEBUS_COMFORT_LM_UNKNOWN) return;
    uint8_t data[] = { 0x00, on ? 0xFF : 0x00 };
    ibus_send_packet(c->ibus, ORANGEBUS_IBUS_DEV_GLO, ORANGEBUS_IBUS_DEV_LCM,
        on ? ORANGEBUS_IBUS_LCM_CMD_BLINK_ON : ORANGEBUS_IBUS_LCM_CMD_BLINK_OFF, data, sizeof(data));
}

static void send_mirror_fold(comfort_t *c, bool fold)
{
    if (c->gmVariant == ORANGEBUS_COMFORT_GM_ZKE3_GM5 || c->gmVariant == ORANGEBUS_COMFORT_GM_ZKE5) {
        uint8_t data[] = { fold ? 0x01 : 0x00, 0x00 };
        ibus_send_packet(c->ibus, ORANGEBUS_IBUS_DEV_GLO, ORANGEBUS_IBUS_DEV_GM,
            ORANGEBUS_IBUS_GM_CMD_MIRROR, data, sizeof(data));
    }
}

static bool is_blink_enabled(comfort_t *c)
{
    return ibus_config_get(c->config, "comfort_blink") != 0;
}

static bool is_locks_enabled(comfort_t *c)
{
    return ibus_config_get(c->config, "comfort_locks") != 0;
}

static bool is_mirrors_enabled(comfort_t *c)
{
    return ibus_config_get(c->config, "comfort_mirrors") != 0;
}

comfort_t *comfort_create(ibus_t *ibus, ibus_config_t *config)
{
    comfort_t *c = calloc(1, sizeof(comfort_t));
    if (!c) return NULL;
    c->ibus = ibus;
    c->config = config;
    return c;
}

void comfort_destroy(comfort_t *c)
{
    free(c);
}

esp_err_t comfort_init(comfort_t *c)
{
    if (!c) return ESP_ERR_INVALID_ARG;
    c->gmVariant = ORANGEBUS_COMFORT_GM_UNKNOWN;
    c->lmVariant = ORANGEBUS_COMFORT_LM_UNKNOWN;
    c->ignitionOn = false;
    c->lastLockState = false;
    c->blinkActive = false;
    c->blinkOffTime = 0;
    c->lastLcmPing = 0;
    c->lastGmPing = 0;
    ESP_LOGI(TAG, "Comfort initialized");
    return ESP_OK;
}

void comfort_tick(comfort_t *c)
{
    if (!c) return;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    if (c->blinkActive && c->blinkOffTime > 0 && now >= c->blinkOffTime) {
        send_blink_command(c, false);
        c->blinkActive = false;
        c->blinkOffTime = 0;
    }

    if ((now - c->lastLcmPing) >= PING_INTERVAL) {
        uint8_t data[] = { 0x00 };
        ibus_send_packet(c->ibus, ORANGEBUS_IBUS_DEV_GLO, ORANGEBUS_IBUS_DEV_LCM,
            ORANGEBUS_IBUS_CMD_MOD_STATUS_REQ, data, sizeof(data));
        c->lastLcmPing = now;
    }

    if ((now - c->lastGmPing) >= PING_INTERVAL) {
        uint8_t data[] = { 0x00 };
        ibus_send_packet(c->ibus, ORANGEBUS_IBUS_DEV_GLO, ORANGEBUS_IBUS_DEV_GM,
            ORANGEBUS_IBUS_CMD_MOD_STATUS_REQ, data, sizeof(data));
        c->lastGmPing = now;
    }
}

void comfort_on_ignition(comfort_t *c, bool on)
{
    if (!c) return;
    c->ignitionOn = on;
}

/* TODO: Ta funkcja nigdy nie jest wywolywana - comfort_on_door_lock nie jest
 * zarejestrowana jako callback I-BUS. Dodatkowo vTaskDelay(500ms) blokuje
 * zadanie I-BUS - nalezy zamienic na opoznienie bez blokowania. */
void comfort_on_door_lock(comfort_t *c, bool locked)
{
    if (!c) return;
    if (!is_locks_enabled(c) && !is_blink_enabled(c)) return;

    bool wasLocked = c->lastLockState;
    c->lastLockState = locked;

    if (locked && !wasLocked) {
        if (is_blink_enabled(c)) {
            send_blink_command(c, true);
            c->blinkOffTime = xTaskGetTickCount() * portTICK_PERIOD_MS + BLINK_ON_MS;
            c->blinkActive = true;
        }
        if (is_locks_enabled(c) && c->gmVariant != ORANGEBUS_COMFORT_GM_UNKNOWN) {
            vTaskDelay(pdMS_TO_TICKS(500));
            send_lock_command(c, true);
        }
    } else if (!locked && wasLocked) {
        if (is_blink_enabled(c)) {
            send_blink_command(c, true);
            c->blinkOffTime = xTaskGetTickCount() * portTICK_PERIOD_MS + (BLINK_ON_MS * 2);
            c->blinkActive = true;
        }
        if (is_mirrors_enabled(c)) {
            send_mirror_fold(c, false);
        }
    }
}

void comfort_on_gm_status(comfort_t *c, uint8_t *data, uint8_t len)
{
    if (!c || len < 3) return;
    uint8_t type = data[0];
    if (type == 0x01) {
        c->gmVariant = ORANGEBUS_COMFORT_GM_ZKE3_GM1;
    } else if (type == 0x05) {
        c->gmVariant = ORANGEBUS_COMFORT_GM_ZKE3_GM5;
    } else if (type == 0x02 || type == 0x06) {
        c->gmVariant = ORANGEBUS_COMFORT_GM_ZKE5;
    }
    ESP_LOGI(TAG, "GM variant: %d", c->gmVariant);
}

void comfort_on_lm_status(comfort_t *c, uint8_t *data, uint8_t len)
{
    if (!c || len < 3) return;
    uint8_t ident = data[0];
    if (ident <= 0x01) {
        c->lmVariant = ORANGEBUS_COMFORT_LM_LME38;
    } else if (ident == 0x02) {
        c->lmVariant = ORANGEBUS_COMFORT_LM_LCM;
    } else if (ident == 0x03) {
        c->lmVariant = ORANGEBUS_COMFORT_LM_LCM_II;
    } else {
        c->lmVariant = ORANGEBUS_COMFORT_LM_LSZ;
    }
    ESP_LOGI(TAG, "LM variant: %d", c->lmVariant);
}

orangebus_comfort_gm_variant_t comfort_get_gm_variant(const comfort_t *c)
{
	return c ? c->gmVariant : ORANGEBUS_COMFORT_GM_UNKNOWN;
}

orangebus_comfort_lm_variant_t comfort_get_lm_variant(const comfort_t *c)
{
	return c ? c->lmVariant : ORANGEBUS_COMFORT_LM_UNKNOWN;
}

void comfort_send_test_blink(comfort_t *c)
{
	if (!c || c->lmVariant == ORANGEBUS_COMFORT_LM_UNKNOWN) return;
	send_blink_command(c, true);
	c->blinkOffTime = xTaskGetTickCount() * portTICK_PERIOD_MS + BLINK_ON_MS;
	c->blinkActive = true;
}
