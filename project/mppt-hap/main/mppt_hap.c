/*
 * mppt_hap.c — HomeKit accessory (README section 5).
 *
 * One accessory, category Outlet:
 *   Outlet "Output"      On = output enabled, Outlet In Use = output delivers
 *                        current; custom: Eve Total Consumption (kWh, read on
 *                        request only) and the energy price (read/write)
 *   Temperature Sensor   TH1, Status Fault on over-temperature / no reading
 *   Fan v2               Active; the local temperature control has priority
 *   Battery Service      present in every mode (100 % in PSU mode)
 */
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <hap.h>
#include <hap_apple_servs.h>
#include <hap_apple_chars.h>
#include <app_hap_setup_payload.h>
#include "led_indicator.h"
#include "mppt_config.h"
#include "mppt_state.h"
#include "mppt_settings.h"
#include "mppt_hap.h"

static const char *TAG = "mppt_hap";

/* Eve "Total Consumption" (kWh), known to the Eve app */
#define UUID_EVE_TOTAL_KWH  "E863F10C-079E-48FF-8F27-9C2605A29F52"
/* Energy price (currency per kWh), own UUID of this project */
#define UUID_ENERGY_PRICE   "4D505054-5052-4943-8001-6D7070743173"

#define IN_USE_ON_A         0.25f   /* Outlet In Use with hysteresis */
#define IN_USE_OFF_A        0.15f
#define TEMP_STEP_C         0.2f    /* notify on changes of at least this */
#define FAN_REVERT_US       (1000 * 1000)

static bool s_running;

static hap_char_t *c_on, *c_in_use, *c_kwh, *c_price;
static hap_char_t *c_temp, *c_temp_fault;
static hap_char_t *c_fan;
static hap_char_t *c_batt_level, *c_batt_charging, *c_batt_low;
static esp_timer_handle_t s_fan_revert;

/* last values sent, to notify only on changes */
static struct {
    bool    on, in_use;
    float   temp, price;
    uint8_t temp_fault, fan, level, charging, low;
} s_last;

/* ----------------------------------------------------------------- helpers */

static void set_bool(hap_char_t *hc, bool *last, bool v)
{
    if (*last != v) {
        hap_val_t val = { .b = v };
        hap_char_update_val(hc, &val);
        *last = v;
    }
}

static void set_u8(hap_char_t *hc, uint8_t *last, uint8_t v)
{
    if (*last != v) {
        hap_val_t val = { .u = v };
        hap_char_update_val(hc, &val);
        *last = v;
    }
}

static bool battery_mode(const mppt_settings_t *cfg)
{
    return cfg->output_mode_charger && cfg->preset != MPPT_BATT_NONE;
}

/* ------------------------------------------------------------------ Outlet */

static int outlet_read(hap_char_t *hc, hap_status_t *status, void *serv_priv, void *read_priv)
{
    (void)serv_priv;
    (void)read_priv;
    if (hc == c_kwh) {
        mppt_state_t st;
        mppt_state_snapshot(&st);
        hap_val_t val = { .f = (float)(st.wh / 1000.0) };
        hap_char_update_val(hc, &val);
    }
    *status = HAP_STATUS_SUCCESS;
    return HAP_SUCCESS;
}

static int outlet_write(hap_write_data_t write_data[], int count, void *serv_priv, void *write_priv)
{
    (void)serv_priv;
    (void)write_priv;
    int ret = HAP_SUCCESS;
    for (int i = 0; i < count; i++) {
        hap_write_data_t *w = &write_data[i];
        mppt_settings_t cfg;
        mppt_settings_get(&cfg);
        if (w->hc == c_on) {
            /* Off: the control stops the output (soft stop in PSU mode) */
            cfg.charging_enabled = w->val.b;
            mppt_settings_set(&cfg);
            ESP_LOGI(TAG, "output %s from HomeKit", w->val.b ? "on" : "off");
            hap_char_update_val(w->hc, &w->val);
            s_last.on = w->val.b;
            *(w->status) = HAP_STATUS_SUCCESS;
        } else if (w->hc == c_price) {
            float p = w->val.f;
            if (p < 0.0f || p > 9.99f) {
                *(w->status) = HAP_STATUS_VAL_INVALID;
                ret = HAP_FAIL;
                continue;
            }
            cfg.energy_price = roundf(p * 100.0f) / 100.0f;
            mppt_settings_set(&cfg);
            hap_val_t val = { .f = cfg.energy_price };
            hap_char_update_val(w->hc, &val);
            s_last.price = cfg.energy_price;
            *(w->status) = HAP_STATUS_SUCCESS;
        } else {
            *(w->status) = HAP_STATUS_RES_ABSENT;
            ret = HAP_FAIL;
        }
    }
    return ret;
}

/* --------------------------------------------------------------------- Fan */

/* A HomeKit Off while the temperature control runs the fan was acknowledged;
 * the fan kept running, so show it running again. */
