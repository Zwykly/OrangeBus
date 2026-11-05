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
/* Smallest complete frame on the wire: SRC + LEN + DST + CMD + CRC. */
#define IBUS_MIN_FRAME_LEN 5
/* A gap longer than this between two bytes means the previous frame was
 * cut short - start assembling a new one. */
#define IBUS_RX_INTER_BYTE_TIMEOUT_MS 50
/* Our own frame echoed back by the transceiver arrives within this window. */
#define IBUS_ECHO_WINDOW_MS 100

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

/* Decodes one valid, CRC-checked frame from another module and fires the
 * matching application event. Frame layout: SRC LEN DST CMD DATA... CRC */
static void process_packet(ibus_t *ibus, uint8_t *pkt, uint8_t len)
{
    if (!ibus || !pkt) return;
    if (len < IBUS_MIN_FRAME_LEN || len > ORANGEBUS_IBUS_MAX_PKT) return;
    uint8_t src = pkt[ORANGEBUS_IBUS_PKT_SRC];
    uint8_t dst = pkt[ORANGEBUS_IBUS_PKT_DST];
    uint8_t cmd = pkt[ORANGEBUS_IBUS_PKT_CMD];
    /* Payload excludes the trailing CRC byte. */
    uint8_t dataLen = len - IBUS_MIN_FRAME_LEN;
    uint8_t *data = &pkt[ORANGEBUS_IBUS_PKT_DB1];

    ESP_LOGD(TAG, "RX: SRC=%02X DST=%02X CMD=%02X LEN=%d", src, dst, cmd, dataLen);

    if (dst == ORANGEBUS_IBUS_DEV_CDC && cmd == ORANGEBUS_IBUS_CMD_MOD_STATUS_REQ) {
        /* "Are you there?" ping for the CD changer (radio: 68 03 18 01 72).
         * Answered straight from the driver - it needs no application state,
         * and the radio drops a CD changer that is slow to answer. */
        ibus_send_module_status(ibus, ORANGEBUS_IBUS_DEV_CDC, src, ORANGEBUS_IBUS_MOD_STATUS_READY);
        dispatch_event(ibus, ORANGEBUS_IBUS_EVT_CDC_PING, data, dataLen);
    } else if (src == ORANGEBUS_IBUS_DEV_RAD && dst == ORANGEBUS_IBUS_DEV_CDC
               && cmd == ORANGEBUS_IBUS_CMD_CDC_REQUEST) {
        /* Every CD changer command: status poll, play, stop, track change... */
        if (dataLen > 0) {
            dispatch_event(ibus, ORANGEBUS_IBUS_EVT_CDC_REQUEST, data, dataLen);
        }
    } else if (src == ORANGEBUS_IBUS_DEV_RAD && cmd == ORANGEBUS_IBUS_CMD_VOL_CTRL) {
        dispatch_event(ibus, ORANGEBUS_IBUS_EVT_RAD_VOLUME, data, dataLen);
    } else if (src == ORANGEBUS_IBUS_DEV_MFL) {
        if (cmd == ORANGEBUS_IBUS_MFL_CMD_BTN_PRESS) {
            dispatch_event(ibus, ORANGEBUS_IBUS_EVT_MFL_BUTTON, data, dataLen);
        } else if (cmd == ORANGEBUS_IBUS_MFL_CMD_VOL_PRESS) {
            dispatch_event(ibus, ORANGEBUS_IBUS_EVT_MFL_VOLUME, data, dataLen);
        }
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
        if (cmd == ORANGEBUS_IBUS_CMD_GT_MENU_SELECT) {
            dispatch_event(ibus, ORANGEBUS_IBUS_EVT_GT_MENU_SELECT, data, dataLen);
        } else if (cmd == ORANGEBUS_IBUS_CMD_GT_CHANGE_UI_REQ) {
            dispatch_event(ibus, ORANGEBUS_IBUS_EVT_GT_CHANGE_UI_REQ, data, dataLen);
        }
    } else if (src == ORANGEBUS_IBUS_DEV_PDC) {
        dispatch_event(ibus, ORANGEBUS_IBUS_EVT_PDC_STATUS, data, dataLen);
    } else if (src == ORANGEBUS_IBUS_DEV_GM) {
        if (cmd == ORANGEBUS_IBUS_CMD_MOD_STATUS_RESP) {
            dispatch_event(ibus, ORANGEBUS_IBUS_EVT_GM_STATUS, data, dataLen);
        } else if (cmd == ORANGEBUS_IBUS_GM_CMD_REMOTE && dataLen >= 1) {
            dispatch_event(ibus, ORANGEBUS_IBUS_EVT_DOOR_LOCK, data, dataLen);
        }
    } else if (src == ORANGEBUS_IBUS_DEV_LCM) {
        if (cmd == ORANGEBUS_IBUS_CMD_MOD_STATUS_RESP) {
            dispatch_event(ibus, ORANGEBUS_IBUS_EVT_LM_STATUS, data, dataLen);
        }
    }
}

