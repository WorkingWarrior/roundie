idf_component_register(SRCS "boot_guard.c" "persistence.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_system nvs_flash)
