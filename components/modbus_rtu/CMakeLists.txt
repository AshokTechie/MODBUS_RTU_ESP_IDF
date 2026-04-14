idf_component_register(
    SRCS "modbus_rtu.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_uart board freertos
)
