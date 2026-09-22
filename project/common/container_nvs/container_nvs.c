/*
   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
 
   Created by Aram Vartanyan, based on Espressif's nvs_flash component, (C) 2022
 */

#include <string.h>
#include <esp_log.h>
#include "nvs_flash.h"
#include "nvs.h"

/* NVS partition label for this storage. container_nvs uses the partition-specific
   NVS API, which needs a partition label; this is the library's only project-
   specific knob. It is configurable via menuconfig (Component config -> Container
   NVS) and defaults to the standard "nvs" partition present in the default
   partition table, so the library is framework-agnostic and works out of the box. */
#ifndef CONFIG_CONTAINER_NVS_PARTITION
#define CONFIG_CONTAINER_NVS_PARTITION "nvs"
#endif
#define CONTAINER_PARTITION CONFIG_CONTAINER_NVS_PARTITION

static const char *TAG = "Container";

esp_err_t ContainerNvsInit(void)
{
	esp_err_t err = nvs_flash_init_partition(CONTAINER_PARTITION);
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) { //to be checked
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase_partition(CONTAINER_PARTITION));
        err = nvs_flash_init_partition(CONTAINER_PARTITION);
    }
    //ESP_ERROR_CHECK( err );
	return err;
}

esp_err_t ContainerNvsGet(const char *name_space, const char *key, uint8_t *val, size_t *val_size)
{
	nvs_handle handle;
    esp_err_t err = nvs_open_from_partition(CONTAINER_PARTITION, name_space, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_get_blob(handle, key, val, val_size);
        nvs_close(handle);
    } else {
        return err;
    }
	return err;
}

esp_err_t ContainerNvsSet(const char *name_space, const char *key, const uint8_t *val, const size_t val_len)
{
    nvs_handle handle;
    esp_err_t err = nvs_open_from_partition(CONTAINER_PARTITION, name_space, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%d) opening NVS handle!", err);
    } else {
        err = nvs_set_blob(handle, key, val, val_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write %s", key);
        } else {
            nvs_commit(handle);
        }
        nvs_close(handle);
    }

    return err;
}

esp_err_t ContainerNvsDelete(const char *name_space, const char *key)
{
    nvs_handle handle;
    esp_err_t err = nvs_open_from_partition(CONTAINER_PARTITION, name_space, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%d) opening NVS handle!", err);
    } else {
        err = nvs_erase_key(handle, key);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to delete %s", key);
        } else {
            nvs_commit(handle);
        }
        nvs_close(handle);
    }

    return err;
}

esp_err_t ContainerNvsDeleteNamespace(const char *name_space)
{
    nvs_handle handle;
    esp_err_t err = nvs_open_from_partition(CONTAINER_PARTITION, name_space, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%d) opening NVS handle!", err);
    } else {
        err = nvs_erase_all(handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to delete %s", name_space);
        } else {
            nvs_commit(handle);
        }
        nvs_close(handle);
    }

    return err;
}

void ContainerNvsErase(void)
{
    ESP_ERROR_CHECK(nvs_flash_erase_partition(CONTAINER_PARTITION));
}
