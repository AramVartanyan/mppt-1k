#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "sdkconfig.h"
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "general_ota.h"

static const char *TAG = "general_ota";

#define OTA_TASK_STACK  (8 * 1024)
#define OTA_TASK_PRIO   1

static general_ota_config_t s_cfg;
static volatile general_ota_state_t s_state = GENERAL_OTA_IDLE;
static char s_available[GENERAL_OTA_VERSION_LEN];
static bool s_inited;

/* ---------------------------------------------------------------- helpers */

static void emit(general_ota_event_t ev, const char *ver, int progress, esp_err_t err)
{
    if (s_cfg.cb) {
        general_ota_info_t info = {
            .event = ev,
            .new_version = ver ? ver : "",
            .progress_percent = progress,
            .error = err,
        };
        s_cfg.cb(&info, s_cfg.cb_ctx);
    }
}

static bool parse_version(const char *s, int *ma, int *mi, int *pa)
{
    *ma = *mi = *pa = 0;
    return s && sscanf(s, "%d.%d.%d", ma, mi, pa) >= 2;
}

/* true when `new_ver` is strictly newer than the running version */
static bool is_newer(const char *new_ver)
{
    int cm, cn, cp, nm, nn, np;
    if (!parse_version(general_ota_running_version(), &cm, &cn, &cp)) {
        ESP_LOGE(TAG, "cannot parse running version '%s'", general_ota_running_version());
        return false;
    }
    if (!parse_version(new_ver, &nm, &nn, &np)) {
        ESP_LOGE(TAG, "cannot parse server version '%s'", new_ver);
        return false;
    }
    if (nm != cm) return nm > cm;
    if (nn != cn) return nn > cn;
    return np > cp;
}

static void http_config(esp_http_client_config_t *hc)
{
    memset(hc, 0, sizeof(*hc));
    hc->url = s_cfg.url ? s_cfg.url : CONFIG_GENERAL_OTA_URL;
    hc->timeout_ms = s_cfg.timeout_ms ? s_cfg.timeout_ms : CONFIG_GENERAL_OTA_RECV_TIMEOUT_MS;
    hc->keep_alive_enable = true;
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    hc->crt_bundle_attach = esp_crt_bundle_attach;
#else
#error "general-ota needs CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y (menuconfig: mbedTLS -> Certificate Bundle)"
#endif
#ifdef CONFIG_GENERAL_OTA_SKIP_COMMON_NAME_CHECK
    hc->skip_cert_common_name_check = true;
#endif
}

/* Open the OTA session and read the descriptor. On ESP_OK the caller owns
 * `handle` and must finish or abort it. */
