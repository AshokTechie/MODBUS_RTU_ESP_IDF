idf_component_register(
    SRCS "app_config.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash nvs_manager json storage storage_manager
)
