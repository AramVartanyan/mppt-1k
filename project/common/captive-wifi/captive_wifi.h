/*
 * captive-wifi — Wi-Fi station management with captive-portal fallback for
 * ESP-IDF 5.x. Created by Aram Vartanyan, (C) 2026. MIT licence.
 *
 * Boot: credentials in NVS → STA with reconnect; none, or too many failed
 * attempts → open SoftAP with a captive portal (DNS catch-all + DHCP option
 * 114) serving a page that scans networks and stores the chosen credentials.
 * Follows the idea of tonyp7/esp32-wifi-manager; structure from the ESP-IDF
 * captive_portal example. Uses driver-independent ESP-IDF APIs only.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAPTIVE_WIFI_VERSION "0.1.0"

typedef enum {
    CAPTIVE_WIFI_EVT_STA_CONNECTED = 0,   /* got IP */
    CAPTIVE_WIFI_EVT_STA_DISCONNECTED,
    CAPTIVE_WIFI_EVT_PORTAL_STARTED,      /* SoftAP + portal up */
    CAPTIVE_WIFI_EVT_PORTAL_STOPPED,
} captive_wifi_event_t;

typedef void (*captive_wifi_cb_t)(captive_wifi_event_t evt, void *ctx);

/* esp_netif, default event loop, Wi-Fi driver, STA netif with `hostname`. */
esp_err_t captive_wifi_init(const char *hostname, captive_wifi_cb_t cb, void *ctx);

/* STA when credentials exist, otherwise the portal. Non-blocking. */
esp_err_t captive_wifi_start(void);

/* Stop Wi-Fi entirely (used when the Wi-Fi setting is switched off). */
esp_err_t captive_wifi_stop(void);

bool        captive_wifi_has_credentials(void);
esp_err_t   captive_wifi_clear_credentials(void);   /* next start → portal */
bool        captive_wifi_is_connected(void);
bool        captive_wifi_portal_active(void);
const char *captive_wifi_ap_ssid(void);             /* "MPPT-xxxxxx" */
esp_err_t   captive_wifi_get_ip(char *buf, size_t len);
int         captive_wifi_rssi(void);                /* dBm, 0 when not connected */

#ifdef __cplusplus
}
#endif
