// PRIMARY FLIGHT DISPLAY
// Attitude (sky/ground, pitch ladder, roll scale, slip), airspeed and altitude tapes with
// rolling digits, vertical speed, heading tape with bug / track / turn trend, wind, CDI,
// parking brake annunciation.
// One custom-drawn object, redrawn whenever a new attitude packet arrives (30 Hz).
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "ui_priv.h"

struct Layout {
    lv_coord_t W, H;
    bool narrow;
    lv_coord_t asi_x, asi_w, alt_x, alt_w, vsi_x, vsi_w;
    lv_coord_t tape_y1, tape_y2, tape_cy;
    lv_coord_t cx, cy;                      // aircraft symbol
    lv_coord_t hdg_x1, hdg_x2, hdg_h;
    lv_coord_t roll_r;
    lv_coord_t ladder_half_w;
    float ppd;                              // pixels per degree of pitch
    const lv_font_t *f_tape, *f_box, *f_alt_big, *f_alt_drum, *f_hdg, *f_small;
};

static lv_obj_t *s_obj;
static Layout L;
static uint32_t s_seq;
static bool s_was_live;

// ---------------------------------------------------------------- attitude

static void fill_horizon(lv_draw_ctx_t *dc, const lv_area_t *obj, float pitch, float roll) {
    lv_area_t a;
    if (!_lv_area_intersect(&a, obj, dc->clip_area)) return;
    lv_color_t *buf = (lv_color_t *)dc->buf;
    const lv_area_t *ba = dc->buf_area;
    lv_coord_t bw = lv_area_get_width(ba);
    float sr = sinf(roll * DEG2RAD), cr = cosf(roll * DEG2RAD);
    float off = pitch * L.ppd;
    float cx = obj->x1 + L.cx, cy = obj->y1 + L.cy;
    lv_color_t sky = COL_SKY, gnd = COL_GROUND;
    lv_coord_t w = a.x2 - a.x1 + 1;
    for (lv_coord_t y = a.y1; y <= a.y2; y++) {
        lv_color_t *row = buf + (int32_t)(y - ba->y1) * bw + (a.x1 - ba->x1);
        // Signed distance below the horizon: s(dx) = dx*sin(roll) + dy*cos(roll) - pitch*ppd
        float base = (y - cy) * cr - off;
        if (fabsf(sr) < 1e-4f) {
            lv_color_fill(row, base > 0 ? gnd : sky, w);
            continue;
        }
        // Geometry is in physical units; horizontal screen distance = physical * XS.
        float xb = cx - base / sr * XS;     // where the horizon crosses this row
        lv_coord_t split;
        if (xb <= a.x1) split = 0;
        else if (xb > a.x2) split = w;
        else split = (lv_coord_t)ceilf(xb) - a.x1;
        lv_color_t left = sr > 0 ? sky : gnd, right = sr > 0 ? gnd : sky;
        if (split > 0) lv_color_fill(row, left, split);
        if (split < w) lv_color_fill(row + split, right, w - split);
    }
}

static void draw_ladder(lv_draw_ctx_t *dc, lv_coord_t ox, lv_coord_t oy, float pitch, float roll) {
    const lv_area_t *saved;
    lv_area_t clip;
    lv_coord_t cx = ox + L.cx, cy = oy + L.cy;
    if (!clip_push(dc, cx - L.ladder_half_w, oy + L.hdg_h + 20, cx + L.ladder_half_w, oy + L.H - 54, &saved, &clip))
        return;
    float sr = sinf(roll * DEG2RAD), cr = cosf(roll * DEG2RAD);
    float ux = cr * XS, uy = -sr;           // along the horizon (screen pixels)
    lv_coord_t long_len = L.narrow ? 90 : 130;
    int k0 = (int)floorf((pitch - 22) / 2.5f), k1 = (int)ceilf((pitch + 22) / 2.5f);
    for (int k = k0; k <= k1; k++) {
        if (k == 0 || k < -36 || k > 36) continue;
        float a = k * 2.5f;
        float d = (pitch - a) * L.ppd;
        float mx = cx + sr * d * XS, my = cy + cr * d;
        float len = (k % 4 == 0) ? long_len : (k % 2 == 0) ? long_len * 0.5f : long_len * 0.25f;
        dr_line(dc, mx - ux * len / 2, my - uy * len / 2, mx + ux * len / 2, my + uy * len / 2, 2, COL_TEXT);
        if (k % 4 == 0) {
            char t[4];
            snprintf(t, sizeof(t), "%d", abs((int)a));
            float lo = len / 2 + 16;
            dr_text(dc, (lv_coord_t)(mx - ux * lo), (lv_coord_t)(my - uy * lo), t, L.f_small, COL_TEXT, TA_CENTER);
            dr_text(dc, (lv_coord_t)(mx + ux * lo), (lv_coord_t)(my + uy * lo), t, L.f_small, COL_TEXT, TA_CENTER);
        }
    }
    clip_pop(dc, saved);
}

