# Roundie Core on the Linux desktop (docs/roundie-core.md, decision D3).
# The firmware keeps its ESP-IDF CMake build; this file builds the same Core
# sources against platform/linux. LVGL comes from managed_components, so run
# `idf.py reconfigure` (or any firmware build) once to fetch it.
project('roundie-desktop', 'c',
        version: '0.1',
        meson_version: '>=1.3',
        default_options: ['c_std=gnu23', 'warning_level=0', 'buildtype=debugoptimized'])

cc = meson.get_compiler('c')
lvgl_root = 'managed_components/lvgl__lvgl'
if not import('fs').is_file(lvgl_root / 'lvgl.h')
  error('LVGL is missing: run a firmware build or idf.py reconfigure to fetch managed_components')
endif

lvgl_sources = run_command('find', lvgl_root / 'src', '-name', '*.c', check: true).stdout().strip().split('\n')

platform_inc = include_directories('platform/linux/include', 'platform/linux')
lvgl_inc = include_directories(lvgl_root)
lvgl = static_library('lvgl', lvgl_sources,
                      include_directories: [platform_inc, lvgl_inc],
                      # LVGL 9.5.0 lv_roller.c shifts negative values left; GCC defines
                      # that (as in the firmware build), so UBSan's report is not a fault.
                      c_args: ['-DLV_CONF_INCLUDE_SIMPLE', '-w', '-fno-sanitize=shift-base'])
lvgl_dep = declare_dependency(link_with: lvgl, include_directories: lvgl_inc,
                              compile_args: ['-DLV_CONF_INCLUDE_SIMPLE'])

core_inc = include_directories(
  'include', 'main',
  'components/roundie_platform/include',
  'components/roundie_launcher/include',
  'components/roundie_app_ui/include',
  'components/roundie_packages', 'components/roundie_packages/include',
  'components/roundie_elf/include',
  'components/roundie_fonts/include',
  'components/roundie_audio/include',
  'components/roundie_board/include',
  'components/roundie_power/include',
  'components/roundie_boot_guard/include',
  'components/roundie_time/include',
  'components/roundie_board/boards/waveshare_s3_touch_lcd_1_85b')

strict = ['-Wall', '-Wextra', '-Werror']

# Roundie Core: the same files the firmware compiles.
core_sources = files(
  'components/roundie_launcher/launcher.c',
  'components/roundie_launcher/render.c',
  'components/roundie_app_ui/bridge.c',
  'components/roundie_app_ui/render.c',
  'components/roundie_packages/format.c',
  'components/roundie_packages/manager.c',
  'components/roundie_elf/validate.c',
  'components/roundie_elf/validate_x86_64.c',
  'components/roundie_elf/manager.c',
  'main/display_scale.c',
  'main/power_gui.c',
  'components/roundie_power/power.c',
  'components/roundie_time/wall_clock.c',
  'components/roundie_time/local_time.c',
  'components/roundie_time/clock_retention.c',
  'components/roundie_time/time_protocol.c',
  'components/roundie_board/rtc_time.c',
  'components/roundie_board/boards/waveshare_s3_touch_lcd_1_85b/sensor_codec.c',
  'components/roundie_fonts/roundie_font_14.c',
  'components/roundie_fonts/roundie_font_20.c',
  'components/roundie_fonts/roundie_font_28.c',
  'components/roundie_fonts/roundie_font_48.c')

sdl = dependency('sdl3', required: get_option('desktop'))
vulkan = dependency('vulkan', required: get_option('desktop'))
psa = dependency('tfpsacrypto', required: get_option('desktop'))
threads = dependency('threads')
libm = cc.find_library('m')
libdl = cc.find_library('dl', required: false)

# linux-x64.rep1 applications (docs/native-apps.md): the desktop's package
# target, and the REP services ld.so binds their imports to.
native_args = ['-DROUNDIE_TARGET_LINUX_X64=1']
rep_exports = meson.current_source_dir() / 'platform/linux/rep_exports.list'

if sdl.found() and vulkan.found() and psa.found()
  executable('roundie-desktop',
             core_sources,
             'platform/linux/platform.c',
             'platform/linux/app_runtime.c',
             'platform/linux/audio.c',
             'components/roundie_audio/stream.c',
             'components/roundie_elf/linux/loader.c',
             'components/roundie_elf/linux/executor.c',
             'platform/linux/board.c',
             'platform/linux/clock.c',
             'platform/linux/power_worker.c',
             'platform/linux/desktop.c',
             'platform/linux/desktop_input.c',
             'platform/linux/vk_present.c',
             include_directories: [core_inc, platform_inc, include_directories('components/roundie_elf')],
             c_args: strict + native_args + ['-DPACKAGES_DIRECTORY="storage/packages"'],
             link_args: ['-Wl,--dynamic-list=' + rep_exports],
             link_depends: rep_exports,
             dependencies: [lvgl_dep, sdl, vulkan, psa, threads, libm, libdl])
endif
