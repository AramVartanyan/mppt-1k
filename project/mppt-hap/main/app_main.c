/* HomeKit MPPT ESP32-S2 */

#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_event.h>
#include <esp_log.h>
//#include <driver/gpio.h>
#include <esp_mac.h>

#include <hap.h>
#include <hap_apple_servs.h>
#include <hap_apple_chars.h>
#include <app_hap_setup_payload.h>

//#include <hap_otaupdate.h>
#include "iot_button.h"

#include "app_wifi.h"
#include "outputwrite.h"
#include "fupdateota.h"

static const char *TAG = "HAP Accessory";

#define ACCESSORY_TASK_PRIORITY  2
#define ACCESSORY_TASK_STACKSIZE 4 * 1024
#define ACCESSORY_TASK_NAME      "hap_accessory"

/* Reset network credentials if button is pressed for more than 3 seconds and then released */
#define RESET_NETWORK_BUTTON_TIMEOUT        3

/* Reset to factory if button is pressed and held for more than 10 seconds */
#define RESET_TO_FACTORY_BUTTON_TIMEOUT     10

#define NO_FAULT        0
#define GENERAL_FAULT   1

#define BATT_NORMAL     0
#define BATT_LOW        1

#define NOT_CHARGING    0
#define CHARGING        1
#define NOT_CHARGEABLE  2

/** Custom UUIDs */
#define HAP_CHAR_CUSTOM_UUID_FW_UPG_STATUS  "d5703b5e-3736-11e8-b467-0ed5f89f718b"
#define HAP_CHAR_CUSTOM_UUID_TROTA          "F0090013-079E-11e8-8F27-9C2605A29F52"
#define HAP_CHAR_CUSTOM_UUID_DISPLAY        "F0090017-079E-11e8-8F27-9C2605A29F52"

static hap_char_t *TriggerUpdateChar;
static hap_char_t *UpdateStatusChar;
static hap_char_t *DisplayOnChar;

//extern fupdateota_status_t otaUpdateStatus = FW_UPG_STATUS_IDLE;

static struct {
    bool OnStateM;
    bool DisplayM;
    bool StatusConnected;
    bool FirmwareUpdate;
    bool DeviceWorkingState;
    bool SystemError;
} container;

int hap_keystore_init();
int hap_keystore_set(const char *name_space, const char *key, const uint8_t *val, const size_t val_len);
int hap_keystore_get(const char *name_space, const char *key, uint8_t *val, size_t *val_size);
int hap_keystore_delete(const char *name_space, const char *key);

void TakeStatusConnected(bool status) {
    container.StatusConnected = status;
    OutputWrite(container.StatusConnected, CONFIG_LED_GPIO, true);
    if (container.StatusConnected){
        //tm1637_print_string(Display, "WiFi"); //0x6A, 0x04, 0x71, 0x04
    } else {
        if (container.DisplayM) {
            //tm1637_print_string(Display, "Lost"); //0x38, 0x5C, 0x6D, 0x78
        }
    }
}

void otaStatus(void *p) {
    
    hap_val_t otaState;
    
    //workaround
    //container.StatusConnected = true;
    
    otaState.i = otaUpdateStatus;
    hap_char_update_val(UpdateStatusChar, &otaState);
    
    if (otaUpdateStatus == -1) {
        //update Failed
    } else {
        while (otaUpdateStatus) {
            OutputWrite(!container.StatusConnected, CONFIG_LED_GPIO, false);
            vTaskDelay(500 / portTICK_PERIOD_MS);
            OutputWrite(container.StatusConnected, CONFIG_LED_GPIO, false);
            vTaskDelay(500 / portTICK_PERIOD_MS);
            
            if (otaUpdateStatus == 2) {
                otaState.i = otaUpdateStatus;
                hap_char_update_val(UpdateStatusChar, &otaState);
                
                vTaskDelay(5 * 1000 / portTICK_PERIOD_MS);
                hap_reboot_accessory();
            }
        }
    }
    
    hap_val_t Trigger;
    container.FirmwareUpdate = false;
    Trigger.b = container.FirmwareUpdate;
    hap_char_update_val(TriggerUpdateChar, &Trigger);
    
    otaState.i = otaUpdateStatus;
    hap_char_update_val(UpdateStatusChar, &otaState);
    
    vTaskDelay(3 * 1000 / portTICK_PERIOD_MS);
    
    vTaskDelete(NULL);
}