static void draw_roll_scale(lv_draw_ctx_t *dc, lv_coord_t ox, lv_coord_t oy, float roll, float slip) {
    lv_coord_t cx = ox + L.cx, cy = oy + L.cy, R = L.roll_r;
    // Scale (rotates with the horizon); arc angles: 0 = 3 o'clock, clockwise.
    dr_arc(dc, cx, cy, R + 1, 2, -60 - roll - 90, 60 - roll - 90, COL_TEXT);
    static const int ticks[] = {-60, -45, -30, -20, -10, 10, 20, 30, 45, 60};
    for (int t : ticks) {
        float al = (t - roll) * DEG2RAD;
        float len = (abs(t) == 30 || abs(t) == 60) ? 16 : 9;
        float s = sinf(al), c = cosf(al);
        dr_line(dc, cx + s * R * XS, cy - c * R, cx + s * (R + len) * XS, cy - c * (R + len), 2, COL_TEXT);
    }
    // Zero index: white triangle on the scale, pointing at the centre.
    {
        float al = -roll * DEG2RAD, s = sinf(al), c = cosf(al);
        float px = cx + s * (R + 2) * XS, py = cy - c * (R + 2);
        float tx = c * XS, ty = s;          // tangent
        lv_point_t p[3] = {{(lv_coord_t)px, (lv_coord_t)py},
                           {(lv_coord_t)(px - tx * 9 + s * 15 * XS), (lv_coord_t)(py - ty * 9 - c * 15)},
                           {(lv_coord_t)(px + tx * 9 + s * 15 * XS), (lv_coord_t)(py + ty * 9 - c * 15)}};
        dr_poly(dc, p, 3, COL_TEXT);
    }
    // Fixed roll pointer and the slip/skid trapezoid under it.
    lv_coord_t ty = cy - R + 2;
    lv_point_t ptr[3] = {{cx, ty}, {(lv_coord_t)(cx - 10), (lv_coord_t)(ty + 15)}, {(lv_coord_t)(cx + 10), (lv_coord_t)(ty + 15)}};
    dr_poly(dc, ptr, 3, COL_YELLOW);
    lv_coord_t sx = (lv_coord_t)(cx + slip * 28);
    lv_point_t trap[4] = {{(lv_coord_t)(sx - 10), (lv_coord_t)(ty + 18)}, {(lv_coord_t)(sx + 10), (lv_coord_t)(ty + 18)},
                          {(lv_coord_t)(sx + 13), (lv_coord_t)(ty + 24)}, {(lv_coord_t)(sx - 13), (lv_coord_t)(ty + 24)}};
    dr_poly(dc, trap, 4, COL_YELLOW);
}

static void draw_aircraft_symbol(lv_draw_ctx_t *dc, lv_coord_t ox, lv_coord_t oy) {
    lv_coord_t cx = ox + L.cx, cy = oy + L.cy;
    float s = L.narrow ? 0.75f : 1.0f;
    float sx = s * XS;
    for (int side = -1; side <= 1; side += 2) {
        lv_point_t p[4] = {{cx, cy},
                           {(lv_coord_t)(cx + side * 78 * sx), (lv_coord_t)(cy + 28 * s)},
                           {(lv_coord_t)(cx + side * 50 * sx), (lv_coord_t)(cy + 28 * s)},
                           {cx, (lv_coord_t)(cy + 13 * s)}};
        dr_poly(dc, p, 4, lv_color_hex(0x000000));
        lv_point_t q[4] = {{cx, (lv_coord_t)(cy + 2)},
                           {(lv_coord_t)(cx + side * 73 * sx), (lv_coord_t)(cy + 26 * s)},
                           {(lv_coord_t)(cx + side * 52 * sx), (lv_coord_t)(cy + 26 * s)},
                           {cx, (lv_coord_t)(cy + 14 * s)}};
        dr_poly(dc, q, 4, COL_YELLOW);
    }
}

// ---------------------------------------------------------------- rolling digits

