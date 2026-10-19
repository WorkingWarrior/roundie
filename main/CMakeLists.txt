idf_component_register(SRCS "main.c" "time_protocol.c" "clock_diagnostic.c" "storage_diagnostics.c" "ota_diagnostics.c" "recovery_diagnostics.c" "elf_diagnostics.c" "package_diagnostics.c"
                       REQUIRES roundie_board lvgl esp_timer esp_lcd
                       PRIV_REQUIRES esp_app_format spi_flash esp_psram heap
                                     esp_driver_usb_serial_jtag roundie_storage roundie_ota roundie_boot_guard roundie_elf roundie_app_ui roundie_packages roundie_launcher app_update esp_partition nvs_flash mbedtls)