static int ReadBattery(void) {
    int BatteryLevel = 31;
    return BatteryLevel;
}

static float CalculateSolarPower(void){
    float SolarPowr = 25.0;
    return SolarPowr;
}

/*
static int read_battery() {
    int BatteryLevel = 0;
    uint16_t voltage = 0;
    if (ESP_OK == adc_read(&voltage)) {
        ESP_LOGI(TAG, "Voltage read: %d\r\n", voltage);
    }
    //Measure the voltage at TOUT pin (A0) - 1V max
    //ADC voltage is %.3f", 1.0 / 1024 * sdk_system_adc_read()
    voltage = (1000.0 / 1024) * voltage; //mV
    BatteryLevel = 1.1 * (voltage - VOLTAGE_CAL);
    
    if (BatteryLevel > 99) {
        BatteryLevel = 100;
    } else if (BatteryLevel < 1) {
        BatteryLevel = 0;
    }
    return BatteryLevel;
}
 */

/**
 * @brief The network reset button callback handler.
 * Useful for testing the Wi-Fi re-configuration feature of WAC2
 */
static void reset_network_handler(void* arg)
{
    hap_reset_network();
}
/**
 * @brief The factory reset button callback handler.
 */
static void reset_to_factory_handler(void* arg)
{
    hap_reset_to_factory();
}

/**
 * The Reset button  GPIO initialisation function.
 * Same button will be used for resetting Wi-Fi network as well as for reset to factory based on
 * the time for which the button is pressed.
 */
static void reset_key_init(uint32_t key_gpio_pin)
{
    button_handle_t handle = iot_button_create(key_gpio_pin, BUTTON_ACTIVE_LOW);
    iot_button_add_on_release_cb(handle, RESET_NETWORK_BUTTON_TIMEOUT, reset_network_handler, NULL);
    iot_button_add_on_press_cb(handle, RESET_TO_FACTORY_BUTTON_TIMEOUT, reset_to_factory_handler, NULL);
}

/**
 * Enable a GPIO Pin for Accessory
 */
static void InputOutputInit(uint32_t key_gpio_pin)
{
    gpio_config_t io_conf;

    io_conf.pin_bit_mask = 1ULL << key_gpio_pin;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);
}

/**
 * Initialize the Smart Accessory Hardware.
 */
static void InitializePlatform(void) {
    InputOutputInit(CONFIG_LED_GPIO);
    container.DeviceWorkingState = true;
    container.SystemError = NO_FAULT;
}

/* Mandatory identify routine for the accessory.
 * In a real accessory, something like LED blink should be implemented
 * got visual identification
 */
static int AccessoryIdentify(hap_acc_t *ha)
{
    ESP_LOGI(TAG, "Accessory identified");
    return HAP_SUCCESS;
}

