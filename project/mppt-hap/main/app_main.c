/* MPPT solar charge controller — ESP32-S2, MPPT32 v1.1 (project mppt1hs2)
 *
 * Boot order:
 *   1. power stage forced off (before anything that can take seconds)
 *   2. NVS, settings, shared state
 *   3. hardware (I2C, ADS1115, LCD, LEDC, NTC, status LED)
 *   4. control task, UI task
 *   5. Wi-Fi (captive-wifi) and HomeKit only if enabled in the settings
 */

#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <nvs_flash.h>

#include "iot_button.h"
#include "outputwrite.h"
#include "led_indicator.h"
#include "container_nvs.h"
#include "general_ota.h"
#include "captive_wifi.h"
#include <hap.h>

#include "mppt_config.h"
#include "mppt_state.h"
#include "mppt_settings.h"
#include "mppt_hal.h"
#include "mppt_sensors.h"
#include "mppt_control.h"
#include "mppt_lcd.h"
#include "mppt_hap.h"

static const char *TAG = "app_main";

/* Reset network credentials if button is pressed for more than 3 seconds and then released */
#define RESET_NETWORK_BUTTON_TIMEOUT        3
/* Reset to factory if button is pressed and held for more than 10 seconds */
#define RESET_TO_FACTORY_BUTTON_TIMEOUT     10

static char s_name[16];
static char s_serial[16];

/* Wi-Fi connection status → LED base level and shared state
 * (kept as a plain function so HomeKit-side code can call it too). */
void TakeStatusConnected(bool status)
{
    ledSteady(status);
    mppt_state_lock();
    mppt_state_get()->wifi_connected = status;
    mppt_state_unlock();
}

static void ota_event(const general_ota_info_t *info, void *ctx)
{
    (void)ctx;
    switch (info->event) {
    case GENERAL_OTA_EVT_DOWNLOAD_START:
        ledOtaStatus(true);
        mppt_ui_message("FW update", info->new_version, 0);
        break;
    case GENERAL_OTA_EVT_UPDATE_AVAILABLE:
        mppt_ui_message("FW available", info->new_version, 3000);
        break;
    case GENERAL_OTA_EVT_UP_TO_DATE:
        mppt_ui_message("FW up to date", general_ota_running_version(), 3000);
        break;
    case GENERAL_OTA_EVT_SUCCESS:
        ledOtaStatus(false);
        mppt_ui_message("FW updated", "rebooting...", 0);
        mppt_control_emergency_stop();
        general_ota_reboot();
        break;
    case GENERAL_OTA_EVT_FAILED:
        ledOtaStatus(false);
        mppt_ui_message("FW update", "failed", 3000);
        break;
    default:
        break;
    }
}

static void wifi_event(captive_wifi_event_t evt, void *ctx)
{
    (void)ctx;
    switch (evt) {
    case CAPTIVE_WIFI_EVT_STA_CONNECTED:
        TakeStatusConnected(true);
        break;
    case CAPTIVE_WIFI_EVT_STA_DISCONNECTED:
        TakeStatusConnected(false);
        break;
    case CAPTIVE_WIFI_EVT_PORTAL_STARTED:
        ledBreathe(true);
        mppt_ui_message("WiFi setup AP:", captive_wifi_ap_ssid(), 0);
        break;
    case CAPTIVE_WIFI_EVT_PORTAL_STOPPED:
        ledBreathe(false);
        break;
    }
    mppt_state_lock();
    mppt_state_get()->portal_active = captive_wifi_portal_active();
    mppt_state_unlock();
}

static void reset_network_handler(void *arg)
{
    (void)arg;
    ledResetNetwork();
    captive_wifi_clear_credentials();
    hap_reset_network();
}

static void reset_to_factory_handler(void *arg)
{
    (void)arg;
    mppt_control_emergency_stop();
    mppt_settings_erase();
    captive_wifi_clear_credentials();
    ledResetOk();
    hap_reset_to_factory();
}

static void reset_key_init(uint32_t key_gpio_pin)
{
    button_handle_t handle = iot_button_create(key_gpio_pin, BUTTON_ACTIVE_LOW);
    iot_button_add_on_release_cb(handle, RESET_NETWORK_BUTTON_TIMEOUT, reset_network_handler, NULL);
    iot_button_add_on_press_cb(handle, RESET_TO_FACTORY_BUTTON_TIMEOUT, reset_to_factory_handler, NULL);
}

static void identity_init(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_name, sizeof(s_name), CONFIG_NAME, mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "device %s, serial %s, firmware %s", s_name, s_serial, CONFIG_APP_PROJECT_VER);
}

void app_main(void)
{
    /* 1. safety first */
    mppt_hal_safe_outputs();

    /* 2. storage and state */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ContainerNvsInit();
    mppt_state_init();
    mppt_settings_init();
    identity_init();

    mppt_settings_t cfg;
    mppt_settings_get(&cfg);
    mppt_state_lock();
    mppt_state_get()->wifi_enabled = cfg.wifi_enabled;
    mppt_state_unlock();

    /* 3. hardware */
    LedConfig led_cfg = {
        .gpio = MPPT_GPIO_LED,
#ifdef CONFIG_LED_ACTIVE_LOW
        .activeLow = true,
#else
        .activeLow = false,
#endif
        .ledcTimer = 1,          /* timer 0 / channel 0 belong to the buck PWM */
        .ledcChannel = 1,
    };
    ledInit(&led_cfg);
    general_ota_config_t ota_cfg = { .cb = ota_event };
    general_ota_init(&ota_cfg);

    ESP_ERROR_CHECK(mppt_hal_init());
    mppt_sensors_init();

    /* 4. tasks */
    ESP_ERROR_CHECK(mppt_control_start());
    ESP_ERROR_CHECK(mppt_ui_start());
    reset_key_init(MPPT_GPIO_RESET);

    /* 5. connectivity */
    if (cfg.wifi_enabled) {
        ESP_ERROR_CHECK(captive_wifi_init(s_name, wifi_event, NULL));
        captive_wifi_start();
        if (cfg.hap_enabled) {
            if (mppt_hap_start(s_name, s_serial) != ESP_OK) {
                ESP_LOGW(TAG, "HomeKit not started");
            }
        }
    } else {
        ESP_LOGI(TAG, "Wi-Fi disabled in settings");
    }
}
