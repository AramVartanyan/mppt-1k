#include "esp_log.h"
#include <hap.h>
#include <hap_apple_servs.h>
#include <hap_apple_chars.h>
#include "mppt_config.h"
#include "mppt_state.h"
#include "mppt_settings.h"
#include "mppt_hap.h"

static const char *TAG = "mppt_hap";
static bool s_running;

esp_err_t mppt_hap_start(const char *name, const char *serial)
{
    /* TODO(phase 4): move the accessory definition here (Switch + custom
     * characteristics, Battery, Temperature Sensor, Fan) and call
     * hap_init / hap_add_accessory / hap_start. */
    ESP_LOGW(TAG, "HomeKit not implemented yet (name %s, serial %s)", name, serial);
    s_running = false;
    return ESP_ERR_NOT_SUPPORTED;
}

void mppt_hap_push(void)
{
    if (!s_running) {
        return;
    }
    /* TODO(phase 4): hap_char_update_val() on changes above thresholds. */
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
