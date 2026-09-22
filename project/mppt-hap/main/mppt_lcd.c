#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "iot_button.h"
#include "mppt_config.h"
#include "mppt_state.h"
#include "mppt_settings.h"
#include "mppt_hal.h"
#include "mppt_telemetry.h"
#include "mppt_hap.h"
#include "mppt_lcd.h"

static const char *TAG = "mppt_ui";

static QueueHandle_t s_events;

/* ---------------------------------------------------------------- buttons */

static void btn_cb(void *arg)
{
    mppt_ui_button_event((mppt_button_event_t)(intptr_t)arg);
}

static void buttons_init(void)
{
    button_handle_t up = iot_button_create(MPPT_GPIO_BTN_UP, BUTTON_ACTIVE_LOW);
    button_handle_t down = iot_button_create(MPPT_GPIO_BTN_DOWN, BUTTON_ACTIVE_LOW);
    button_handle_t menu = iot_button_create(MPPT_GPIO_BTN_MENU, BUTTON_ACTIVE_LOW);

    iot_button_set_evt_cb(up, BUTTON_CB_TAP, btn_cb, (void *)MPPT_BTN_UP_SHORT);
    iot_button_set_evt_cb(down, BUTTON_CB_TAP, btn_cb, (void *)MPPT_BTN_DOWN_SHORT);
    iot_button_set_evt_cb(menu, BUTTON_CB_TAP, btn_cb, (void *)MPPT_BTN_MENU_SHORT);

    /* auto-repeat after 1 s, every 150 ms */
    iot_button_set_serial_cb(up, 1, pdMS_TO_TICKS(150), btn_cb, (void *)MPPT_BTN_UP_HOLD);
    iot_button_set_serial_cb(down, 1, pdMS_TO_TICKS(150), btn_cb, (void *)MPPT_BTN_DOWN_HOLD);

    /* MENU: 2 s exit/cancel, 10 s factory reset */
    iot_button_add_on_press_cb(menu, 2, btn_cb, (void *)MPPT_BTN_MENU_LONG);
    iot_button_add_on_press_cb(menu, 10, btn_cb, (void *)MPPT_BTN_MENU_RESET);
}

void mppt_ui_button_event(mppt_button_event_t ev)
{
    if (s_events) {
        xQueueSend(s_events, &ev, 0);
    }
}

/* -------------------------------------------------------------------- LCD */

void mppt_ui_message(const char *line1, const char *line2, uint32_t hold_ms)
{
    /* TODO(phase 2): queue a transient message for the UI task. */
    (void)hold_ms;
    ESP_LOGI(TAG, "LCD: [%s] [%s]", line1 ? line1 : "", line2 ? line2 : "");
}

static void lcd_splash(void)
{
    const hd44780_t *lcd = mppt_hal_lcd();
    if (!lcd) {
        return;
    }
    hd44780_clear(lcd);
    hd44780_gotoxy(lcd, 0, 0);
    hd44780_puts(lcd, "MPPT INITIALIZED");
    hd44780_gotoxy(lcd, 0, 1);
    char line[17];
    snprintf(line, sizeof(line), "FIRMWARE %-7s", CONFIG_APP_PROJECT_VER);
    hd44780_puts(lcd, line);
}

/* ---------------------------------------------------------------- UI task */

static void ui_task(void *arg)
{
    (void)arg;
    mppt_button_event_t ev;
    TickType_t last_lcd = 0, last_log = 0, last_hap = 0;
    ESP_LOGI(TAG, "UI task started");
    for (;;) {
        if (xQueueReceive(s_events, &ev, pdMS_TO_TICKS(100)) == pdTRUE) {
            /* TODO(phase 2): menu state machine (8_LCD_Menu.ino, 3 buttons) */
            ESP_LOGI(TAG, "button event %d", ev);
        }
        TickType_t now = xTaskGetTickCount();
        if (now - last_lcd >= pdMS_TO_TICKS(MPPT_LCD_INTERVAL_MS)) {
            last_lcd = now;
            /* TODO(phase 2): display pages 1..5 */
        }
        if (now - last_log >= pdMS_TO_TICKS(MPPT_TELEMETRY_INTERVAL_MS)) {
            last_log = now;
            mppt_telemetry_log();
        }
        if (now - last_hap >= pdMS_TO_TICKS(MPPT_HAP_PUSH_INTERVAL_MS)) {
            last_hap = now;
            mppt_hap_push();
        }
    }
}

esp_err_t mppt_ui_start(void)
{
    s_events = xQueueCreate(8, sizeof(mppt_button_event_t));
    if (!s_events) {
        return ESP_ERR_NO_MEM;
    }
    buttons_init();
    lcd_splash();
    BaseType_t ok = xTaskCreate(ui_task, "mppt_ui", MPPT_UI_TASK_STACK, NULL,
                                MPPT_UI_TASK_PRIO, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