// Shows ((n * step) mod `mod`) for n around value/step, rolling smoothly between steps.
static void drum(lv_draw_ctx_t *dc, lv_coord_t x_right, lv_coord_t cy, lv_coord_t y1, lv_coord_t y2, float value,
                 int step, int mod, const char *fmt, const lv_font_t *f, lv_color_t c) {
    const lv_area_t *saved;
    lv_area_t clip;
    if (!clip_push(dc, x_right - digit_w(f) * 3, y1, x_right + 2, y2, &saved, &clip)) return;
    float q = value / step;
    float n = floorf(q), frac = q - n;
    float sp = lv_font_get_line_height(f) * 1.0f;
    for (int k = -1; k <= 2; k++) {
        int v = (((int)n + k) * step) % mod;
        if (v < 0) v += mod;
        char t[8];
        snprintf(t, sizeof(t), fmt, v);
        dr_digits(dc, x_right, (lv_coord_t)(cy + (frac - k) * sp), t, f, c);
    }
    clip_pop(dc, saved);
}

// ---------------------------------------------------------------- tapes

static void draw_asi(lv_draw_ctx_t *dc, lv_coord_t ox, lv_coord_t oy) {
    const Pfd &p = g_t.pfd;
    lv_coord_t x1 = ox + L.asi_x, x2 = x1 + L.asi_w, y1 = oy + L.tape_y1, y2 = oy + L.tape_y2, cy = oy + L.tape_cy;
    dr_rect(dc, x1, y1, x2, y2, COL_TAPE, LV_OPA_70);
    float ppk = (float)(y2 - y1) / 60.0f;
    float v = p.ias < 20 ? 20 : p.ias;
    const lv_area_t *saved;
    lv_area_t clip;
    if (clip_push(dc, x1, y1, x2, y2, &saved, &clip)) {
        auto ky = [&](float kt) { return cy - (kt - v) * ppk; };
        // Colour bands on the right edge
        auto band = [&](float a, float b, lv_coord_t bx1, lv_coord_t bx2, lv_color_t c) {
            float ya = ky(b), yb = ky(a);
            if (yb < y1 || ya > y2) return;
            dr_rect(dc, bx1, (lv_coord_t)LV_MAX(ya, y1), bx2, (lv_coord_t)LV_MIN(yb, y2), c);
        };
        band(20, V_S0, x2 - 6, x2, COL_RED);
        band(V_S0, V_FE, x2 - 11, x2 - 7, COL_TEXT);
        band(V_S1, V_NO, x2 - 6, x2, COL_GREEN);
        band(V_NO, V_NE, x2 - 6, x2, COL_YELLOW);
        band(V_NE, 400, x2 - 6, x2, COL_RED);
        int k0 = (int)floorf((v - 34) / 5) * 5, k1 = (int)ceilf((v + 34) / 5) * 5;
        for (int kt = LV_MAX(k0, 20); kt <= k1; kt += 5) {
            float y = ky(kt);
            bool major = kt % 10 == 0;
            dr_line(dc, x2 - 12 - (major ? 8 : 0), y, x2 - 12, y, 2, COL_TEXT);
            if (major) {
                char t[6];
                snprintf(t, sizeof(t), "%d", kt);
                dr_text(dc, x2 - 26, (lv_coord_t)y, t, L.f_tape, COL_TEXT, TA_RIGHT);
            }
        }
        // Selected airspeed bug (FLC)
        if (g_live && (!strcmp(g_t.ap.vert, "IAS") || !strcmp(g_t.ap.vert, "FLC"))) {
            float by = LV_CLAMP(y1 + 6, ky(g_t.ap.ias), y2 - 6);
            dr_rect(dc, x2 - 6, (lv_coord_t)by - 7, x2, (lv_coord_t)by + 7, COL_CYAN);
        }
        clip_pop(dc, saved);
    }
    // Pointer box
    lv_coord_t bh = L.narrow ? 19 : 22;
    lv_coord_t bx2 = x2 - 10;
    dr_rect(dc, x1 - 2, cy - bh, bx2, cy + bh, lv_color_hex(0x000000), LV_OPA_COVER, 3, 2, COL_TEXT);
    lv_point_t notch[3] = {{(lv_coord_t)(bx2 - 1), (lv_coord_t)(cy - 9)}, {(lv_coord_t)(bx2 + 9), cy}, {(lv_coord_t)(bx2 - 1), (lv_coord_t)(cy + 9)}};
    dr_poly(dc, notch, 3, COL_TEXT);
    lv_point_t notch_in[3] = {{(lv_coord_t)(bx2 - 3), (lv_coord_t)(cy - 6)}, {(lv_coord_t)(bx2 + 5), cy}, {(lv_coord_t)(bx2 - 3), (lv_coord_t)(cy + 6)}};
    dr_poly(dc, notch_in, 3, lv_color_hex(0x000000));
    if (p.ias < 20) {
        dr_text(dc, (x1 + bx2) / 2, cy, "---", L.f_box, COL_TEXT, TA_CENTER);
    } else {
        lv_coord_t dw = digit_w(L.f_box);
        lv_coord_t xr = bx2 - 6;
        char t[6];
        snprintf(t, sizeof(t), "%d", (int)floorf(p.ias) / 10);
        dr_digits(dc, xr - dw, cy, t, L.f_box, COL_TEXT);
        drum(dc, xr, cy, cy - bh + 2, cy + bh - 2, p.ias, 1, 10, "%d", L.f_box, COL_TEXT);
    }
    char t[24];
    snprintf(t, sizeof(t), "TAS %.0fKT", p.tas);
    dr_text(dc, x1, y2 + 12, t, L.f_small, COL_TEXT);
    snprintf(t, sizeof(t), "GS %.0fKT", p.gs);
    dr_text(dc, x1, y2 + 30, t, L.f_small, COL_TEXT);
}

