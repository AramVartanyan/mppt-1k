/*
 * LCD pages, numbered menu (two visible lines, marker on the active item) and
 * button handling. Port of FUGU 8_LCD_Menu.ino to three buttons:
 *   pages:   UP / DOWN switch page, MENU opens the menu
 *   menu:    UP / DOWN scroll, MENU selects, MENU long (2 s) = Exit
 *   edit:    UP / DOWN change (hold = auto-repeat), MENU confirms, MENU long cancels
 *   dialog:  UP / DOWN move between Yes / No, MENU confirms
 * 7 s without a key returns to the pages (except the OTA prompt).
 */
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "iot_button.h"
#include "general_ota.h"
#include "captive_wifi.h"
#include "mppt_config.h"
#include "mppt_state.h"
#include "mppt_settings.h"
#include "mppt_hal.h"
#include "mppt_sensors.h"
#include "mppt_control.h"
#include "mppt_telemetry.h"
#include "mppt_hap.h"
#include "mppt_system.h"
#include "mppt_lcd.h"

static const char *TAG = "mppt_ui";

#define LCD_COLS 16
#define PAGE_COUNT 5

/* ----------------------------------------------------------------- types */

typedef enum { UI_PAGES, UI_MENU, UI_EDIT, UI_DIALOG } ui_mode_t;

typedef enum {
    IT_PRESET, IT_OUTMODE, IT_ALGO, IT_VMAX, IT_VMIN, IT_ICHG, IT_CHARGING,
    IT_FAN, IT_FANTEMP, IT_TMAX, IT_BACKLIGHT, IT_BLSLEEP, IT_CNTRESET,
    IT_PRICE, IT_TELEM, IT_RESETCNT, IT_DEVSETUP, IT_EXIT,
    /* Device Setup */
    IT_HAP, IT_WIFI, IT_FWUPD, IT_WIFIRESET, IT_HAPRESET, IT_FACTORY, IT_INFO, IT_BACK,
} item_id_t;

typedef enum { DLG_NONE, DLG_HAP_TOGGLE, DLG_WIFI_TOGGLE, DLG_WIFI_RESET, DLG_HAP_RESET,
               DLG_FACTORY, DLG_RESET_COUNTERS, DLG_OTA } dialog_t;

/* ------------------------------------------------------------------ state */

static QueueHandle_t s_events;
static SemaphoreHandle_t s_msg_mutex;

static ui_mode_t s_mode = UI_PAGES;
static int s_page;
static bool s_in_devsetup;
static item_id_t s_items[24];
static int s_item_count;
static int s_sel;                    /* selected item index */
static int s_top;                    /* first visible item */
static mppt_settings_t s_edit;       /* working copy while editing */
static item_id_t s_edit_item;
static dialog_t s_dialog;
static bool s_dialog_yes;
static int64_t s_last_key_us;
static int64_t s_backlight_key_us;
static bool s_backlight_on = true;

static char s_msg1[LCD_COLS + 1], s_msg2[LCD_COLS + 1];
static int64_t s_msg_until_us;       /* 0 = none, -1 = sticky */
static bool s_msg_dirty;

static char s_ota_version[GENERAL_OTA_VERSION_LEN];
static char s_ota_declined[GENERAL_OTA_VERSION_LEN];

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

/* --------------------------------------------------------------- messages */

void mppt_ui_message(const char *line1, const char *line2, uint32_t hold_ms)
{
    if (!s_msg_mutex) {
        return;
    }
    xSemaphoreTake(s_msg_mutex, portMAX_DELAY);
    snprintf(s_msg1, sizeof(s_msg1), "%-16s", line1 ? line1 : "");
    snprintf(s_msg2, sizeof(s_msg2), "%-16s", line2 ? line2 : "");
    s_msg_until_us = hold_ms ? esp_timer_get_time() + (int64_t)hold_ms * 1000 : -1;
    s_msg_dirty = true;
    xSemaphoreGive(s_msg_mutex);
    ESP_LOGI(TAG, "LCD: [%s] [%s]", s_msg1, s_msg2);
}

static void message_clear(void)
{
    xSemaphoreTake(s_msg_mutex, portMAX_DELAY);
    s_msg_until_us = 0;
    xSemaphoreGive(s_msg_mutex);
}

