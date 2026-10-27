// LVGL 8.4 configuration for the 7" MFD. Anything not set here takes LVGL's default
// from lv_conf_internal.h.
#ifndef LV_CONF_H
#define LV_CONF_H
#include <stdint.h>

#define LV_COLOR_DEPTH     16
#define LV_COLOR_16_SWAP   0        // RGB panel takes native little-endian RGB565
#define LV_COLOR_SCREEN_TRANSP 0

// Allocations in PSRAM (lv_mem_psram.h): the internal RAM is kept for WiFi and drivers.
#define LV_MEM_CUSTOM      1
#define LV_MEM_CUSTOM_INCLUDE  "lv_mem_psram.h"
#define LV_MEM_CUSTOM_ALLOC    lv_psram_alloc
#define LV_MEM_CUSTOM_FREE     lv_psram_free
#define LV_MEM_CUSTOM_REALLOC  lv_psram_realloc

#define LV_DISP_DEF_REFR_PERIOD  33       // 30 fps cap: less PSRAM traffic
#define LV_INDEV_DEF_READ_PERIOD 15
#define LV_DPI_DEF               130

#ifdef ARDUINO
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())
#else
#define LV_TICK_CUSTOM 0
#endif

#define LV_DRAW_COMPLEX        1
#define LV_SHADOW_CACHE_SIZE   0
#define LV_CIRCLE_CACHE_SIZE   32
#define LV_LAYER_SIMPLE_BUF_SIZE (24 * 1024)
#define LV_IMG_CACHE_DEF_SIZE  0
#define LV_GRADIENT_MAX_STOPS  2

#define LV_USE_LOG             0
#define LV_USE_ASSERT_NULL     1
#define LV_USE_ASSERT_MALLOC   1
#define LV_USE_ASSERT_STYLE    0
#define LV_USE_PERF_MONITOR    0
#define LV_USE_MEM_MONITOR     0
#define LV_USE_USER_DATA       1

#define LV_FONT_MONTSERRAT_12  1
#define LV_FONT_MONTSERRAT_14  1
#define LV_FONT_MONTSERRAT_16  1
#define LV_FONT_MONTSERRAT_18  1
#define LV_FONT_MONTSERRAT_20  1
#define LV_FONT_MONTSERRAT_24  1
#define LV_FONT_MONTSERRAT_28  1
#define LV_FONT_MONTSERRAT_32  1
#define LV_FONT_MONTSERRAT_40  1
#define LV_FONT_DEFAULT        &lv_font_montserrat_16

#define LV_USE_ARC        0
#define LV_USE_BAR        0
#define LV_USE_BTN        1
#define LV_USE_BTNMATRIX  1
#define LV_USE_CANVAS     0
#define LV_USE_CHECKBOX   0
#define LV_USE_DROPDOWN   0
#define LV_USE_IMG        1
#define LV_USE_LABEL      1
#define LV_LABEL_LONG_TXT_HINT 0
#define LV_USE_LINE       0
#define LV_USE_ROLLER     0
#define LV_USE_SLIDER     0
#define LV_USE_SWITCH     0
#define LV_USE_TEXTAREA   1
#define LV_USE_TABLE      0

#define LV_USE_ANIMIMG    0
#define LV_USE_CALENDAR   0
#define LV_USE_CHART      0
#define LV_USE_COLORWHEEL 0
#define LV_USE_IMGBTN     0
#define LV_USE_KEYBOARD   1
#define LV_USE_LED        0
#define LV_USE_LIST       0
#define LV_USE_MENU       0
#define LV_USE_METER      0
#define LV_USE_MSGBOX     0
#define LV_USE_SPAN       0
#define LV_USE_SPINBOX    0
#define LV_USE_SPINNER    0
#define LV_USE_TABVIEW    0
#define LV_USE_TILEVIEW   0
#define LV_USE_WIN        0

#define LV_USE_FLEX       1
#define LV_USE_GRID       0

#define LV_USE_THEME_DEFAULT 0
#define LV_USE_THEME_BASIC   0
#define LV_USE_THEME_MONO    0

#define LV_BUILD_EXAMPLES 0
#endif // LV_CONF_H
