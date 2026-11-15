idf_component_register(SRCS "bridge.c" "render.c"
    INCLUDE_DIRS "include" "../../include"
    PRIV_REQUIRES lvgl roundie_fonts roundie_platform)
