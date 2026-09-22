#include "esp_log.h"
#include "mppt_config.h"
#include "mppt_state.h"
#include "mppt_settings.h"
#include "mppt_hal.h"
#include "mppt_sensors.h"

static const char *TAG = "mppt_sensors";

esp_err_t mppt_sensors_init(void)
{
    double wh;
    uint32_t run_s;
    mppt_settings_load_counters(&wh, &run_s);
    mppt_state_lock();
    mppt_state_t *st = mppt_state_get();
    st->wh = wh;
    st->run_seconds = run_s;
    st->cs_midpoint_v = MPPT_CAL_CURRENT_MID_V;
    mppt_state_unlock();
    ESP_LOGI(TAG, "counters: %.1f Wh, %lu s", wh, (unsigned long)run_s);
    return ESP_OK;
}

void mppt_sensors_read(void)
{
    /* TODO(phase 2): port of 2_Read_Sensors.ino
     *  - TH1 via ADS1115 channel MPPT_ADS_CH_TEMP, Beta formula, MPPT_CAL_AVG_TEMP
     *  - Vin/Vout: MPPT_CAL_AVG_VOLTAGE samples * divider ratios
     *  - current: MPPT_CAL_AVG_CURRENT samples * 1.33, midpoint, 66 mV/A
     *  - automatic midpoint calibration while buck disabled and no FLV/OOV
     *  - power source detection, power, SOC, Wh/time accounting
     */
}

void mppt_sensors_reset_counters(void)
{
    mppt_state_lock();
    mppt_state_t *st = mppt_state_get();
    st->wh = 0;
    st->run_seconds = 0;
    st->days_running = 0;
    mppt_state_unlock();
    mppt_settings_save_counters(0, 0);
}
