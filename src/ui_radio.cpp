// RADIOS
// COM1/COM2/NAV1/NAV2 active and standby with swap keys, transponder code and mode, and a
// numeric keypad for entering standby frequencies and squawk codes. Full width: the
// keypad sits beside the radios. Split screen: it pops up over them.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_priv.h"

enum Field { F_NONE = -1, F_COM1, F_COM2, F_NAV1, F_NAV2, F_XPDR, F_COUNT };
static const char *FIELD_ID[F_COUNT] = {"com1", "com2", "nav1", "nav2", "xpdr"};
static const char *FIELD_NAME[F_COUNT] = {"COM1", "COM2", "NAV1", "NAV2", "XPDR"};
static const char *XPDR_MODES[] = {"STBY", "GND", "ALT"};

static bool s_narrow;
static lv_obj_t *s_root;
static lv_obj_t *s_act[4], *s_stby_btn[F_COUNT];
static lv_obj_t *s_mode_btn[3];
static lv_obj_t *s_nav_name[2], *s_nav_tag[2];  // NAV1/NAV2 row names: tap one to show it in the top bar
static lv_obj_t *s_pad;                 // keypad container
static lv_obj_t *s_pad_title, *s_pad_entry;
static lv_obj_t *s_keys[12];
static Field s_field = F_NONE;
static Field s_pending = F_NONE;        // radio_preselect() before the page exists
static char s_entry[8];
static uint32_t s_seq;
static bool s_was_live;

static bool is_com(Field f) { return f == F_COM1 || f == F_COM2; }
static bool is_nav(Field f) { return f == F_NAV1 || f == F_NAV2; }
static int max_digits(Field f) { return is_com(f) ? 6 : is_nav(f) ? 5 : 4; }

static const char *stby_text(Field f) {
    const Radios &r = g_t.rad;
    switch (f) {
    case F_COM1: return r.com1_stby;
    case F_COM2: return r.com2_stby;
    case F_NAV1: return r.nav1_stby;
    case F_NAV2: return r.nav2_stby;
    case F_XPDR: return r.xpdr;
    default: return "";
    }
}

static const char *act_text(int i) {
    const Radios &r = g_t.rad;
    const char *v[4] = {r.com1_act, r.com2_act, r.nav1_act, r.nav2_act};
    return v[i];
}

// "1183" -> "118.3__" for COM, "11" -> "11_.__" for NAV, "12" -> "12__" for XPDR.
static void format_entry(Field f, const char *digits, char *out) {
    int n = max_digits(f), len = strlen(digits), o = 0;
    for (int i = 0; i < n; i++) {
        if (i == 3 && f != F_XPDR) out[o++] = '.';
        out[o++] = i < len ? digits[i] : '_';
    }
    out[o] = 0;
}

// ---------------------------------------------------------------- keypad

static void refresh_pad() {
    if (!s_pad) return;
    char buf[16];
    if (s_field == F_NONE) {
        ui_set_text(s_pad_title, "SELECT A STANDBY FREQUENCY OR SQUAWK");
        ui_set_text(s_pad_entry, "");
    } else {
        snprintf(buf, sizeof(buf), "%s %s", FIELD_NAME[s_field], s_field == F_XPDR ? "CODE" : "STBY");
        ui_set_text(s_pad_title, buf);
        if (s_entry[0]) {
            format_entry(s_field, s_entry, buf);
            ui_set_text(s_pad_entry, buf);
            ui_set_color(s_pad_entry, COL_CYAN);
        } else {
            ui_set_text(s_pad_entry, g_live ? stby_text(s_field) : "");
            ui_set_color(s_pad_entry, COL_DIM);
        }
    }
    for (int k = 0; k < 12; k++) {
        bool digit = k < 9 || k == 10;
        int d = k < 9 ? k + 1 : 0;
        bool ok = s_field != F_NONE && (!digit || s_field != F_XPDR || d <= 7);
        ui_button_look(s_keys[k], ok ? (k == 11 ? BTN_SELECTED : BTN_NORMAL) : BTN_DISABLED);
    }
}

