#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "led_indicator.h"
#include "captive_wifi.h"
#include "mppt_control.h"
#include "mppt_hap.h"
#include "mppt_system.h"

static const char *TAG = "mppt_system";

void mppt_system_reboot(void)
{
    mppt_control_emergency_stop();
    ESP_LOGI(TAG, "rebooting");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

void mppt_system_wifi_reset(void)
{
    ESP_LOGW(TAG, "Wi-Fi reset");
    mppt_control_emergency_stop();
    ledResetNetwork();
    captive_wifi_clear_credentials();
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
}

void mppt_system_hap_reset(void)
{
    ESP_LOGW(TAG, "HomeKit pairing reset");
    mppt_control_emergency_stop();
    if (mppt_hap_reset_pairings() == ESP_OK) {
        /* the SDK reboots asynchronously; fall back if it does not */
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    esp_restart();
}

void mppt_system_factory_reset(void)
{
    ESP_LOGW(TAG, "factory reset");
    mppt_control_emergency_stop();
    ledResetOk();                       /* blocking pattern */
    nvs_flash_erase();                  /* settings, counters, cwifi, HAP data */
    esp_restart();
}
