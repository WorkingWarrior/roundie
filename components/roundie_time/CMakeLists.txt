# Roundie Core time (docs/roundie-core.md, M3): shared wall-clock policy,
# local time and the RT1 parser; idf/clock_port.c is the ESP-IDF port.
idf_component_register(SRCS "wall_clock.c" "local_time.c" "clock_retention.c" "time_protocol.c" "idf/clock_port.c"
                       INCLUDE_DIRS "include"
                       REQUIRES roundie_board roundie_elf
                       PRIV_REQUIRES roundie_platform esp_hw_support esp_libc esp_system freertos nvs_flash)
