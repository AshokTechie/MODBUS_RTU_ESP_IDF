idf_component_register(
    SRCS "smartload_mqtt.c"
    INCLUDE_DIRS "include"
    REQUIRES mqtt json nvs_manager storage_manager mbedtls
)
