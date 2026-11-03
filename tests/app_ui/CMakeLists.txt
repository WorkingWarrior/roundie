cmake_minimum_required(VERSION 3.20)
project(roundie_ui_host C CXX ASM)
set(CMAKE_C_STANDARD 23)
add_compile_options(-g -fsanitize=address,undefined -fno-omit-frame-pointer)
add_link_options(-fsanitize=address,undefined)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
add_subdirectory("${ROOT}/managed_components/lvgl__lvgl" lvgl)
# LVGL's default font: an object library, so every executable links its objects.
add_library(roundie_fonts OBJECT "${ROOT}/components/roundie_fonts/roundie_font_14.c"
    "${ROOT}/components/roundie_fonts/roundie_font_28.c"
    "${ROOT}/components/roundie_fonts/roundie_font_48.c")
target_link_libraries(roundie_fonts PRIVATE lvgl)
add_executable(ui_tests ../test_app_ui.c
    "${ROOT}/components/roundie_app_ui/bridge.c"
    "${ROOT}/components/roundie_app_ui/render.c"
    "${ROOT}/components/roundie_launcher/launcher.c"
    "${ROOT}/components/roundie_launcher/render.c"
    "${ROOT}/components/roundie_packages/format.c"
    "${ROOT}/components/roundie_elf/validate.c"
    "${ROOT}/examples/ui_elf/ui.c")
target_compile_options(ui_tests PRIVATE -Wall -Wextra -Werror)
target_include_directories(ui_tests PRIVATE ../elf_mocks "${ROOT}/include"
    "${ROOT}/components/roundie_fonts/include"
    "${ROOT}/components/roundie_launcher/include" "${ROOT}/components/roundie_packages/include"
    "${ROOT}/components/roundie_app_ui/include" "${ROOT}/components/roundie_elf/include")
target_link_libraries(ui_tests PRIVATE roundie_fonts lvgl m pthread)
add_executable(round_tests ../test_app_round.c
    "${ROOT}/components/roundie_app_ui/bridge.c"
    "${ROOT}/components/roundie_app_ui/render.c"
    "${ROOT}/components/roundie_elf/validate.c"
    "${ROOT}/examples/round_elf/round.c"
    "${ROOT}/examples/watch_elf/watch.c"
    "${ROOT}/examples/stopwatch_elf/stopwatch.c")
# Three example applications in one host executable: rename the others' exports.
set_source_files_properties("${ROOT}/examples/watch_elf/watch.c" PROPERTIES COMPILE_DEFINITIONS
    "rep_app_entry_v3=watch_app_entry_v3;rep_abi_metadata=watch_abi_metadata")
set_source_files_properties("${ROOT}/examples/stopwatch_elf/stopwatch.c" PROPERTIES COMPILE_DEFINITIONS
    "rep_app_entry_v3=stopwatch_app_entry_v3;rep_abi_metadata=stopwatch_abi_metadata")
target_compile_options(round_tests PRIVATE -Wall -Wextra -Werror)
target_include_directories(round_tests PRIVATE ../elf_mocks "${ROOT}/include"
    "${ROOT}/components/roundie_fonts/include" "${ROOT}/components/roundie_app_ui/include" "${ROOT}/components/roundie_elf/include")
target_link_libraries(round_tests PRIVATE roundie_fonts lvgl m pthread)
add_executable(launcher_tests ../test_launcher.c
    "${ROOT}/main/gui_diagnostics.c"
    "${ROOT}/components/roundie_launcher/launcher.c"
    "${ROOT}/components/roundie_launcher/render.c"
    "${ROOT}/components/roundie_packages/format.c")
target_compile_options(launcher_tests PRIVATE -Wall -Wextra -Werror)
target_include_directories(launcher_tests PRIVATE ../elf_mocks ../gui_mocks "${ROOT}/include" "${ROOT}/main"
    "${ROOT}/components/roundie_launcher/include"
    "${ROOT}/components/roundie_packages/include"
    "${ROOT}/components/roundie_app_ui/include" "${ROOT}/components/roundie_elf/include")
target_link_libraries(launcher_tests PRIVATE roundie_fonts lvgl m pthread)
add_executable(display_tests ../test_display_render.c)
target_compile_options(display_tests PRIVATE -Wall -Wextra -Werror)
target_link_libraries(display_tests PRIVATE roundie_fonts lvgl m pthread)
