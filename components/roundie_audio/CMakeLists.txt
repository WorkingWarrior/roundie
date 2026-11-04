idf_component_register(SRCS "audio.c" "tone.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_i2c
                       PRIV_REQUIRES esp_driver_i2s esp_driver_gpio esp_codec_dev freertos)
