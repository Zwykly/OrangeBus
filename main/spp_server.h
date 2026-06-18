#ifndef SPP_SERVER_H
#define SPP_SERVER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "orangebus.h"

spp_server_t *spp_server_create(eq_processor_t *eq, ibus_t *ibus, cdc_t *cdc, tel_t *tel, ibus_config_t *config, comfort_t *comfort, avrcp_controller_t *avrcp, volatile bool *uiModeChanged);
void spp_server_destroy(spp_server_t *spp);

esp_err_t spp_server_init(spp_server_t *spp);
bool spp_server_is_connected(const spp_server_t *spp);
esp_err_t spp_server_send(spp_server_t *spp, const char *msg);

#endif