static void select_field(Field f) {
    s_field = f;
    s_entry[0] = 0;
    for (int i = 0; i < F_COUNT; i++)
        if (s_stby_btn[i]) ui_button_look(s_stby_btn[i], i == f ? BTN_SELECTED : BTN_NORMAL);
    if (s_narrow && s_pad) {
        if (f == F_NONE) lv_obj_add_flag(s_pad, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(s_pad, LV_OBJ_FLAG_HIDDEN);
    }
    refresh_pad();
}

static bool enter() {
    char msg[40];
    int len = strlen(s_entry);
    if (s_field == F_XPDR) {
        if (len != 4) return false;
        cmd_xpdr(s_entry);
        snprintf(msg, sizeof(msg), "SQUAWK %s", s_entry);
    } else {
        int n = max_digits(s_field);
        if (len < 3) return false;
        char d[8];
        strcpy(d, s_entry);
        while ((int)strlen(d) < n) strcat(d, "0");
        int mhz = (d[0] - '0') * 100 + (d[1] - '0') * 10 + (d[2] - '0');
        int sub = atoi(d + 3);              // kHz (COM) or 10 kHz (NAV)
        float f = mhz + sub / (is_com(s_field) ? 1000.0f : 100.0f);
        if (is_com(s_field)) {
            if (f < 118.0f || f > 136.991f || sub % 5) return false;
            snprintf(msg, sizeof(msg), "%s STBY %d.%03d", FIELD_NAME[s_field], mhz, sub);
        } else {
            if (f < 108.0f || f > 117.951f || sub % 5) return false;
            snprintf(msg, sizeof(msg), "%s STBY %d.%02d", FIELD_NAME[s_field], mhz, sub);
        }
        cmd_freq(FIELD_ID[s_field], f);
    }
    ui_toast(msg, COL_CYAN);
    return true;
}

static void key_cb(lv_event_t *e) {
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_field == F_NONE) return;
    int len = strlen(s_entry);
    if (k < 9 || k == 10) {
        char d = k < 9 ? '1' + k : '0';
        if (s_field == F_XPDR && d > '7') return;
        if (len == 0 && s_field != F_XPDR && d != '1') {    // frequencies all start with 1
            s_entry[len++] = '1';
            s_entry[len] = 0;
        }
        if (len < max_digits(s_field)) {
            s_entry[len++] = d;
            s_entry[len] = 0;
        }
        // A full squawk goes out straight away.
        if (s_field == F_XPDR && len == 4 && enter()) s_entry[0] = 0;
    } else if (k == 9) {                    // backspace; long press clears
        if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) s_entry[0] = 0;
        else if (len) s_entry[len - 1] = 0;
    } else if (k == 11) {                   // ENT
        if (enter()) {
            s_entry[0] = 0;
            if (s_narrow) select_field(F_NONE);
        } else {
            ui_toast(s_field == F_XPDR ? "ENTER 4 DIGITS 0-7" : "INVALID FREQUENCY", COL_YELLOW);
        }
    }
    refresh_pad();
}

static void build_keypad(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, bool popup) {
    s_pad = ui_box(parent, x, y, w, h);
    lv_obj_set_style_bg_color(s_pad, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(s_pad, LV_OPA_COVER, 0);
    if (popup) {
        lv_obj_set_style_border_width(s_pad, 2, 0);
        lv_obj_set_style_border_color(s_pad, COL_CYAN, 0);
        lv_obj_add_flag(s_pad, LV_OBJ_FLAG_CLICKABLE);      // swallow taps on the gaps
    }
    s_pad_title = ui_label(s_pad, FONT_14, COL_CYAN, "");
    lv_obj_set_pos(s_pad_title, 10, 6);
    lv_obj_t *box = ui_box(s_pad, 8, 26, w - 16 - (popup ? 104 : 0), 56);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, COL_BORDER, 0);
    lv_obj_set_style_radius(box, 4, 0);
    s_pad_entry = ui_label(box, FONT_40, COL_CYAN, "");
    lv_obj_center(s_pad_entry);
    if (popup) {
        lv_obj_t *cancel = ui_button(s_pad, "CANCEL", FONT_16, 92, 56, [](lv_event_t *) { select_field(F_NONE); }, nullptr);
        lv_obj_set_pos(cancel, w - 100, 26);
    }
    static const char *labels[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", LV_SYMBOL_BACKSPACE, "0", "ENT"};
    lv_coord_t gap = 6, top = 90;
    lv_coord_t kw = (w - 8 - gap * 2 - 8) / 3, kh = (h - top - 8 - gap * 3) / 4;
    for (int k = 0; k < 12; k++) {
        s_keys[k] = ui_button(s_pad, labels[k], k == 11 ? FONT_24 : FONT_32, kw, kh, key_cb, (void *)(intptr_t)k);
        lv_obj_set_pos(s_keys[k], 8 + (k % 3) * (kw + gap), top + (k / 3) * (kh + gap));
        if (k == 9) lv_obj_add_event_cb(s_keys[k], key_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)k);
    }
}

