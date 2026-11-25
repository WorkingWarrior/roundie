# WatchFaceHost and installable watch faces (docs/watch-face-platform.md).
idf_component_register(SRCS "format.c" "text.c" "host.c" "declarative.c" "fallback.c" "gui.c" "store.c"
    INCLUDE_DIRS "include" "../../include"
    REQUIRES roundie_elf
    PRIV_REQUIRES lvgl roundie_fonts roundie_platform mbedtls)
