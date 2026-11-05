#include "power_manager.h"
#include <stdlib.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ibus.h"
#include "ibus_config.h"
#include "cdc.h"
#include "comfort.h"
#include "a2dp_sink.h"
#include "hfp_client.h"
#include "audio_output.h"

#define TAG "POWER"

typedef enum {
    PM_RUNNING,        /* normal operation, checking the policy every tick */
    PM_POWERING_OFF,   /* PWR_HOLD pulled low, waiting for the rails to drop */
} pm_state_t;

struct power_manager_t {
    power_manager_deps_t deps;
    pm_state_t state;
    uint32_t bootMs;            /* when the manager started (~boot) */
    uint32_t offStartMs;        /* when PWR_HOLD was pulled low */
    uint32_t retryAfterMs;      /* no new attempt before this time */
    uint32_t failedOffCount;
    bool forceOff;              /* set by the CLI 'z' command */
    bool wasMutedBeforeOff;     /* restore the user's mute state on failure */
    const char *blockReason;    /* why we are still on (for the 'P' status) */
};

static uint32_t now_ms(void)
{
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

/* Wrap-safe "now is at or past deadline" for uint32 millisecond timestamps. */
static bool time_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

/* Open-drain output: "1" releases the line (BOB-12009 pull-ups hold TH3122 EN
 * high -> board stays on), "0" pulls EN low (-> board switches itself off). */
static void pwr_hold_release(void) { gpio_set_level((gpio_num_t)ORANGEBUS_PWR_HOLD, 1); }
static void pwr_hold_assert(void)  { gpio_set_level((gpio_num_t)ORANGEBUS_PWR_HOLD, 0); }

power_manager_t *power_manager_create(const power_manager_deps_t *deps)
{
    if (!deps) return NULL;
    power_manager_t *pm = calloc(1, sizeof(power_manager_t));
    if (pm) pm->deps = *deps;
    return pm;
}

void power_manager_init(power_manager_t *pm)
{
    if (!pm) return;
    /* Set the level before switching to output so the pin never glitches low.
     * No internal pull-down (it would fight the self-hold pull-ups). */
    gpio_set_level((gpio_num_t)ORANGEBUS_PWR_HOLD, 1);
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << ORANGEBUS_PWR_HOLD,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    pwr_hold_release();

    pm->state = PM_RUNNING;
    pm->bootMs = now_ms();
    pm->retryAfterMs = pm->bootMs;
    ESP_LOGI(TAG, "Power manager ready (PWR_HOLD=GPIO%d, gate=%d, idle=%ds)",
        ORANGEBUS_PWR_HOLD, ibus_config_get(pm->deps.config, IBUS_CFG_PWR_GATE),
        ibus_config_get(pm->deps.config, IBUS_CFG_PWR_IDLE_S));
}

/* The power-off policy. Every condition must allow it; the first one that
 * does not is remembered as blockReason. */
static bool may_power_off(power_manager_t *pm, uint32_t now)
{
    const power_manager_deps_t *d = &pm->deps;
    uint32_t idleLimitMs = (uint32_t)ibus_config_get(d->config, IBUS_CFG_PWR_IDLE_S) * 1000;
    orangebus_hfp_state_t hfpState = hfp_client_get_state(d->hfp);
    bool callInProgress = hfpState == ORANGEBUS_HFP_INCOMING || hfpState == ORANGEBUS_HFP_ACTIVE
        || hfpState == ORANGEBUS_HFP_AUDIO_OPEN;

    if (!ibus_config_get(d->config, IBUS_CFG_PWR_GATE))              { pm->blockReason = "disabled"; return false; }
    /* Debug mode bypasses ibus_process(), so bus activity would be invisible. */
    if (ibus_is_debug_mode(d->ibus))                                 { pm->blockReason = "ibus debug"; return false; }
    if (now - pm->bootMs < ORANGEBUS_PWR_MIN_AWAKE_MS)               { pm->blockReason = "min awake"; return false; }
    if (!time_reached(now, pm->retryAfterMs))                        { pm->blockReason = "retry back-off"; return false; }
    if (cdc_is_ignition_on(d->cdc))                                  { pm->blockReason = "ignition on"; return false; }
    if (now - ibus_get_last_rx_ms(d->ibus) < idleLimitMs)            { pm->blockReason = "bus active"; return false; }
    if (callInProgress)                                              { pm->blockReason = "call"; return false; }
    if (a2dp_sink_get_state(d->a2dp) == ORANGEBUS_A2DP_PLAYING)      { pm->blockReason = "playing"; return false; }
    if (comfort_is_busy(d->comfort))                                 { pm->blockReason = "comfort pending"; return false; }
    pm->blockReason = NULL;
    return true;
}

static void begin_power_off(power_manager_t *pm, uint32_t now)
{
    ESP_LOGW(TAG, "Powering off (bus idle, ignition off)");
    /* Mute first so the amplifier does not pop as the rails collapse. */
    pm->wasMutedBeforeOff = audio_output_is_muted(pm->deps.audio);
    audio_output_set_mute(pm->deps.audio, true);
    /* Never cut the transceiver in the middle of one of our frames. */
    uart_wait_tx_done(ORANGEBUS_IBUS_UART_NUM, pdMS_TO_TICKS(100));
    pwr_hold_assert();
    pm->state = PM_POWERING_OFF;
    pm->offStartMs = now;
}

void power_manager_tick(power_manager_t *pm)
{
    if (!pm) return;
    uint32_t now = now_ms();

    switch (pm->state) {
    case PM_RUNNING:
        if (pm->forceOff || may_power_off(pm, now)) {
            pm->forceOff = false;
            begin_power_off(pm, now);
        }
        break;

    case PM_POWERING_OFF:
        /* Normally the 3V3 rail collapses within a few hundred ms and this
         * never runs. Still alive means the bus woke the TH3122 again or the
         * ESP32 is on USB power (bench): release the line and carry on.
         * Never esp_restart() here - a reset would release the pin anyway
         * and leave the board half shut down. */
        if (now - pm->offStartMs >= ORANGEBUS_PWR_OFF_GRACE_MS) {
            pwr_hold_release();
            audio_output_set_mute(pm->deps.audio, pm->wasMutedBeforeOff);
            pm->failedOffCount++;
            pm->retryAfterMs = now + ORANGEBUS_PWR_RETRY_MS;
            pm->state = PM_RUNNING;
            ESP_LOGW(TAG, "Power-off did not happen (bus woke us or USB-powered), retry in %d s",
                ORANGEBUS_PWR_RETRY_MS / 1000);
        }
        break;
    }
}

void power_manager_request_off(power_manager_t *pm)
{
    if (pm && pm->state == PM_RUNNING) pm->forceOff = true;
}

void power_manager_print_status(const power_manager_t *pm)
{
    if (!pm) return;
    uint32_t now = now_ms();
    ESP_LOGI(TAG, "state=%s gate=%d up=%lus busIdle=%lus blocked=%s failedOff=%lu",
        pm->state == PM_RUNNING ? "RUNNING" : "POWERING_OFF",
        ibus_config_get(pm->deps.config, IBUS_CFG_PWR_GATE),
        (unsigned long)((now - pm->bootMs) / 1000),
        (unsigned long)((now - ibus_get_last_rx_ms(pm->deps.ibus)) / 1000),
        pm->blockReason ? pm->blockReason : "-",
        (unsigned long)pm->failedOffCount);
}
