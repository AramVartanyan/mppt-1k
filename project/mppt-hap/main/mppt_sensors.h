/*
 * mppt_sensors.h — FUGU Read_Sensors(): voltages, current, temperature,
 * power, SOC, energy accounting. Called from the control task every cycle.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load persisted counters, reset averaging state. */
esp_err_t mppt_sensors_init(void);

/* One measurement cycle; updates mppt_state. */
void mppt_sensors_read(void);

/* FUGU resetVariables(): energy and time counters. */
void mppt_sensors_reset_counters(void);

#ifdef __cplusplus
}
#endif