static void draw_alt(lv_draw_ctx_t *dc, lv_coord_t ox, lv_coord_t oy) {
    const Pfd &p = g_t.pfd;
    lv_coord_t x1 = ox + L.alt_x, x2 = x1 + L.alt_w, y1 = oy + L.tape_y1, y2 = oy + L.tape_y2, cy = oy + L.tape_cy;
    dr_rect(dc, x1, y1, x2, y2, COL_TAPE, LV_OPA_70);
    float ppf = (float)(y2 - y1) / 600.0f;
    float v = p.alt;
    auto fy = [&](float ft) { return cy - (ft - v) * ppf; };
    const lv_area_t *saved;
    lv_area_t clip;
    if (clip_push(dc, x1, y1, x2, y2, &saved, &clip)) {
        int k0 = (int)floorf((v - 320) / 20) * 20, k1 = (int)ceilf((v + 320) / 20) * 20;
        for (int ft = k0; ft <= k1; ft += 20) {
            float y = fy(ft);
            bool major = ft % 100 == 0;
            dr_line(dc, x1, y, x1 + (major ? 14 : 7), y, 2, COL_TEXT);
            if (major) {
                char t[8];
                snprintf(t, sizeof(t), "%d", ft);
                dr_text(dc, x1 + 18, (lv_coord_t)y, t, L.f_tape, COL_TEXT);
            }
        }
        if (g_live) {
            float by = LV_CLAMP(y1 + 6, fy(g_t.ap.alt), y2 - 6);
            lv_point_t b1[4] = {{x1, (lv_coord_t)(by - 9)}, {(lv_coord_t)(x1 + 8), (lv_coord_t)(by - 9)},
                                {(lv_coord_t)(x1 + 8), (lv_coord_t)(by + 9)}, {x1, (lv_coord_t)(by + 9)}};
            dr_poly(dc, b1, 4, COL_CYAN);
        }
        clip_pop(dc, saved);
    }
    // Selected altitude above the tape, baro setting below.
    char t[24];
    if (g_live) {
        snprintf(t, sizeof(t), "%.0f", g_t.ap.alt);
        dr_rect(dc, x1, y1 - 30, x2, y1 - 3, lv_color_hex(0x000000), LV_OPA_COVER, 3, 1, COL_BORDER);
        dr_text(dc, (x1 + x2) / 2, y1 - 16, t, L.f_tape, COL_CYAN, TA_CENTER);
    }
    snprintf(t, sizeof(t), "%.2fIN", p.baro);
    dr_text(dc, (x1 + x2) / 2, y2 + 14, t, L.f_small, COL_CYAN, TA_CENTER);

    // Pointer box: hundreds and up in big digits, last two digits rolling in 20 ft steps.
    lv_coord_t bh = L.narrow ? 21 : 25;
    lv_coord_t bx1 = x1 + 4;
    lv_coord_t bx2 = x2 + 2;
    dr_rect(dc, bx1, cy - bh, bx2, cy + bh, lv_color_hex(0x000000), LV_OPA_COVER, 3, 2, COL_TEXT);
    lv_point_t notch[3] = {{(lv_coord_t)(bx1 + 1), (lv_coord_t)(cy - 9)}, {(lv_coord_t)(bx1 - 9), cy}, {(lv_coord_t)(bx1 + 1), (lv_coord_t)(cy + 9)}};
    dr_poly(dc, notch, 3, COL_TEXT);
    lv_point_t notch_in[3] = {{(lv_coord_t)(bx1 + 3), (lv_coord_t)(cy - 6)}, {(lv_coord_t)(bx1 - 5), cy}, {(lv_coord_t)(bx1 + 3), (lv_coord_t)(cy + 6)}};
    dr_poly(dc, notch_in, 3, lv_color_hex(0x000000));
    lv_coord_t drum_r = bx2 - 5;
    lv_coord_t drum_l = drum_r - digit_w(L.f_alt_drum) * 2;
    int hundreds = (int)floorf(v / 100.0f);
    snprintf(t, sizeof(t), "%d", hundreds);
    dr_digits(dc, drum_l - 1, cy, t, L.f_alt_big, COL_TEXT);
    drum(dc, drum_r, cy, cy - bh + 2, cy + bh - 2, v, 20, 100, "%02d", L.f_alt_drum, COL_TEXT);
}

