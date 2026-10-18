idf_component_register(SRCS "ota.c" "ota_protocol.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES app_update bootloader_support esp_app_format
                                     esp_partition mbedtls)
