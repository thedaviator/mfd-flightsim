// ENGINE INDICATING SYSTEM
// Arc gauges (RPM, manifold pressure, fuel pressure, oil temperature and pressure),
// CHT and EGT bar graphs per cylinder, linear gauges for fuel, volts and amps.
// Ranges and limits come from config.h.
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "ui_priv.h"

struct Spec {
    const char *title, *unit, *fmt;
    float min, max, g_lo, g_hi, y_hi, red;
};
static const Spec SP_RPM = {"RPM", "", "%.0f", G_RPM};
static const Spec SP_MAP = {"MAN", "IN HG", "%.1f", G_MAP};
static const Spec SP_FUELP = {"FUEL", "PSI", "%.1f", G_FUELP};
static const Spec SP_OILT = {"OIL", "\xC2\xB0" "F", "%.0f", G_OILT};
static const Spec SP_OILP = {"OIL", "PSI", "%.0f", G_OILP};
static const Spec SP_CHT = {"CHT", "\xC2\xB0" "F", "%.0f", G_CHT};
static const Spec SP_EGT = {"EGT", "\xC2\xB0" "F", "%.0f", G_EGT};
static const Spec SP_VOLTS = {"VOLTS", "", "%.1f", G_VOLTS};
static const Spec SP_AMPS = {"AMPS", "", "%+.0f", G_AMPS};

static lv_obj_t *s_obj;
static lv_coord_t s_w, s_h;
static uint32_t s_seq;
static bool s_was_live;

static lv_color_t value_color(const Spec &s, float v) {
    if (s.red > 0 && v >= s.red) return COL_RED;
    if (s.y_hi > 0 && v > s.g_hi) return COL_YELLOW;
    return COL_TEXT;
}

static float frac(const Spec &s, float v) { return LV_CLAMP(0.0f, (v - s.min) / (s.max - s.min), 1.0f); }

// ---------------------------------------------------------------- arc gauge (240 deg sweep)

static void arc_gauge(lv_draw_ctx_t *dc, lv_coord_t cx, lv_coord_t cy, lv_coord_t r, const Spec &s, float v,
                      bool big) {
    const float A0 = 150, SWEEP = 240;
    auto ang = [&](float x) { return A0 + frac(s, x) * SWEEP; };
    lv_coord_t bw = big ? 9 : 7;
    dr_arc(dc, cx, cy, r, 3, A0, A0 + SWEEP, lv_color_hex(0x5A6670));
    if (s.g_hi > s.g_lo) dr_arc(dc, cx, cy, r, bw, ang(s.g_lo), ang(s.g_hi), COL_GREEN);
    if (s.y_hi > s.g_hi) dr_arc(dc, cx, cy, r, bw, ang(s.g_hi), ang(s.y_hi), COL_YELLOW);
    if (s.red > 0) {
        float a = ang(s.red) * DEG2RAD;
        dr_line(dc, cx + cosf(a) * (r - bw - 5) * XS, cy + sinf(a) * (r - bw - 5), cx + cosf(a) * (r + 3) * XS, cy + sinf(a) * (r + 3),
                4, COL_RED);
    }
    // Pointer: white wedge from the hub towards the arc.
    float a = ang(v) * DEG2RAD, c = cosf(a), sn = sinf(a);
    float tip = r - 2, base = r * (big ? 0.7f : 0.5f), hw = big ? 6 : 5;
    lv_point_t p[3] = {{(lv_coord_t)(cx + c * tip * XS), (lv_coord_t)(cy + sn * tip)},
                       {(lv_coord_t)(cx + (c * base - sn * hw) * XS), (lv_coord_t)(cy + sn * base + c * hw)},
                       {(lv_coord_t)(cx + (c * base + sn * hw) * XS), (lv_coord_t)(cy + sn * base - c * hw)}};
    dr_poly(dc, p, 3, COL_TEXT);
    char t[16];
    snprintf(t, sizeof(t), s.fmt, v);
    dr_text(dc, cx, cy + (big ? 6 : 4), t, big ? FONT_32 : FONT_20, value_color(s, v), TA_CENTER);
    char lbl[24];
    snprintf(lbl, sizeof(lbl), s.unit[0] ? "%s %s" : "%s", s.title, s.unit);
    dr_text(dc, cx, cy + r * 0.62f + (big ? 6 : 4), lbl, big ? FONT_16 : FONT_14, COL_DIM, TA_CENTER);
}

// ---------------------------------------------------------------- bar graph per cylinder

