/*
 * mppt_config.h — compile-time constants derived from Kconfig (menuconfig →
 * "MPPT Solar Charger"). Integer Kconfig values are converted to the float
 * units the control code works in. Nothing here is read at runtime from NVS;
 * user-editable settings live in mppt_settings.h.
 */
#pragma once

#include "sdkconfig.h"

/* ---- GPIO ---- */
#define MPPT_GPIO_PWM          CONFIG_MPPT_GPIO_PWM
#define MPPT_GPIO_BUCK_EN      CONFIG_MPPT_GPIO_BUCK_EN
#define MPPT_GPIO_BACKFLOW     CONFIG_MPPT_GPIO_BACKFLOW
#define MPPT_GPIO_FAN          CONFIG_MPPT_GPIO_FAN
#define MPPT_GPIO_PWR12_GOOD   CONFIG_MPPT_GPIO_PWR12_GOOD
#define MPPT_GPIO_ADS_RDY      CONFIG_MPPT_GPIO_ADS_RDY
#define MPPT_GPIO_BTN_UP       CONFIG_MPPT_GPIO_BTN_UP
#define MPPT_GPIO_BTN_DOWN     CONFIG_MPPT_GPIO_BTN_DOWN
#define MPPT_GPIO_BTN_MENU     CONFIG_MPPT_GPIO_BTN_MENU
#define MPPT_GPIO_RESET        CONFIG_RESET_GPIO
#define MPPT_GPIO_LED          CONFIG_LED_GPIO

/* ---- I2C ---- */
#define MPPT_I2C0_SDA          CONFIG_MPPT_I2C0_SDA
#define MPPT_I2C0_SCL          CONFIG_MPPT_I2C0_SCL
#define MPPT_I2C1_SDA          CONFIG_MPPT_I2C1_SDA
#define MPPT_I2C1_SCL          CONFIG_MPPT_I2C1_SCL
#define MPPT_I2C_FREQ_HZ       CONFIG_MPPT_I2C_FREQ_HZ

/* ---- ADS1115 ---- */
#define MPPT_ADS_ADDR          CONFIG_MPPT_ADS_ADDR
#define MPPT_ADS_CH_VIN        CONFIG_MPPT_ADS_CH_VIN
#define MPPT_ADS_CH_VOUT       CONFIG_MPPT_ADS_CH_VOUT
#define MPPT_ADS_CH_CURRENT    CONFIG_MPPT_ADS_CH_CURRENT
#define MPPT_ADS_CH_TEMP       CONFIG_MPPT_ADS_CH_TEMP
#define MPPT_ADS_PGA_INDEX     CONFIG_MPPT_ADS_PGA_INDEX

/* ---- Calibration (FUGU names kept in the comments) ---- */
#define MPPT_CAL_VIN_RATIO        (CONFIG_MPPT_CAL_VIN_RATIO_X1000 / 1000.0f)      /* inVoltageDivRatio  */
#define MPPT_CAL_VOUT_RATIO       (CONFIG_MPPT_CAL_VOUT_RATIO_X1000 / 1000.0f)     /* outVoltageDivRatio */
#define MPPT_CAL_CURRENT_SCALE    (CONFIG_MPPT_CAL_CURRENT_SCALE_X1000 / 1000.0f)  /* 1.3300             */
#define MPPT_CAL_CURRENT_SENS_V_A (CONFIG_MPPT_CAL_CURRENT_SENS_UV_PER_A / 1e6f)   /* currentSensV       */
#define MPPT_CAL_CURRENT_MID_V    (CONFIG_MPPT_CAL_CURRENT_MIDPOINT_MV / 1000.0f)  /* currentMidPoint    */
#define MPPT_CAL_AVG_VOLTAGE      CONFIG_MPPT_CAL_AVG_VOLTAGE                       /* avgCountVS         */
#define MPPT_CAL_AVG_CURRENT      CONFIG_MPPT_CAL_AVG_CURRENT                       /* avgCountCS         */
#define MPPT_CAL_AVG_TEMP         CONFIG_MPPT_CAL_AVG_TEMP                          /* avgCountTS         */
#define MPPT_CAL_VOLTAGE_DROPOUT  (CONFIG_MPPT_CAL_VOLTAGE_DROPOUT_MV / 1000.0f)    /* voltageDropout     */
#define MPPT_CAL_VBAT_THRESH      (CONFIG_MPPT_CAL_VBAT_THRESH_MV / 1000.0f)        /* voltageBatteryThresh */
#define MPPT_CAL_IIN_ABS          (CONFIG_MPPT_CAL_IIN_ABS_MA / 1000.0f)            /* currentInAbsolute  */
#define MPPT_CAL_IOUT_ABS         (CONFIG_MPPT_CAL_IOUT_ABS_MA / 1000.0f)           /* currentOutAbsolute */
#define MPPT_CAL_VIN_SYS_MIN      (CONFIG_MPPT_CAL_VIN_SYS_MIN_MV / 1000.0f)        /* vInSystemMin       */
#define MPPT_CAL_VOUT_SYS_MAX     (CONFIG_MPPT_CAL_VOUT_SYS_MAX_MV / 1000.0f)       /* vOutSystemMax      */
#define MPPT_CAL_PPWM_MARGIN      (CONFIG_MPPT_CAL_PPWM_MARGIN_PERMILLE / 10.0f)    /* PPWM_margin (%)    */
#define MPPT_CAL_PWM_MAX_DC       (CONFIG_MPPT_CAL_PWM_MAX_DC_PERMILLE / 10.0f)     /* PWM_MaxDC (%)      */
#define MPPT_CAL_EFFICIENCY       (CONFIG_MPPT_CAL_EFFICIENCY_PERMILLE / 1000.0f)   /* efficiencyRate     */

