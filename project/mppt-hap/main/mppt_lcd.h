/*
 * mppt_lcd.h — LCD pages, numbered menu and the three buttons (UP, DOWN,
 * MENU). Runs in the UI task together with telemetry and HomeKit pushes.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MPPT_BTN_UP_SHORT = 0,
    MPPT_BTN_DOWN_SHORT,
    MPPT_BTN_MENU_SHORT,
    MPPT_BTN_UP_HOLD,       /* auto-repeat while held */
    MPPT_BTN_DOWN_HOLD,
    MPPT_BTN_MENU_LONG,     /* 2 s: exit / cancel */
    MPPT_BTN_MENU_RESET,    /* 10 s: factory reset */
} mppt_button_event_t;

/* Buttons (iot_button), LCD splash, UI task. */
esp_err_t mppt_ui_start(void);

/* Post a button event to the UI task (ISR-safe callbacks call this). */
void mppt_ui_button_event(mppt_button_event_t ev);

/* Text shown on the LCD by other modules (portal SSID, OTA progress). */
void mppt_ui_message(const char *line1, const char *line2, uint32_t hold_ms);

#ifdef __cplusplus
}
#endif
