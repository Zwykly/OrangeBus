#ifndef SPP_PRIVATE_H
#define SPP_PRIVATE_H

#include "spp_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define SPP_MAX_CMD 256

/* spp_server_t is shared by the transport (spp_server.c) and the command
 * parser (spp_commands.c). */
struct spp_server_t {
    bool connected;
    uint32_t handle;
    eq_processor_t *eq;
    ibus_t *ibus;
    cdc_t *cdc;
    tel_t *tel;
    ibus_config_t *config;
    comfort_t *comfort;
    avrcp_controller_t *avrcp;
    SemaphoreHandle_t dataReady;
    SemaphoreHandle_t bufMutex;
    char cmd_buf[SPP_MAX_CMD];
    int cmd_len;
};

/* Sends a reply to the connected SPP client (spp_server.c). */
void spp_send_response(spp_server_t *spp, const char *msg);

/* Executes one command line (spp_commands.c). */
void spp_process_line(spp_server_t *spp, const char *line);

#endif
