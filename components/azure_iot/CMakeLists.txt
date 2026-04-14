idf_component_register(
    SRCS "azure_iot.c" "sas_token.c"
    INCLUDE_DIRS "include"
    REQUIRES mqtt_mgr ota app_config wifi_mgr json mbedtls
)
