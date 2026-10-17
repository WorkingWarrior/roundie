idf_component_register(SRCS "main.c"
                       REQUIRES roundie_board lvgl esp_timer esp_lcd
                       PRIV_REQUIRES esp_app_format spi_flash esp_psram heap)
