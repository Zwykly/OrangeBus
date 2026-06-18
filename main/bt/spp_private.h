#ifndef SPP_PRIVATE_H
#define SPP_PRIVATE_H

#include "spp_server.h"

#define SPP_MAX_CMD 256

/* Definicja struktury spp_server_t - wspoldzielona miedzy transportem a komendami */
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
    volatile bool *uiModeChanged;
    char cmd_buf[SPP_MAX_CMD];
    int cmd_len;
};

/* Wysyla tekst odpowiedzi do klienta SPP - implementacja w spp_server.c */
void spp_send_response(spp_server_t *spp, const char *msg);

/* Przetwarza pojedyncza linie komendy - implementacja w spp_commands.c */
void spp_process_line(spp_server_t *spp, const char *line);

#endif
