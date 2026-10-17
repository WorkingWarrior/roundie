idf_component_register(SRCS "board.c" "lcd_init_tables.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_lcd esp_lcd_touch
                       PRIV_REQUIRES esp_driver_i2c esp_driver_spi esp_driver_ledc
                                     esp_driver_gpio freertos)
