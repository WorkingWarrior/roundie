cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "0.0.0-otg-probe")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# The firmware's OTA path accepts only this project name. The image is a
# lab probe that never confirms itself, so the bootloader rolls it back.
project(roundie_rep)

# Direct flashing is refused: the probe only ever travels through RO1 into the
# spare OTA slot, never over the bootloader or partition table.
add_custom_target(probe_flash_forbidden
    COMMAND ${CMAKE_COMMAND} -E echo "OTG probe: install through RO1 only"
    COMMAND ${CMAKE_COMMAND} -E false)
foreach(PROBE_FLASH_TARGET flash app-flash bootloader-flash partition-table-flash otadata-flash erase-flash)
    if(TARGET ${PROBE_FLASH_TARGET})
        add_dependencies(${PROBE_FLASH_TARGET} probe_flash_forbidden)
    endif()
endforeach()
