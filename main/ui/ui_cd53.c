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
    bool active;
    uint32_t lastMetaTime;
    char displayText[CD53_DISPLAY_TEXT_MAX];
};

/* Creates the CD53 display UI (inactive until set_active). */
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

/* Resets the UI state. */
esp_err_t ui_cd53_init(ui_cd53_t *ui)
{
    if (!ui) return ESP_ERR_INVALID_ARG;
    ui->ignitionOn = false;
    ui->lastMetaTime = 0;
    memset(ui->displayText, 0, sizeof(ui->displayText));
    ESP_LOGI(TAG, "CD53 UI initialized");
    return ESP_OK;
}

/* Re-sends the current text every refresh interval (the display may have
 * overwritten it). Silent with the ignition off or when inactive. */
void ui_cd53_tick(ui_cd53_t *ui)
{
    if (!ui || !ui->active || !ui->ignitionOn) return;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (strlen(ui->displayText) > 0 && (now - ui->lastMetaTime) >= CD53_REFRESH_INTERVAL) {
        ibus_send_tel_title_text(ui->ibus, ORANGEBUS_IBUS_TEL_TITLE_DEFAULT,
            ui->displayText, ORANGEBUS_IBUS_TEL_TITLE_OPT_SET);
        ui->lastMetaTime = now;
    }
}

/* Stores the track text and shows it right away if this UI is active. */
void ui_cd53_show_title(ui_cd53_t *ui, const char *text)
{
    if (!ui || !text) return;
    strncpy(ui->displayText, text, sizeof(ui->displayText) - 1);
    ui->displayText[sizeof(ui->displayText) - 1] = '\0';
    if (ui->active) {
        ibus_send_tel_title_text(ui->ibus, ORANGEBUS_IBUS_TEL_TITLE_DEFAULT,
            ui->displayText, ORANGEBUS_IBUS_TEL_TITLE_OPT_SET);
        ui->lastMetaTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
    }
}

/* Clears our text from the display. */
void ui_cd53_clear(ui_cd53_t *ui)
{
    if (!ui) return;
    memset(ui->displayText, 0, sizeof(ui->displayText));
    if (ui->active) {
        ibus_send_tel_title_text(ui->ibus, ORANGEBUS_IBUS_TEL_TITLE_DEFAULT,
            "", ORANGEBUS_IBUS_TEL_TITLE_OPT_SET);
    }
}

/* Ignition changed: clear the display when it goes off. */
void ui_cd53_on_ignition(ui_cd53_t *ui, bool on)
{
    if (!ui) return;
    ui->ignitionOn = on;
    if (!on && ui->active) {
        ui_cd53_clear(ui);
    }
}

/* Enables / disables this UI; clears the display when disabled. */
void ui_cd53_set_active(ui_cd53_t *ui, bool active)
{
    if (!ui) return;
    if (ui->active && !active) {
        ui_cd53_clear(ui);
    }
    ui->active = active;
}
