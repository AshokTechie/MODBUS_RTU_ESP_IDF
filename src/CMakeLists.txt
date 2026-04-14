idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES board modbus_rtu smartload_protocol storage wifi_mgr app_config ota azure_iot
)
