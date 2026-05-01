idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_twai esp_timer esp_driver_gpio 
)
