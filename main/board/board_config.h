#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include <stdint.h>

/* --- I2S - wyjscie audio DAC / wejscie mikrofonu PCM1808 --- */
#define ORANGEBUS_I2S_BCK 26
#define ORANGEBUS_I2S_WS 25
#define ORANGEBUS_I2S_DATA 22
#define ORANGEBUS_I2S_MCLK 0
#define ORANGEBUS_I2S_MIC_DATA 35

/* --- I-BUS UART (9600 8E1) --- */
#define ORANGEBUS_IBUS_TX 17
#define ORANGEBUS_IBUS_RX 16

#define ORANGEBUS_IBUS_UART_NUM UART_NUM_2
#define ORANGEBUS_IBUS_BAUD 9600
#define ORANGEBUS_IBUS_RX_BUF_SIZE 256
#define ORANGEBUS_IBUS_TX_BUF_SIZE 128
#define ORANGEBUS_IBUS_MAX_PKT 64

/* --- Power gating: GPIO27 -> BOB-12009 ch3 -> TH3122 EN ---
 * released/high = stay powered, low = TH3122 sleeps and cuts the buck input.
 * Not a strapping pin; never enable its internal pull-down, gpio_hold or
 * deep sleep on it. */
#define ORANGEBUS_PWR_HOLD 27

#define ORANGEBUS_PWR_MIN_AWAKE_MS   30000   /* never power off sooner after boot */
#define ORANGEBUS_PWR_OFF_GRACE_MS    3000   /* still alive after this -> power-off failed */
#define ORANGEBUS_PWR_RETRY_MS      300000   /* back-off after a failed power-off */

#endif