static bool message_active(void)
{
    xSemaphoreTake(s_msg_mutex, portMAX_DELAY);
    bool active = s_msg_until_us == -1 ||
                  (s_msg_until_us > 0 && esp_timer_get_time() < s_msg_until_us);
    if (!active) {
        s_msg_until_us = 0;
    }
    xSemaphoreGive(s_msg_mutex);
    return active;
}

/* ------------------------------------------------------------ LCD output */

static void lcd_lines(const char *l1, const char *l2)
{
    const hd44780_t *lcd = mppt_hal_lcd();
    if (!lcd) {
        return;
    }
    char b[LCD_COLS + 1];
    snprintf(b, sizeof(b), "%-16.16s", l1);
    hd44780_gotoxy(lcd, 0, 0);
    hd44780_puts(lcd, b);
    snprintf(b, sizeof(b), "%-16.16s", l2);
    hd44780_gotoxy(lcd, 0, 1);
    hd44780_puts(lcd, b);
}

/* FUGU displayConfig1 energy field: fits 8 characters */
static void fmt_energy(double wh, char *out, size_t len)
{
    if (wh < 10)            snprintf(out, len, "%.3fWh", wh);
    else if (wh < 100)      snprintf(out, len, "%.2fWh", wh);
    else if (wh < 1000)     snprintf(out, len, "%.1fWh", wh);
    else if (wh < 10000)    snprintf(out, len, "%.2fkWh", wh / 1000);
    else if (wh < 100000)   snprintf(out, len, "%.1fkWh", wh / 1000);
    else if (wh < 1000000)  snprintf(out, len, "%.0fkWh", wh / 1000);
    else if (wh < 10000000) snprintf(out, len, "%.2fMWh", wh / 1e6);
    else                    snprintf(out, len, "%.1fMWh", wh / 1e6);
}

static void render_pages(void)
{
    mppt_state_t st;
    mppt_settings_t cfg;
    mppt_state_snapshot(&st);
    mppt_settings_get(&cfg);
    char l1[LCD_COLS + 8], l2[LCD_COLS + 8], e[12];

    switch (s_page) {
    case 0:  /* power, energy, days | SOC, Vout, Iout */
        fmt_energy(st.wh, e, sizeof(e));
        snprintf(l1, sizeof(l1), "%3.0fW %-8s%2.0fd", st.p_in, e, st.days_running);
        if (cfg.output_mode_charger && cfg.preset == MPPT_BATT_NONE) {
            snprintf(l2, sizeof(l2), "Select battery!");
        } else if (st.faults & MPPT_FAULT_BNC) {
            snprintf(l2, sizeof(l2), "%3d%% NOBAT %4.1fA", st.soc_percent, st.i_out);
        } else {
            snprintf(l2, sizeof(l2), "%3d%% %4.1fV %4.1fA", st.soc_percent, st.v_out, st.i_out);
        }
        break;
    case 1:  /* input | output */
        snprintf(l1, sizeof(l1), "%3.0fW %4.1fV %4.1fA", st.p_in, st.v_in, st.i_in);
        snprintf(l2, sizeof(l2), "%3d%% %4.1fV %4.1fA", st.soc_percent, st.v_out, st.i_out);
        break;
    case 2: { /* energy, SOC | bar graph */
        fmt_energy(st.wh, e, sizeof(e));
        snprintf(l1, sizeof(l1), "%3.0fW %-7s %3d%%", st.p_in, e, st.soc_percent);
        int bars = st.soc_percent / 7;      /* 0..14 blocks, 16 would need 6.25 */
        if (bars > LCD_COLS) bars = LCD_COLS;
        for (int i = 0; i < LCD_COLS; i++) {
            l2[i] = (i < bars) ? (char)0xFF : ' ';
        }
        l2[LCD_COLS] = 0;
        break;
    }
    case 3:  /* temperature */
        snprintf(l1, sizeof(l1), "TEMPERATURE STAT");
        if (isnan(st.temp_c)) {
            snprintf(l2, sizeof(l2), " --.-\xDF""C FAN %s", st.fan_on ? "ON " : "OFF");
        } else {
            snprintf(l2, sizeof(l2), "%5.1f\xDF""C FAN %s", st.temp_c, st.fan_on ? "ON " : "OFF");
        }
        break;
    default: /* savings */
        snprintf(l1, sizeof(l1), "SAVED %7.2f EUR", st.wh / 1000.0 * cfg.energy_price);
        snprintf(l2, sizeof(l2), "%6.2fkWh @%4.2f", st.wh / 1000.0, cfg.energy_price);
        break;
    }
    lcd_lines(l1, l2);
}

