# general-ota

HTTPS firmware update component for ESP-IDF 5.x, without HomeKit dependencies.

Derived from `fupdateota` (ESP32 path). Differences:

- no pairing guard and no HomeKit status values; the application decides when an update may
  run and shows the result on its own display / LED
- `general_ota_check()` fetches only the image descriptor and aborts: "up to date" or
  "vX.Y.Z available" without downloading the image
- events with progress percentage for indication
- certificates from the mbedTLS bundle (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`)

Only a strictly newer `major.minor.patch` than the running app descriptor
(`CONFIG_APP_PROJECT_VER`) is installed.

```c
static void ota_cb(const general_ota_info_t *i, void *ctx) { /* LCD / LED */ }

general_ota_config_t cfg = { .cb = ota_cb };
general_ota_init(&cfg);
general_ota_check();     // → GENERAL_OTA_EVT_UP_TO_DATE / _UPDATE_AVAILABLE
general_ota_update();    // → ... GENERAL_OTA_EVT_SUCCESS, then general_ota_reboot()
```

`menuconfig → General OTA`: URL, timeout, CN check, reboot delay.