static void draw_vsi(lv_draw_ctx_t *dc, lv_coord_t ox, lv_coord_t oy) {
    const Pfd &p = g_t.pfd;
    lv_coord_t x1 = ox + L.vsi_x, x2 = x1 + L.vsi_w, cy = oy + L.tape_cy;
    lv_coord_t half = (L.tape_y2 - L.tape_y1) / 2 - 20;
    dr_rect(dc, x1, cy - half - 8, x2, cy + half + 8, COL_TAPE, LV_OPA_70, 4);
    float ppf = half / 2000.0f;
    for (int f = -2000; f <= 2000; f += 500) {
        lv_coord_t y = (lv_coord_t)(cy - f * ppf);
        bool major = f % 1000 == 0;
        dr_line(dc, x1, y, x1 + (major ? 10 : 6), y, 2, COL_TEXT);
        if (major && f) {
            char t[3];
            snprintf(t, sizeof(t), "%d", abs(f) / 1000);
            dr_text(dc, x1 + 18, y, t, L.f_small, COL_TEXT, TA_CENTER);
        }
    }
    if (g_live && !strcmp(g_t.ap.vert, "VS")) {
        lv_coord_t by = (lv_coord_t)(cy - LV_CLAMP(-2100, g_t.ap.vs, 2100) * ppf);
        dr_rect(dc, x1, by - 7, x1 + 6, by + 7, COL_CYAN);
    }
    float vs = LV_CLAMP(-2200.0f, p.vsi, 2200.0f);
    lv_coord_t y = (lv_coord_t)(cy - vs * ppf);
    lv_coord_t bx1 = x1 + 2;
    dr_rect(dc, bx1, y - 9, x2 + 2, y + 9, lv_color_hex(0x000000), LV_OPA_COVER, 2, 1, COL_TEXT);
    lv_point_t tri[3] = {{bx1, (lv_coord_t)(y - 6)}, {(lv_coord_t)(bx1 - 7), y}, {bx1, (lv_coord_t)(y + 6)}};
    dr_poly(dc, tri, 3, COL_TEXT);
    if (fabsf(p.vsi) >= 100) {
        char t[8];
        snprintf(t, sizeof(t), "%d", (int)(roundf(p.vsi / 50.0f) * 50));
        dr_text(dc, (bx1 + x2 + 2) / 2, y, t, FONT_12, COL_TEXT, TA_CENTER);
    }
}

