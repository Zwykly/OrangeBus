#ifndef IBUS_PRIVATE_H
#define IBUS_PRIVATE_H

#include "ibus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#define IBUS_UART_QUEUE_LEN 20

/* ibus_t is shared by the driver (RX), protocol (TX) and debug sources. */
struct ibus_t {
    uint8_t rxBuf[ORANGEBUS_IBUS_MAX_PKT];
    uint8_t rxLen;
    uint32_t rxLastByteMs;
    orangebus_ibus_cb_t callbacks[ORANGEBUS_IBUS_EVT_COUNT];
    uint8_t txBuf[ORANGEBUS_IBUS_MAX_PKT];
    SemaphoreHandle_t txMutex;
    uint8_t lastTxBuf[ORANGEBUS_IBUS_MAX_PKT];
    uint8_t lastTxLen;
    uint32_t lastTxMs;
    QueueHandle_t uartQueue;
    bool debugMode;
    bool uartInstalled;
    uint32_t lastRxFrameMs; /* last valid, non-echo frame from another module */
    bool firstFrameSeen;   /* set on the first valid foreign frame since boot */
    ibus_config_t *config;
};

/* Writes a finished frame to the UART, or only logs it in debug mode (ibus_protocol.c). */
void ibus_send_raw(ibus_t *ibus, const uint8_t *buf, uint8_t len);

/* Readable device / command names for debug logs (ibus_debug.c). */
const char *ibus_describe_src(uint8_t src);
const char *ibus_describe_dst(uint8_t dst);
const char *ibus_describe_cmd(uint8_t src, uint8_t dst, uint8_t cmd);

#endif
