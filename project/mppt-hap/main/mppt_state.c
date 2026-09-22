#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mppt_state.h"

static mppt_state_t s_state;
static SemaphoreHandle_t s_mutex;

void mppt_state_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
}

void mppt_state_lock(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

void mppt_state_unlock(void)
{
    xSemaphoreGive(s_mutex);
}

mppt_state_t *mppt_state_get(void)
{
    return &s_state;
}

void mppt_state_snapshot(mppt_state_t *out)
{
    mppt_state_lock();
    *out = s_state;
    mppt_state_unlock();
}
