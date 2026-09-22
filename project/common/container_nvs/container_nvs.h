/*
   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
 
   Created by Aram Vartanyan, based on Espressif's nvs_flash component, (C) 2022
*/

#ifndef _CONTAINERNVS_H_
#define _CONTAINERNVS_H_

#pragma once
//includes

#ifdef __cplusplus
extern "C" {
#endif

/*
*@brief Container Storage Initialisation
*
*/
esp_err_t ContainerNvsInit(void);

/*
*@brief Get value of container by namespace
*@param
*
*/
esp_err_t ContainerNvsGet(const char *name_space, const char *key, uint8_t *val, size_t *val_size);

/*
*@brief Set value to the container by namespace
*@param
*
*/
esp_err_t ContainerNvsSet(const char *name_space, const char *key, const uint8_t *val, const size_t val_len);

/*
*@brief Delete container value by namespace
*@param
*
*/
esp_err_t ContainerNvsDelete(const char *name_space, const char *key);

/*
*@brief Delete namespace
*@param
*
*/
esp_err_t ContainerNvsDeleteNamespace(const char *name_space);

/*
*@brief Erase all data in container storage
*
*/
void ContainerNvsErase(void);

#ifdef __cplusplus
}
#endif

#endif /* _CONTAINERNVS_H_ */
