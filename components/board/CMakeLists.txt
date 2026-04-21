idf_component_register(
    SRCS "board.c" "board_utils.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_gpio fatfs sdmmc efuse
)
