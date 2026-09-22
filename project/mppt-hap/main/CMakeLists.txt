idf_component_register(
    SRCS
        "app_main.c"
        "mppt_state.c"
        "mppt_hal.c"
        "mppt_sensors.c"
        "mppt_control.c"
        "mppt_settings.c"
        "mppt_lcd.c"
        "mppt_telemetry.c"
        "mppt_hap.c"
    INCLUDE_DIRS "."
    REQUIRES
        # ESP-IDF
        driver esp_adc esp_timer nvs_flash esp_wifi esp_event esp_netif app_update
        # HomeKit SDK
        esp_hap_core esp_hap_apple_profiles esp_hap_platform app_hap_setup_payload
        # project/common
        button outputwrite fupdateota container_nvs esp32-ads1115 captive-wifi
        # managed components (idf_component.yml)
        ntc_driver hd44780)