static void fan_revert_cb(void *arg)
{
    (void)arg;
    mppt_state_t st;
    mppt_state_snapshot(&st);
    s_last.fan = 0xFF;                   /* force the notification */
    set_u8(c_fan, &s_last.fan, st.fan_on ? 1 : 0);
}

static int fan_write(hap_write_data_t write_data[], int count, void *serv_priv, void *write_priv)
{
    (void)serv_priv;
    (void)write_priv;
    int ret = HAP_SUCCESS;
    for (int i = 0; i < count; i++) {
        hap_write_data_t *w = &write_data[i];
        if (w->hc != c_fan) {
            *(w->status) = HAP_STATUS_RES_ABSENT;
            ret = HAP_FAIL;
            continue;
        }
        bool on = w->val.u != 0;
        mppt_state_lock();
        mppt_state_t *st = mppt_state_get();
        bool forced = st->fan_auto_on;   /* temperature control has it */
        if (!forced) {
            st->fan_hap_on = on;
        }
        mppt_state_unlock();
        ESP_LOGI(TAG, "fan %s from HomeKit%s", on ? "on" : "off",
                 forced ? " ignored: cooling" : "");
        /* HAP expects the answer now; the real state follows */
        hap_char_update_val(w->hc, &w->val);
        s_last.fan = on ? 1 : 0;
        *(w->status) = HAP_STATUS_SUCCESS;
        if (!on && forced && s_fan_revert) {
            esp_timer_stop(s_fan_revert);
            esp_timer_start_once(s_fan_revert, FAN_REVERT_US);
        }
    }
    return ret;
}

/* ---------------------------------------------------------------- identify */

static int identify(hap_acc_t *ha)
{
    (void)ha;
    ESP_LOGI(TAG, "identify");
    ledFlash(5, 200);
    return HAP_SUCCESS;
}

/* -------------------------------------------------------------------- API */

esp_err_t mppt_hap_start(const char *name, const char *serial)
{
    if (s_running) {
        return ESP_OK;
    }
    mppt_settings_t cfg;
    mppt_settings_get(&cfg);
    mppt_state_t st;
    mppt_state_snapshot(&st);

    if (hap_init(HAP_TRANSPORT_WIFI) != HAP_SUCCESS) {
        ESP_LOGE(TAG, "hap_init failed");
        return ESP_FAIL;
    }

    hap_acc_cfg_t acc_cfg = {
        .name = (char *)name,
        .manufacturer = CONFIG_MANIFACTURER,
        .model = CONFIG_MODEL,
        .serial_num = (char *)serial,
        .fw_rev = CONFIG_APP_PROJECT_VER,
        .hw_rev = CONFIG_HW,
        .pv = "1.1.0",
        .identify_routine = identify,
        .cid = HAP_CID_OUTLET,
    };
    hap_acc_t *acc = hap_acc_create(&acc_cfg);
    if (!acc) {
        ESP_LOGE(TAG, "accessory not created");
        return ESP_FAIL;
    }
    uint8_t product_data[] = {'M', 'P', 'P', 'T', '3', '2', 'S', '2'};
    hap_acc_add_product_data(acc, product_data, sizeof(product_data));
    hap_acc_add_wifi_transport_service(acc, 0);

    /* Outlet "Output" (primary) */
    hap_serv_t *outlet = hap_serv_outlet_create(cfg.charging_enabled, false);
    hap_serv_add_char(outlet, hap_char_name_create("Output"));
    c_on = hap_serv_get_char_by_uuid(outlet, HAP_CHAR_UUID_ON);
    c_in_use = hap_serv_get_char_by_uuid(outlet, HAP_CHAR_UUID_OUTLET_IN_USE);
    /* energy: read on request only, never notified */
    c_kwh = hap_char_float_create(UUID_EVE_TOTAL_KWH, HAP_CHAR_PERM_PR, (float)(st.wh / 1000.0));
    hap_char_float_set_constraints(c_kwh, 0.0f, 1000000.0f, 0.001f);
    hap_char_add_description(c_kwh, "Total Consumption");
    hap_serv_add_char(outlet, c_kwh);
    c_price = hap_char_float_create(UUID_ENERGY_PRICE,
                                    HAP_CHAR_PERM_PR | HAP_CHAR_PERM_PW | HAP_CHAR_PERM_EV,
                                    cfg.energy_price);
    hap_char_float_set_constraints(c_price, 0.0f, 9.99f, 0.01f);
    hap_char_add_description(c_price, "Energy price per kWh");
    hap_serv_add_char(outlet, c_price);
    hap_serv_set_read_cb(outlet, outlet_read);
    hap_serv_set_write_cb(outlet, outlet_write);
    hap_serv_mark_primary(outlet);
    hap_acc_add_serv(acc, outlet);

    /* Temperature Sensor */
    float t = isnan(st.temp_c) ? 0.0f : st.temp_c;
    hap_serv_t *temp = hap_serv_temperature_sensor_create(t);
    hap_serv_add_char(temp, hap_char_name_create("Temperature"));
    c_temp = hap_serv_get_char_by_uuid(temp, HAP_CHAR_UUID_CURRENT_TEMPERATURE);
    hap_char_float_set_constraints(c_temp, -40.0f, 125.0f, 0.1f);
    c_temp_fault = hap_char_status_fault_create(0);
    hap_serv_add_char(temp, c_temp_fault);
    hap_acc_add_serv(acc, temp);

    /* Fan */
    hap_serv_t *fan = hap_serv_fan_v2_create(st.fan_on ? 1 : 0);
    hap_serv_add_char(fan, hap_char_name_create("Fan"));
    c_fan = hap_serv_get_char_by_uuid(fan, HAP_CHAR_UUID_ACTIVE);
    hap_serv_set_write_cb(fan, fan_write);
    hap_acc_add_serv(acc, fan);

    /* Battery, in every mode */
    hap_serv_t *batt = hap_serv_battery_service_create(100, 0, 0);
    hap_serv_add_char(batt, hap_char_name_create("Battery"));
    c_batt_level = hap_serv_get_char_by_uuid(batt, HAP_CHAR_UUID_BATTERY_LEVEL);
    c_batt_charging = hap_serv_get_char_by_uuid(batt, HAP_CHAR_UUID_CHARGING_STATE);
    c_batt_low = hap_serv_get_char_by_uuid(batt, HAP_CHAR_UUID_STATUS_LOW_BATTERY);
    hap_acc_add_serv(acc, batt);

    hap_add_accessory(acc);

    const esp_timer_create_args_t targs = { .callback = fan_revert_cb, .name = "hap_fan" };
    esp_timer_create(&targs, &s_fan_revert);

#ifdef CONFIG_USE_HARDCODED_SETUP_CODE
    /* test builds only; production devices read the setup info from factory_nvs */
    hap_set_setup_code(CONFIG_SETUP_CODE);
    hap_set_setup_id(CONFIG_SETUP_ID);
    app_hap_setup_payload(CONFIG_SETUP_CODE, CONFIG_SETUP_ID, false, acc_cfg.cid);
#endif

    if (hap_start() != HAP_SUCCESS) {
        ESP_LOGE(TAG, "hap_start failed");
        return ESP_FAIL;
    }
    /* values the characteristics were created with; 0xFF / NAN make the
     * first push send the rest */
    s_last.on = cfg.charging_enabled;
    s_last.in_use = false;
    s_last.price = cfg.energy_price;
    s_last.temp = NAN;
    s_last.temp_fault = s_last.fan = s_last.level = s_last.charging = s_last.low = 0xFF;
    s_running = true;
    mppt_state_lock();
    mppt_state_get()->hap_running = true;
    mppt_state_unlock();
    ESP_LOGI(TAG, "HomeKit started as %s", name);
    mppt_hap_push();
    return ESP_OK;
}

