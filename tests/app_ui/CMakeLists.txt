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
    "${ROOT}/components/roundie_launcher/include" "${ROOT}/components/roundie_packages/include"
    "${ROOT}/components/roundie_app_ui/include" "${ROOT}/components/roundie_elf/include")
target_link_libraries(ui_tests PRIVATE lvgl m pthread)
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
target_link_libraries(launcher_tests PRIVATE lvgl m pthread)