/* ------------------------------------------------------------------ items */

static const char *item_label(item_id_t id)
{
    switch (id) {
    case IT_PRESET:    return "Battery";
    case IT_OUTMODE:   return "OutMode";
    case IT_ALGO:      return "Algo";
    case IT_VMAX:      return s_edit.output_mode_charger ? "BatMax" : "OutVolt";
    case IT_VMIN:      return "BatMin";
    case IT_ICHG:      return "Current";
    case IT_CHARGING:  return "Charging";
    case IT_FAN:       return "Fan";
    case IT_FANTEMP:   return "FanTemp";
    case IT_TMAX:      return "MaxTemp";
    case IT_BACKLIGHT: return "Backlt";
    case IT_BLSLEEP:   return "BLsleep";
    case IT_CNTRESET:  return "CntRst";
    case IT_PRICE:     return "Price";
    case IT_TELEM:     return "Telem";
    case IT_RESETCNT:  return "Reset counters";
    case IT_DEVSETUP:  return "Device Setup";
    case IT_EXIT:      return "Exit";
    case IT_HAP:       return s_edit.hap_enabled ? "Disable HAP" : "Enable HAP";
    case IT_WIFI:      return s_edit.wifi_enabled ? "Disable WiFi" : "Enable WiFi";
    case IT_FWUPD:     return "FW Update";
    case IT_WIFIRESET: return "Reset WiFi";
    case IT_HAPRESET:  return "HAP Reset";
    case IT_FACTORY:   return "Factory Reset";
    case IT_INFO:      return "Info";
    case IT_BACK:      return "Exit";
    }
    return "?";
}

static const char *bl_sleep_name(uint8_t m)
{
    static const char *n[] = {"Never", "10s", "5min", "1h", "6h", "12h", "1day", "3day", "1wk", "1mo"};
    return m < 10 ? n[m] : "?";
}

static const char *cnt_reset_name(uint8_t m)
{
    static const char *n[] = {"Never", "Day", "Week", "Month", "Year"};
    return m < 5 ? n[m] : "?";
}

/* value text (≤ 6 chars) from the working copy */
static void item_value(item_id_t id, char *out, size_t len)
{
    switch (id) {
    case IT_PRESET: {
        const char *n = mppt_settings_preset_name(s_edit.preset);
        snprintf(out, len, "%.6s", n);                /* "LiFePO4 12V" → "LiFePO" */
        if (s_edit.preset == MPPT_BATT_LFP_12V) snprintf(out, len, "LFP12");
        if (s_edit.preset == MPPT_BATT_LFP_24V) snprintf(out, len, "LFP24");
        if (s_edit.preset == MPPT_BATT_PB_12V)  snprintf(out, len, "Pb12");
        if (s_edit.preset == MPPT_BATT_PB_24V)  snprintf(out, len, "Pb24");
        break;
    }
    case IT_OUTMODE:   snprintf(out, len, "%s", s_edit.output_mode_charger ? "Chrg" : "PSU"); break;
    case IT_ALGO:      snprintf(out, len, "%s", s_edit.mppt_mode ? "MPPT" : "CC-CV"); break;
    case IT_VMAX:      snprintf(out, len, "%.1fV", s_edit.vbat_max); break;
    case IT_VMIN:      snprintf(out, len, "%.1fV", s_edit.vbat_min); break;
    case IT_ICHG:      snprintf(out, len, "%.1fA", s_edit.i_charge); break;
    case IT_CHARGING:  snprintf(out, len, "%s", s_edit.charging_enabled ? "On" : "Off"); break;
    case IT_FAN:       snprintf(out, len, "%s", s_edit.fan_enabled ? "On" : "Off"); break;
    case IT_FANTEMP:   snprintf(out, len, "%dC", s_edit.fan_temp_c); break;
    case IT_TMAX:      snprintf(out, len, "%dC", s_edit.temp_max_c); break;
    case IT_BACKLIGHT: snprintf(out, len, "%s", s_edit.lcd_backlight ? "On" : "Off"); break;
    case IT_BLSLEEP:   snprintf(out, len, "%s", bl_sleep_name(s_edit.backlight_sleep)); break;
    case IT_CNTRESET:  snprintf(out, len, "%s", cnt_reset_name(s_edit.counter_reset)); break;
    case IT_PRICE:     snprintf(out, len, "%.2f", s_edit.energy_price); break;
    case IT_TELEM:     snprintf(out, len, "%d", s_edit.telemetry_mode); break;
    default:           out[0] = 0; break;
    }
}