static void cyl_bars(lv_draw_ctx_t *dc, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, const Spec &s,
                     const float *vals, int n, bool compact) {
    if (n <= 0) return;
    int hot = 0;
    for (int i = 1; i < n; i++)
        if (vals[i] > vals[hot]) hot = i;
    char t[24];
    snprintf(t, sizeof(t), "%s %s", s.title, s.unit);
    dr_text(dc, x + w / 2, y + 8, t, compact ? FONT_14 : FONT_16, COL_DIM, TA_CENTER);
    snprintf(t, sizeof(t), s.fmt, vals[hot]);
    dr_text(dc, x + w / 2, y + (compact ? 26 : 32), t, compact ? FONT_18 : FONT_24, value_color(s, vals[hot]), TA_CENTER);

    lv_coord_t top = y + (compact ? 40 : 52), bot = y + h - 18;
    lv_coord_t gap = 4, bw = (w - gap * (n + 1)) / n;
    for (int i = 0; i < n; i++) {
        lv_coord_t bx = x + gap + i * (bw + gap);
        dr_rect(dc, bx, top, bx + bw, bot, lv_color_hex(0x222A32));
        lv_coord_t fy = (lv_coord_t)(bot - frac(s, vals[i]) * (bot - top));
        lv_color_t c = value_color(s, vals[i]);
        if (s.g_hi <= s.g_lo) c = i == hot ? COL_CYAN : lv_color_hex(0xC8D2DA);    // EGT: no ranges
        else if (c.full == COL_TEXT.full) c = COL_GREEN;
        dr_rect(dc, bx, fy, bx + bw, bot, c);
        snprintf(t, sizeof(t), "%d", i + 1);
        dr_text(dc, bx + bw / 2, bot + 9, t, FONT_12, i == hot ? COL_CYAN : COL_DIM, TA_CENTER);
    }
    // Limit lines across the bars
    auto ly = [&](float v) { return (lv_coord_t)(bot - frac(s, v) * (bot - top)); };
    if (s.y_hi > s.g_hi) dr_line(dc, x + 2, ly(s.y_hi), x + w - 2, ly(s.y_hi), 2, COL_YELLOW);
    if (s.red > 0 && s.red < s.max) dr_line(dc, x + 2, ly(s.red), x + w - 2, ly(s.red), 2, COL_RED);
}

// ---------------------------------------------------------------- linear gauge

static void hbar(lv_draw_ctx_t *dc, lv_coord_t x, lv_coord_t y, lv_coord_t w, const Spec &s, float v,
                 const char *label, bool compact) {
    dr_text(dc, x, y + 9, label, compact ? FONT_14 : FONT_16, COL_DIM);
    char t[16];
    snprintf(t, sizeof(t), s.fmt, v);
    dr_text(dc, x + w, y + 9, t, compact ? FONT_16 : FONT_20, value_color(s, v), TA_RIGHT);
    lv_coord_t by = y + (compact ? 22 : 26);
    dr_rect(dc, x, by, x + w, by + 8, lv_color_hex(0x2A323A), LV_OPA_COVER, 2);
    auto px = [&](float val) { return (lv_coord_t)(x + frac(s, val) * w); };
    if (s.g_hi > s.g_lo) dr_rect(dc, px(s.g_lo), by, px(s.g_hi), by + 8, COL_GREEN);
    if (s.y_hi > s.g_hi) dr_rect(dc, px(s.g_hi), by, px(s.y_hi), by + 8, COL_YELLOW);
    if (s.red > 0 && s.red <= s.max) dr_rect(dc, px(s.red) - 2, by - 3, px(s.red) + 1, by + 11, COL_RED);
    lv_coord_t vx = px(v);
    lv_point_t tri[3] = {{(lv_coord_t)(vx - 7), (lv_coord_t)(by - 9)}, {(lv_coord_t)(vx + 7), (lv_coord_t)(by - 9)},
                         {vx, (lv_coord_t)(by + 3)}};
    dr_poly(dc, tri, 3, COL_TEXT);
}

static void fuel_gauges(lv_draw_ctx_t *dc, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t step) {
    const Eis &e = g_t.eis;
    float cap = e.fuel_cap > 1 ? e.fuel_cap : FUEL_TANK_GAL;
    Spec fs = {"FUEL", "GAL", "%.1f", 0, cap, cap * 0.25f, cap, 0, 0};
    bool compact = step < 50;
    hbar(dc, x, y, w, fs, e.fuel_l, "FUEL L", compact);
    hbar(dc, x, y + step, w, fs, e.fuel_r, "FUEL R", compact);
    hbar(dc, x, y + step * 2, w, SP_VOLTS, e.volts, "VOLTS", compact);
    hbar(dc, x, y + step * 3, w, SP_AMPS, e.amps, "AMPS", compact);
}

