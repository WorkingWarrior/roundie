cmake_minimum_required(VERSION 3.16)

set(ROUNDIE_IDF_COMMIT "fff9895c82d744c7237be8847347bdd1b07c6643")
execute_process(
    COMMAND git -C "$ENV{IDF_PATH}" rev-parse HEAD
    OUTPUT_VARIABLE ROUNDIE_ACTIVE_IDF_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE ROUNDIE_IDF_GIT_RESULT)
if(NOT ROUNDIE_IDF_GIT_RESULT EQUAL 0 OR
   NOT ROUNDIE_ACTIVE_IDF_COMMIT STREQUAL ROUNDIE_IDF_COMMIT)
    message(FATAL_ERROR "Roundie requires ESP-IDF v6.1 commit ${ROUNDIE_IDF_COMMIT}")
endif()

set(ROUNDIE_PROFILE "legacy" CACHE STRING "Flash layout: legacy or rep")
set_property(CACHE ROUNDIE_PROFILE PROPERTY STRINGS legacy rep)
if(ROUNDIE_PROFILE STREQUAL "rep")
    if(NOT DEFINED SDKCONFIG)
        set(SDKCONFIG "${CMAKE_SOURCE_DIR}/sdkconfig.rep")
    endif()
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.rep.defaults")
    set(ROUNDIE_PROJECT_NAME roundie_rep)
elseif(ROUNDIE_PROFILE STREQUAL "legacy")
    if(NOT DEFINED SDKCONFIG)
        set(SDKCONFIG "${CMAKE_SOURCE_DIR}/sdkconfig")
    endif()
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
    set(ROUNDIE_PROJECT_NAME roundie)
else()
    message(FATAL_ERROR "Unknown ROUNDIE_PROFILE: ${ROUNDIE_PROFILE}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "0.2.4")
endif()
project(${ROUNDIE_PROJECT_NAME})

# LittleFS 1.22.2 otherwise converts a negative lfs_fs_size() result to a
# successful full-disk report. Keep managed sources pristine and fail closed
# on upstream drift. Only this translation unit is replaced in the build.
idf_component_get_property(ROUNDIE_LITTLEFS_DIR joltwallet__littlefs COMPONENT_DIR)
idf_component_get_property(ROUNDIE_LITTLEFS_LIB joltwallet__littlefs COMPONENT_LIB)
set(ROUNDIE_LITTLEFS_SOURCE "${ROUNDIE_LITTLEFS_DIR}/src/esp_littlefs.c")
set(ROUNDIE_LITTLEFS_PATCHED "${CMAKE_BINARY_DIR}/esp_littlefs_capacity.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/tools/patch_littlefs.py" "${ROUNDIE_LITTLEFS_SOURCE}")
execute_process(COMMAND ${PYTHON} "${CMAKE_SOURCE_DIR}/tools/patch_littlefs.py"
    "${ROUNDIE_LITTLEFS_SOURCE}" "${ROUNDIE_LITTLEFS_PATCHED}"
    RESULT_VARIABLE ROUNDIE_PATCH_RESULT)
if(NOT ROUNDIE_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR "LittleFS capacity error patch failed")
endif()
get_target_property(ROUNDIE_LITTLEFS_SOURCES ${ROUNDIE_LITTLEFS_LIB} SOURCES)
list(REMOVE_ITEM ROUNDIE_LITTLEFS_SOURCES "${ROUNDIE_LITTLEFS_SOURCE}")
list(APPEND ROUNDIE_LITTLEFS_SOURCES "${ROUNDIE_LITTLEFS_PATCHED}")
set_property(TARGET ${ROUNDIE_LITTLEFS_LIB} PROPERTY SOURCES "${ROUNDIE_LITTLEFS_SOURCES}")
set_property(SOURCE "${ROUNDIE_LITTLEFS_PATCHED}" DIRECTORY "${ROUNDIE_LITTLEFS_DIR}"
    PROPERTY COMPILE_FLAGS "-DLFS_CONFIG=lfs_config.h")

if(NOT CONFIG_ESPTOOLPY_FLASHSIZE_16MB OR
   NOT CONFIG_PARTITION_TABLE_OFFSET EQUAL 32768)
    message(FATAL_ERROR "Roundie requires 16 MiB Flash and table at 0x8000")
endif()
if(ROUNDIE_PROFILE STREQUAL "rep")
    if(NOT CONFIG_PARTITION_TABLE_CUSTOM OR
       NOT CONFIG_PARTITION_TABLE_CUSTOM_FILENAME STREQUAL "partitions/rep.csv" OR
       NOT CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)
        message(FATAL_ERROR "REP profile/config mismatch; regenerate sdkconfig.rep")
    endif()
    if(NOT CONFIG_ROUNDIE_RECOVERY_USB)
        message(FATAL_ERROR "REP requires USB recovery independent of GUI and development RTC/OTA commands")
    endif()
else()
    if(NOT CONFIG_PARTITION_TABLE_SINGLE_APP OR CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)
        message(FATAL_ERROR "Legacy profile/config mismatch; regenerate sdkconfig")
    endif()
endif()
file(WRITE "${CMAKE_BINARY_DIR}/roundie-profile.txt" "${ROUNDIE_PROFILE}\n")
if(CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK OR CONFIG_SECURE_BOOT OR CONFIG_SECURE_FLASH_ENC_ENABLED)
    message(FATAL_ERROR "Milestone 2C.1 does not support security/eFuse policy changes")
endif()
if(CONFIG_ROUNDIE_OTA_TEST_UNCONFIRMED_RESET AND NOT PROJECT_VER MATCHES "-rollback-test$")
    message(FATAL_ERROR "Armed OTA images require an explicit PROJECT_VER ending in -rollback-test")
endif()
message(STATUS "Roundie layout: ${ROUNDIE_PROFILE}; image: ${ROUNDIE_PROJECT_NAME}.bin")
if(CONFIG_ROUNDIE_BOOT_GUARD_TEST AND NOT PROJECT_VER MATCHES "-boot-test$")
    message(FATAL_ERROR "BootGuard diagnostic images require PROJECT_VER ending in -boot-test")
endif()

# Generated IDF flashing bypasses the live partition/profile guards. Require
# the project updaters for both profiles, including app-only development work.
add_custom_target(roundie_raw_flash_forbidden
    COMMAND ${CMAKE_COMMAND} -E echo
        "Raw IDF flashing is disabled. Use tools/flash_rep.py or tools/flash_bringup.py."
    COMMAND ${CMAKE_COMMAND} -E false)
foreach(ROUNDIE_FLASH_TARGET flash app-flash bootloader-flash partition-table-flash otadata-flash)
    if(TARGET ${ROUNDIE_FLASH_TARGET})
        add_dependencies(${ROUNDIE_FLASH_TARGET} roundie_raw_flash_forbidden)
    endif()
endforeach()