static bool item_editable(item_id_t id)
{
    return id <= IT_TELEM;
}

static void item_adjust(item_id_t id, int dir)
{
    switch (id) {
    case IT_PRESET: {
        int p = (int)s_edit.preset + dir;
        if (p < 0) p = MPPT_BATT_PRESET_COUNT - 1;
        if (p >= MPPT_BATT_PRESET_COUNT) p = 0;
        s_edit.preset = (mppt_battery_preset_t)p;
        float vmax, vmin;
        if (mppt_settings_preset_voltages(s_edit.preset, &vmax, &vmin)) {
            s_edit.vbat_max = vmax;
            s_edit.vbat_min = vmin;
        }
        break;
    }
    case IT_OUTMODE:   s_edit.output_mode_charger = !s_edit.output_mode_charger; break;
    case IT_ALGO:      s_edit.mppt_mode = !s_edit.mppt_mode; break;
    case IT_VMAX:
        s_edit.vbat_max += 0.1f * dir;
        if (s_edit.vbat_max < 0) s_edit.vbat_max = 0;
        if (s_edit.vbat_max > MPPT_CAL_VOUT_SYS_MAX) s_edit.vbat_max = MPPT_CAL_VOUT_SYS_MAX;
        if (s_edit.preset != MPPT_BATT_NONE) s_edit.preset = MPPT_BATT_CUSTOM;
        break;
    case IT_VMIN:
        s_edit.vbat_min += 0.1f * dir;
        if (s_edit.vbat_min < 0) s_edit.vbat_min = 0;
        if (s_edit.vbat_min > s_edit.vbat_max) s_edit.vbat_min = s_edit.vbat_max;
        if (s_edit.preset != MPPT_BATT_NONE) s_edit.preset = MPPT_BATT_CUSTOM;
        break;
    case IT_ICHG:
        s_edit.i_charge += 0.5f * dir;
        if (s_edit.i_charge < 0) s_edit.i_charge = 0;
        if (s_edit.i_charge > MPPT_CAL_IIN_ABS) s_edit.i_charge = MPPT_CAL_IIN_ABS;
        break;
    case IT_CHARGING:  s_edit.charging_enabled = !s_edit.charging_enabled; break;
    case IT_FAN:       s_edit.fan_enabled = !s_edit.fan_enabled; break;
    case IT_FANTEMP:
        if ((dir > 0 && s_edit.fan_temp_c < 100) || (dir < 0 && s_edit.fan_temp_c > 0)) s_edit.fan_temp_c += dir;
        break;
    case IT_TMAX:
        if ((dir > 0 && s_edit.temp_max_c < 120) || (dir < 0 && s_edit.temp_max_c > 0)) s_edit.temp_max_c += dir;
        break;
    case IT_BACKLIGHT: s_edit.lcd_backlight = !s_edit.lcd_backlight; break;
    case IT_BLSLEEP:   s_edit.backlight_sleep = (s_edit.backlight_sleep + 10 + dir) % 10; break;
    case IT_CNTRESET:  s_edit.counter_reset = (s_edit.counter_reset + 5 + dir) % 5; break;
    case IT_PRICE:
        s_edit.energy_price += 0.01f * dir;
        if (s_edit.energy_price < 0) s_edit.energy_price = 0;
        if (s_edit.energy_price > 9.99f) s_edit.energy_price = 9.99f;
        break;
    case IT_TELEM:     s_edit.telemetry_mode = (s_edit.telemetry_mode + 4 + dir) % 4; break;
    default: break;
    }
}

/* ------------------------------------------------------------------- menu */