static esp_err_t begin_and_describe(esp_https_ota_handle_t *handle, esp_app_desc_t *desc)
{
    esp_http_client_config_t hc;
    http_config(&hc);
    esp_https_ota_config_t oc = { .http_config = &hc };

    esp_err_t err = esp_https_ota_begin(&oc, handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_begin: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_https_ota_get_img_desc(*handle, desc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_get_img_desc: %s", esp_err_to_name(err));
        esp_https_ota_abort(*handle);
        *handle = NULL;
    }
    return err;
}

/* ------------------------------------------------------------------ check */

/* ESP_OK newer, ESP_ERR_NOT_FOUND up to date, else error. */
static esp_err_t do_check(esp_https_ota_handle_t *keep_handle, char *ver, size_t len)
{
    esp_https_ota_handle_t h = NULL;
    esp_app_desc_t desc;
    esp_err_t err = begin_and_describe(&h, &desc);
    if (err != ESP_OK) {
        return err;
    }
    if (ver && len) {
        snprintf(ver, len, "%s", desc.version);
    }
    bool newer = s_cfg.skip_version_check || is_newer(desc.version);
    ESP_LOGI(TAG, "running %s, server %s → %s", general_ota_running_version(), desc.version,
             newer ? "update available" : "up to date");
    if (newer && keep_handle) {
        *keep_handle = h;           /* continue with the download */
    } else {
        esp_https_ota_abort(h);
    }
    return newer ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t general_ota_check_blocking(char *version, size_t len, int timeout_ms)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (general_ota_busy()) return ESP_ERR_INVALID_STATE;
    int saved = s_cfg.timeout_ms;
    if (timeout_ms > 0) s_cfg.timeout_ms = timeout_ms;
    s_state = GENERAL_OTA_CHECKING;
    esp_err_t err = do_check(NULL, version, len);
    s_cfg.timeout_ms = saved;
    if (err == ESP_OK) {
        snprintf(s_available, sizeof(s_available), "%s", version ? version : "");
        s_state = GENERAL_OTA_AVAILABLE;
    } else if (err == ESP_ERR_NOT_FOUND) {
        s_state = GENERAL_OTA_UP_TO_DATE;
    } else {
        s_state = GENERAL_OTA_FAILED;
    }
    return err;
}

/* ------------------------------------------------------------------ tasks */

static void check_task(void *arg)
{
    (void)arg;
    char ver[GENERAL_OTA_VERSION_LEN] = "";
    s_state = GENERAL_OTA_CHECKING;
    emit(GENERAL_OTA_EVT_CHECK_START, NULL, 0, ESP_OK);
    esp_err_t err = do_check(NULL, ver, sizeof(ver));
    if (err == ESP_OK) {
        snprintf(s_available, sizeof(s_available), "%s", ver);
        s_state = GENERAL_OTA_AVAILABLE;
        emit(GENERAL_OTA_EVT_UPDATE_AVAILABLE, ver, 0, ESP_OK);
    } else if (err == ESP_ERR_NOT_FOUND) {
        s_state = GENERAL_OTA_UP_TO_DATE;
        emit(GENERAL_OTA_EVT_UP_TO_DATE, ver, 0, ESP_OK);
    } else {
        s_state = GENERAL_OTA_FAILED;
        emit(GENERAL_OTA_EVT_FAILED, NULL, 0, err);
    }
    vTaskDelete(NULL);
}

static void update_task(void *arg)
{
    (void)arg;
    char ver[GENERAL_OTA_VERSION_LEN] = "";
    esp_https_ota_handle_t h = NULL;

    s_state = GENERAL_OTA_CHECKING;
    emit(GENERAL_OTA_EVT_CHECK_START, NULL, 0, ESP_OK);
    esp_err_t err = do_check(&h, ver, sizeof(ver));
    if (err == ESP_ERR_NOT_FOUND) {
        s_state = GENERAL_OTA_UP_TO_DATE;
        emit(GENERAL_OTA_EVT_UP_TO_DATE, ver, 0, ESP_OK);
        goto done;
    }
    if (err != ESP_OK) {
        s_state = GENERAL_OTA_FAILED;
        emit(GENERAL_OTA_EVT_FAILED, NULL, 0, err);
        goto done;
    }

    snprintf(s_available, sizeof(s_available), "%s", ver);
    s_state = GENERAL_OTA_DOWNLOADING;
    emit(GENERAL_OTA_EVT_UPDATE_AVAILABLE, ver, 0, ESP_OK);
    emit(GENERAL_OTA_EVT_DOWNLOAD_START, ver, 0, ESP_OK);

    int total = esp_https_ota_get_image_size(h);
    int last_pct = -1;
    for (;;) {
        err = esp_https_ota_perform(h);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        int read = esp_https_ota_get_image_len_read(h);
        int pct = (total > 0) ? (int)((int64_t)read * 100 / total) : -1;
        if (pct != last_pct && (pct < 0 || pct % 5 == 0)) {
            last_pct = pct;
            emit(GENERAL_OTA_EVT_PROGRESS, ver, pct, ESP_OK);
        }
    }

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
        ESP_LOGE(TAG, "download incomplete or failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(h);
        s_state = GENERAL_OTA_FAILED;
        emit(GENERAL_OTA_EVT_FAILED, ver, 0, err == ESP_OK ? ESP_FAIL : err);
        goto done;
    }

    err = esp_https_ota_finish(h);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "image validation failed, image is corrupted");
        }
        ESP_LOGE(TAG, "esp_https_ota_finish: %s", esp_err_to_name(err));
        s_state = GENERAL_OTA_FAILED;
        emit(GENERAL_OTA_EVT_FAILED, ver, 0, err);
        goto done;
    }

    ESP_LOGI(TAG, "update to %s written, reboot to activate", ver);
    s_state = GENERAL_OTA_SUCCESS;
    emit(GENERAL_OTA_EVT_SUCCESS, ver, 100, ESP_OK);
    if (s_cfg.auto_reboot) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_GENERAL_OTA_REBOOT_DELAY_MS));
        general_ota_reboot();
    }

done:
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------- API */

esp_err_t general_ota_init(const general_ota_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    s_cfg = *cfg;
    s_available[0] = 0;
    s_state = GENERAL_OTA_IDLE;
    s_inited = true;
    ESP_LOGI(TAG, "running firmware %s, url %s", general_ota_running_version(),
             s_cfg.url ? s_cfg.url : CONFIG_GENERAL_OTA_URL);
    return ESP_OK;
}

bool general_ota_busy(void)
{
    return s_state == GENERAL_OTA_CHECKING || s_state == GENERAL_OTA_DOWNLOADING;
}

static esp_err_t start_task(TaskFunction_t fn, const char *name)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (general_ota_busy()) return ESP_ERR_INVALID_STATE;
    if (s_state == GENERAL_OTA_SUCCESS) return ESP_ERR_INVALID_STATE; /* reboot first */
    if (xTaskCreate(fn, name, OTA_TASK_STACK, NULL, OTA_TASK_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t general_ota_check(void)
{
    return start_task(check_task, "ota_check");
}

esp_err_t general_ota_update(void)
{
    return start_task(update_task, "ota_update");
}

general_ota_state_t general_ota_get_state(void)
{
    return s_state;
}

const char *general_ota_running_version(void)
{
    return esp_app_get_description()->version;
}

const char *general_ota_available_version(void)
{
    return s_available;
}

void general_ota_reboot(void)
{
    ESP_LOGI(TAG, "rebooting");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}
