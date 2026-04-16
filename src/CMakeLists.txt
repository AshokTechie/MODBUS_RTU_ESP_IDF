idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES board modbus_rtu smartload_protocol storage storage_manager wifi_mgr app_config ota azure_iot smartload_http smartload_mqtt
)