static void build_menu(void)
{
    s_item_count = 0;
    if (!s_in_devsetup) {
        const item_id_t main_items[] = {
            IT_PRESET, IT_OUTMODE, IT_ALGO, IT_VMAX, IT_VMIN, IT_ICHG, IT_CHARGING, IT_FAN,
            IT_FANTEMP, IT_TMAX, IT_BACKLIGHT, IT_BLSLEEP, IT_CNTRESET, IT_PRICE, IT_TELEM,
            IT_RESETCNT, IT_DEVSETUP, IT_EXIT
        };
        for (size_t i = 0; i < sizeof(main_items) / sizeof(main_items[0]); i++) {
            s_items[s_item_count++] = main_items[i];
        }
    } else {
        s_items[s_item_count++] = IT_HAP;
        if (!s_edit.hap_enabled) {
            s_items[s_item_count++] = IT_WIFI;
        }
        s_items[s_item_count++] = IT_FWUPD;
        s_items[s_item_count++] = IT_WIFIRESET;
        if (s_edit.hap_enabled) {
            s_items[s_item_count++] = IT_HAPRESET;
        }
        s_items[s_item_count++] = IT_FACTORY;
        s_items[s_item_count++] = IT_INFO;
        s_items[s_item_count++] = IT_BACK;
    }
    if (s_sel >= s_item_count) s_sel = s_item_count - 1;
    if (s_top > s_sel) s_top = s_sel;
    if (s_top + 1 < s_sel) s_top = s_sel - 1;
}

static void menu_open(void)
{
    mppt_settings_get(&s_edit);
    s_in_devsetup = false;
    s_sel = 0;
    s_top = 0;
    build_menu();
    s_mode = UI_MENU;
}

static void menu_line(int idx, char *out, size_t len)
{
    if (idx >= s_item_count) {
        snprintf(out, len, "%-16s", "");
        return;
    }
    item_id_t id = s_items[idx];
    char val[8] = "";
    item_value(id, val, sizeof(val));
    char body[LCD_COLS + 8], tmp[64];
    snprintf(body, sizeof(body), "%d %s", idx + 1, item_label(id));
    int pad = LCD_COLS - 1 - (int)strlen(body) - (int)strlen(val);
    if (pad < 1) pad = 1;
    snprintf(tmp, sizeof(tmp), "%c%s%*s%s", (idx == s_sel) ? '>' : ' ', body, pad, "", val);
    snprintf(out, len, "%.16s", tmp);
}

static void render_menu(void)
{
    char l1[LCD_COLS + 16], l2[LCD_COLS + 16];
    menu_line(s_top, l1, sizeof(l1));
    menu_line(s_top + 1, l2, sizeof(l2));
    lcd_lines(l1, l2);
}

static void render_edit(void)
{
    char l1[LCD_COLS + 8], l2[LCD_COLS + 8], val[8];
    item_value(s_edit_item, val, sizeof(val));
    snprintf(l1, sizeof(l1), "%s", item_label(s_edit_item));
    snprintf(l2, sizeof(l2), "> %s", val);
    lcd_lines(l1, l2);
}

static const char *dialog_title(void)
{
    switch (s_dialog) {
    case DLG_HAP_TOGGLE:     return s_edit.hap_enabled ? "Disable HAP+rst?" : "Enable HAP+rst?";
    case DLG_WIFI_TOGGLE:    return s_edit.wifi_enabled ? "Disable WiFi+rst" : "Enable WiFi+rst";
    case DLG_WIFI_RESET:     return "Reset WiFi?";
    case DLG_HAP_RESET:      return "Reset HAP pair?";
    case DLG_FACTORY:        return "Factory reset?";
    case DLG_RESET_COUNTERS: return "Reset counters?";
    case DLG_OTA:            return "Update Firmware?";
    default:                 return "";
    }
}

static void render_dialog(void)
{
    char l2[LCD_COLS + 8];
    snprintf(l2, sizeof(l2), "%cYes      %cNo", s_dialog_yes ? '>' : ' ', s_dialog_yes ? ' ' : '>');
    lcd_lines(dialog_title(), l2);
}

static void dialog_open(dialog_t d)
{
    s_dialog = d;
    s_dialog_yes = false;
    s_mode = UI_DIALOG;
}

static void ui_exit_to_pages(void)
{
    s_mode = UI_PAGES;
    s_dialog = DLG_NONE;
}

/* --------------------------------------------------------------- actions */

