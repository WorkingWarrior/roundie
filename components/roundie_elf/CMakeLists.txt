idf_component_register(SRCS "validate.c" "manager.c" "executor.c" "idf/loader.c" "crash.c" "idf/crash_target.c" "idf/executor_stack.c"
    INCLUDE_DIRS "include" "../../include"
    PRIV_REQUIRES elf_loader esp_timer heap esp_system esp_mm esp_hw_support hal soc mbedtls xtensa roundie_app_ui roundie_audio roundie_platform)
# Panic hook of the retained crash record (idf/crash_target.c, M9).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
