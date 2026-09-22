/*
 * mppt_telemetry.h — FUGU Onboard_Telemetry(): one log line per interval in
 * the selected mode (0 off, 1 all, 2 essential, 3 numbers only).
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void mppt_telemetry_log(void);

#ifdef __cplusplus
}
#endif
