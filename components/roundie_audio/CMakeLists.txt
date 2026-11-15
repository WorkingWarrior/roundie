idf_component_register(SRCS "audio.c" "tone.c" "stream.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES roundie_board esp_codec_dev freertos esp_timer heap)
