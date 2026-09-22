#include "esp_log.h"
#include "mppt_state.h"
#include "mppt_settings.h"
#include "mppt_telemetry.h"

static const char *TAG = "telemetry";

void mppt_telemetry_log(void)
{
    mppt_settings_t cfg;
    mppt_settings_get(&cfg);
    if (cfg.telemetry_mode == 0) {
        return;
    }
    mppt_state_t st;
    mppt_state_snapshot(&st);
    switch (cfg.telemetry_mode) {
    case 1:
        ESP_LOGI(TAG, "ERR:%d FLT:0x%02x MPPTA:%d CM:%d BYP:%d EN:%d FAN:%d WiFi:%d "
                      "PI:%.0f PWM:%d PPWM:%d VI:%.1f VO:%.1f CI:%.2f CO:%.2f Wh:%.2f Temp:%.1f "
                      "CSMPV:%.3f CSV:%.3f SOC:%d%% T:%lu LoopT:%.3fms",
                 st.err_now, st.faults, cfg.mppt_mode, cfg.output_mode_charger,
                 st.backflow_enabled, st.buck_enabled, st.fan_on, st.wifi_connected,
                 st.p_in, st.pwm, st.ppwm, st.v_in, st.v_out, st.i_in, st.i_out, st.wh, st.temp_c,
                 st.cs_midpoint_v, st.cs_raw_v, st.soc_percent, (unsigned long)st.run_seconds, st.loop_ms);
        break;
    case 2:
        ESP_LOGI(TAG, "PI:%.0f PWM:%d PPWM:%d VI:%.1f VO:%.1f CI:%.2f CO:%.2f Wh:%.2f Temp:%.1f "
                      "EN:%d FAN:%d SOC:%d%% T:%lu LoopT:%.3fms",
                 st.p_in, st.pwm, st.ppwm, st.v_in, st.v_out, st.i_in, st.i_out, st.wh, st.temp_c,
                 st.buck_enabled, st.fan_on, st.soc_percent, (unsigned long)st.run_seconds, st.loop_ms);
        break;
    default:
        ESP_LOGI(TAG, "%.0f %.1f %.1f %.2f %.2f %.2f %.1f %d %d %d %lu %.3f",
                 st.p_in, st.v_in, st.v_out, st.i_in, st.i_out, st.wh, st.temp_c,
                 st.buck_enabled, st.fan_on, st.soc_percent, (unsigned long)st.run_seconds, st.loop_ms);
        break;
    }
}
