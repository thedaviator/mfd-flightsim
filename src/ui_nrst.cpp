// NEAREST AIRPORTS
// Scrollable list from the bridge: identifier and name, bearing (with a pointer relative
// to the nose), distance, longest runway and tower / CTAF frequency, and the airport's
// current METAR with its flight category when it has one (the bridge fetches them from
// aviationweather.gov). Tapping an airport puts its frequency in COM1 standby.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_priv.h"

struct Row {
    lv_obj_t *obj, *icao, *name, *brg, *arrow, *dist, *rwy, *freq, *ftype, *cat, *metar;
};

// Flight category badge colours: VFR green, MVFR blue, IFR red, LIFR magenta.
static bool cat_color(const char *cat, lv_color_t &bg, lv_color_t &fg) {
    fg = COL_TEXT;
    if (!strcmp(cat, "VFR")) { bg = COL_GREEN; fg = COL_BG; }
    else if (!strcmp(cat, "MVFR")) bg = lv_color_hex(0x2878FF);
    else if (!strcmp(cat, "IFR")) bg = COL_RED;
    else if (!strcmp(cat, "LIFR")) bg = COL_MAGENTA;
    else return false;
    return true;
}

static lv_obj_t *s_list, *s_empty;
static Row s_rows[MAX_NRST];
static bool s_narrow;
static uint32_t s_seq, s_arrow_ms;
static bool s_was_live;
static NrstApt s_shown[MAX_NRST];       // what the rows show (for tap handling)
static int s_n;

static void arrow_draw(lv_event_t *e) {
    lv_draw_ctx_t *dc = lv_event_get_draw_ctx(e);
    lv_obj_t *o = lv_event_get_target(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(o);
    if (i >= s_n) return;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    float cx = (a.x1 + a.x2) / 2.0f, cy = (a.y1 + a.y2) / 2.0f;
    float rel = (s_shown[i].brg - g_t.pfd.hdg) * DEG2RAD;
    float s = sinf(rel), c = cosf(rel);
    dr_line(dc, cx - s * 12 * XS, cy + c * 12, cx + s * 6 * XS, cy - c * 6, 3, COL_CYAN);
    lv_point_t head[3] = {{(lv_coord_t)(cx + s * 14 * XS), (lv_coord_t)(cy - c * 14)},
                          {(lv_coord_t)(cx + (s * 3 - c * 7) * XS), (lv_coord_t)(cy - c * 3 - s * 7)},
                          {(lv_coord_t)(cx + (s * 3 + c * 7) * XS), (lv_coord_t)(cy - c * 3 + s * 7)}};
    dr_poly(dc, head, 3, COL_CYAN);
}

static void row_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= s_n) return;
    const NrstApt &a = s_shown[i];
    char msg[48];
    if (!a.freq[0]) {
        snprintf(msg, sizeof(msg), "%s: NO FREQUENCY", a.icao);
        ui_toast(msg, COL_YELLOW);
        return;
    }
    cmd_freq("com1", (float)atof(a.freq));
    snprintf(msg, sizeof(msg), "COM1 STBY %s  %s %s", a.freq, a.icao, a.ftype);
    ui_toast(msg, COL_CYAN);
}

static lv_obj_t *col_label(lv_obj_t *row, const lv_font_t *f, lv_color_t c, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                           lv_text_align_t align) {
    lv_obj_t *l = ui_label(row, f, c, "");
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, align, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    return l;
}

static void header(lv_obj_t *parent, const char *t, lv_coord_t x, lv_coord_t w, lv_text_align_t align) {
    lv_obj_t *l = ui_label(parent, FONT_14, COL_DIM, t);
    lv_obj_set_pos(l, x, 6);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, align, 0);
}

