idf_component_register(SRCS "main.c"
                    PRIV_REQUIRES bt nvs_flash driver esp_driver_gpio esp_driver_i2s
                    INCLUDE_DIRS ".")