// ---------------------------------------------------------------- radio rows

static void stby_cb(lv_event_t *e) {
    Field f = (Field)(intptr_t)lv_event_get_user_data(e);
    select_field(s_field == f && !s_narrow ? F_NONE : f);
}

static void swap_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    cmd_swap(FIELD_ID[i]);
}

static void mode_cb(lv_event_t *e) { cmd_xpdr_mode(XPDR_MODES[(intptr_t)lv_event_get_user_data(e)]); }

// The NAV row the top bar shows: its name in cyan with a "TOP BAR" tag under it.
static void refresh_top_nav() {
    for (int k = 0; k < 2; k++) {
        if (!s_nav_name[k]) continue;
        bool on = ui_top_nav() == k + 1;
        ui_set_color(s_nav_name[k], on ? COL_CYAN : COL_DIM);
        if (on) lv_obj_clear_flag(s_nav_tag[k], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_nav_tag[k], LV_OBJ_FLAG_HIDDEN);
    }
}

static void top_nav_cb(lv_event_t *e) {
    int nav = (int)(intptr_t)lv_event_get_user_data(e);
    if (nav == ui_top_nav()) return;
    ui_set_top_nav(nav);
    refresh_top_nav();
    ui_toast(nav == 2 ? "TOP BAR SHOWS NAV2" : "TOP BAR SHOWS NAV1", COL_CYAN);
}

static void vfr_cb(lv_event_t *) {
    cmd_xpdr("1200");
    ui_toast("SQUAWK 1200 (VFR)", COL_CYAN);
}

