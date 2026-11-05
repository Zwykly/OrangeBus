#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

/* Power gating (TH3122 EN on GPIO27 / PWR_HOLD, see FIRMWARE_POWER_GATING.md).
 *
 * The board stays powered while PWR_HOLD is released (pull-ups keep TH3122 EN
 * high). Pulling it low puts the TH3122 to sleep; its 5 V output drops, the
 * Pololu switch opens and the ESP32 loses power. Any I-BUS traffic wakes the
 * TH3122 again and the ESP32 cold-boots.
 *
 * Policy: power off only when the ignition is off, the bus has been silent for
 * pwr_idle_s seconds, no call / music is active and nothing is pending. */

#include <stdbool.h>
#include "orangebus.h"

typedef struct power_manager_t power_manager_t;

/* Modules the power-off policy looks at. */
typedef struct {
    ibus_t *ibus;
    ibus_config_t *config;
    cdc_t *cdc;
    comfort_t *comfort;
    a2dp_sink_t *a2dp;
    hfp_client_t *hfp;
    audio_output_t *audio;
} power_manager_deps_t;

power_manager_t *power_manager_create(const power_manager_deps_t *deps);
void power_manager_init(power_manager_t *pm);
/* Call every 100 ms from ibus_task. Non-blocking. */
void power_manager_tick(power_manager_t *pm);
/* CLI test hook: skip the policy and try to power off now. */
void power_manager_request_off(power_manager_t *pm);
void power_manager_print_status(const power_manager_t *pm);

#endif