/* ---- PWM / MPPT ---- */
#define MPPT_PWM_FREQ_HZ          CONFIG_MPPT_PWM_FREQ_HZ
#define MPPT_PWM_RESOLUTION_BITS  CONFIG_MPPT_PWM_RESOLUTION_BITS
#define MPPT_STEP                 CONFIG_MPPT_STEP

/* ---- NTC ---- */
#define MPPT_NTC_R25_OHM          CONFIG_MPPT_NTC_R25_OHM
#define MPPT_NTC_BETA             CONFIG_MPPT_NTC_BETA
#define MPPT_NTC_RFIXED_OHM       CONFIG_MPPT_NTC_RFIXED_OHM
#define MPPT_NTC_VDD_MV           CONFIG_MPPT_NTC_VDD_MV
#ifdef CONFIG_MPPT_NTC_TO_VCC
#define MPPT_NTC_TO_VCC           1
#else
#define MPPT_NTC_TO_VCC           0
#endif

/* ---- Timing ---- */
#define MPPT_ROUTINE_INTERVAL_MS  CONFIG_MPPT_ROUTINE_INTERVAL_MS
#define MPPT_LCD_INTERVAL_MS      CONFIG_MPPT_LCD_INTERVAL_MS
#define MPPT_HAP_PUSH_INTERVAL_MS CONFIG_MPPT_HAP_PUSH_INTERVAL_MS
#define MPPT_ERROR_WINDOW_MS      CONFIG_MPPT_ERROR_WINDOW_MS
#define MPPT_ERROR_COUNT_LIMIT    CONFIG_MPPT_ERROR_COUNT_LIMIT
#define MPPT_MENU_TIMEOUT_S       CONFIG_MPPT_MENU_TIMEOUT_S
#define MPPT_WH_PERSIST_MINUTES   CONFIG_MPPT_WH_PERSIST_MINUTES
#define MPPT_OTA_AUTOCHECK_S      CONFIG_MPPT_OTA_AUTOCHECK_S
#define MPPT_TELEMETRY_INTERVAL_MS CONFIG_MPPT_TELEMETRY_INTERVAL_MS

/* ---- Factory defaults for runtime settings ---- */
#define MPPT_DEF_VBAT_MAX         (CONFIG_MPPT_DEF_VBAT_MAX_CV / 100.0f)
#define MPPT_DEF_VBAT_MIN         (CONFIG_MPPT_DEF_VBAT_MIN_CV / 100.0f)
#define MPPT_DEF_CHARGE_CURRENT   (CONFIG_MPPT_DEF_CHARGE_CURRENT_CA / 100.0f)
#define MPPT_DEF_FAN_TEMP_C       CONFIG_MPPT_DEF_FAN_TEMP_C
#define MPPT_DEF_TEMP_MAX_C       CONFIG_MPPT_DEF_TEMP_MAX_C
#define MPPT_DEF_ENERGY_PRICE     (CONFIG_MPPT_DEF_ENERGY_PRICE_CENTS / 100.0f)
#define MPPT_DEF_TELEMETRY_MODE   CONFIG_MPPT_DEF_TELEMETRY_MODE

/* ---- Tasks ---- */
#define MPPT_CONTROL_TASK_PRIO    5
#define MPPT_CONTROL_TASK_STACK   (6 * 1024)
#define MPPT_UI_TASK_PRIO         1
#define MPPT_UI_TASK_STACK        (4 * 1024)
