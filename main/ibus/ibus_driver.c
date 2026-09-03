#include "ibus.h"
#include "ibus_private.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ibus_config.h"

#define TAG "IBUS"

/* Minimum IBUS length field: DST + CMD + CRC. The packet builder uses
 * len = 3 + dataLen, so 3 is a valid zero-payload frame (CODE_REVIEW 1.4). */
#define IBUS_MIN_LEN_FIELD 3
#define IBUS_MAX_LEN_FIELD (ORANGEBUS_IBUS_MAX_PKT - 2)

static bool ibus_len_field_valid(uint8_t pktLen)
{
    return pktLen >= IBUS_MIN_LEN_FIELD && pktLen <= IBUS_MAX_LEN_FIELD;
}

/* Drop the oldest byte and keep the remainder for resync. */
static void ibus_resync_shift(ibus_t *ibus)
{
    if (ibus->rxLen <= 1) {
        ibus->rxLen = 0;
        return;
    }
    memmove(ibus->rxBuf, ibus->rxBuf + 1, ibus->rxLen - 1);
    ibus->rxLen--;
}

/* TODO: s_instance nie jest nigdzie odczytywane - martwy kod, do usuniecia */
static ibus_t *s_instance = NULL;

uint8_t ibus_crc(const uint8_t *buf, uint8_t len)
{
    uint8_t ck = 0;
    for (uint8_t i = 0; i < len; i++) {
        ck ^= buf[i];
    }
    return ck;
}

static void dispatch_event(ibus_t *ibus, orangebus_ibus_event_t event, uint8_t *data, uint8_t len)
{
    if (event < ORANGEBUS_IBUS_EVT_COUNT && ibus->callbacks[event] != NULL) {
        ibus->callbacks[event](data, len);
    }
}

/* Dekoduje pakiet I-BUS i wywoluje odpowiednie callbacki zdarzen */
static void process_packet(ibus_t *ibus, uint8_t *pkt, uint8_t len)
{
    if (!ibus || !pkt) return;
    if (len < 5 || len > ORANGEBUS_IBUS_MAX_PKT) return;
    uint8_t src = pkt[ORANGEBUS_IBUS_PKT_SRC];
    uint8_t dst = pkt[ORANGEBUS_IBUS_PKT_DST];
    uint8_t cmd = pkt[ORANGEBUS_IBUS_PKT_CMD];
    /* Payload excludes the trailing CRC byte. */
    uint8_t dataLen = len - 5;
    uint8_t *data = &pkt[ORANGEBUS_IBUS_PKT_DB1];

    ESP_LOGI(TAG, "RX: SRC=%02X DST=%02X CMD=%02X LEN=%d", src, dst, cmd, dataLen);

    if (src == ORANGEBUS_IBUS_DEV_RAD && cmd == ORANGEBUS_IBUS_CMD_CDC_REQUEST && dst == ORANGEBUS_IBUS_DEV_CDC) {
        if (dataLen > 0 && data[0] != ORANGEBUS_IBUS_CDC_CMD_GET_STATUS) {
            dispatch_event(ibus, ORANGEBUS_IBUS_EVT_CDC_BUTTON_PRESS, data, dataLen);
        }
        dispatch_event(ibus, ORANGEBUS_IBUS_EVT_CDC_STATUS_REQ, data, dataLen);
    } else if (src == ORANGEBUS_IBUS_DEV_RAD && cmd == ORANGEBUS_IBUS_CMD_VOL_CTRL) {
        dispatch_event(ibus, ORANGEBUS_IBUS_EVT_VOLUME_CHANGE, data, dataLen);
    } else if (src == ORANGEBUS_IBUS_DEV_MFL && cmd == 0x3B) {
        dispatch_event(ibus, ORANGEBUS_IBUS_EVT_MFL_BUTTON_PRESS, data, dataLen);
    } else if (src == ORANGEBUS_IBUS_DEV_IKE && cmd == ORANGEBUS_IBUS_CMD_IKE_IGN_RESP) {
        dispatch_event(ibus, ORANGEBUS_IBUS_EVT_IGNITION_STATUS, data, dataLen);
    } else if (src == ORANGEBUS_IBUS_DEV_MID) {
        if (cmd == ORANGEBUS_IBUS_MID_BUTTON_PRESS) {
            dispatch_event(ibus, ORANGEBUS_IBUS_EVT_MID_BUTTON_PRESS, data, dataLen);
        } else if (cmd == ORANGEBUS_IBUS_MID_CMD_SET_MODE) {
            dispatch_event(ibus, ORANGEBUS_IBUS_EVT_MID_MODE_CHANGE, data, dataLen);
        }
    } else if (src == ORANGEBUS_IBUS_DEV_BMBT) {
        dispatch_event(ibus, ORANGEBUS_IBUS_EVT_BMBT_BUTTON_PRESS, data, dataLen);
    } else if (src == ORANGEBUS_IBUS_DEV_GT) {
        if (cmd == 0x31) {
            dispatch_event(ibus, ORANGEBUS_IBUS_EVT_GT_MENU_SELECT, data, dataLen);
        } else if (cmd == 0x20) {
            dispatch_event(ibus, ORANGEBUS_IBUS_EVT_GT_CHANGE_UI_REQ, data, dataLen);
        }
    } else if (src == ORANGEBUS_IBUS_DEV_PDC) {
        dispatch_event(ibus, ORANGEBUS_IBUS_EVT_PDC_STATUS, data, dataLen);
    }
}