static int AccessoryRead(hap_char_t *hc, hap_status_t *status_code, void *serv_priv, void *read_priv)
{
    int ret = HAP_SUCCESS;
    if (hap_req_get_ctrl_id(read_priv)) {
        ESP_LOGI(TAG, "Received read from %s", hap_req_get_ctrl_id(read_priv));
    } //returns the HAP controller ID

    if (!strcmp(hap_char_get_type_uuid(hc), HAP_CHAR_UUID_ON)) {
        const hap_val_t *cur_val = hap_char_get_val(hc);
        ESP_LOGI(TAG, "Current Switch value is %s", cur_val->b ? "On" : "Off");
        hap_val_t SwitchState = {.b = container.OnStateM};
        hap_char_update_val(hc, &SwitchState);
        *status_code = HAP_STATUS_SUCCESS;
    } else if (!strcmp(hap_char_get_type_uuid(hc), HAP_CHAR_CUSTOM_UUID_DISPLAY)) {
        const hap_val_t *cur_val = hap_char_get_val(hc);
        ESP_LOGI(TAG, "Currently the display Light is %s", cur_val->b ? "Enabled" : "Disabled");
        hap_val_t Display = {.b = container.DisplayM};
        hap_char_update_val(hc, &Display);
        *status_code = HAP_STATUS_SUCCESS;
    } else if (!strcmp(hap_char_get_type_uuid(hc), HAP_CHAR_CUSTOM_UUID_FW_UPG_STATUS)) {
        const hap_val_t *cur_val = hap_char_get_val(hc);
        ESP_LOGI(TAG, "Previous value was %d", cur_val->i);
        hap_val_t UpdateStatus = {.i = otaUpdateStatus};
        ESP_LOGI(TAG, "Controller reads the Update status %d", UpdateStatus.i);
        hap_char_update_val(hc, &UpdateStatus);
        *status_code = HAP_STATUS_SUCCESS;
    } else if (!strcmp(hap_char_get_type_uuid(hc), HAP_CHAR_CUSTOM_UUID_TROTA)) {
        const hap_val_t *cur_val = hap_char_get_val(hc);
        ESP_LOGI(TAG, "Currently the OTA is %s", cur_val->b ? "Triggered" : "Inactive");
        hap_val_t FirmwareUpdateTrigger = {.b = container.FirmwareUpdate};
        hap_char_update_val(hc, &FirmwareUpdateTrigger);
        *status_code = HAP_STATUS_SUCCESS;
    } else {
        *status_code = HAP_STATUS_RES_ABSENT;
        ret = HAP_FAIL;
    }
    return ret;
}

static int AccessoryWrite(hap_write_data_t write_data[], int count,
        void *serv_priv, void *write_priv)
{
    int i, ret = HAP_SUCCESS;
    hap_write_data_t *write;
    for (i = 0; i < count; i++) {
        write = &write_data[i];
        if (!strcmp(hap_char_get_type_uuid(write->hc), HAP_CHAR_UUID_ON)) {
            ESP_LOGI(TAG, "Received Write. Switch %s", write->val.b ? "On" : "Off");
            /* TODO: Control Actual Hardware */
            
            container.OnStateM = write->val.b;
            OutputWrite(container.OnStateM, CONFIG_LED_GPIO, false);
            
            hap_char_update_val(write->hc, &(write->val));
            *(write->status) = HAP_STATUS_SUCCESS;
        } else if (!strcmp(hap_char_get_type_uuid(write->hc), HAP_CHAR_CUSTOM_UUID_DISPLAY)) {
            ESP_LOGI(TAG, "The embeded display is %s", write->val.b ? "Enabled" : "Disabled");
            
            container.DisplayM = write->val.b;
            ret = hap_keystore_set("switch", "display", (uint8_t *)&write->val.b, sizeof(uint8_t));
            if (ret != HAP_SUCCESS) {
                ESP_LOGE(TAG, "Could not write Display state");
            }
            
            hap_char_update_val(write->hc, &(write->val));
            *(write->status) = HAP_STATUS_SUCCESS;
            
        } else if (!strcmp(hap_char_get_type_uuid(write->hc), HAP_CHAR_CUSTOM_UUID_TROTA)) {
            ESP_LOGI(TAG, "The OTA update is %s", write->val.b ? "Triggered" : "Inactive");
            
            container.FirmwareUpdate = write->val.b;
            
            if (container.FirmwareUpdate) {

                if (otaUpdateStatus != FW_UPG_STATUS_UPGRADING) {
                    otaUpdate();
                    vTaskDelay(500 / portTICK_PERIOD_MS);
                    xTaskCreate(otaStatus, "otaState", 2*1024, NULL, 3, NULL);
                }
            }
            
            hap_char_update_val(write->hc, &(write->val));
            *(write->status) = HAP_STATUS_SUCCESS;
            
        } else {
            *(write->status) = HAP_STATUS_RES_ABSENT;
        }
    }
    return ret;
}

