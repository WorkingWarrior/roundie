cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(roundie_usb_mtp_compile_probe)

# Compile component archives only. There is deliberately no app_main and no
# deployable application. Never use this project for a hardware experiment.
add_custom_target(probe_flash_forbidden
    COMMAND ${CMAKE_COMMAND} -E echo "Compile-only USB probe: deployment forbidden"
    COMMAND ${CMAKE_COMMAND} -E false)
foreach(PROBE_FLASH_TARGET flash app-flash bootloader-flash partition-table-flash otadata-flash)
    if(TARGET ${PROBE_FLASH_TARGET})
        add_dependencies(${PROBE_FLASH_TARGET} probe_flash_forbidden)
    endif()
endforeach()