static void draw_heading_tape(lv_draw_ctx_t *dc, lv_coord_t ox, lv_coord_t oy) {
    const Pfd &p = g_t.pfd;
    lv_coord_t x1 = ox + L.hdg_x1, x2 = ox + L.hdg_x2, y1 = oy, y2 = oy + L.hdg_h, xc = (x1 + x2) / 2;
    dr_rect(dc, x1, y1, x2, y2, COL_TAPE, LV_OPA_80);
    float span = L.narrow ? 36 : 50;
    float ppdh = (x2 - x1) / span;
    float h = p.hdg;
    auto hx = [&](float d) { return xc + wrap180(d - h) * ppdh; };
    const lv_area_t *saved;
    lv_area_t clip;
    if (clip_push(dc, x1, y1, x2, y2 + 8, &saved, &clip)) {
        int d0 = (int)floorf((h - span / 2 - 5) / 5) * 5;
        for (int d = d0; d <= h + span / 2 + 5; d += 5) {
            float x = xc + (d - h) * ppdh;
            int dd = ((d % 360) + 360) % 360;
            bool major = dd % 10 == 0;
            dr_line(dc, x, y2 - (major ? 11 : 6), x, y2, 2, COL_TEXT);
            if (major) {
                char t[4];
                const char *lbl = t;
                if (dd == 0) lbl = "N";
                else if (dd == 90) lbl = "E";
                else if (dd == 180) lbl = "S";
                else if (dd == 270) lbl = "W";
                else snprintf(t, sizeof(t), "%d", dd / 10);
                dr_text(dc, (lv_coord_t)x, y1 + 13, lbl, L.f_hdg, COL_TEXT, TA_CENTER);
            }
        }
        if (g_live) {
            // Heading bug (cyan, notched), clamped to the tape ends.
            float bx = LV_CLAMP(x1 + 4, hx(g_t.ap.hdg), x2 - 4);
            lv_coord_t b = y2;
            for (int side = -1; side <= 1; side += 2) {
                lv_point_t q[5] = {{(lv_coord_t)(bx + side * 10), b}, {(lv_coord_t)bx, b}, {(lv_coord_t)bx, (lv_coord_t)(b - 4)},
                                   {(lv_coord_t)(bx + side * 4), (lv_coord_t)(b - 9)}, {(lv_coord_t)(bx + side * 10), (lv_coord_t)(b - 9)}};
                dr_poly(dc, q, 5, COL_CYAN);
            }
            // Ground track: magenta diamond
            float tx = hx(p.trk);
            if (tx > x1 && tx < x2) {
                lv_point_t dmd[4] = {{(lv_coord_t)tx, (lv_coord_t)(y2 - 12)}, {(lv_coord_t)(tx + 6), (lv_coord_t)(y2 - 6)},
                                     {(lv_coord_t)tx, y2}, {(lv_coord_t)(tx - 6), (lv_coord_t)(y2 - 6)}};
                dr_poly(dc, dmd, 4, COL_MAGENTA);
            }
        }
        clip_pop(dc, saved);
    }
    // Turn-rate trend: where the heading will be in 6 seconds.
    float tr = LV_CLAMP(-(span / 2), p.turn * 6, span / 2);
    if (fabsf(tr) > 0.5f) dr_line(dc, xc, y2 + 3, xc + tr * ppdh, y2 + 3, 4, COL_MAGENTA);
    // Current heading box
    char t[8];
    snprintf(t, sizeof(t), "%03d", ((int)lroundf(h) % 360 + 360) % 360);
    lv_coord_t bw = L.narrow ? 25 : 36;
    dr_rect(dc, xc - bw, y1, xc + bw, y2 - 4, lv_color_hex(0x000000), LV_OPA_COVER, 3, 2, COL_TEXT);
    dr_text(dc, xc, (y1 + y2 - 4) / 2, t, L.narrow ? FONT_18 : FONT_24, COL_TEXT, TA_CENTER);
    lv_point_t tri[3] = {{(lv_coord_t)(xc - 7), (lv_coord_t)(y2 - 5)}, {(lv_coord_t)(xc + 7), (lv_coord_t)(y2 - 5)}, {xc, (lv_coord_t)(y2 + 3)}};
    dr_poly(dc, tri, 3, COL_TEXT);
}

// ---------------------------------------------------------------- nav, wind, OAT

static void draw_nav_info(lv_draw_ctx_t *dc, lv_coord_t ox, lv_coord_t oy) {
    const NavInfo &n = g_t.nav;
    lv_coord_t cx = ox + L.cx;
    lv_coord_t yb = oy + L.H - 14;
    if (n.has_to) {
        char t[40];
        if (n.has_dtk) snprintf(t, sizeof(t), "%s  %.1fNM  DTK %03.0f", n.to, n.to_dist, n.dtk);
        else snprintf(t, sizeof(t), "%s  %.1fNM  BRG %03.0f", n.to, n.to_dist, n.to_brg);
        lv_coord_t tw = text_width(t, L.f_small);
        dr_rect(dc, cx - tw / 2 - 8, yb - 11, cx + tw / 2 + 8, yb + 11, lv_color_hex(0x000000), LV_OPA_60, 4);
        dr_text(dc, cx, yb, t, L.f_small, COL_MAGENTA, TA_CENTER);
    }
    if (n.has_dtk) {
        // Lateral deviation, full scale 2 nm either side (1 nm in TERM, Direct-To near the airport).
        lv_coord_t y = yb - 26, half = L.narrow ? 70 : 100;
        for (int i = -2; i <= 2; i++)
            if (i) dr_circle(dc, cx + i * half / 2, y, 4, COL_TEXT, 2);
        dr_line(dc, cx, y - 9, cx, y + 9, 2, COL_TEXT);
        float fs = n.cdi_fs > 0 ? n.cdi_fs : 2.0f;
        if (fs < 1.5f) dr_text(dc, cx - half - 26, y, "TERM", FONT_12, COL_GREEN, TA_CENTER);
        float dev = LV_CLAMP(-1.0f, -n.xtk / fs, 1.0f);
        lv_coord_t dx = (lv_coord_t)(cx + dev * half);
        lv_point_t d[4] = {{dx, (lv_coord_t)(y - 8)}, {(lv_coord_t)(dx + 8), y}, {dx, (lv_coord_t)(y + 8)}, {(lv_coord_t)(dx - 8), y}};
        dr_poly(dc, d, 4, COL_MAGENTA);
    }
}

