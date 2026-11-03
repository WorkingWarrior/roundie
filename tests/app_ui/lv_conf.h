#ifndef LV_CONF_H
#define LV_CONF_H
#define LV_COLOR_DEPTH 16
#define LV_MEM_SIZE (64 * 1024)
#define LV_USE_OS LV_OS_NONE
#define LV_USE_THORVG_INTERNAL 0
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_ASSERT_OBJ 1
#define LV_USE_ASSERT_MEM_INTEGRITY 1
/* As in the firmware: the generated fonts with Polish letters. */
#define LV_FONT_CUSTOM_DECLARE LV_FONT_DECLARE(roundie_font_14)
#define LV_FONT_DEFAULT &roundie_font_14
#define LV_USE_SNAPSHOT 1 /* tests/test_app_round.c previews */
#endif
