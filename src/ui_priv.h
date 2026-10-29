#pragma once
// Shared between the UI source files: theme, page interface, widget and drawing helpers.
#include <lvgl.h>
#include <math.h>

#include "telemetry.h"
#include "ui.h"

// ---------------------------------------------------------------- theme (Garmin-like)

#define COL_BG        lv_color_hex(0x000000)
#define COL_PANEL     lv_color_hex(0x14181C)
#define COL_PANEL2    lv_color_hex(0x1E242A)
#define COL_BORDER    lv_color_hex(0x4A5560)
#define COL_TEXT      lv_color_hex(0xFFFFFF)
#define COL_DIM       lv_color_hex(0x8A96A0)
#define COL_CYAN      lv_color_hex(0x00E5FF)
#define COL_GREEN     lv_color_hex(0x00E040)
#define COL_MAGENTA   lv_color_hex(0xFF40FF)
#define COL_YELLOW    lv_color_hex(0xFFE000)
#define COL_RED       lv_color_hex(0xFF2020)
#define COL_SKY       lv_color_hex(0x1060C8)
#define COL_GROUND    lv_color_hex(0x7A5028)
#define COL_TAPE      lv_color_hex(0x202830)

#define FONT_12 (&lv_font_montserrat_12)
#define FONT_14 (&lv_font_montserrat_14)
#define FONT_16 (&lv_font_montserrat_16)
#define FONT_18 (&lv_font_montserrat_18)
#define FONT_20 (&lv_font_montserrat_20)
#define FONT_24 (&lv_font_montserrat_24)
#define FONT_28 (&lv_font_montserrat_28)
#define FONT_32 (&lv_font_montserrat_32)
#define FONT_40 (&lv_font_montserrat_40)

#define TOPBAR_H  44
#define SOFTKEY_H 52
#define MAIN_H    (480 - TOPBAR_H - SOFTKEY_H)
#define SPLIT_LEFT_W 380

// Pixel aspect. The 7" panel's active area is 154.2 x 85.9 mm for 800 x 480 pixels, so a
// pixel is 7.7% wider than it is tall. Horizontal offsets in anything round or rotated
// are multiplied by XS so it looks right on the glass (dr_circle / dr_arc do this).
#define XS 0.9286f

#define DEG2RAD 0.017453292f
#define RAD2DEG 57.29578f

// ---------------------------------------------------------------- shared state

extern Telemetry g_t;           // snapshot taken at the start of every ui_tick
extern uint32_t g_now;          // ms
extern bool g_live;             // flight data is arriving

// ---------------------------------------------------------------- pages

struct PageDef {
    const char *key;            // softkey label
    void (*create)(lv_obj_t *parent, lv_coord_t w, lv_coord_t h);
    void (*update)();           // every ui_tick while visible
    void (*destroy)();          // forget object pointers (the shell deletes the objects)
};
extern const PageDef PAGE_PFD, PAGE_EIS, PAGE_RADIO, PAGE_MAP, PAGE_NRST, PAGE_WX, PAGE_AP, PAGE_SETUP;
extern UiPlatform g_platform;

// Radio page: preselect a field for keypad entry ("com1", "com2", "nav1", "nav2", "xpdr").
void radio_preselect(const char *field);
// Which NAV radio the top bar shows: 1 or 2 (chosen on the RADIO page, kept in flash).
int ui_top_nav();
void ui_set_top_nav(int nav);

// Direct-To window (ui_dct.cpp), over the page area and the softkeys.
void dct_window_build(lv_obj_t *scr, lv_obj_t *softkey);
void dct_window_open();
void dct_window_close();
void dct_window_toggle();
bool dct_window_visible();
void dct_ui_tick();             // every ui_tick: window input, lookups, arrival alert
// The Garmin D-> symbol, centred at (cx, cy).
void dr_dct_icon(lv_draw_ctx_t *dc, lv_coord_t cx, lv_coord_t cy, lv_color_t c);

void ui_toast(const char *msg, lv_color_t color);
// True when the bridge offers firmware newer than what is running.
bool ui_update_available();
void ui_start_update();
// Screen sleep timeout in minutes (0 = never), as set on the Settings page.
int ui_sleep_min();
void ui_set_sleep_min(int minutes);

