/*
 * mppt_state.h — shared measurement and status snapshot.
 *
 * Written by the control task (mppt_sensors / mppt_control), read by the UI
 * task, telemetry and HomeKit. Access goes through mppt_state_lock() /
 * mppt_state_unlock(), or mppt_state_snapshot() for a consistent copy.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Protection flags (FUGU names) */
#define MPPT_FAULT_OTE  (1u << 0)  /* over-temperature */
#define MPPT_FAULT_IOC  (1u << 1)  /* input over-current */
#define MPPT_FAULT_OOC  (1u << 2)  /* output over-current */
#define MPPT_FAULT_OOV  (1u << 3)  /* output over-voltage */
#define MPPT_FAULT_FLV  (1u << 4)  /* fatally low system voltage */
#define MPPT_FAULT_IUV  (1u << 5)  /* input under-voltage */
#define MPPT_FAULT_BNC  (1u << 6)  /* battery not connected */
#define MPPT_FAULT_REC  (1u << 7)  /* recovery pending (IUV cleared) */

typedef enum {
    MPPT_SOURCE_NONE = 0,   /* powered from USB only */
    MPPT_SOURCE_SOLAR,      /* Vin > Vout */
    MPPT_SOURCE_BATTERY,    /* Vin < Vout */
} mppt_source_t;

typedef struct {
    /* measurements */
    float v_in;             /* V, PV input */
    float v_out;            /* V, battery / output */
    float i_in;             /* A, input current (ACS712) */
    float i_out;            /* A, output current (computed) */
    float p_in;             /* W */
    float p_out;            /* W */
    float temp_c;           /* °C, TH1 via ADS1115 */
    float temp2_c;          /* °C, TH2 via on-chip ADC */
    bool  temp2_valid;
    float cs_midpoint_v;    /* auto-calibrated current sensor midpoint */
    float cs_raw_v;         /* current sensor voltage after scaling */
    mppt_source_t source;
    int   soc_percent;

    /* energy */
    double   wh;            /* accumulated harvested energy */
    uint32_t run_seconds;   /* charger active time */
    float    days_running;

    /* control */
    int  pwm;
    int  ppwm;
    int  pwm_max;
    int  pwm_max_limited;
    bool buck_enabled;
    bool backflow_enabled;
    bool fan_on;
    bool pwr12_good;
    uint16_t faults;        /* MPPT_FAULT_* */
    int  err_now;           /* faults in this cycle (FUGU ERR) */
    int  err_count;         /* faults in the current window */
    float loop_ms;

    /* connectivity */
    bool wifi_enabled;
    bool wifi_connected;
    bool portal_active;
    bool hap_running;
    int  hap_paired_controllers;
    char ip[16];
    int  rssi;
} mppt_state_t;

/* Create the mutex; call once before the tasks start. */
void mppt_state_init(void);

/* Lock / unlock the shared state (short critical sections only). */
void mppt_state_lock(void);
void mppt_state_unlock(void);

/* Direct pointer for use between lock/unlock. */
mppt_state_t *mppt_state_get(void);

/* Consistent copy for readers (UI, telemetry, HomeKit). */
void mppt_state_snapshot(mppt_state_t *out);

#ifdef __cplusplus
}
#endif
