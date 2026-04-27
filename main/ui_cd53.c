#include "ui_cd53.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ibus.h"

#define TAG "UI_CD53"
#define CD53_DISPLAY_TEXT_MAX 81
#define CD53_REFRESH_INTERVAL 10000

struct ui_cd53_t {
    ibus_t *ibus;
    ibus_config_t *config;
    bool ignitionOn;
    uint32_t lastMetaTime;
    char displayText[CD53_DISPLAY_TEXT_MAX];
};

ui_cd53_t *ui_cd53_create(ibus_t *ibus, ibus_config_t *config)
{
    ui_cd53_t *ui = calloc(1, sizeof(ui_cd53_t));
    if (!ui) return NULL;
    ui->ibus = ibus;
    ui->config = config;
    return ui;
}

void ui_cd53_destroy(ui_cd53_t *ui)
{
    free(ui);
}

esp_err_t ui_cd53_init(ui_cd53_t *ui)
{
    if (!ui) return ESP_ERR_INVALID_ARG;
    ui->ignitionOn = false;
    ui->lastMetaTime = 0;
    memset(ui->displayText, 0, sizeof(ui->displayText));
    ESP_LOGI(TAG, "CD53 UI initialized");
    return ESP_OK;
}

void ui_cd53_tick(ui_cd53_t *ui)
{
    if (!ui || !ui->ignitionOn) return;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (strlen(ui->displayText) > 0 && (now - ui->lastMetaTime) >= CD53_REFRESH_INTERVAL) {
        ibus_send_tel_title_text(ui->ibus, BLUEBUS_IBUS_TEL_TITLE_DEFAULT,
            ui->displayText, BLUEBUS_IBUS_TEL_TITLE_OPT_SET);
        ui->lastMetaTime = now;
    }
}

void ui_cd53_show_title(ui_cd53_t *ui, const char *text)
{
    if (!ui || !text) return;
    strncpy(ui->displayText, text, sizeof(ui->displayText) - 1);
    ui->displayText[sizeof(ui->displayText) - 1] = '\0';
    ibus_send_tel_title_text(ui->ibus, BLUEBUS_IBUS_TEL_TITLE_DEFAULT,
        ui->displayText, BLUEBUS_IBUS_TEL_TITLE_OPT_SET);
    ui->lastMetaTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
}

void ui_cd53_clear(ui_cd53_t *ui)
{
    if (!ui) return;
    memset(ui->displayText, 0, sizeof(ui->displayText));
    ibus_send_tel_title_text(ui->ibus, BLUEBUS_IBUS_TEL_TITLE_DEFAULT,
        "", BLUEBUS_IBUS_TEL_TITLE_OPT_SET);
}

void ui_cd53_on_ignition(ui_cd53_t *ui, bool on)
{
    if (!ui) return;
    ui->ignitionOn = on;
    if (!on) {
        ui_cd53_clear(ui);
    }
}
