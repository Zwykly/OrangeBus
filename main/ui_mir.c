#include "ui_mir.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ibus.h"

#define TAG "UI_MIR"
#define MIR_REFRESH_INTERVAL 10000

struct ui_mir_t {
    ibus_t *ibus;
    ibus_config_t *config;
    bool ignitionOn;
    uint32_t lastMetaTime;
    char displayText[BLUEBUS_IBUS_MIR_MAX_CHARS + 1];
};

ui_mir_t *ui_mir_create(ibus_t *ibus, ibus_config_t *config)
{
    ui_mir_t *ui = calloc(1, sizeof(ui_mir_t));
    if (!ui) return NULL;
    ui->ibus = ibus;
    ui->config = config;
    return ui;
}

void ui_mir_destroy(ui_mir_t *ui)
{
    free(ui);
}

esp_err_t ui_mir_init(ui_mir_t *ui)
{
    if (!ui) return ESP_ERR_INVALID_ARG;
    ui->ignitionOn = false;
    ui->lastMetaTime = 0;
    memset(ui->displayText, 0, sizeof(ui->displayText));
    ESP_LOGI(TAG, "MIR UI initialized");
    return ESP_OK;
}

void ui_mir_tick(ui_mir_t *ui)
{
    if (!ui || !ui->ignitionOn) return;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (strlen(ui->displayText) > 0 && (now - ui->lastMetaTime) >= MIR_REFRESH_INTERVAL) {
        ibus_send_business_nav_title(ui->ibus, ui->displayText);
        ui->lastMetaTime = now;
    }
}

void ui_mir_show_title(ui_mir_t *ui, const char *text)
{
    if (!ui || !text) return;
    uint8_t len = strlen(text);
    if (len > BLUEBUS_IBUS_MIR_MAX_CHARS) len = BLUEBUS_IBUS_MIR_MAX_CHARS;
    memcpy(ui->displayText, text, len);
    ui->displayText[len] = '\0';
    ibus_send_business_nav_title(ui->ibus, ui->displayText);
    ui->lastMetaTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
}

void ui_mir_clear(ui_mir_t *ui)
{
    if (!ui) return;
    memset(ui->displayText, 0, sizeof(ui->displayText));
    ibus_send_business_nav_title(ui->ibus, "");
}

void ui_mir_on_ignition(ui_mir_t *ui, bool on)
{
    if (!ui) return;
    ui->ignitionOn = on;
    if (!on) {
        ui_mir_clear(ui);
    }
}