ibus_t *ibus_create(ibus_config_t *config)
{
    ibus_t *ibus = calloc(1, sizeof(ibus_t));
    if (!ibus) return NULL;
    ibus->config = config;
    return ibus;
}

void ibus_destroy(ibus_t *ibus)
{
    if (ibus) {
        if (ibus->uart_installed) {
            uart_driver_delete(ORANGEBUS_IBUS_UART_NUM);
        }
        free(ibus);
    }
}

esp_err_t ibus_init(ibus_t *ibus)
{
    if (!ibus) return ESP_ERR_INVALID_ARG;

    uart_config_t uart_config = {
        .baud_rate = ORANGEBUS_IBUS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_EVEN,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_driver_install(ORANGEBUS_IBUS_UART_NUM,
        ORANGEBUS_IBUS_RX_BUF_SIZE, ORANGEBUS_IBUS_TX_BUF_SIZE, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "UART driver install failed: %s. I-BUS unavailable, continuing...", esp_err_to_name(ret));
        ibus->uart_installed = false;
        ibus->debugMode = false;
        s_instance = ibus;
        return ESP_OK;
    }

    ret = uart_param_config(ORANGEBUS_IBUS_UART_NUM, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART param config failed: %s", esp_err_to_name(ret));
        uart_driver_delete(ORANGEBUS_IBUS_UART_NUM);
        ibus->uart_installed = false;
        return ESP_OK;
    }

    ret = uart_set_pin(ORANGEBUS_IBUS_UART_NUM,
        ORANGEBUS_IBUS_TX, ORANGEBUS_IBUS_RX,
        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART set pin failed: %s", esp_err_to_name(ret));
        uart_driver_delete(ORANGEBUS_IBUS_UART_NUM);
        ibus->uart_installed = false;
        return ESP_OK;
    }

    gpio_pullup_en((gpio_num_t)ORANGEBUS_IBUS_RX);

    ibus->uart_installed = true;
    ibus->debugMode = false;
    s_instance = ibus;
    ESP_LOGI(TAG, "I-BUS UART initialized (9600 8E1, TX=%d, RX=%d, debug=%s)",
        ORANGEBUS_IBUS_TX, ORANGEBUS_IBUS_RX, ibus->debugMode ? "ON" : "OFF");
    return ESP_OK;
}

void ibus_register_callback(ibus_t *ibus, orangebus_ibus_event_t event, orangebus_ibus_cb_t cb)
{
    if (ibus && event < ORANGEBUS_IBUS_EVT_COUNT) {
        ibus->callbacks[event] = cb;
    }
}

/* Odczytuje dane z UART i skladaje pakiety I-BUS z weryfikacja CRC */
void ibus_process(ibus_t *ibus)
{
    if (!ibus) return;
    if (ibus->debugMode) return;
    if (!ibus->uart_installed) return;
    uint8_t buf[32];
    int readLen;
    while ((readLen = uart_read_bytes(ORANGEBUS_IBUS_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(10))) > 0) {
        for (int i = 0; i < readLen; i++) {
            uint8_t b = buf[i];
            if (ibus->rxLen == 0) {
                ibus->rxBuf[0] = b;
                ibus->rxLen = 1;
                ibus->rxLastByte = xTaskGetTickCount() * portTICK_PERIOD_MS;
                continue;
            }
            uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (now - ibus->rxLastByte > 50) {
                ibus->rxLen = 0;
                ibus->rxBuf[0] = b;
                ibus->rxLen = 1;
                ibus->rxLastByte = now;
                continue;
            }
            ibus->rxLastByte = now;
            if (ibus->rxLen >= ORANGEBUS_IBUS_MAX_PKT) {
                ibus_resync_shift(ibus);
                continue;
            }
            ibus->rxBuf[ibus->rxLen++] = b;
            if (ibus->rxLen >= 2) {
                uint8_t pktLen = ibus->rxBuf[ORANGEBUS_IBUS_PKT_LEN];
                if (!ibus_len_field_valid(pktLen)) {
                    /* Corrupted length byte: resync byte-by-byte so the
                     * remainder of the stream can still yield valid frames. */
                    ibus_resync_shift(ibus);
                    continue;
                }
                uint8_t expectedTotal = pktLen + 2;
                if (ibus->rxLen >= expectedTotal) {
                    uint8_t calcCrc = ibus_crc(ibus->rxBuf, expectedTotal - 1);
                    if (calcCrc == ibus->rxBuf[expectedTotal - 1]) {
                        process_packet(ibus, ibus->rxBuf, expectedTotal);
                        ibus->rxLen = 0;
                    } else {
                        /* CRC mismatch: first byte was not a real SOF. */
                        ibus_resync_shift(ibus);
                    }
                }
            }
        }
    }
}

bool ibus_is_debug_mode(const ibus_t *ibus)
{
    return ibus ? ibus->debugMode : false;
}

void ibus_set_debug_mode(ibus_t *ibus, bool enabled)
{
    if (!ibus) return;
    ibus->debugMode = enabled;
    ESP_LOGI(TAG, "Debug mode: %s (runtime only, not persisted)", enabled ? "ON" : "OFF");
}
