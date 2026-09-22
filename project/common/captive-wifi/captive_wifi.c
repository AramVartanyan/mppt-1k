#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "captive_wifi.h"

static const char *TAG = "captive_wifi";

#define NS          "cwifi"
#define KEY_SSID    "ssid"
#define KEY_PASS    "pass"

static captive_wifi_cb_t s_cb;
static void *s_cb_ctx;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static bool s_connected;
static bool s_portal;
static int  s_fail_count;
static char s_ap_ssid[32];

static void emit(captive_wifi_event_t evt)
{
    if (s_cb) {
        s_cb(evt, s_cb_ctx);
    }
}

/* ------------------------------------------------------------ credentials */

static esp_err_t creds_load(wifi_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t l1 = sizeof(cfg->sta.ssid), l2 = sizeof(cfg->sta.password);
    err = nvs_get_str(h, KEY_SSID, (char *)cfg->sta.ssid, &l1);
    if (err == ESP_OK) {
        err = nvs_get_str(h, KEY_PASS, (char *)cfg->sta.password, &l2);
        if (err == ESP_ERR_NVS_NOT_FOUND) {      /* open network */
            cfg->sta.password[0] = 0;
            err = ESP_OK;
        }
    }
    nvs_close(h);
    return err;
}

bool captive_wifi_has_credentials(void)
{
    wifi_config_t cfg = {0};
    return creds_load(&cfg) == ESP_OK && cfg.sta.ssid[0] != 0;
}

esp_err_t captive_wifi_clear_credentials(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_erase_all(h);
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "credentials cleared");
    return err;
}

/* ------------------------------------------------------------------ portal */

static esp_err_t portal_start(void)
{
    /* TODO(phase 4): SoftAP s_ap_ssid (open), DNS catch-all, DHCP option 114,
     * esp_http_server with /, /scan, /connect, /status, stop before HAP. */
    ESP_LOGW(TAG, "portal not implemented yet (would start AP %s)", s_ap_ssid);
    s_portal = true;
    emit(CAPTIVE_WIFI_EVT_PORTAL_STARTED);
    return ESP_OK;
}

/* -------------------------------------------------------------------- STA */

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        esp_netif_create_ip6_linklocal(s_sta_netif);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "connected, IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        s_fail_count = 0;
        emit(CAPTIVE_WIFI_EVT_STA_CONNECTED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_connected) {
            emit(CAPTIVE_WIFI_EVT_STA_DISCONNECTED);
        }
        s_connected = false;
        s_fail_count++;
        ESP_LOGI(TAG, "disconnected (%d), reconnecting", s_fail_count);
        /* TODO(phase 4): after N failures with no IP ever obtained → portal. */
        esp_wifi_connect();
    }
}

esp_err_t captive_wifi_init(const char *hostname, captive_wifi_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (hostname) {
        esp_netif_set_hostname(s_sta_netif, hostname);
    }

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "MPPT-%02X%02X%02X", mac[3], mac[4], mac[5]);

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    return esp_wifi_init(&cfg);
}

esp_err_t captive_wifi_start(void)
{
    wifi_config_t cfg = {0};
    if (creds_load(&cfg) != ESP_OK || cfg.sta.ssid[0] == 0) {
        return portal_start();
    }
    cfg.sta.threshold.authmode = cfg.sta.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;
    ESP_LOGI(TAG, "connecting to %s", cfg.sta.ssid);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    return esp_wifi_start();
}

esp_err_t captive_wifi_stop(void)
{
    s_portal = false;
    esp_err_t err = esp_wifi_stop();
    if (s_connected) {
        s_connected = false;
        emit(CAPTIVE_WIFI_EVT_STA_DISCONNECTED);
    }
    (void)s_ap_netif;
    return err;
}

bool captive_wifi_is_connected(void)
{
    return s_connected;
}

bool captive_wifi_portal_active(void)
{
    return s_portal;
}

const char *captive_wifi_ap_ssid(void)
{
    return s_ap_ssid;
}

esp_err_t captive_wifi_get_ip(char *buf, size_t len)
{
    if (!s_connected || !s_sta_netif) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_netif_ip_info_t ip;
    esp_err_t err = esp_netif_get_ip_info(s_sta_netif, &ip);
    if (err == ESP_OK) {
        snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
    }
    return err;
}

int captive_wifi_rssi(void)
{
    wifi_ap_record_t ap;
    if (!s_connected || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
}
