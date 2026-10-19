idf_component_register(SRCS "launcher.c" "render.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES roundie_packages roundie_elf roundie_app_ui lvgl esp_timer)