static void nrst_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h) {
    s_narrow = w < 600;
    // Column x positions: id/name, arrow, bearing, distance, runway, frequency
    lv_coord_t x_brg = s_narrow ? 150 : 330, x_dist = s_narrow ? 236 : 430, x_rwy = 540, x_freq = s_narrow ? 300 : 640;
    lv_coord_t row_w = w - 12, row_h = 78;
    header(parent, "AIRPORT", 14, 140, LV_TEXT_ALIGN_LEFT);
    header(parent, "BRG", x_brg + 38, 60, LV_TEXT_ALIGN_LEFT);
    header(parent, "DIS", x_dist, 64, LV_TEXT_ALIGN_RIGHT);
    if (!s_narrow) header(parent, "RWY", x_rwy, 80, LV_TEXT_ALIGN_RIGHT);
    header(parent, "FREQ", x_freq, row_w - x_freq - 10, LV_TEXT_ALIGN_RIGHT);

    s_list = ui_box(parent, 0, 26, w, h - 26);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_bg_color(s_list, COL_CYAN, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_list, 6, LV_PART_SCROLLBAR);

    for (int i = 0; i < MAX_NRST; i++) {
        Row &r = s_rows[i];
        r.obj = ui_box(s_list, 6, i * (row_h + 4), row_w, row_h);
        lv_obj_set_style_bg_color(r.obj, i % 2 ? COL_PANEL : COL_PANEL2, 0);
        lv_obj_set_style_bg_opa(r.obj, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(r.obj, lv_color_hex(0x30404E), LV_STATE_PRESSED);
        lv_obj_set_style_radius(r.obj, 5, 0);
        lv_obj_add_flag(r.obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(r.obj, LV_OBJ_FLAG_SCROLL_CHAIN);
        lv_obj_clear_flag(r.obj, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        lv_obj_add_event_cb(r.obj, row_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        r.icao = col_label(r.obj, FONT_24, COL_CYAN, 8, 3, 130, LV_TEXT_ALIGN_LEFT);
        r.name = col_label(r.obj, FONT_14, COL_DIM, 8, 34, s_narrow ? 136 : 300, LV_TEXT_ALIGN_LEFT);
        lv_label_set_long_mode(r.name, LV_LABEL_LONG_DOT);
        lv_obj_set_height(r.name, lv_font_get_line_height(FONT_14));
        r.arrow = ui_box(r.obj, x_brg, 13, 32, 32);
        lv_obj_set_user_data(r.arrow, (void *)(intptr_t)i);
        lv_obj_add_event_cb(r.arrow, arrow_draw, LV_EVENT_DRAW_MAIN, nullptr);
        r.brg = col_label(r.obj, FONT_20, COL_TEXT, x_brg + 36, 17, 60, LV_TEXT_ALIGN_LEFT);
        r.dist = col_label(r.obj, FONT_20, COL_TEXT, x_dist - 10, 17, 76, LV_TEXT_ALIGN_RIGHT);
        r.rwy = s_narrow ? nullptr : col_label(r.obj, FONT_20, COL_TEXT, x_rwy - 20, 17, 100, LV_TEXT_ALIGN_RIGHT);
        r.freq = col_label(r.obj, s_narrow ? FONT_20 : FONT_24, COL_GREEN, x_freq, 6, row_w - x_freq - 10,
                           LV_TEXT_ALIGN_RIGHT);
        r.ftype = col_label(r.obj, FONT_12, COL_DIM, x_freq, 38, row_w - x_freq - 10, LV_TEXT_ALIGN_RIGHT);
        r.cat = ui_label(r.obj, FONT_12, COL_BG, "");
        lv_obj_set_pos(r.cat, 80, 8);
        lv_obj_set_size(r.cat, 46, 20);
        lv_obj_set_style_text_align(r.cat, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(r.cat, 3, 0);
        lv_obj_set_style_radius(r.cat, 4, 0);
        lv_obj_set_style_bg_opa(r.cat, LV_OPA_COVER, 0);
        r.metar = col_label(r.obj, FONT_14, lv_color_hex(0xC8D2DA), 8, 56, row_w - 16, LV_TEXT_ALIGN_LEFT);
        lv_label_set_long_mode(r.metar, LV_LABEL_LONG_DOT);
        lv_obj_set_height(r.metar, lv_font_get_line_height(FONT_14));
        lv_obj_add_flag(r.obj, LV_OBJ_FLAG_HIDDEN);
    }
    s_empty = ui_label(parent, FONT_20, COL_YELLOW, "");
    lv_obj_align(s_empty, LV_ALIGN_CENTER, 0, 0);
    s_seq = s_arrow_ms = 0;
    s_n = -1;
    s_was_live = !g_live;
}

static void nrst_update() {
    if (!s_list) return;
    if (g_t.seq_slow != s_seq || g_live != s_was_live) {
        s_seq = g_t.seq_slow;
        s_was_live = g_live;
        s_n = g_live ? g_t.n_nrst : 0;
        memcpy(s_shown, g_t.nrst, sizeof(NrstApt) * s_n);
        ui_set_text(s_empty, !g_live ? "NO POSITION DATA" : s_n ? "" : "NO AIRPORTS IN RANGE");
        char t[32];
        for (int i = 0; i < MAX_NRST; i++) {
            Row &r = s_rows[i];
            if (i >= s_n) {
                lv_obj_add_flag(r.obj, LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            const NrstApt &a = s_shown[i];
            lv_obj_clear_flag(r.obj, LV_OBJ_FLAG_HIDDEN);
            ui_set_text(r.icao, a.icao);
            ui_set_text(r.name, a.name);
            snprintf(t, sizeof(t), "%03d\xC2\xB0", a.brg);
            ui_set_text(r.brg, t);
            snprintf(t, sizeof(t), a.dist < 10 ? "%.1fNM" : "%.0fNM", a.dist);
            ui_set_text(r.dist, t);
            if (r.rwy) {
                snprintf(t, sizeof(t), "%dFT", (int)a.rwy);
                ui_set_text(r.rwy, t);
            }
            ui_set_text(r.freq, a.freq[0] ? a.freq : "---.---");
            ui_set_color(r.freq, a.freq[0] ? COL_GREEN : COL_DIM);
            ui_set_text(r.ftype, a.ftype);
            lv_color_t bg, fg;
            if (cat_color(a.cat, bg, fg)) {
                ui_set_text(r.cat, a.cat);
                ui_set_color(r.cat, fg);
                lv_obj_set_style_bg_color(r.cat, bg, 0);
                lv_obj_set_x(r.cat, 8 + text_width(a.icao, FONT_24) + 8);
                lv_obj_clear_flag(r.cat, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(r.cat, LV_OBJ_FLAG_HIDDEN);
            }
            ui_set_text(r.metar, a.metar[0] ? a.metar : "NO METAR");
            ui_set_color(r.metar, a.metar[0] ? lv_color_hex(0xC8D2DA) : COL_DIM);
            lv_obj_invalidate(r.arrow);
        }
    } else if (g_now - s_arrow_ms >= 250) {
        // Bearing pointers follow the heading between list updates.
        s_arrow_ms = g_now;
        for (int i = 0; i < s_n; i++) lv_obj_invalidate(s_rows[i].arrow);
    }
}

static void nrst_destroy() { s_list = nullptr; }

const PageDef PAGE_NRST = {"NRST", nrst_create, nrst_update, nrst_destroy};
