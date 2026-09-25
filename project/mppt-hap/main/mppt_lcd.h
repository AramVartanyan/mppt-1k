/*
 * mppt_lcd.h — LCD pages, numbered menu and the three buttons (UP, DOWN,
 * MENU). Runs in the UI task together with telemetry and HomeKit pushes.
 */
#pragma once

#include <stdint.h>
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

/* Post a button event to the UI task. */
void mppt_ui_button_event(mppt_button_event_t ev);

/* Transient text over the pages. hold_ms = 0 → sticky until the next message
 * or any button press. Thread-safe. */
void mppt_ui_message(const char *line1, const char *line2, uint32_t hold_ms);

/* "Update Firmware? Yes/No" dialog (No selected). Stays until answered.
 * Ignored when the same version was already declined in this boot. */
void mppt_ui_ota_offer(const char *version);

#ifdef __cplusplus
}
#endif