static void draw_wind_oat(lv_draw_ctx_t *dc, lv_coord_t ox, lv_coord_t oy) {
    const NavInfo &n = g_t.nav;
    lv_coord_t x = ox + L.asi_x + L.asi_w + 10, y = oy + L.H - 70;
    if (L.narrow) x = ox + L.asi_x + L.asi_w + 6;
    char t[24];
    if (n.wind_spd >= 1 && !L.narrow) {
        dr_rect(dc, x, y, x + 62, y + 56, lv_color_hex(0x000000), LV_OPA_60, 4);
        // Arrow pointing where the wind blows, relative to the nose.
        float hdg_true = g_t.pfd.hdg + n.magvar;
        float rel = (n.wind_dir + 180 - hdg_true) * DEG2RAD;
        float s = sinf(rel), c = cosf(rel);
        lv_coord_t ax = x + 31, ay = y + 20;
        dr_line(dc, ax - s * 13 * XS, ay + c * 13, ax + s * 13 * XS, ay - c * 13, 3, COL_TEXT);
        lv_point_t head[3] = {{(lv_coord_t)(ax + s * 16 * XS), (lv_coord_t)(ay - c * 16)},
                              {(lv_coord_t)(ax + (s * 5 - c * 7) * XS), (lv_coord_t)(ay - c * 5 - s * 7)},
                              {(lv_coord_t)(ax + (s * 5 + c * 7) * XS), (lv_coord_t)(ay - c * 5 + s * 7)}};
        dr_poly(dc, head, 3, COL_TEXT);
        snprintf(t, sizeof(t), "%.0fKT", n.wind_spd);
        dr_text(dc, x + 31, y + 45, t, FONT_14, COL_TEXT, TA_CENTER);
    }
    snprintf(t, sizeof(t), "OAT %.0f\xC2\xB0" "C", g_t.pfd.oat);
    lv_coord_t ax = ox + L.alt_x + L.alt_w / 2;
    dr_text(dc, ax, oy + L.tape_y2 + 32, t, L.f_small, COL_TEXT, TA_CENTER);
}

// Parking brake annunciation. Set while stopped or taxiing slowly: a yellow caution.
// Set while moving faster (a takeoff roll with the brake on) or in the air: a red warning
// that flashes. Full width: in the sky left of the roll scale; split: under the heading tape.
#define PBRK_WARN_GS 10.0f      // kt
static void draw_park_brake(lv_draw_ctx_t *dc, lv_coord_t ox, lv_coord_t oy) {
    const Pfd &p = g_t.pfd;
    if (!p.park_brake) return;
    bool warn = !p.on_ground || p.gs > PBRK_WARN_GS;
    if (warn && (g_now / 500) % 2) return;          // 1 Hz flash
    const char *t = "PARK BRAKE";
    lv_coord_t tw = text_width(t, FONT_18), bw = tw + 20, bh = 28;
    lv_coord_t x1 = L.narrow ? ox + L.cx - bw / 2 : ox + L.asi_x + L.asi_w + 12;
    lv_coord_t y1 = oy + L.hdg_h + (L.narrow ? 4 : 16);
    dr_rect(dc, x1, y1, x1 + bw, y1 + bh, warn ? COL_RED : COL_YELLOW, LV_OPA_COVER, 4, 1, lv_color_hex(0x000000));
    dr_text(dc, x1 + bw / 2, y1 + bh / 2, t, FONT_18, warn ? COL_TEXT : COL_BG, TA_CENTER);
}

