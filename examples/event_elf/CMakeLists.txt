cmake_minimum_required(VERSION 3.16)
project(rep_event NONE)
find_program(REP_CC xtensa-esp32s3-elf-gcc REQUIRED)
find_program(REP_STRIP xtensa-esp-elf-strip REQUIRED)
execute_process(COMMAND ${REP_CC} -dumpfullversion OUTPUT_VARIABLE REP_GCC
                OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT REP_GCC STREQUAL "15.2.0")
    message(FATAL_ERROR "Use Roundie's pinned IDF v6.1 GCC 15.2.0 toolchain")
endif()
add_custom_command(OUTPUT event.app.elf
    COMMAND ${REP_CC} -std=c11 -Os -Wall -Wextra -Werror -mlongcalls
        -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections
        -fno-builtin -fno-stack-protector -nostdlib -nostartfiles -shared
        -Wl,--gc-sections -Wl,-e,rep_app_entry_v2 -Wl,--undefined=rep_abi_metadata
        -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/../hello_elf/text-padding.ld
        -I${CMAKE_CURRENT_SOURCE_DIR}/../../include
        ${CMAKE_CURRENT_SOURCE_DIR}/event.c -o event.app.elf
    COMMAND ${REP_STRIP} --strip-unneeded --remove-section=.comment
        --remove-section=.got.loc --remove-section=.dynamic
        --remove-section=.xt.lit --remove-section=.xt.prop
        --remove-section=.xtensa.info event.app.elf
    DEPENDS event.c ../hello_elf/text-padding.ld ../../include/rep/rep_app_v2.h VERBATIM)
add_custom_target(elf ALL DEPENDS event.app.elf)
