idf_component_register(
    SRCS "main.c" "pnc32.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bt nvs_flash driver esp_timer esp_hw_support esp_driver_uart
)