static void settings_commit(void)
{
    mppt_settings_set(&s_edit);
    mppt_settings_get(&s_edit);          /* re-read normalised copy */
    if (mppt_hal_lcd_present()) {
        mppt_hal_lcd_backlight(s_edit.lcd_backlight);
        s_backlight_on = s_edit.lcd_backlight;
    }
}

static void show_info(void)
{
    char l1[LCD_COLS + 8], l2[LCD_COLS + 8], ip[16] = "-";
    mppt_state_t st;
    mppt_state_snapshot(&st);
    captive_wifi_get_ip(ip, sizeof(ip));
    snprintf(l1, sizeof(l1), "FW %s", general_ota_running_version());
    if (st.wifi_connected) {
        snprintf(l2, sizeof(l2), "%s", ip);
    } else if (st.portal_active) {
        snprintf(l2, sizeof(l2), "AP %s", captive_wifi_ap_ssid());
    } else {
        snprintf(l2, sizeof(l2), "WiFi %s", st.wifi_enabled ? "off-line" : "disabled");
    }
    mppt_ui_message(l1, l2, 5000);
}

static void dialog_yes(void)
{
    switch (s_dialog) {
    case DLG_HAP_TOGGLE:
        s_edit.hap_enabled = !s_edit.hap_enabled;
        if (s_edit.hap_enabled) s_edit.wifi_enabled = true;
        settings_commit();
        lcd_lines("Rebooting...", "");
        mppt_system_reboot();
        break;
    case DLG_WIFI_TOGGLE:
        s_edit.wifi_enabled = !s_edit.wifi_enabled;
        settings_commit();
        lcd_lines("Rebooting...", "");
        mppt_system_reboot();
        break;
    case DLG_WIFI_RESET:
        lcd_lines("WiFi reset", "Rebooting...");
        mppt_system_wifi_reset();
        break;
    case DLG_HAP_RESET:
        lcd_lines("HAP reset", "Rebooting...");
        mppt_system_hap_reset();
        break;
    case DLG_FACTORY:
        lcd_lines("Factory reset", "Rebooting...");
        mppt_system_factory_reset();
        break;
    case DLG_RESET_COUNTERS:
        mppt_sensors_reset_counters();
        mppt_ui_message("Counters reset", "", 2000);
        break;
    case DLG_OTA:
        mppt_control_emergency_stop();
        if (general_ota_update() != ESP_OK) {
            mppt_ui_message("FW update", "busy / error", 3000);
        } else {
            mppt_ui_message("FW update", "starting...", 0);
        }
        break;
    default:
        break;
    }
}

static void item_activate(item_id_t id)
{
    if (item_editable(id)) {
        s_edit_item = id;
        s_mode = UI_EDIT;
        return;
    }
    switch (id) {
    case IT_RESETCNT:  dialog_open(DLG_RESET_COUNTERS); break;
    case IT_DEVSETUP:  s_in_devsetup = true; s_sel = 0; s_top = 0; build_menu(); break;
    case IT_EXIT:      ui_exit_to_pages(); break;
    case IT_HAP:       dialog_open(DLG_HAP_TOGGLE); break;
    case IT_WIFI:      dialog_open(DLG_WIFI_TOGGLE); break;
    case IT_FWUPD: {
        mppt_state_t st;
        mppt_state_snapshot(&st);
        if (!st.wifi_connected) {
            mppt_ui_message("FW Update", "no WiFi", 3000);
        } else if (general_ota_check() != ESP_OK) {
            mppt_ui_message("FW Update", "busy", 3000);
        } else {
            s_ota_declined[0] = 0;       /* manual request: ask again */
            mppt_ui_message("FW Update", "checking...", 10000);
        }
        ui_exit_to_pages();
        break;
    }
    case IT_WIFIRESET: dialog_open(DLG_WIFI_RESET); break;
    case IT_HAPRESET:  dialog_open(DLG_HAP_RESET); break;
    case IT_FACTORY:   dialog_open(DLG_FACTORY); break;
    case IT_INFO:      show_info(); ui_exit_to_pages(); break;
    case IT_BACK:      s_in_devsetup = false; s_sel = 0; s_top = 0; build_menu(); break;
    default: break;
    }
}

/* -------------------------------------------------------------- handling */