// ---------------------------------------------------------------- widget helpers

lv_obj_t *ui_box(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h);
lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text);
lv_obj_t *ui_button(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_coord_t w, lv_coord_t h,
                    lv_event_cb_t cb, void *user);
// Button looks: normal, active (green), selected (cyan), disabled.
enum BtnLook { BTN_NORMAL, BTN_ACTIVE, BTN_SELECTED, BTN_ARMED, BTN_DISABLED };
void ui_button_look(lv_obj_t *btn, BtnLook look);
lv_obj_t *ui_button_label(lv_obj_t *btn);
void ui_set_text(lv_obj_t *label, const char *text);       // only when changed
void ui_set_color(lv_obj_t *label, lv_color_t c);
// An object that draws itself in an LV_EVENT_DRAW_MAIN callback.
lv_obj_t *ui_canvas(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, lv_event_cb_t draw_cb);

// ---------------------------------------------------------------- drawing helpers
// For use inside LV_EVENT_DRAW_MAIN. Coordinates are absolute screen pixels.

enum TextAlign { TA_LEFT, TA_CENTER, TA_RIGHT };

void dr_line(lv_draw_ctx_t *dc, float x1, float y1, float x2, float y2, lv_coord_t w, lv_color_t c,
             lv_opa_t opa = LV_OPA_COVER);
void dr_rect(lv_draw_ctx_t *dc, lv_coord_t x1, lv_coord_t y1, lv_coord_t x2, lv_coord_t y2, lv_color_t bg,
             lv_opa_t bg_opa = LV_OPA_COVER, lv_coord_t radius = 0, lv_coord_t border_w = 0,
             lv_color_t border = lv_color_hex(0xFFFFFF));
// Text with its vertical centre at y.
void dr_text(lv_draw_ctx_t *dc, lv_coord_t x, lv_coord_t y, const char *txt, const lv_font_t *font, lv_color_t c,
             TextAlign align = TA_LEFT, lv_opa_t opa = LV_OPA_COVER);
lv_coord_t text_width(const char *txt, const lv_font_t *font);
void dr_poly(lv_draw_ctx_t *dc, const lv_point_t *pts, uint16_t n, lv_color_t c, lv_opa_t opa = LV_OPA_COVER);
void dr_circle(lv_draw_ctx_t *dc, lv_coord_t cx, lv_coord_t cy, lv_coord_t r, lv_color_t c,
               lv_coord_t width /* 0 = filled */, lv_opa_t opa = LV_OPA_COVER);
// Circle of vertical radius r (drawn as an XS-corrected ellipse). Arc from start to end
// degrees: 0 = 3 o'clock, clockwise; r is the outer radius, also XS-corrected.
void dr_arc(lv_draw_ctx_t *dc, lv_coord_t cx, lv_coord_t cy, lv_coord_t r, lv_coord_t w, float start, float end,
            lv_color_t c, bool rounded = false);
// Restrict drawing to a rectangle until clip_pop.
bool clip_push(lv_draw_ctx_t *dc, lv_coord_t x1, lv_coord_t y1, lv_coord_t x2, lv_coord_t y2, const lv_area_t **saved,
               lv_area_t *storage);
void clip_pop(lv_draw_ctx_t *dc, const lv_area_t *saved);

// Fixed-pitch digits (Montserrat digits have different widths): right-aligned at x_right,
// vertically centred at y.
void dr_digits(lv_draw_ctx_t *dc, lv_coord_t x_right, lv_coord_t y, const char *txt, const lv_font_t *font,
               lv_color_t c);
lv_coord_t digit_w(const lv_font_t *font);

// ---------------------------------------------------------------- geometry

static inline float wrap360(float a) {
    a = fmodf(a, 360.0f);
    return a < 0 ? a + 360.0f : a;
}
static inline float wrap180(float a) {
    a = wrap360(a);
    return a > 180.0f ? a - 360.0f : a;
}
// Position of (lat, lon) relative to the aircraft, in nm east / north.
void geo_offset_nm(double lat0, double lon0, double lat, double lon, float &east, float &north);
