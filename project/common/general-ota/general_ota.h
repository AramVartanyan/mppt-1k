/*
 * general-ota — HTTPS firmware update for ESP-IDF 5.x without HomeKit ties.
 * Version: 1.0.0
 * Created by Aram Vartanyan, (C) 2026. MIT licence.
 *
 * Derived from fupdateota (ESP32 path): esp_https_ota streaming update with
 * a strict "only newer major.minor.patch" check against the running app
 * descriptor. New here:
 *   - no dependency on the HomeKit SDK (no pairing guard, no HAP status
 *     values); the application decides when an update may run
 *   - a check-only call that fetches the image descriptor and aborts, so a
 *     display can show "up to date" or "vX.Y.Z available" without a download
 *   - events with progress for LCD / LED indication
 *   - server certificates from the mbedTLS certificate bundle
 *     (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y in menuconfig / sdkconfig.defaults)
 *
 * All calls are non-blocking unless named *_blocking; one operation at a time.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GENERAL_OTA_VERSION "1.0.0"
#define GENERAL_OTA_VERSION_LEN 32

typedef enum {
    GENERAL_OTA_IDLE = 0,
    GENERAL_OTA_CHECKING,       /* fetching the image descriptor */
    GENERAL_OTA_DOWNLOADING,    /* writing the new image */
    GENERAL_OTA_UP_TO_DATE,     /* last check: running version is current */
    GENERAL_OTA_AVAILABLE,      /* last check: a newer image is on the server */
    GENERAL_OTA_SUCCESS,        /* image written and validated; reboot pending */
    GENERAL_OTA_FAILED,         /* see general_ota_info_t.error */
} general_ota_state_t;

typedef enum {
    GENERAL_OTA_EVT_CHECK_START = 0,
    GENERAL_OTA_EVT_UP_TO_DATE,
    GENERAL_OTA_EVT_UPDATE_AVAILABLE,  /* new_version valid */
    GENERAL_OTA_EVT_DOWNLOAD_START,
    GENERAL_OTA_EVT_PROGRESS,          /* progress_percent valid (-1 if size unknown) */
    GENERAL_OTA_EVT_SUCCESS,           /* new image ready; reboot to activate */
    GENERAL_OTA_EVT_FAILED,            /* error valid */
} general_ota_event_t;

typedef struct {
    general_ota_event_t event;
    const char *new_version;    /* server image version (may be "" ) */
    int progress_percent;
    esp_err_t error;
} general_ota_info_t;

typedef void (*general_ota_cb_t)(const general_ota_info_t *info, void *ctx);

typedef struct {
    const char *url;            /* NULL → CONFIG_GENERAL_OTA_URL */
    int timeout_ms;             /* 0 → CONFIG_GENERAL_OTA_RECV_TIMEOUT_MS */
    bool skip_version_check;    /* install any valid image (testing) */
    bool auto_reboot;           /* esp_restart() after SUCCESS (+delay) */
    general_ota_cb_t cb;        /* optional, called from the OTA task */
    void *cb_ctx;
} general_ota_config_t;

/* Store the configuration. Call once; safe before Wi-Fi is up. */
esp_err_t general_ota_init(const general_ota_config_t *cfg);

/* Check only: descriptor fetch + version compare, no flash writes.
 * Returns ESP_ERR_INVALID_STATE if another operation is running. */
esp_err_t general_ota_check(void);

/* Check and, if newer (or skip_version_check), download and install.
 * Returns ESP_ERR_INVALID_STATE if another operation is running. */
esp_err_t general_ota_update(void);

/* Synchronous check for simple callers. `version` receives the server
 * version when a newer image exists. ESP_OK = newer available,
 * ESP_ERR_NOT_FOUND = up to date, other = network / parse error. */
esp_err_t general_ota_check_blocking(char *version, size_t len, int timeout_ms);

general_ota_state_t general_ota_get_state(void);
const char *general_ota_running_version(void);     /* from the app descriptor */
const char *general_ota_available_version(void);   /* "" until a check found one */
bool general_ota_busy(void);

/* Reboot into the new image (after GENERAL_OTA_SUCCESS). */
void general_ota_reboot(void);

#ifdef __cplusplus
}
#endif
