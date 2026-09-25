/*
 * mppt_control.h — FUGU Device_Protection(), backflowControl(),
 * Charging_Algorithm() and System_Processes(), run from the control task.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create the control task (priority MPPT_CONTROL_TASK_PRIO). The buck stays
 * disabled until the protection logic allows it. */
esp_err_t mppt_control_start(void);

/* Force the power stage off (used by fatal paths and before reboot). */
void mppt_control_emergency_stop(void);

#ifdef __cplusplus
}
#endif