ibus_t *ibus_create(ibus_config_t *config)
{
    ibus_t *ibus = calloc(1, sizeof(ibus_t));
    if (!ibus) return NULL;
    ibus->config = config;
    ibus->txMutex = xSemaphoreCreateMutex();
    if (!ibus->txMutex) {
        free(ibus);
        return NULL;
    }
    return ibus;
}

void ibus_destroy(ibus_t *ibus)
{
    if (ibus) {
        if (ibus->uart_installed) {
            uart_driver_delete(ORANGEBUS_IBUS_UART_NUM);
        }
        ibus->uartQueue = NULL;
        ibus->uart_installed = false;
        if (ibus->txMutex) vSemaphoreDelete(ibus->txMutex);
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
        ORANGEBUS_IBUS_RX_BUF_SIZE, ORANGEBUS_IBUS_TX_BUF_SIZE,
        IBUS_UART_QUEUE_LEN, &ibus->uartQueue, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "UART driver install failed: %s. I-BUS unavailable, continuing...", esp_err_to_name(ret));
        ibus->uart_installed = false;
        ibus->debugMode = false;
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

/* Handles one UART driver event. Returns true for "data received".
 * Overflow / buffer-full drops everything buffered and restarts frame
 * assembly, so a noisy bus cannot wedge the parser. */
static bool ibus_handle_uart_event(ibus_t *ibus, const uart_event_t *event)
{
    switch (event->type) {
    case UART_DATA:
        return true;
    case UART_FIFO_OVF:
    case UART_BUFFER_FULL:
        ibus->rxLen = 0;
        uart_flush_input(ORANGEBUS_IBUS_UART_NUM);
        xQueueReset(ibus->uartQueue);
        return false;
    default:
        /* Parity / framing errors: the CRC check rejects the damaged frame. */
        return false;
    }
}

/* Processes every queued UART event without blocking. Returns true when
 * at least one of them was "data received". */
static bool ibus_drain_events(ibus_t *ibus)
{
    if (!ibus->uartQueue) return false;
    bool dataSeen = false;
    uart_event_t event;
    while (xQueueReceive(ibus->uartQueue, &event, 0) == pdTRUE) {
        dataSeen |= ibus_handle_uart_event(ibus, &event);
    }
    return dataSeen;
}

/* Sleeps up to timeout_ms waiting for RX activity, so ibus_task wakes as soon
 * as a byte arrives instead of polling. Returns true if data is waiting. */
bool ibus_wait_for_data(ibus_t *ibus, uint32_t timeout_ms)
{
    if (!ibus || !ibus->uart_installed || !ibus->uartQueue) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms));
        return false;
    }
    if (ibus_drain_events(ibus)) return true;
    uart_event_t event;
    if (xQueueReceive(ibus->uartQueue, &event, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }
    bool dataSeen = ibus_handle_uart_event(ibus, &event);
    return ibus_drain_events(ibus) || dataSeen;
}

