# The public API (include/) is board-independent; one board implementation
# under boards/ is compiled, chosen by CONFIG_ROUNDIE_BOARD_DIR. Requirements
# cannot depend on Kconfig, so they are the union of all boards' needs.
set(srcs "gauge_restore.c" "rtc_time.c")
set(includes "include")
set(priv_includes "")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(board_dir "boards/${CONFIG_ROUNDIE_BOARD_DIR}")
    if(NOT CONFIG_ROUNDIE_BOARD_DIR OR NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/${board_dir}/board.cmake")
        message(FATAL_ERROR "No board implementation for CONFIG_ROUNDIE_BOARD_DIR='${CONFIG_ROUNDIE_BOARD_DIR}'")
    endif()
    if(DEFINED ROUNDIE_BOARD AND NOT ROUNDIE_BOARD STREQUAL CONFIG_ROUNDIE_BOARD_DIR)
        message(FATAL_ERROR "ROUNDIE_BOARD=${ROUNDIE_BOARD} but sdkconfig selects ${CONFIG_ROUNDIE_BOARD_DIR}; "
                            "use a fresh sdkconfig and build directory for another board")
    endif()
    # Sets board_srcs (relative to board_dir).
    include("${CMAKE_CURRENT_LIST_DIR}/${board_dir}/board.cmake")
    foreach(source ${board_srcs})
        list(APPEND srcs "${board_dir}/${source}")
    endforeach()
    list(APPEND includes "${board_dir}/include")
    list(APPEND priv_includes "${board_dir}")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${includes}
                       PRIV_INCLUDE_DIRS ${priv_includes}
                       REQUIRES esp_codec_dev
                       PRIV_REQUIRES esp_lcd esp_lcd_touch esp_driver_i2c esp_driver_i2s esp_driver_spi esp_driver_ledc esp_timer
                                     esp_driver_gpio esp_hw_support esp_hal_gpspi freertos)