static int BatteryRead(hap_char_t *hc, hap_status_t *status, void *serv_priv, void *read_priv)
{
    int ret = HAP_SUCCESS;
    if (!strcmp(hap_char_get_type_uuid(hc), HAP_CHAR_UUID_BATTERY_LEVEL)) {
        const hap_val_t *cur_val = hap_char_get_val(hc);
        hap_val_t BatteryLevelValue;
        BatteryLevelValue.i = ReadBattery();
        ESP_LOGI(TAG, "The battery level was %d", cur_val->i);
        ESP_LOGI(TAG, "The read battery level is %d", BatteryLevelValue.i);
        hap_char_update_val(hc, &BatteryLevelValue);
        *status = HAP_STATUS_SUCCESS;
    } else if (!strcmp(hap_char_get_type_uuid(hc), HAP_CHAR_UUID_CHARGING_STATE)) {
        hap_val_t ChargingState;
        //ChargingState.i = !gpio_get_level(CHARGE_GPIO);
        if (container.DeviceWorkingState) {
           ChargingState.i = NOT_CHARGING;
        } else {
           ChargingState.i = CHARGING;
        }
        
        //NOT_CHARGEABLE  2
        //In case in the menu, the battery is set as none
        
        ESP_LOGI(TAG, "Currently the battery is %s", ChargingState.i ? "Charging" : "Not charging");
        hap_char_update_val(hc, &ChargingState);
        *status = HAP_STATUS_SUCCESS;
    } else if (!strcmp(hap_char_get_type_uuid(hc), HAP_CHAR_UUID_STATUS_LOW_BATTERY)) {
        hap_val_t LowBatteryState;
        hap_val_t BatteryLevelValue;
        BatteryLevelValue.i = ReadBattery();
        if (BatteryLevelValue.i < 20) {
           LowBatteryState.i = BATT_LOW;
        } else {
           LowBatteryState.i = BATT_NORMAL;
        }
        ESP_LOGI(TAG, "Low battery %s", LowBatteryState.i ? "Yes" : "No");
        hap_char_update_val(hc, &LowBatteryState);
        *status = HAP_STATUS_SUCCESS;
    } else {
        *status = HAP_STATUS_RES_ABSENT;
        ret = HAP_FAIL;
    }
    return ret;
}

static int LightSensorRead(hap_char_t *hc, hap_status_t *status, void *serv_priv, void *read_priv)
{
    int ret = HAP_SUCCESS;
    if (!strcmp(hap_char_get_type_uuid(hc), HAP_CHAR_UUID_CURRENT_AMBIENT_LIGHT_LEVEL)) {
        const hap_val_t *cur_val = hap_char_get_val(hc);
        hap_val_t AmbientLightLevel = {.f = CalculateSolarPower()};
        ESP_LOGI(TAG, "The previous Solar power was %f", cur_val->f);
        ESP_LOGI(TAG, "The current Solar power is %f", AmbientLightLevel.f);
        hap_char_update_val(hc, &AmbientLightLevel);
        *status = HAP_STATUS_SUCCESS;
    } else if (!strcmp(hap_char_get_type_uuid(hc), HAP_CHAR_UUID_STATUS_ACTIVE)) {
        hap_val_t StatusActive;
        StatusActive.b = container.DeviceWorkingState;
        ESP_LOGI(TAG, "The Status of Solar panel is %s", StatusActive.b ? "Active" : "Not active");
        hap_char_update_val(hc, &StatusActive);
        *status = HAP_STATUS_SUCCESS;
    } else if (!strcmp(hap_char_get_type_uuid(hc), HAP_CHAR_UUID_STATUS_FAULT)) {
        hap_val_t StatusFault;
        if (container.SystemError) {
            StatusFault.i = GENERAL_FAULT;
        } else {
            StatusFault.i = NO_FAULT;
        }
        ESP_LOGI(TAG, "Status Fault %s", StatusFault.i ? "Yes" : "No");
        hap_char_update_val(hc, &StatusFault);
        *status = HAP_STATUS_SUCCESS;
    } else {
        *status = HAP_STATUS_RES_ABSENT;
        ret = HAP_FAIL;
    }
    return ret;
}

