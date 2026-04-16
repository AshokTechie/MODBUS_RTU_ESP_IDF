idf_component_register(
    SRCS "smartload_http.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client json freertos wifi_mgr app_config nvs_manager storage_manager mbedtls
)
