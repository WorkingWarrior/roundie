idf_component_register(SRCS "storage.c" "bounded_io.c" "io_forward.c" "claim.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES esp_partition joltwallet__littlefs freertos roundie_platform)