static void ff_text(lv_draw_ctx_t *dc, lv_coord_t x, lv_coord_t y, TextAlign align, bool two_lines) {
    const Eis &e = g_t.eis;
    char t[40];
    float total = e.fuel_l + e.fuel_r;
    int end_min = e.ff > 0.5f ? (int)(total / e.ff * 60) : -1;
    if (two_lines) {
        snprintf(t, sizeof(t), "FF %.1f GPH", e.ff);
        dr_text(dc, x, y, t, FONT_20, COL_TEXT, align);
        if (end_min >= 0) snprintf(t, sizeof(t), "END %d:%02d   %.0f GAL", end_min / 60, end_min % 60, total);
        else snprintf(t, sizeof(t), "%.0f GAL", total);
        dr_text(dc, x, y + 24, t, FONT_16, COL_DIM, align);
    } else {
        if (end_min >= 0) snprintf(t, sizeof(t), "FF %.1f GPH   END %d:%02d", e.ff, end_min / 60, end_min % 60);
        else snprintf(t, sizeof(t), "FF %.1f GPH", e.ff);
        dr_text(dc, x, y, t, FONT_16, COL_TEXT, align);
    }
}

// ---------------------------------------------------------------- page

static void draw_cb(lv_event_t *ev) {
    lv_draw_ctx_t *dc = lv_event_get_draw_ctx(ev);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target(ev), &a);
    lv_coord_t x = a.x1, y = a.y1;
    if (!g_live) {
        dr_text(dc, x + s_w / 2, y + s_h / 2, "NO ENGINE DATA", FONT_24, COL_YELLOW, TA_CENTER);
        return;
    }
    const Eis &e = g_t.eis;
    if (s_w >= 600) {
        arc_gauge(dc, x + 100, y + 108, 84, SP_RPM, e.rpm, true);
        arc_gauge(dc, x + 290, y + 108, 84, SP_MAP, e.mrp, true);
        arc_gauge(dc, x + 66, y + 270, 52, SP_FUELP, e.fuel_p, false);
        arc_gauge(dc, x + 195, y + 270, 52, SP_OILT, e.oil_t, false);
        arc_gauge(dc, x + 324, y + 270, 52, SP_OILP, e.oil_p, false);
        ff_text(dc, x + 195, y + 356, TA_CENTER, false);
        dr_line(dc, x + 398, y + 10, x + 398, y + s_h - 10, 1, COL_BORDER);
        cyl_bars(dc, x + 408, y + 6, 90, s_h - 16, SP_EGT, e.egt, e.ncyl, false);
        cyl_bars(dc, x + 503, y + 6, 90, s_h - 16, SP_CHT, e.cht, e.ncyl, false);
        dr_line(dc, x + 600, y + 10, x + 600, y + s_h - 10, 1, COL_BORDER);
        fuel_gauges(dc, x + 614, y + 14, 172, 62);
        ff_text(dc, x + 700, y + 286, TA_CENTER, true);
    } else {
        lv_coord_t q = s_w / 4;
        arc_gauge(dc, x + q, y + 82, 68, SP_RPM, e.rpm, true);
        arc_gauge(dc, x + 3 * q, y + 82, 68, SP_MAP, e.mrp, true);
        lv_coord_t s6 = s_w / 6;
        arc_gauge(dc, x + s6, y + 200, 40, SP_FUELP, e.fuel_p, false);
        arc_gauge(dc, x + 3 * s6, y + 200, 40, SP_OILT, e.oil_t, false);
        arc_gauge(dc, x + 5 * s6, y + 200, 40, SP_OILP, e.oil_p, false);
        dr_line(dc, x + 8, y + 248, x + s_w - 8, y + 248, 1, COL_BORDER);
        cyl_bars(dc, x + 6, y + 252, 96, 130, SP_EGT, e.egt, e.ncyl, true);
        cyl_bars(dc, x + 106, y + 252, 96, 130, SP_CHT, e.cht, e.ncyl, true);
        fuel_gauges(dc, x + 220, y + 252, s_w - 232, 33);
    }
}

static void eis_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h) {
    s_w = w;
    s_h = h;
    s_obj = ui_canvas(parent, 0, 0, w, h, draw_cb);
    s_seq = 0;
    s_was_live = !g_live;
}

static void eis_update() {
    if (!s_obj) return;
    if (g_t.seq_mid != s_seq || g_live != s_was_live) {
        s_seq = g_t.seq_mid;
        s_was_live = g_live;
        lv_obj_invalidate(s_obj);
    }
}

static void eis_destroy() { s_obj = nullptr; }

const PageDef PAGE_EIS = {"EIS", eis_create, eis_update, eis_destroy};
