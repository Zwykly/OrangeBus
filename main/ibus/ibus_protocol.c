#include "ibus.h"
#include "ibus_private.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "IBUS"

/* Bus-idle arbitration (CODE_REVIEW 1.3): HZ=100 quantizes ms timing to
 * 10 ms ticks, so the idle threshold must be >= 1 tick to be observable. */
#define IBUS_TX_IDLE_MS 10
#define IBUS_TX_DEFER_RETRIES 5

/* Wysyla surowy bufor: w trybie debug loguje z opisem, w normalnym trybie zapisuje na UART */
void ibus_send_raw(ibus_t *ibus, const uint8_t *buf, uint8_t len)
{
    if (ibus->debugMode) {
        char hex[ORANGEBUS_IBUS_MAX_PKT * 3 + 1];
        int pos = 0;
        for (uint8_t i = 0; i < len; i++) {
            pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", buf[i]);
        }
        if (len >= 4) {
            ESP_LOGI(TAG, "[DBG-TX] %s| %s->%s %s", hex,
                     ibus_describe_src(buf[0]), ibus_describe_dst(buf[2]), ibus_describe_cmd(buf[0], buf[2], buf[3]));
        } else {
            ESP_LOGI(TAG, "[DBG-TX] %s", hex);
        }
        return;
    }
    if (!ibus->uart_installed) return;
    if (!buf || len == 0 || len > ORANGEBUS_IBUS_MAX_PKT) return;
    /* Listen-before-talk: defer while a byte arrived within the idle window
     * so we do not collide with radio/GM/LCM frames. Bounded so a babbling
     * bus cannot stall the caller forever (full serialization in T6). */
    for (int i = 0; i < IBUS_TX_DEFER_RETRIES; i++) {
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if ((now - ibus->rxLastByte) >= IBUS_TX_IDLE_MS) break;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    uart_write_bytes(ORANGEBUS_IBUS_UART_NUM, buf, len);
    uart_wait_tx_done(ORANGEBUS_IBUS_UART_NUM, pdMS_TO_TICKS(50));
    if (len <= ORANGEBUS_IBUS_MAX_PKT) {
        memcpy(ibus->lastTxBuf, buf, len);
        ibus->lastTxLen = len;
        ibus->lastTxMs = xTaskGetTickCount() * portTICK_PERIOD_MS;
    }
}

/* Skladaje pakiet I-BUS (src|len|dst|cmd|data...|crc) i wysyla przez send_raw */
void ibus_send_packet(ibus_t *ibus, uint8_t src, uint8_t dst, uint8_t cmd, const uint8_t *data, uint8_t dataLen)
{
    if (!ibus) return;
    uint8_t len = 3 + dataLen;
    uint8_t *tx = ibus->txBuf;
    tx[0] = src;
    tx[1] = len;
    tx[2] = dst;
    tx[3] = cmd;
    if (dataLen > 0 && data != NULL) {
        memcpy(&tx[4], data, dataLen);
    }
    uint8_t crcPos = 4 + dataLen;
    tx[crcPos] = ibus_crc(tx, crcPos);
    ibus_send_raw(ibus, tx, crcPos + 1);
}

void ibus_send_cdc_status(ibus_t *ibus, uint8_t status, uint8_t function)
{
    uint8_t data[] = {0x00, status, function, 0x00, 0x01, 0x00, 0x00, ORANGEBUS_IBUS_CDC_DISC_ALL, 0x00, 0x00, 0x00};
    ibus_send_packet(ibus, ORANGEBUS_IBUS_DEV_CDC, ORANGEBUS_IBUS_DEV_RAD, ORANGEBUS_IBUS_CMD_CDC_RESPONSE, data, sizeof(data));
}

void ibus_send_tel_status(ibus_t *ibus, uint8_t status)
{
    uint8_t data[] = {status};
    ibus_send_packet(ibus, ORANGEBUS_IBUS_DEV_TEL, ORANGEBUS_IBUS_DEV_RAD, ORANGEBUS_IBUS_TEL_CMD_STATUS, data, sizeof(data));
}

void ibus_send_tel_led_status(ibus_t *ibus, uint8_t ledStatus)
{
    uint8_t data[] = {0x00, 0x00, ledStatus};
    ibus_send_packet(ibus, ORANGEBUS_IBUS_DEV_TEL, ORANGEBUS_IBUS_DEV_MID, ORANGEBUS_IBUS_TEL_CMD_LED_STATUS, data, sizeof(data));
}

void ibus_send_tel_title_text(ibus_t *ibus, uint8_t titleType, const char *text, uint8_t options)
{
    uint8_t buf[ORANGEBUS_IBUS_MAX_PKT - 6];
    uint8_t idx = 0;
    buf[idx++] = titleType;
    buf[idx++] = options;
    if (text != NULL) {
        uint8_t textLen = strlen(text);
        for (uint8_t i = 0; i < textLen && idx < sizeof(buf) - 1; i++) {
            buf[idx++] = text[i];
        }
    }
    ibus_send_packet(ibus, ORANGEBUS_IBUS_DEV_TEL, ORANGEBUS_IBUS_DEV_MID, ORANGEBUS_IBUS_TEL_CMD_TITLE_TEXT, buf, idx);
}

void ibus_send_mid_text(ibus_t *ibus, uint8_t cmd, const char *text, uint8_t len)
{
    uint8_t buf[ORANGEBUS_IBUS_MAX_PKT - 6];
    uint8_t idx = 0;
    if (text != NULL && len > 0) {
        uint8_t copyLen = len < sizeof(buf) ? len : sizeof(buf);
        memcpy(buf, text, copyLen);
        idx = copyLen;
    }
    ibus_send_packet(ibus, ORANGEBUS_IBUS_DEV_MID, ORANGEBUS_IBUS_DEV_RAD, cmd, buf, idx);
}

void ibus_send_mid_set_mode(ibus_t *ibus, uint8_t mode, uint8_t type)
{
    uint8_t data[] = {mode, type};
    ibus_send_packet(ibus, ORANGEBUS_IBUS_DEV_MID, ORANGEBUS_IBUS_DEV_RAD, ORANGEBUS_IBUS_MID_CMD_SET_MODE, data, sizeof(data));
}

void ibus_send_gt_title(ibus_t *ibus, const char *text)
{
    uint8_t buf[48];
    uint8_t idx = 0;
    if (text) {
        uint8_t tlen = strlen(text);
        for (uint8_t i = 0; i < tlen && idx < sizeof(buf); i++) {
            buf[idx++] = text[i];
        }
    }
    ibus_send_packet(ibus, ORANGEBUS_IBUS_DEV_GT, ORANGEBUS_IBUS_DEV_BMBT, ORANGEBUS_IBUS_CMD_GT_WRITE_TITLE, buf, idx);
}

void ibus_send_gt_write_zone(ibus_t *ibus, uint8_t zone, const char *text)
{
    uint8_t buf[48];
    uint8_t idx = 0;
    buf[idx++] = zone;
    buf[idx++] = 0x40;
    if (text) {
        uint8_t tlen = strlen(text);
        for (uint8_t i = 0; i < tlen && idx < sizeof(buf) - 1; i++) {
            buf[idx++] = text[i];
        }
    }
    ibus_send_packet(ibus, ORANGEBUS_IBUS_DEV_GT, ORANGEBUS_IBUS_DEV_BMBT, ORANGEBUS_IBUS_CMD_GT_WRITE_ZONE, buf, idx);
}

void ibus_send_gt_write_index(ibus_t *ibus, uint8_t index, const char *text)
{
    uint8_t buf[48];
    uint8_t idx = 0;
    buf[idx++] = 0x01;
    buf[idx++] = index;
    if (text) {
        uint8_t tlen = strlen(text);
        for (uint8_t i = 0; i < tlen && idx < sizeof(buf) - 1; i++) {
            buf[idx++] = text[i];
        }
    }
    ibus_send_packet(ibus, ORANGEBUS_IBUS_DEV_GT, ORANGEBUS_IBUS_DEV_BMBT, ORANGEBUS_IBUS_CMD_GT_WRITE_INDEX, buf, idx);
}

void ibus_send_gt_clear(ibus_t *ibus)
{
    ibus_send_packet(ibus, ORANGEBUS_IBUS_DEV_GT, ORANGEBUS_IBUS_DEV_BMBT, ORANGEBUS_IBUS_CMD_GT_CLEAR, NULL, 0);
}

void ibus_send_business_nav_title(ibus_t *ibus, const char *text)
{
    if (!ibus || !text) return;
    uint8_t length = strlen(text);
    if (length > ORANGEBUS_IBUS_MIR_MAX_CHARS) {
        length = ORANGEBUS_IBUS_MIR_MAX_CHARS;
    }
    uint8_t buf[ORANGEBUS_IBUS_MIR_MAX_CHARS + 3];
    buf[0] = 0x40;
    buf[1] = 0x30;
    memcpy(buf + 2, text, length);
    ibus_send_packet(ibus, ORANGEBUS_IBUS_DEV_RAD, ORANGEBUS_IBUS_DEV_GT, ORANGEBUS_IBUS_CMD_GT_WRITE_TITLE, buf, length + 2);
}

void ibus_send_dsp_config(ibus_t *ibus, uint8_t mode)
{
    uint8_t data[] = {mode};
    ibus_send_packet(ibus, ORANGEBUS_IBUS_DEV_RAD, ORANGEBUS_IBUS_DEV_DSP, ORANGEBUS_IBUS_DSP_CMD_CONFIG_SET, data, sizeof(data));
}
