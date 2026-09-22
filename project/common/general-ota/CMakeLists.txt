idf_component_register(SRCS "general_ota.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_https_ota esp_http_client app_update esp_app_format mbedtls)
