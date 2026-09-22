/*
 * mppt_hal.h — hardware access for the MPPT32 board.
 *
 * Owns: both I2C buses (i2c_master), the ADS1115, the HD44780 LCD (PCF8574
 * write callback), the LEDC buck PWM, the discrete outputs (buck enable,
 * backflow, fan), the power-good input and the redundant NTC on the on-chip
 * ADC. All other modules use these functions and never touch drivers.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "hd44780.h"

#ifdef __cplusplus
extern "C" {
#endif

/* First call in app_main(): buck disabled, PWM 0, backflow off, fan off.
 * Uses plain GPIO only, safe to call before anything else is initialised. */
esp_err_t mppt_hal_safe_outputs(void);

/* I2C buses, ADS1115, LCD, LEDC, NTC2, status LED. */
esp_err_t mppt_hal_init(void);

/* ---- power stage ---- */
void     mppt_hal_buck_enable(bool on);          /* IR2104 SD# */
void     mppt_hal_set_pwm(uint32_t duty);        /* 0 .. mppt_hal_pwm_max() */
uint32_t mppt_hal_pwm_max(void);                 /* (1 << resolution) - 1 */
void     mppt_hal_backflow(bool on);             /* input MOSFET */
void     mppt_hal_fan(bool on);
bool     mppt_hal_pwr12_good(void);              /* LM5164 PGOOD */

/* ---- analog ---- */
/* Single-shot ADS1115 reading of channel 0..3 in volts (at the ADC pin). */
esp_err_t mppt_hal_ads_read(uint8_t channel, float *volts);
/* Redundant TH2 on the on-chip ADC; ESP_ERR_INVALID_STATE while Wi-Fi is on. */
esp_err_t mppt_hal_ntc2_read(float *temp_c);

/* ---- LCD ---- */
bool               mppt_hal_lcd_present(void);
const hd44780_t   *mppt_hal_lcd(void);
esp_err_t          mppt_hal_lcd_backlight(bool on);

#ifdef __cplusplus
}
#endif
