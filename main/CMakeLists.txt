idf_component_register(SRCS "main.c" "app_runtime.c" "clock_diagnostic.c" "display_scale.c" "storage_diagnostics.c" "ota_diagnostics.c" "recovery_diagnostics.c" "elf_diagnostics.c" "package_diagnostics.c" "face_diagnostics.c" "gui_diagnostics.c" "power_manager.c" "power_gui.c" "storage_mode_screen.c" "storage_usb.c" "storage_import.c" "audio_diagnostics.c"
                       LDFRAGMENTS "lvgl_iram.lf"
                       REQUIRES roundie_board lvgl roundie_fonts esp_timer
                       PRIV_REQUIRES esp_app_format spi_flash esp_psram heap roundie_platform roundie_time
                                     esp_driver_usb_serial_jtag roundie_storage roundie_ota roundie_boot_guard roundie_elf roundie_app_ui roundie_packages roundie_launcher roundie_watchface roundie_power roundie_storage_mode roundie_audio espressif__esp_tinyusb fatfs vfs esp_blockdev app_update esp_partition nvs_flash mbedtls)