static void draw_no_data(lv_draw_ctx_t *dc, const lv_area_t *a) {
    dr_rect(dc, a->x1, a->y1, a->x2, a->y2, lv_color_hex(0x101418));
    lv_coord_t cx = a->x1 + L.cx, cy = a->y1 + L.cy;
    const char *hint = "Ensure you have MSFS2024 or X-Plane 12 running";
    lv_coord_t hw = LV_MIN((lv_coord_t)(text_width(hint, FONT_14) / 2 + 20), (lv_coord_t)((a->x2 - a->x1) / 2 - 6));
    dr_rect(dc, cx - hw, cy - 34, cx + hw, cy + 34, lv_color_hex(0x000000), LV_OPA_COVER, 6, 2, COL_YELLOW);
    dr_text(dc, cx, cy - 11, "NO FLIGHT DATA", FONT_20, COL_YELLOW, TA_CENTER);
    dr_text(dc, cx, cy + 14, hint, FONT_14, COL_TEXT, TA_CENTER);
}

// ---------------------------------------------------------------- page

static void draw_cb(lv_event_t *e) {
    lv_draw_ctx_t *dc = lv_event_get_draw_ctx(e);
    lv_obj_t *obj = lv_event_get_target(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    if (!g_live) {
        draw_no_data(dc, &a);
        return;
    }
    const Pfd &p = g_t.pfd;
    fill_horizon(dc, &a, p.pitch, p.roll);
    // Horizon line
    {
        float sr = sinf(p.roll * DEG2RAD), cr = cosf(p.roll * DEG2RAD);
        float d = p.pitch * L.ppd;
        float mx = a.x1 + L.cx + sr * d * XS, my = a.y1 + L.cy + cr * d;
        dr_line(dc, mx - cr * 900 * XS, my + sr * 900, mx + cr * 900 * XS, my - sr * 900, 2, COL_TEXT);
    }
    draw_ladder(dc, a.x1, a.y1, p.pitch, p.roll);
    draw_roll_scale(dc, a.x1, a.y1, p.roll, p.slip);
    draw_aircraft_symbol(dc, a.x1, a.y1);
    draw_nav_info(dc, a.x1, a.y1);
    draw_wind_oat(dc, a.x1, a.y1);
    draw_heading_tape(dc, a.x1, a.y1);
    draw_asi(dc, a.x1, a.y1);
    draw_alt(dc, a.x1, a.y1);
    draw_vsi(dc, a.x1, a.y1);
    draw_park_brake(dc, a.x1, a.y1);
}

static void pfd_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h) {
    L.W = w;
    L.H = h;
    L.narrow = w < 600;
    if (L.narrow) {
        L.asi_x = 4; L.asi_w = 70;
        L.vsi_w = 32; L.vsi_x = w - L.vsi_w - 2;
        L.alt_w = 80; L.alt_x = L.vsi_x - L.alt_w - 8;
        L.f_tape = FONT_14; L.f_box = FONT_24; L.f_alt_big = FONT_24; L.f_alt_drum = FONT_16;
        L.f_hdg = FONT_14; L.f_small = FONT_14;
        L.ladder_half_w = 75;
    } else {
        L.asi_x = 8; L.asi_w = 88;
        L.vsi_w = 40; L.vsi_x = w - L.vsi_w - 4;
        L.alt_w = 100; L.alt_x = L.vsi_x - L.alt_w - 10;
        L.f_tape = FONT_18; L.f_box = FONT_28; L.f_alt_big = FONT_28; L.f_alt_drum = FONT_20;
        L.f_hdg = FONT_16; L.f_small = FONT_16;
        L.ladder_half_w = 130;
    }
    L.hdg_h = 34;
    L.tape_y1 = 52;
    L.tape_y2 = h - 46;
    L.tape_cy = (L.tape_y1 + L.tape_y2) / 2;
    L.cx = (L.asi_x + L.asi_w + L.alt_x) / 2;
    L.cy = L.tape_cy;
    L.hdg_x1 = L.asi_x + L.asi_w + (L.narrow ? 8 : 40);
    L.hdg_x2 = L.alt_x - (L.narrow ? 12 : 40);
    L.ppd = 9.0f;
    L.roll_r = LV_MIN(L.cy - L.hdg_h - 14, (L.alt_x - L.asi_x - L.asi_w) / 2 - 18);
    s_obj = ui_canvas(parent, 0, 0, w, h, draw_cb);
    s_seq = 0;
    s_was_live = !g_live;
}

static void pfd_update() {
    if (!s_obj) return;
    if (g_t.seq_fast != s_seq || g_live != s_was_live) {
        s_seq = g_t.seq_fast;
        s_was_live = g_live;
        lv_obj_invalidate(s_obj);
    }
}

static void pfd_destroy() { s_obj = nullptr; }

const PageDef PAGE_PFD = {"PFD", pfd_create, pfd_update, pfd_destroy};
