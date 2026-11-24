idf_component_register(SRCS "main.c" INCLUDE_DIRS "."
    PRIV_REQUIRES lingasr esp_driver_i2s esp_driver_gpio esp_timer heap)
