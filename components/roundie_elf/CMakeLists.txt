idf_component_register(SRCS "validate.c" "manager.c" "executor.c"
    INCLUDE_DIRS "include" "../../include"
    PRIV_REQUIRES elf_loader esp_timer heap esp_system roundie_app_ui roundie_audio)
