/*
 * mppt_settings.h — user settings and persistent counters in NVS
 * (container_nvs, namespace "mppt"). Replaces FUGU's EEPROM byte layout.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MPPT_BATT_NONE = 0,     /* PSU / no battery: Battery service 100 %, not chargeable */
    MPPT_BATT_PB_12V,
    MPPT_BATT_PB_24V,
    MPPT_BATT_LFP_12V,      /* LiFePO4 4S */
    MPPT_BATT_LFP_24V,      /* LiFePO4 8S */
    MPPT_BATT_CUSTOM,
    MPPT_BATT_PRESET_COUNT
} mppt_battery_preset_t;

typedef struct {
    uint32_t version;           /* layout version, bump on change */

    /* charger */
    bool  charging_enabled;     /* !chargingPause */
    bool  output_mode_charger;  /* output_Mode: true = charger, false = PSU */
    bool  mppt_mode;            /* MPPT_Mode: true = MPPT, false = CC-CV only */
    mppt_battery_preset_t preset;
    float vbat_max;             /* voltageBatteryMax */
    float vbat_min;             /* voltageBatteryMin */
    float i_charge;             /* currentCharging */

    /* cooling */
    bool    fan_enabled;
    uint8_t fan_temp_c;         /* temperatureFan */
    uint8_t temp_max_c;         /* temperatureMax */

    /* connectivity */
    bool wifi_enabled;
    bool hap_enabled;           /* implies wifi_enabled */

    /* UI */
    bool    lcd_backlight;
    uint8_t backlight_sleep;    /* 0 never .. 9 (FUGU backlightSleepMode) */
    uint8_t counter_reset;      /* telemCounterReset: 0 never, 1 day, 2 week, 3 month, 4 year */
    uint8_t telemetry_mode;     /* serialTelemMode 0..3 */
    float   energy_price;       /* currency per kWh */
} mppt_settings_t;

/* Load from NVS or fall back to factory defaults (also stores them). */
esp_err_t mppt_settings_init(void);

/* Copy of the current settings. */
void mppt_settings_get(mppt_settings_t *out);

/* Replace and persist. Applies preset voltages when preset != CUSTOM/NONE. */
esp_err_t mppt_settings_set(const mppt_settings_t *in);

/* Fill defaults from Kconfig. */
void mppt_settings_defaults(mppt_settings_t *out);

/* Preset voltages; returns false for NONE / CUSTOM (values untouched). */
bool mppt_settings_preset_voltages(mppt_battery_preset_t p, float *vmax, float *vmin);
const char *mppt_settings_preset_name(mppt_battery_preset_t p);

/* Energy counters (separate NVS key, written on the persistence schedule). */
esp_err_t mppt_settings_save_counters(double wh, uint32_t run_seconds);
esp_err_t mppt_settings_load_counters(double *wh, uint32_t *run_seconds);

/* Erase settings and counters (factory reset). */
esp_err_t mppt_settings_erase(void);

#ifdef __cplusplus
}
#endif