static void radio_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h) {
    s_narrow = w < 600;
    s_root = parent;
    s_pad = nullptr;
    memset(s_stby_btn, 0, sizeof(s_stby_btn));
    lv_coord_t row_h = 70, gap = 4, y0 = 8;
    const lv_font_t *f_freq = s_narrow ? FONT_24 : FONT_32;
    lv_coord_t x_act = s_narrow ? 62 : 84, x_swap = s_narrow ? 178 : 226, w_swap = s_narrow ? 54 : 64;
    lv_coord_t x_stby = s_narrow ? 240 : 298, w_stby = s_narrow ? 170 : 164;
    for (int i = 0; i < F_COUNT; i++) {
        lv_coord_t y = y0 + i * (row_h + gap);
        lv_obj_t *row = ui_box(parent, 4, y, (s_narrow ? w : 474) - 8, row_h);
        lv_obj_set_style_bg_color(row, COL_PANEL, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_t *name;
        if (is_nav((Field)i)) {
            // The whole name area is a tap target that picks the top bar's NAV radio.
            int k = i - F_NAV1;
            lv_obj_t *tap = ui_box(row, 0, 0, x_act - 6, row_h);
            lv_obj_add_flag(tap, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_radius(tap, 6, 0);
            lv_obj_set_style_bg_color(tap, lv_color_hex(0x30404E), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(tap, LV_OPA_COVER, LV_STATE_PRESSED);
            lv_obj_add_event_cb(tap, top_nav_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(k + 1));
            name = ui_label(tap, s_narrow ? FONT_16 : FONT_20, COL_DIM, FIELD_NAME[i]);
            lv_obj_align(name, LV_ALIGN_LEFT_MID, 8, -7);
            s_nav_name[k] = name;
            s_nav_tag[k] = ui_label(tap, FONT_12, COL_CYAN, s_narrow ? "TOP" : "TOP BAR");
            lv_obj_align(s_nav_tag[k], LV_ALIGN_LEFT_MID, 8, 14);
        } else {
            name = ui_label(row, s_narrow ? FONT_16 : FONT_20, COL_DIM, FIELD_NAME[i]);
            lv_obj_align(name, LV_ALIGN_LEFT_MID, 8, 0);
        }
        if (i < 4) {
            s_act[i] = ui_label(row, f_freq, COL_TEXT, "---.---");
            lv_obj_align(s_act[i], LV_ALIGN_LEFT_MID, x_act - 4, 0);
            lv_obj_t *sw = ui_button(row, LV_SYMBOL_SHUFFLE, FONT_24, w_swap, 52, swap_cb, (void *)(intptr_t)i);
            lv_obj_align(sw, LV_ALIGN_LEFT_MID, x_swap - 4, 0);
            s_stby_btn[i] = ui_button(row, "---.---", f_freq, w_stby, 58, stby_cb, (void *)(intptr_t)i);
            lv_obj_align(s_stby_btn[i], LV_ALIGN_LEFT_MID, x_stby - 4, 0);
            ui_set_color(ui_button_label(s_stby_btn[i]), COL_CYAN);
        } else {
            s_stby_btn[i] = ui_button(row, "----", f_freq, s_narrow ? 100 : 112, 58, stby_cb, (void *)(intptr_t)i);
            lv_obj_align(s_stby_btn[i], LV_ALIGN_LEFT_MID, (s_narrow ? 60 : 72) - 4, 0);
            lv_coord_t mx = s_narrow ? 166 : 192, mw = s_narrow ? 56 : 62;
            for (int m = 0; m < 3; m++) {
                s_mode_btn[m] = ui_button(row, XPDR_MODES[m], FONT_16, mw, 52, mode_cb, (void *)(intptr_t)m);
                lv_obj_align(s_mode_btn[m], LV_ALIGN_LEFT_MID, mx - 4 + m * (mw + 4), 0);
            }
            lv_obj_t *vfr = ui_button(row, "VFR", FONT_16, s_narrow ? 60 : 66, 52, vfr_cb, nullptr);
            lv_obj_align(vfr, LV_ALIGN_LEFT_MID, mx - 4 + 3 * (mw + 4), 0);
        }
    }
    if (s_narrow) {
        build_keypad(parent, 0, 0, w, h, true);
        lv_obj_add_flag(s_pad, LV_OBJ_FLAG_HIDDEN);
    } else {
        build_keypad(parent, 480, 0, w - 480, h, false);
    }
    refresh_top_nav();
    s_seq = 0;
    s_was_live = !g_live;
    Field f = s_pending != F_NONE ? s_pending : F_NONE;
    s_pending = F_NONE;
    select_field(s_narrow ? f : (f == F_NONE ? F_COM1 : f));
}

static void radio_update() {
    if (!s_root) return;
    if (g_t.seq_mid == s_seq && g_live == s_was_live) return;
    s_seq = g_t.seq_mid;
    s_was_live = g_live;
    for (int i = 0; i < 4; i++) {
        ui_set_text(s_act[i], g_live ? act_text(i) : "---.---");
        ui_set_text(ui_button_label(s_stby_btn[i]), g_live ? stby_text((Field)i) : "---.---");
    }
    ui_set_text(ui_button_label(s_stby_btn[F_XPDR]), g_live ? g_t.rad.xpdr : "----");
    for (int m = 0; m < 3; m++) {
        bool on = g_live && (!strcmp(g_t.rad.xpdr_mode, XPDR_MODES[m]) || (m == 2 && !strcmp(g_t.rad.xpdr_mode, "ON")));
        ui_button_look(s_mode_btn[m], on ? BTN_ACTIVE : BTN_NORMAL);
    }
    if (!s_entry[0]) refresh_pad();
}

static void radio_destroy() {
    s_root = nullptr;
    s_pad = nullptr;
    s_field = F_NONE;
    memset(s_stby_btn, 0, sizeof(s_stby_btn));
    memset(s_nav_name, 0, sizeof(s_nav_name));
    memset(s_nav_tag, 0, sizeof(s_nav_tag));
}

void radio_preselect(const char *field) {
    for (int i = 0; i < F_COUNT; i++)
        if (!strcmp(field, FIELD_ID[i])) {
            if (s_root) select_field((Field)i);
            else s_pending = (Field)i;
        }
}

const PageDef PAGE_RADIO = {"RADIO", radio_create, radio_update, radio_destroy};
