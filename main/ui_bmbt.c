#include "ui_bmbt.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ibus.h"

#define TAG "UI_BMBT"
#define BMBT_REFRESH_INTERVAL 5000

struct ui_bmbt_t {
    ibus_t *ibus;
    ibus_config_t *config;
    bool ignitionOn;
    bool playing;
    uint32_t lastRefresh;
    char title[UI_BMBT_META_MAX];
    char artist[UI_BMBT_META_MAX];
    char album[UI_BMBT_META_MAX];
};

static void bmbt_show_dashboard(ui_bmbt_t *ui)
{
    ibus_send_gt_clear(ui->ibus);
    ibus_send_gt_title(ui->ibus, ui->playing ? "BlueBus" : "BlueBus [Paused]");

    char line[49];
    if (strlen(ui->title) > 0) {
        snprintf(line, sizeof(line), "Title: %s", ui->title);
        ibus_send_gt_write_zone(ui->ibus, 0x01, line);
    }
    if (strlen(ui->artist) > 0) {
        snprintf(line, sizeof(line), "Artist: %s", ui->artist);
        ibus_send_gt_write_index(ui->ibus, 0, line);
    }
    if (strlen(ui->album) > 0) {
        snprintf(line, sizeof(line), "Album: %s", ui->album);
        ibus_send_gt_write_index(ui->ibus, 1, line);
    }
    ibus_send_gt_write_index(ui->ibus, 3, "Settings...");
    ibus_send_gt_write_index(ui->ibus, 5, ui->playing ? "[Playing]" : "[Paused]");
}

ui_bmbt_t *ui_bmbt_create(ibus_t *ibus, ibus_config_t *config)
{
    ui_bmbt_t *ui = calloc(1, sizeof(ui_bmbt_t));
    if (!ui) return NULL;
    ui->ibus = ibus;
    ui->config = config;
    return ui;
}

void ui_bmbt_destroy(ui_bmbt_t *ui)
{
    free(ui);
}

esp_err_t ui_bmbt_init(ui_bmbt_t *ui)
{
    if (!ui) return ESP_ERR_INVALID_ARG;
    ui->ignitionOn = false;
    ui->playing = false;
    ui->lastRefresh = 0;
    memset(ui->title, 0, sizeof(ui->title));
    memset(ui->artist, 0, sizeof(ui->artist));
    memset(ui->album, 0, sizeof(ui->album));
    ESP_LOGI(TAG, "BMBT UI initialized");
    return ESP_OK;
}

void ui_bmbt_tick(ui_bmbt_t *ui)
{
    if (!ui || !ui->ignitionOn) return;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if ((now - ui->lastRefresh) >= BMBT_REFRESH_INTERVAL) {
        bmbt_show_dashboard(ui);
        ui->lastRefresh = now;
    }
}

void ui_bmbt_on_metadata(ui_bmbt_t *ui, const char *title, const char *artist, const char *album)
{
    if (!ui) return;
    if (title) {
        strncpy(ui->title, title, sizeof(ui->title) - 1);
        ui->title[sizeof(ui->title) - 1] = '\0';
    }
    if (artist) {
        strncpy(ui->artist, artist, sizeof(ui->artist) - 1);
        ui->artist[sizeof(ui->artist) - 1] = '\0';
    }
    if (album) {
        strncpy(ui->album, album, sizeof(ui->album) - 1);
        ui->album[sizeof(ui->album) - 1] = '\0';
    }
    if (ui->ignitionOn) {
        bmbt_show_dashboard(ui);
        ui->lastRefresh = xTaskGetTickCount() * portTICK_PERIOD_MS;
    }
}

void ui_bmbt_on_playback(ui_bmbt_t *ui, bool playing)
{
    if (!ui) return;
    ui->playing = playing;
    if (ui->ignitionOn) {
        bmbt_show_dashboard(ui);
        ui->lastRefresh = xTaskGetTickCount() * portTICK_PERIOD_MS;
    }
}

void ui_bmbt_on_ignition(ui_bmbt_t *ui, bool on)
{
    if (!ui) return;
    ui->ignitionOn = on;
    if (on) {
        bmbt_show_dashboard(ui);
        ui->lastRefresh = xTaskGetTickCount() * portTICK_PERIOD_MS;
    } else {
        ibus_send_gt_clear(ui->ibus);
    }
}

void ui_bmbt_clear(ui_bmbt_t *ui)
{
    if (!ui) return;
    ibus_send_gt_clear(ui->ibus);
}
