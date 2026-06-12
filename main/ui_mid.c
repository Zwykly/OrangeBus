#include "ui_mid.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ibus.h"

#define TAG "UI_MID"
#define MID_DISPLAY_TEXT_MAX 81
#define MID_REFRESH_INTERVAL 10000

struct ui_mid_t {
ibus_t *ibus;
ibus_config_t *config;
bool ignitionOn;
bool active;
bool telModeActive;
uint32_t lastMetaTime;
char displayText[MID_DISPLAY_TEXT_MAX];
};

ui_mid_t *ui_mid_create(ibus_t *ibus, ibus_config_t *config)
{
    ui_mid_t *ui = calloc(1, sizeof(ui_mid_t));
    if (!ui) return NULL;
    ui->ibus = ibus;
    ui->config = config;
    return ui;
}

void ui_mid_destroy(ui_mid_t *ui)
{
    free(ui);
}

esp_err_t ui_mid_init(ui_mid_t *ui)
{
    if (!ui) return ESP_ERR_INVALID_ARG;
    ui->ignitionOn = false;
    ui->telModeActive = false;
    ui->lastMetaTime = 0;
    memset(ui->displayText, 0, sizeof(ui->displayText));
    ESP_LOGI(TAG, "MID UI initialized");
    return ESP_OK;
}

void ui_mid_tick(ui_mid_t *ui)
{
	if (!ui || !ui->active || !ui->ignitionOn || !ui->telModeActive) return;
	uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
	if (strlen(ui->displayText) > 0 && (now - ui->lastMetaTime) >= MID_REFRESH_INTERVAL) {
		ibus_send_mid_text(ui->ibus, BLUEBUS_IBUS_MID_CMD_MODE, ui->displayText,
			BLUEBUS_IBUS_MID_MAX_CHARS);
		ui->lastMetaTime = now;
	}
}

void ui_mid_show_title(ui_mid_t *ui, const char *text)
{
	if (!ui || !text) return;
	strncpy(ui->displayText, text, sizeof(ui->displayText) - 1);
	ui->displayText[sizeof(ui->displayText) - 1] = '\0';
	if (ui->active && ui->telModeActive) {
		ibus_send_mid_text(ui->ibus, BLUEBUS_IBUS_MID_CMD_MODE, ui->displayText,
			BLUEBUS_IBUS_MID_MAX_CHARS);
		ui->lastMetaTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
	}
}

void ui_mid_clear(ui_mid_t *ui)
{
	if (!ui) return;
	memset(ui->displayText, 0, sizeof(ui->displayText));
	if (ui->active && ui->telModeActive) {
		ibus_send_mid_text(ui->ibus, BLUEBUS_IBUS_MID_CMD_MODE, "", 0);
	}
}

void ui_mid_on_ignition(ui_mid_t *ui, bool on)
{
	if (!ui) return;
	ui->ignitionOn = on;
	if (!on && ui->active && ui->telModeActive) {
		ibus_send_mid_set_mode(ui->ibus, BLUEBUS_IBUS_DEV_TEL, 0x00);
		ui->telModeActive = false;
	}
}

void ui_mid_on_cdc_start(ui_mid_t *ui)
{
	if (!ui) return;
	if (!ui->telModeActive && ui->active) {
		ibus_send_mid_set_mode(ui->ibus, BLUEBUS_IBUS_DEV_TEL, 0x02);
		ui->telModeActive = true;
	}
	if (strlen(ui->displayText) > 0 && ui->active) {
		ibus_send_mid_text(ui->ibus, BLUEBUS_IBUS_MID_CMD_MODE, ui->displayText,
			BLUEBUS_IBUS_MID_MAX_CHARS);
		ui->lastMetaTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
	}
}

void ui_mid_on_cdc_stop(ui_mid_t *ui)
{
	if (!ui) return;
	if (ui->telModeActive && ui->active) {
		ibus_send_mid_set_mode(ui->ibus, BLUEBUS_IBUS_DEV_TEL, 0x00);
		ui->telModeActive = false;
	}
}

void ui_mid_set_active(ui_mid_t *ui, bool active)
{
	if (!ui) return;
	if (ui->active && !active && ui->telModeActive) {
		ibus_send_mid_set_mode(ui->ibus, BLUEBUS_IBUS_DEV_TEL, 0x00);
		ui->telModeActive = false;
	}
	ui->active = active;
}
