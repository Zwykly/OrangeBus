#ifndef IBUS_PRIVATE_H
#define IBUS_PRIVATE_H

#include "ibus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define IBUS_UART_QUEUE_LEN 20

/* Definicja struktury ibus_t - wspoldzielona miedzy modulami implementacji */
struct ibus_t {
    uint8_t rxBuf[ORANGEBUS_IBUS_MAX_PKT];
    uint8_t rxLen;
    uint32_t rxLastByte;
    orangebus_ibus_cb_t callbacks[ORANGEBUS_IBUS_EVT_COUNT];
    uint8_t txBuf[ORANGEBUS_IBUS_MAX_PKT];
    QueueHandle_t uartQueue;
    bool debugMode;
    bool uart_installed;
    ibus_config_t *config;
};

/* Wysyla surowy bufor na UART lub loguje w trybie debug - implementacja w ibus_protocol.c */
void ibus_send_raw(ibus_t *ibus, const uint8_t *buf, uint8_t len);

/* Opisowe nazwy urzadzen i komend dla logow debug - implementacja w ibus_debug.c */
const char *ibus_describe_src(uint8_t src);
const char *ibus_describe_dst(uint8_t dst);
const char *ibus_describe_cmd(uint8_t src, uint8_t dst, uint8_t cmd);

#endif
