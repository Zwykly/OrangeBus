#include "ibus.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ibus_config.h"

#define TAG "IBUS"

struct ibus_t {
    uint8_t rxBuf[ORANGEBUS_IBUS_MAX_PKT];
    uint8_t rxLen;
    uint32_t rxLastByte;
    orangebus_ibus_cb_t callbacks[ORANGEBUS_IBUS_EVT_COUNT];
    uint8_t txBuf[ORANGEBUS_IBUS_MAX_PKT];
    bool debugMode;
    bool uart_installed;
    ibus_config_t *config;
};

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

static void process_packet(ibus_t *ibus, uint8_t *pkt, uint8_t len)
{
    uint8_t src = pkt[ORANGEBUS_IBUS_PKT_SRC];
    uint8_t dst = pkt[ORANGEBUS_IBUS_PKT_DST];
    uint8_t cmd = pkt[ORANGEBUS_IBUS_PKT_CMD];
    uint8_t dataLen = len - 4;
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

    gpio_pullup_en(GPIO_NUM_16);

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

static const char *describe_src(uint8_t src)
{
    switch (src) {
    case ORANGEBUS_IBUS_DEV_CDC: return "CDC";
    case ORANGEBUS_IBUS_DEV_TEL: return "TEL";
    case ORANGEBUS_IBUS_DEV_RAD: return "RAD";
    case ORANGEBUS_IBUS_DEV_GT:  return "GT";
    case ORANGEBUS_IBUS_DEV_MID: return "MID";
    case ORANGEBUS_IBUS_DEV_DSP: return "DSP";
    default: return "???";
    }
}

static const char *describe_dst(uint8_t dst)
{
    switch (dst) {
    case ORANGEBUS_IBUS_DEV_RAD:  return "RAD";
    case ORANGEBUS_IBUS_DEV_MID:  return "MID";
    case ORANGEBUS_IBUS_DEV_BMBT: return "BMBT";
    case ORANGEBUS_IBUS_DEV_GT:   return "GT";
    case ORANGEBUS_IBUS_DEV_DSP:  return "DSP";
    case ORANGEBUS_IBUS_DEV_IKE:  return "IKE";
    case ORANGEBUS_IBUS_DEV_LCM:  return "LCM";
    case ORANGEBUS_IBUS_DEV_GM:   return "GM";
    default: return "???";
    }
}

static const char *describe_cmd(uint8_t src, uint8_t dst, uint8_t cmd)
{
    if (src == ORANGEBUS_IBUS_DEV_CDC && dst == ORANGEBUS_IBUS_DEV_RAD && cmd == ORANGEBUS_IBUS_CMD_CDC_RESPONSE)
        return "CDC_STATUS";
    if (src == ORANGEBUS_IBUS_DEV_TEL && cmd == ORANGEBUS_IBUS_TEL_CMD_STATUS)
        return "TEL_STATUS";
    if (src == ORANGEBUS_IBUS_DEV_TEL && cmd == ORANGEBUS_IBUS_TEL_CMD_LED_STATUS)
        return "TEL_LED";
    if (src == ORANGEBUS_IBUS_DEV_TEL && cmd == ORANGEBUS_IBUS_TEL_CMD_TITLE_TEXT)
        return "TEL_TITLE";
    if (src == ORANGEBUS_IBUS_DEV_MID && cmd == ORANGEBUS_IBUS_MID_CMD_SET_MODE)
        return "MID_SET_MODE";
    if (src == ORANGEBUS_IBUS_DEV_MID && dst == ORANGEBUS_IBUS_DEV_RAD)
        return "MID_TEXT";
    if (src == ORANGEBUS_IBUS_DEV_GT && cmd == ORANGEBUS_IBUS_CMD_GT_WRITE_TITLE)
        return "GT_TITLE";
    if (src == ORANGEBUS_IBUS_DEV_GT && cmd == ORANGEBUS_IBUS_CMD_GT_WRITE_ZONE)
        return "GT_ZONE";
    if (src == ORANGEBUS_IBUS_DEV_GT && cmd == ORANGEBUS_IBUS_CMD_GT_WRITE_INDEX)
        return "GT_INDEX";
    if (src == ORANGEBUS_IBUS_DEV_GT && cmd == ORANGEBUS_IBUS_CMD_GT_CLEAR)
        return "GT_CLEAR";
    if (src == ORANGEBUS_IBUS_DEV_RAD && dst == ORANGEBUS_IBUS_DEV_GT && cmd == ORANGEBUS_IBUS_CMD_GT_WRITE_TITLE)
        return "RAD->GT_TITLE(BUS_NAV)";
    if (src == ORANGEBUS_IBUS_DEV_RAD && dst == ORANGEBUS_IBUS_DEV_DSP && cmd == ORANGEBUS_IBUS_DSP_CMD_CONFIG_SET)
        return "DSP_CONFIG";
    return "OTHER";
}

static void send_raw(ibus_t *ibus, const uint8_t *buf, uint8_t len)
{
    if (ibus->debugMode) {
        char hex[ORANGEBUS_IBUS_MAX_PKT * 3 + 1];
        int pos = 0;
        for (uint8_t i = 0; i < len; i++) {
            pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", buf[i]);
        }
        if (len >= 4) {
            ESP_LOGI(TAG, "[DBG-TX] %s| %s->%s %s", hex,
                     describe_src(buf[0]), describe_dst(buf[2]), describe_cmd(buf[0], buf[2], buf[3]));
        } else {
            ESP_LOGI(TAG, "[DBG-TX] %s", hex);
        }
        return;
    }
    if (!ibus->uart_installed) return;
    uart_write_bytes(ORANGEBUS_IBUS_UART_NUM, buf, len);
    uart_wait_tx_done(ORANGEBUS_IBUS_UART_NUM, pdMS_TO_TICKS(50));
}

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
    send_raw(ibus, tx, crcPos + 1);
}

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
                ibus->rxLen = 0;
                continue;
            }
            ibus->rxBuf[ibus->rxLen++] = b;
            if (ibus->rxLen >= 3) {
                uint8_t pktLen = ibus->rxBuf[ORANGEBUS_IBUS_PKT_LEN];
                uint8_t expectedTotal = pktLen + 2;
                if (ibus->rxLen >= expectedTotal) {
                    uint8_t calcCrc = ibus_crc(ibus->rxBuf, expectedTotal - 1);
                    if (calcCrc == ibus->rxBuf[expectedTotal - 1]) {
                        process_packet(ibus, ibus->rxBuf, expectedTotal);
                    }
                    ibus->rxLen = 0;
                }
            }
        }
    }
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
