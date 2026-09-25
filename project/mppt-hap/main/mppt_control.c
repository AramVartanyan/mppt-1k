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

static void system_processes(const mppt_settings_t *cfg)
{
    /* TODO(phase 3): fan control, loop time, auto counter reset. */
    (void)cfg;
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
        /* Phase 1: no ADS1115 traffic yet, idle at 100 ms. The ADS1115
         * conversion waits pace the loop once mppt_sensors_read() is ported. */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t mppt_control_start(void)
{
    mppt_control_emergency_stop();
    BaseType_t ok = xTaskCreate(control_task, "mppt_ctrl", MPPT_CONTROL_TASK_STACK,
                                NULL, MPPT_CONTROL_TASK_PRIO, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