/*The main thread for handling the Smart Accessory Accessory */
static void AccessoryThreadEntry(void *p)
{
    char AccessoryName[16];
    char WiFiMacAddress[16];
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    sprintf(WiFiMacAddress, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "Device WIFI mac_address: %s", WiFiMacAddress);

    sprintf(AccessoryName, CONFIG_NAME, mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG,"Device Name: %s", AccessoryName);
    
    hap_acc_t *accessory;
    hap_serv_t *service;
    
    //hap_cfg_t hap_cfg;
    //hap_get_config(&hap_cfg);
    //hap_cfg.unique_param = UNIQUE_NAME;
    //hap_set_config(&hap_cfg);

    /* Initialize the HAP core */
    hap_init(HAP_TRANSPORT_WIFI);

    /* Initialise the mandatory parameters for Accessory which will be added as
     * the mandatory services internally
     */
    hap_acc_cfg_t cfg = {
        .name = AccessoryName,
        .manufacturer = CONFIG_MANIFACTURER,
        .model = CONFIG_MODEL,
        .serial_num = WiFiMacAddress,
        .fw_rev = CONFIG_APP_PROJECT_VER,
        .hw_rev = CONFIG_HW,
        .pv = "1.1.0",
        .identify_routine = AccessoryIdentify,
        .cid = HAP_CID_SWITCH,
    };
    /* Create accessory object */
    accessory = hap_acc_create(&cfg);

    /* Add a dummy Product Data */
    uint8_t product_data[] = {'M','P','P','T','C','T','R','L'};
    hap_acc_add_product_data(accessory, product_data, sizeof(product_data));

    /* Add Wi-Fi Transport service required for HAP Spec R16 */
    hap_acc_add_wifi_transport_service(accessory, 0);

    //HAP_SERV_UUID_SWITCH
    service = hap_serv_switch_create(false);
    hap_serv_add_char(service, hap_char_name_create(AccessoryName));

    //RelayState = hap_serv_get_char_by_uuid(service, HAP_CHAR_UUID_ON);
    
    DisplayOnChar = hap_char_bool_create(HAP_CHAR_CUSTOM_UUID_DISPLAY, HAP_CHAR_PERM_PR | HAP_CHAR_PERM_PW | HAP_CHAR_PERM_EV, 0);
    hap_serv_add_char(service, DisplayOnChar);
    hap_char_add_description(DisplayOnChar, "Display Light");
    
    TriggerUpdateChar = hap_char_bool_create(HAP_CHAR_CUSTOM_UUID_TROTA, HAP_CHAR_PERM_PR | HAP_CHAR_PERM_PW | HAP_CHAR_PERM_EV, 0);
    hap_serv_add_char(service, TriggerUpdateChar);
    hap_char_add_description(TriggerUpdateChar, "Firmware Update");
    
    UpdateStatusChar = hap_char_int_create(HAP_CHAR_CUSTOM_UUID_FW_UPG_STATUS, HAP_CHAR_PERM_PR | HAP_CHAR_PERM_EV, 0);
    hap_serv_add_char(service, UpdateStatusChar);
    hap_char_add_description(UpdateStatusChar, "FW Update Status");
    
    /* Set the write callback for the service */
    hap_serv_set_write_cb(service, AccessoryWrite);

    /* Set the read callback for the service (optional) */
    hap_serv_set_read_cb(service, AccessoryRead);
    
    /* Add the Accessory Service to the Accessory Object */
    hap_acc_add_serv(accessory, service);
    
    //Create the Battery service
    
    //HAP_SERV_UUID_BATTERY_SERVICE
    //hap_serv_t *hap_serv_battery_service_create(uint8_t battery_level, uint8_t charging_state, uint8_t status_low_battery);
    service = hap_serv_battery_service_create(50, NOT_CHARGING, BATT_NORMAL); //BatteryLevelValue.i, ChargingState.i, LowBatteryState.i
        
    //HAP_CHAR_UUID_BATTERY_LEVEL
    //HAP_CHAR_UUID_CHARGING_STATE
    //HAP_CHAR_UUID_STATUS_LOW_BATTERY
    hap_serv_set_read_cb(service, BatteryRead);
    hap_acc_add_serv(accessory, service);
    
    //Create and Add characteristics to the Light sensor service
    
    //HAP_SERV_UUID_LIGHT_SENSOR
    //hap_serv_t *hap_serv_light_sensor_create(float curr_ambient_light_level);
    service = hap_serv_light_sensor_create(100.0);
    
    //hap_char_t *hap_char_status_active_create(bool status_active);
    hap_serv_add_char(service, hap_char_status_active_create(true));
    //hap_char_t *hap_char_status_fault_create(uint8_t status_fault);
    hap_serv_add_char(service, hap_char_status_fault_create(NO_FAULT));
    
    //HAP_CHAR_UUID_CURRENT_AMBIENT_LIGHT_LEVEL
    //HAP_CHAR_UUID_STATUS_ACTIVE
    //HAP_CHAR_UUID_STATUS_FAULT
    hap_serv_set_read_cb(service, LightSensorRead);
    hap_acc_add_serv(accessory, service);
    
    /* Create the Firmware Upgrade HomeKit Custom Service.
     * Please refer the FW Upgrade documentation under components/homekit/extras/include/hap_fw_upgrade.h
     * and the top level README for more information.
     */
    
    /* Add the Accessory to the HomeKit Database */
    hap_add_accessory(accessory);
    
    /* Register a common button for reset Wi-Fi network and reset to factory.
     */
    reset_key_init(CONFIG_RESET_GPIO);

    /* Query the controller count (just for information) */
    ESP_LOGI(TAG, "Accessory is paired with %d controllers",
                hap_get_paired_controller_count());

    /* Initialize the appliance specific hardware. */
    InitializePlatform();

    /* For production accessories, the setup code shouldn't be programmed on to
     * the device. Instead, the setup info, derived from the setup code must
     * be used. Use the factory_nvs_gen utility to generate this data and then
     * flash it into the factory NVS partition.
     *
     * By default, the setup ID and setup info will be read from the factory_nvs
     * Flash partition and so, is not required to set here explicitly.
     *
     * However, for testing purpose, this can be overridden by using hap_set_setup_code()
     * and hap_set_setup_id() APIs, as has been done here.
     */
#ifdef CONFIG_USE_HARDCODED_SETUP_CODE
    hap_set_setup_code(CONFIG_SETUP_CODE);
    hap_set_setup_id(CONFIG_SETUP_ID);
#ifdef CONFIG_APP_WIFI_USE_WAC_PROVISIONING
    app_hap_setup_payload(CONFIG_SETUP_CODE, CONFIG_SETUP_ID, true, cfg.cid);
#else
    app_hap_setup_payload(CONFIG_SETUP_CODE, CONFIG_SETUP_ID, false, cfg.cid);
#endif
#endif

    /* Enable Hardware MFi authentication (applicable only for MFi variant of SDK) */
    //hap_enable_mfi_auth(HAP_MFI_AUTH_HW);

    /* Initialize Wi-Fi */
    app_wifi_init(AccessoryName);
    
    /* After all the initializations are done, start the HAP core */
    hap_start();
    /* Start Wi-Fi */
    app_wifi_start(portMAX_DELAY);

    /* The task ends here. The read/write callbacks will be invoked by the HAP Framework */
    vTaskDelete(NULL);
}

void app_main()
{
    /* Create the application thread */
    xTaskCreate(AccessoryThreadEntry, ACCESSORY_TASK_NAME, ACCESSORY_TASK_STACKSIZE,
                NULL, ACCESSORY_TASK_PRIORITY, NULL);
}

