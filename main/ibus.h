#ifndef IBUS_H
#define IBUS_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "bluebus.h"

ibus_t *ibus_create(ibus_config_t *config);
void ibus_destroy(ibus_t *ibus);
esp_err_t ibus_init(ibus_t *ibus);
void ibus_process(ibus_t *ibus);
void ibus_register_callback(ibus_t *ibus, bluebus_ibus_event_t event, bluebus_ibus_cb_t cb);
void ibus_send_packet(ibus_t *ibus, uint8_t src, uint8_t dst, uint8_t cmd, const uint8_t *data, uint8_t dataLen);
void ibus_send_cdc_status(ibus_t *ibus, uint8_t status, uint8_t function);
void ibus_send_tel_status(ibus_t *ibus, uint8_t status);
void ibus_send_tel_led_status(ibus_t *ibus, uint8_t ledStatus);
void ibus_send_tel_title_text(ibus_t *ibus, uint8_t titleType, const char *text, uint8_t options);
void ibus_send_mid_text(ibus_t *ibus, uint8_t cmd, const char *text, uint8_t len);
void ibus_send_mid_set_mode(ibus_t *ibus, uint8_t mode, uint8_t type);
void ibus_send_gt_title(ibus_t *ibus, const char *text);
void ibus_send_gt_write_zone(ibus_t *ibus, uint8_t zone, const char *text);
void ibus_send_gt_write_index(ibus_t *ibus, uint8_t index, const char *text);
void ibus_send_gt_clear(ibus_t *ibus);
void ibus_send_business_nav_title(ibus_t *ibus, const char *text);
void ibus_send_dsp_config(ibus_t *ibus, uint8_t mode);
bool ibus_is_debug_mode(const ibus_t *ibus);
void ibus_set_debug_mode(ibus_t *ibus, bool enabled);
uint8_t ibus_crc(const uint8_t *buf, uint8_t len);

#endif
