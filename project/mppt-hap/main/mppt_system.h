/*
 * mppt_system.h — reboot and the three reset levels, shared by the IO0
 * button handlers and the LCD Device Setup menu. Every path stops the power
 * stage first and never calls into the HomeKit SDK unless it is running.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Stop the power stage, let the LCD/log settle, esp_restart(). */
void mppt_system_reboot(void);

/* Erase Wi-Fi credentials only (HAP pairing and settings kept), reboot. */
void mppt_system_wifi_reset(void);

/* Remove HomeKit pairings only (Wi-Fi and settings kept), reboot.
 * No-op reboot when HAP is not running. */
void mppt_system_hap_reset(void);

/* Erase the whole NVS partition (settings, counters, Wi-Fi, HAP data), reboot. */
void mppt_system_factory_reset(void);

#ifdef __cplusplus
}
#endif