static void backlight_wake(void)
{
    s_backlight_key_us = esp_timer_get_time();
    if (!s_backlight_on && s_edit.lcd_backlight) {
        mppt_hal_lcd_backlight(true);
        s_backlight_on = true;
    }
}

static void backlight_tick(void)
{
    mppt_settings_t cfg;
    mppt_settings_get(&cfg);
    if (!cfg.lcd_backlight) {
        if (s_backlight_on) { mppt_hal_lcd_backlight(false); s_backlight_on = false; }
        return;
    }
    static const int64_t intervals_s[] = {0, 10, 300, 3600, 21600, 43200, 86400, 259200, 604800, 2419200};
    uint8_t m = cfg.backlight_sleep < 10 ? cfg.backlight_sleep : 0;
    if (m == 0 || s_mode != UI_PAGES) {
        if (!s_backlight_on) { mppt_hal_lcd_backlight(true); s_backlight_on = true; }
        return;
    }
    bool should_be_on = esp_timer_get_time() - s_backlight_key_us < intervals_s[m] * 1000000LL;
    if (should_be_on != s_backlight_on) {
        mppt_hal_lcd_backlight(should_be_on);
        s_backlight_on = should_be_on;
    }
}

static void handle_button(mppt_button_event_t ev)
{
    s_last_key_us = esp_timer_get_time();
    backlight_wake();

    if (ev == MPPT_BTN_MENU_RESET) {
        lcd_lines("Factory reset", "Rebooting...");
        mppt_system_factory_reset();
        return;
    }

    /* any key dismisses a message (the dialog underneath stays) */
    if (message_active()) {
        message_clear();
        if (s_mode == UI_PAGES) {
            return;
        }
    }

    switch (s_mode) {
    case UI_PAGES:
        if (ev == MPPT_BTN_UP_SHORT)   s_page = (s_page + PAGE_COUNT - 1) % PAGE_COUNT;
        if (ev == MPPT_BTN_DOWN_SHORT) s_page = (s_page + 1) % PAGE_COUNT;
        if (ev == MPPT_BTN_MENU_SHORT) menu_open();
        if (ev == MPPT_BTN_MENU_LONG)  mppt_ui_message("Keep holding 10s", "for factory rst", 3000);
        break;

    case UI_MENU:
        if (ev == MPPT_BTN_UP_SHORT || ev == MPPT_BTN_UP_HOLD) {
            if (s_sel > 0) s_sel--;
        } else if (ev == MPPT_BTN_DOWN_SHORT || ev == MPPT_BTN_DOWN_HOLD) {
            if (s_sel < s_item_count - 1) s_sel++;
        } else if (ev == MPPT_BTN_MENU_SHORT) {
            item_activate(s_items[s_sel]);
        } else if (ev == MPPT_BTN_MENU_LONG) {
            if (s_in_devsetup) item_activate(IT_BACK); else ui_exit_to_pages();
        }
        if (s_top > s_sel) s_top = s_sel;
        if (s_top + 1 < s_sel) s_top = s_sel - 1;
        break;

    case UI_EDIT:
        if (ev == MPPT_BTN_UP_SHORT || ev == MPPT_BTN_UP_HOLD)     item_adjust(s_edit_item, +1);
        if (ev == MPPT_BTN_DOWN_SHORT || ev == MPPT_BTN_DOWN_HOLD) item_adjust(s_edit_item, -1);
        if (ev == MPPT_BTN_MENU_SHORT) {
            settings_commit();
            build_menu();
            s_mode = UI_MENU;
        }
        if (ev == MPPT_BTN_MENU_LONG) {
            mppt_settings_get(&s_edit);  /* discard */
            build_menu();
            s_mode = UI_MENU;
        }
        break;

    case UI_DIALOG:
        if (ev == MPPT_BTN_UP_SHORT || ev == MPPT_BTN_DOWN_SHORT ||
            ev == MPPT_BTN_UP_HOLD || ev == MPPT_BTN_DOWN_HOLD) {
            s_dialog_yes = !s_dialog_yes;
        } else if (ev == MPPT_BTN_MENU_SHORT) {
            dialog_t d = s_dialog;
            bool yes = s_dialog_yes;
            if (d == DLG_OTA && !yes) {
                snprintf(s_ota_declined, sizeof(s_ota_declined), "%s", s_ota_version);
            }
            if (d == DLG_RESET_COUNTERS || d == DLG_OTA) {
                ui_exit_to_pages();
            } else {
                s_dialog = DLG_NONE;
                s_mode = UI_MENU;
                build_menu();
            }
            if (yes) {
                s_dialog = d;
                dialog_yes();
                s_dialog = DLG_NONE;
            }
        } else if (ev == MPPT_BTN_MENU_LONG) {
            if (s_dialog == DLG_OTA) {
                snprintf(s_ota_declined, sizeof(s_ota_declined), "%s", s_ota_version);
                ui_exit_to_pages();
            } else {
                s_dialog = DLG_NONE;
                s_mode = UI_MENU;
            }
        }
        break;
    }
}