/* Drops the first `count` bytes of the RX buffer and keeps the rest. */
static void ibus_rx_consume(ibus_t *ibus, uint8_t count)
{
    if (count >= ibus->rxLen) {
        ibus->rxLen = 0;
        return;
    }
    memmove(ibus->rxBuf, ibus->rxBuf + count, ibus->rxLen - count);
    ibus->rxLen -= count;
}

/* A complete, CRC-valid frame sits at the start of rxBuf: drop it if it is the
 * echo of our own transmission, otherwise hand it to the dispatcher. */
static void ibus_rx_handle_frame(ibus_t *ibus, uint8_t frameLen, uint32_t now)
{
    /* Echo filter: most transceivers loop our own TX back on RX. Snapshot the
     * TX fields once - another task may update them while we compare. */
    uint8_t lastTxLen = ibus->lastTxLen;
    uint32_t lastTxMs = ibus->lastTxMs;
    bool isEcho = (lastTxLen == frameLen
        && (now - lastTxMs) <= IBUS_ECHO_WINDOW_MS
        && memcmp(ibus->rxBuf, ibus->lastTxBuf, frameLen) == 0);
    if (isEcho) return;

    if (!ibus->firstFrameSeen) {
        /* The bus is alive (the car woke up, or we booted into a running car). */
        ibus->firstFrameSeen = true;
        dispatch_event(ibus, ORANGEBUS_IBUS_EVT_FIRST_FRAME, NULL, 0);
    }
    process_packet(ibus, ibus->rxBuf, frameLen);
}

/* Extracts as many frames as possible from the RX buffer. On a bad length or
 * CRC the first byte was not a real start-of-frame: drop it and re-scan the
 * remainder immediately, so a valid frame behind garbage is never stranded
 * waiting for the next byte. */
static void ibus_rx_parse_buffer(ibus_t *ibus, uint32_t now)
{
    while (ibus->rxLen >= 2) {
        uint8_t lenField = ibus->rxBuf[ORANGEBUS_IBUS_PKT_LEN];
        if (!ibus_len_field_valid(lenField)) {
            ibus_resync_shift(ibus);
            continue;
        }
        uint8_t frameLen = lenField + 2;
        if (ibus->rxLen < frameLen) return; /* wait for the rest of the frame */

        uint8_t expectedCrc = ibus_crc(ibus->rxBuf, frameLen - 1);
        if (expectedCrc == ibus->rxBuf[frameLen - 1]) {
            ibus_rx_handle_frame(ibus, frameLen, now);
            ibus_rx_consume(ibus, frameLen);
        } else {
            ibus_resync_shift(ibus);
        }
    }
}

/* Reads everything the UART has buffered (without blocking - ibus_wait_for_data
 * does the waiting) and assembles CRC-checked I-BUS frames. */
void ibus_process(ibus_t *ibus)
{
    if (!ibus) return;
    if (ibus->debugMode) return;
    if (!ibus->uart_installed) return;
    ibus_drain_events(ibus);
    uint8_t chunk[32];
    int readLen;
    while ((readLen = uart_read_bytes(ORANGEBUS_IBUS_UART_NUM, chunk, sizeof(chunk), 0)) > 0) {
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        /* Long silence since the last byte: whatever is buffered is a broken
         * frame, start over with this chunk. */
        if (ibus->rxLen > 0 && (now - ibus->rxLastByte) > IBUS_RX_INTER_BYTE_TIMEOUT_MS) {
            ibus->rxLen = 0;
        }
        ibus->rxLastByte = now;
        for (int i = 0; i < readLen; i++) {
            if (ibus->rxLen >= ORANGEBUS_IBUS_MAX_PKT) {
                ibus_resync_shift(ibus);
            }
            ibus->rxBuf[ibus->rxLen++] = chunk[i];
            ibus_rx_parse_buffer(ibus, now);
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
