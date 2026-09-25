#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "container_nvs.h"
#include "mppt_config.h"
#include "mppt_settings.h"

static const char *TAG = "mppt_settings";

#define NS              "mppt"
#define KEY_SETTINGS    "settings"
#define KEY_COUNTERS    "counters"
#define SETTINGS_VERSION 1

typedef struct {
    double   wh;
    uint32_t run_seconds;
} counters_blob_t;

static mppt_settings_t s_settings;
static SemaphoreHandle_t s_mutex;

void mppt_settings_defaults(mppt_settings_t *o)
{
    memset(o, 0, sizeof(*o));
    o->version = SETTINGS_VERSION;
    o->charging_enabled = true;
    o->output_mode_charger = true;
    o->mppt_mode = true;
    o->preset = MPPT_BATT_NONE;
    o->vbat_max = MPPT_DEF_VBAT_MAX;
    o->vbat_min = MPPT_DEF_VBAT_MIN;
    o->i_charge = MPPT_DEF_CHARGE_CURRENT;
    o->fan_enabled = true;
    o->fan_temp_c = MPPT_DEF_FAN_TEMP_C;
    o->temp_max_c = MPPT_DEF_TEMP_MAX_C;
    o->wifi_enabled = false;
    o->hap_enabled = false;
    o->lcd_backlight = true;
    o->backlight_sleep = 0;
    o->counter_reset = 0;
    o->telemetry_mode = MPPT_DEF_TELEMETRY_MODE;
    o->energy_price = MPPT_DEF_ENERGY_PRICE;
}

bool mppt_settings_preset_voltages(mppt_battery_preset_t p, float *vmax, float *vmin)
{
    switch (p) {
    case MPPT_BATT_PB_12V:  *vmax = 14.40f; *vmin = 11.80f; return true;
    case MPPT_BATT_PB_24V:  *vmax = 28.80f; *vmin = 23.60f; return true;
    case MPPT_BATT_LFP_12V: *vmax = 14.40f; *vmin = 12.00f; return true;
    case MPPT_BATT_LFP_24V: *vmax = 28.80f; *vmin = 24.00f; return true;
    default: return false;
    }
}

const char *mppt_settings_preset_name(mppt_battery_preset_t p)
{
    static const char *names[MPPT_BATT_PRESET_COUNT] = {
        "None", "Pb 12V", "Pb 24V", "LiFePO4 12V", "LiFePO4 24V", "Custom"
    };
    return (p < MPPT_BATT_PRESET_COUNT) ? names[p] : "?";
}

static esp_err_t store(void)
{
    return ContainerNvsSet(NS, KEY_SETTINGS, (const uint8_t *)&s_settings, sizeof(s_settings));
}

esp_err_t mppt_settings_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
    mppt_settings_t loaded;
    size_t len = sizeof(loaded);
    esp_err_t err = ContainerNvsGet(NS, KEY_SETTINGS, (uint8_t *)&loaded, &len);
    if (err == ESP_OK && len == sizeof(loaded) && loaded.version == SETTINGS_VERSION) {
        s_settings = loaded;
        ESP_LOGI(TAG, "settings loaded from NVS");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "no valid settings in NVS (%s), using defaults", esp_err_to_name(err));
    mppt_settings_defaults(&s_settings);
    return store();
}

void mppt_settings_get(mppt_settings_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_settings;
    xSemaphoreGive(s_mutex);
}

esp_err_t mppt_settings_set(const mppt_settings_t *in)
{
    mppt_settings_t n = *in;
    n.version = SETTINGS_VERSION;
    if (n.hap_enabled) {
        n.wifi_enabled = true;          /* HAP keeps Wi-Fi on */
    }
    float vmax, vmin;
    if (mppt_settings_preset_voltages(n.preset, &vmax, &vmin)) {
        n.vbat_max = vmax;
        n.vbat_min = vmin;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_settings = n;
    esp_err_t err = store();
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t mppt_settings_save_counters(double wh, uint32_t run_seconds)
{
    counters_blob_t c = { .wh = wh, .run_seconds = run_seconds };
    return ContainerNvsSet(NS, KEY_COUNTERS, (const uint8_t *)&c, sizeof(c));
}

esp_err_t mppt_settings_load_counters(double *wh, uint32_t *run_seconds)
{
    counters_blob_t c;
    size_t len = sizeof(c);
    esp_err_t err = ContainerNvsGet(NS, KEY_COUNTERS, (uint8_t *)&c, &len);
    if (err != ESP_OK || len != sizeof(c)) {
        *wh = 0;
        *run_seconds = 0;
        return err;
    }
    *wh = c.wh;
    *run_seconds = c.run_seconds;
    return ESP_OK;
}

esp_err_t mppt_settings_erase(void)
{
    esp_err_t err = ContainerNvsDeleteNamespace(NS);
    mppt_settings_defaults(&s_settings);
    return err;
}
