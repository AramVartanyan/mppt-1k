/*
 * mppt_hap.h — HomeKit accessory (Switch "Charger" + custom characteristics,
 * Battery, Temperature Sensor, Fan). Started only when the HAP setting is on
 * and Wi-Fi is up.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build the accessory database and start the HAP core. `name` is the
 * accessory name, `serial` the Wi-Fi MAC string. */
esp_err_t mppt_hap_start(const char *name, const char *serial);

/* Push changed values to paired controllers (called from the UI task). */
void mppt_hap_push(void);

bool mppt_hap_is_running(void);

#ifdef __cplusplus
}
#endif
