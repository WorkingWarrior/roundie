idf_component_register(SRCS "boot_guard.c" "persistence.c" "history.c" "diagnostics.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_system nvs_flash)