void mppt_hap_push(void)
{
    if (!s_running) {
        return;
    }
    mppt_settings_t cfg;
    mppt_settings_get(&cfg);
    mppt_state_t st;
    mppt_state_snapshot(&st);

    /* Outlet */
    set_bool(c_on, &s_last.on, cfg.charging_enabled);
    bool in_use = s_last.in_use ? st.i_out > IN_USE_OFF_A : st.i_out > IN_USE_ON_A;
    set_bool(c_in_use, &s_last.in_use, in_use);
    if (fabsf(cfg.energy_price - s_last.price) >= 0.005f) {
        hap_val_t val = { .f = cfg.energy_price };
        hap_char_update_val(c_price, &val);
        s_last.price = cfg.energy_price;
    }

    /* Temperature */
    bool no_temp = isnan(st.temp_c);
    if (!no_temp && !(fabsf(st.temp_c - s_last.temp) < TEMP_STEP_C)) {
        hap_val_t val = { .f = st.temp_c };
        hap_char_update_val(c_temp, &val);
        s_last.temp = st.temp_c;
    }
    set_u8(c_temp_fault, &s_last.temp_fault, (no_temp || (st.faults & MPPT_FAULT_OTE)) ? 1 : 0);

    /* Fan (real state; a reverted HomeKit Off is handled by the timer) */
    set_u8(c_fan, &s_last.fan, st.fan_on ? 1 : 0);

    /* Battery */
    bool batt = battery_mode(&cfg);
    uint8_t level = batt ? (uint8_t)(st.soc_percent < 0 ? 0 : st.soc_percent > 100 ? 100 : st.soc_percent) : 100;
    set_u8(c_batt_level, &s_last.level, level);
    set_u8(c_batt_charging, &s_last.charging, st.buck_enabled ? 1 : 0);
    set_u8(c_batt_low, &s_last.low, (batt && level < 10) ? 1 : 0);
}

bool mppt_hap_is_running(void)
{
    return s_running;
}

esp_err_t mppt_hap_reset_pairings(void)
{
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    return hap_reset_pairings() == HAP_SUCCESS ? ESP_OK : ESP_FAIL;
}

int mppt_hap_paired_count(void)
{
    return s_running ? hap_get_paired_controller_count() : 0;
}