void mppt_ui_ota_offer(const char *version)
{
    if (!version || !version[0]) {
        return;
    }
    if (strcmp(version, s_ota_declined) == 0) {
        return;
    }
    snprintf(s_ota_version, sizeof(s_ota_version), "%s", version);
    message_clear();
    dialog_open(DLG_OTA);
}

/* ---------------------------------------------------------------- render */

static void render(void)
{
    if (!mppt_hal_lcd_present()) {
        return;
    }
    if (s_mode == UI_DIALOG) {
        render_dialog();
        return;
    }
    if (message_active()) {
        xSemaphoreTake(s_msg_mutex, portMAX_DELAY);
        lcd_lines(s_msg1, s_msg2);
        s_msg_dirty = false;
        xSemaphoreGive(s_msg_mutex);
        return;
    }
    switch (s_mode) {
    case UI_MENU:  render_menu(); break;
    case UI_EDIT:  render_edit(); break;
    default:       render_pages(); break;
    }
}

static void lcd_splash(void)
{
    char line[LCD_COLS + 8];
    snprintf(line, sizeof(line), "FIRMWARE %s", CONFIG_APP_PROJECT_VER);
    lcd_lines("MPPT INITIALIZED", line);
}

/* ---------------------------------------------------------------- UI task */

static void ui_task(void *arg)
{
    (void)arg;
    mppt_button_event_t ev;
    int64_t last_lcd = 0, last_log = 0, last_hap = 0;
    lcd_splash();
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGI(TAG, "UI task started");
    for (;;) {
        bool redraw = false;
        if (xQueueReceive(s_events, &ev, pdMS_TO_TICKS(50)) == pdTRUE) {
            handle_button(ev);
            redraw = true;
        }
        int64_t now = esp_timer_get_time();

        /* inactivity timeout (not for the OTA prompt) */
        if (s_mode != UI_PAGES && !(s_mode == UI_DIALOG && s_dialog == DLG_OTA) &&
            now - s_last_key_us > (int64_t)MPPT_MENU_TIMEOUT_S * 1000000LL) {
            mppt_settings_get(&s_edit);          /* discard unconfirmed edit */
            ui_exit_to_pages();
            redraw = true;
        }
        if (s_msg_dirty) {
            redraw = true;
        }
        if (redraw || now - last_lcd >= (int64_t)MPPT_LCD_INTERVAL_MS * 1000) {
            last_lcd = now;
            render();
            backlight_tick();
        }
        if (now - last_log >= (int64_t)MPPT_TELEMETRY_INTERVAL_MS * 1000) {
            last_log = now;
            mppt_telemetry_log();
        }
        if (now - last_hap >= (int64_t)MPPT_HAP_PUSH_INTERVAL_MS * 1000) {
            last_hap = now;
            mppt_hap_push();
        }
    }
}

esp_err_t mppt_ui_start(void)
{
    s_events = xQueueCreate(8, sizeof(mppt_button_event_t));
    s_msg_mutex = xSemaphoreCreateMutex();
    if (!s_events || !s_msg_mutex) {
        return ESP_ERR_NO_MEM;
    }
    mppt_settings_get(&s_edit);
    s_backlight_on = s_edit.lcd_backlight;
    if (mppt_hal_lcd_present()) {
        mppt_hal_lcd_backlight(s_backlight_on);
    }
    s_last_key_us = s_backlight_key_us = esp_timer_get_time();
    buttons_init();
    BaseType_t ok = xTaskCreate(ui_task, "mppt_ui", MPPT_UI_TASK_STACK, NULL,
                                MPPT_UI_TASK_PRIO, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
