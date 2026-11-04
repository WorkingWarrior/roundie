idf_component_register(SRCS "sensor_codec.c" "board.c" "lcd_init_tables.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_lcd esp_lcd_touch esp_driver_i2c
                       PRIV_REQUIRES esp_driver_spi esp_driver_ledc esp_timer
                                     esp_driver_gpio esp_hw_support esp_hal_gpspi freertos)
