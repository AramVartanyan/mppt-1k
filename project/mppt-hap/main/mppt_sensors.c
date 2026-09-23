/*
 * Port of FUGU 2_Read_Sensors.ino.
 *
 * Differences: TH1 is read through the ADS1115 (channel MPPT_ADS_CH_TEMP) and
 * converted with the Beta equation from the divider voltage; the redundant TH2
 * on the on-chip ADC is read only while Wi-Fi is off; energy counters are
 * persisted on a schedule (mppt_sensors_persist).
 */
#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "mppt_config.h"
#include "mppt_state.h"
#include "mppt_settings.h"
#include "mppt_hal.h"
#include "mppt_sensors.h"

static const char *TAG = "mppt_sensors";

static float   s_ts_acc;           /* TS accumulator (volts) */
static int     s_ts_n;             /* sampleStoreTS */
static int64_t s_last_routine_us;  /* prevRoutineMillis */
static int64_t s_last_persist_us;
static int64_t s_last_cycle_us;    /* loop stopwatch */
static double  s_sec_acc;          /* fractional seconds carried between cycles */
static double  s_wh_persisted;

/* --------------------------------------------------------------- helpers */

/* NTC divider node voltage → °C (Beta equation). NAN when out of range. */
static float ntc_temp_from_volts(float v)
{
    const float vdd = MPPT_NTC_VDD_MV / 1000.0f;
    const float rf = (float)MPPT_NTC_RFIXED_OHM;
    if (v <= 0.01f || v >= vdd - 0.01f) {
        return NAN;
    }
    float r = MPPT_NTC_TO_VCC ? rf * (vdd / v - 1.0f)      /* Vcc → NTC → node → Rf → GND */
                              : rf * v / (vdd - v);        /* Vcc → Rf → node → NTC → GND */
    float inv_t = 1.0f / 298.15f + logf(r / (float)MPPT_NTC_R25_OHM) / (float)MPPT_NTC_BETA;
    return 1.0f / inv_t - 273.15f;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ------------------------------------------------------------------- API */

esp_err_t mppt_sensors_init(void)
{
    double wh;
    uint32_t run_s;
    mppt_settings_load_counters(&wh, &run_s);
    s_wh_persisted = wh;
    mppt_state_lock();
    mppt_state_t *st = mppt_state_get();
    st->wh = wh;
    st->run_seconds = run_s;
    st->days_running = run_s / 86400.0f;
    st->cs_midpoint_v = MPPT_CAL_CURRENT_MID_V;
    st->temp_c = NAN;
    st->temp2_c = NAN;
    mppt_state_unlock();
    s_last_routine_us = s_last_persist_us = s_last_cycle_us = esp_timer_get_time();
    ESP_LOGI(TAG, "counters: %.1f Wh, %lu s", wh, (unsigned long)run_s);
    return ESP_OK;
}

void mppt_sensors_persist(void)
{
    mppt_state_t st;
    mppt_state_snapshot(&st);
    if (mppt_settings_save_counters(st.wh, st.run_seconds) == ESP_OK) {
        s_wh_persisted = st.wh;
        s_last_persist_us = esp_timer_get_time();
        ESP_LOGI(TAG, "counters saved: %.1f Wh, %lu s", st.wh, (unsigned long)st.run_seconds);
    }
}

void mppt_sensors_reset_counters(void)
{
    mppt_state_lock();
    mppt_state_t *st = mppt_state_get();
    st->wh = 0;
    st->run_seconds = 0;
    st->days_running = 0;
    mppt_state_unlock();
    s_sec_acc = 0;
    mppt_settings_save_counters(0, 0);
    s_wh_persisted = 0;
}

void mppt_sensors_read(void)
{
    mppt_settings_t cfg;
    mppt_settings_get(&cfg);

    float v;
    float vsi = 0, vso = 0, csi = 0;
    int nv = 0, nc = 0;

    /* ---- temperature sensor: lite averaging, one sample per cycle ---- */
    float temp_c = NAN;
    bool temp_updated = false;
    if (mppt_hal_ads_read(MPPT_ADS_CH_TEMP, &v) == ESP_OK) {
        s_ts_acc += v;
        s_ts_n++;
    }
    if (s_ts_n >= MPPT_CAL_AVG_TEMP) {
        temp_c = ntc_temp_from_volts(s_ts_acc / s_ts_n);
        temp_updated = true;
        s_ts_acc = 0;
        s_ts_n = 0;
    }

    /* ---- voltage sensors: instantaneous averaging ---- */
    for (int i = 0; i < MPPT_CAL_AVG_VOLTAGE; i++) {
        if (mppt_hal_ads_read(MPPT_ADS_CH_VIN, &v) == ESP_OK) {
            vsi += v;
            if (mppt_hal_ads_read(MPPT_ADS_CH_VOUT, &v) == ESP_OK) {
                vso += v;
                nv++;
            }
        }
    }
    /* ---- current sensor ---- */
    for (int i = 0; i < MPPT_CAL_AVG_CURRENT; i++) {
        if (mppt_hal_ads_read(MPPT_ADS_CH_CURRENT, &v) == ESP_OK) {
            csi += v;
            nc++;
        }
    }
    if (nv == 0 || nc == 0) {
        return;                         /* ADS1115 missing: keep last values */
    }

    const float v_in  = (vsi / nv) * MPPT_CAL_VIN_RATIO;
    const float v_out = (vso / nv) * MPPT_CAL_VOUT_RATIO;
    const float cs    = (csi / nc) * MPPT_CAL_CURRENT_SCALE;   /* CSI_converted */

    /* ---- redundant TH2 (on-chip ADC, Wi-Fi off only) ---- */
    float temp2 = NAN;
    bool temp2_valid = false;
    if (temp_updated) {
        temp2_valid = (mppt_hal_ntc2_read(&temp2) == ESP_OK);
    }

    const int64_t now = esp_timer_get_time();

    mppt_state_lock();
    mppt_state_t *st = mppt_state_get();

    /* current: (CSI_converted - midpoint) * -1 / sensitivity */
    float i_in = (st->cs_midpoint_v - cs) / MPPT_CAL_CURRENT_SENS_V_A;
    if (i_in < 0) {
        i_in = 0;
    }
    const float i_out = (v_out > 0) ? (v_in * i_in) / v_out : 0;

    /* power source detection */
    if (v_in <= 3 && v_out <= 3) {
        st->source = MPPT_SOURCE_NONE;      /* USB only */
    } else if (v_in > v_out) {
        st->source = MPPT_SOURCE_SOLAR;
    } else {
        st->source = MPPT_SOURCE_BATTERY;
    }

    /* automatic current sensor calibration (FUGU: buck off, no FLV / OOV) */
    if (!st->buck_enabled && !(st->faults & (MPPT_FAULT_FLV | MPPT_FAULT_OOV))) {
        st->cs_midpoint_v = cs - 0.003f;
    }

    st->v_in = v_in;
    st->v_out = v_out;
    st->i_in = i_in;
    st->i_out = i_out;
    st->cs_raw_v = cs;
    st->p_in = v_in * i_in;
    st->p_out = v_in * i_in * MPPT_CAL_EFFICIENCY;
    if (temp_updated) {
        st->temp_c = temp_c;
        st->temp2_c = temp2;
        st->temp2_valid = temp2_valid;
    }

    /* state of charge */
    const float span = cfg.vbat_max - cfg.vbat_min;
    st->soc_percent = (span > 0.01f)
        ? (int)clampf(((v_out - cfg.vbat_min) / span) * 101.0f, 0, 100) : 0;

    /* time dependent accounting every MPPT_ROUTINE_INTERVAL_MS */
    if (now - s_last_routine_us >= (int64_t)MPPT_ROUTINE_INTERVAL_MS * 1000) {
        const double dt_s = (now - s_last_routine_us) / 1e6;
        s_last_routine_us = now;
        st->wh += st->p_in * dt_s / 3600.0;
        s_sec_acc += dt_s;
        while (s_sec_acc >= 1.0) {
            st->run_seconds++;
            s_sec_acc -= 1.0;
        }
        st->days_running = st->run_seconds / 86400.0f;

        /* automatic counter reset (FUGU System_Processes) */
        const float limits[] = {0, 1, 7, 30, 365};
        if (cfg.counter_reset > 0 && cfg.counter_reset < 5 && st->days_running > limits[cfg.counter_reset]) {
            st->wh = 0;
            st->run_seconds = 0;
            st->days_running = 0;
        }
    }

    /* loop stopwatch */
    st->loop_ms = (now - s_last_cycle_us) / 1000.0f;
    s_last_cycle_us = now;

    const bool persist_due = (now - s_last_persist_us >= (int64_t)MPPT_WH_PERSIST_MINUTES * 60 * 1000000LL)
                             && (st->wh != s_wh_persisted);
    mppt_state_unlock();

    if (persist_due) {
        mppt_sensors_persist();
    }
}
