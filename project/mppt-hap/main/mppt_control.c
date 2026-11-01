#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "mppt_config.h"
#include "mppt_state.h"
#include "mppt_settings.h"
#include "mppt_hal.h"
#include "mppt_sensors.h"
#include "mppt_control.h"

static const char *TAG = "mppt_control";

void mppt_control_emergency_stop(void)
{
    mppt_hal_set_pwm(0);
    mppt_hal_buck_enable(false);
    mppt_hal_backflow(false);
    mppt_state_lock();
    mppt_state_t *st = mppt_state_get();
    st->pwm = 0;
    st->buck_enabled = false;
    st->backflow_enabled = false;
    mppt_state_unlock();
}

static void protection(const mppt_settings_t *cfg)
{
    /* TODO(phase 3): port of 3_Device_Protection.ino (error window fixed,
     * backflowControl once per cycle, PWR12 power-good checked before
     * enabling IR2104). */
    (void)cfg;
}

/* Fan output. The local temperature control has priority: when it wants
 * the fan it takes over and clears a HomeKit On, so the fan stops as the
 * settings say once the temperature falls again. */
static void fan_output(void)
{
    static bool s_fan_out, s_auto_prev, s_first = true;
    mppt_state_lock();
    mppt_state_t *st = mppt_state_get();
    if (st->fan_auto_on && !s_auto_prev) {
        st->fan_hap_on = false;
    }
    s_auto_prev = st->fan_auto_on;
    bool on = st->fan_auto_on || st->fan_hap_on;
    st->fan_on = on;
    mppt_state_unlock();
    if (on != s_fan_out || s_first) {
        mppt_hal_fan(on);
        s_fan_out = on;
        s_first = false;
    }
}

static void system_processes(const mppt_settings_t *cfg)
{
    /* TODO(phase 3): fan_auto_on from the temperature (with hysteresis),
     * loop time, auto counter reset. */
    (void)cfg;
    fan_output();
}

static void charging(const mppt_settings_t *cfg)
{
    /* TODO(phase 3): port of 4_Charging_Algorithm.ino (predictive PWM,
     * P&O MPPT with MPPT_STEP, CC-CV, PSU mode). Phase 1: buck stays off. */
    (void)cfg;
    mppt_hal_buck_enable(false);
}

static void control_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    mppt_settings_t cfg;
    ESP_LOGI(TAG, "control task started");
    for (;;) {
        esp_task_wdt_reset();
        mppt_settings_get(&cfg);
        mppt_sensors_read();
        protection(&cfg);
        system_processes(&cfg);
        charging(&cfg);
        /* As in FUGU the loop runs back to back: the ADS1115 conversion waits
         * (RDY interrupt) pace it at roughly 15 ms. One tick of delay keeps
         * the idle task (task WDT) fed even when the ADS1115 is missing and
         * the reads return at once. */
        vTaskDelay(1);
    }
}

esp_err_t mppt_control_start(void)
{
    mppt_control_emergency_stop();
    BaseType_t ok = xTaskCreate(control_task, "mppt_ctrl", MPPT_CONTROL_TASK_STACK,
                                NULL, MPPT_CONTROL_TASK_PRIO, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
