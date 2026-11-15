#ifndef LV_CONF_H
#define LV_CONF_H

/* LVGL 9.5.0 configuration of the Linux desktop. The values that change
 * rendering mirror the firmware's LVGL Kconfig (sdkconfig: CONFIG_LV_*), so
 * the desktop draws the same pixels; tests/test_desktop.py compares them
 * (docs/roundie-core.md). */

#define LV_COLOR_DEPTH 16
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_MEM_SIZE (64 * 1024)
#define LV_DEF_REFR_PERIOD 33
#define LV_DPI_DEF 130
#define LV_USE_OS LV_OS_NONE

#define LV_USE_DRAW_SW 1
#define LV_DRAW_SW_DRAW_UNIT_CNT 1
#define LV_DRAW_SW_COMPLEX 1
#define LV_DRAW_SW_SHADOW_CACHE_SIZE 0
#define LV_DRAW_SW_CIRCLE_CACHE_SIZE 4
#define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NONE

#define LV_FONT_MONTSERRAT_14 1
/* Google Sans Flex Medium with Polish letters, as the firmware's top-level
 * CMakeLists.txt sets it (LVGL's Kconfig cannot name a custom default). */
#define LV_FONT_CUSTOM_DECLARE LV_FONT_DECLARE(roundie_font_14)
#define LV_FONT_DEFAULT &roundie_font_14
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

#define LV_USE_THORVG_INTERNAL 0
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_ASSERT_STYLE 0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ 0

#endif
