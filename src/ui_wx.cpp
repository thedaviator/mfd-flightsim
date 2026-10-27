// WEATHER
// RADAR: heading-up 120 deg sector with an animated sweep over the precipitation grid
// from the bridge. LIGHTNING: 360 deg stormscope with strikes fading over three minutes.
// The grid (north up, centred where the aircraft was) and the strikes are placed
// relative to the aircraft here. Neither simulator exports radar returns or lightning:
// the bridge gets them from the internet (RainViewer, NOAA GOES) or generates them, and
// sends a status line for each that the page shows under the mode name.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_priv.h"

#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif

static const float RADAR_RANGES[] = {10, 20, 40, 80, 160};
static const float LTNG_RANGES[] = {25, 50, 100, 200};
static int s_radar_rng = 2, s_ltng_rng = 1;
static bool s_ltng = false;

static lv_obj_t *s_obj, *s_btn_radar, *s_btn_ltng;
static lv_coord_t s_w, s_h;
static uint32_t s_last_draw, s_range_sent_ms;

// The radar sector as an image with alpha, redrawn from the grid only when the grid,
// heading, range or position changes (drawing the grid cell by cell every frame of the
// sweep animation would be far too slow).
static uint8_t *s_img;
static size_t s_img_cap;
static lv_img_dsc_t s_img_dsc;
struct RasterKey {
    uint32_t seq;
    float hdg, range, R, ge, gn;
};
static RasterKey s_key;
static bool s_key_valid;

static float range_nm() { return s_ltng ? LTNG_RANGES[s_ltng_rng] : RADAR_RANGES[s_radar_rng]; }

static lv_color_t level_color(int k) {
    switch (k) {
    case 1: return lv_color_hex(0x00B030);
    case 2: return lv_color_hex(0xF0E000);
    case 3: return lv_color_hex(0xF02020);
    default: return lv_color_hex(0xFF30FF);
    }
}

// Screen position of (lat, lon) for a heading-up display centred at (cx, cy).
static void place(float lat, float lon, float cx, float cy, float ppn, float s, float c, float &x, float &y) {
    float e, n;
    geo_offset_nm(g_t.nav.lat, g_t.nav.lon, lat, lon, e, n);
    x = cx + (e * c - n * s) * ppn * XS;
    y = cy - (e * s + n * c) * ppn;
}

static void *img_alloc(size_t n) {
#ifdef ARDUINO
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(n);
#endif
}

// Fill the sector image (apex at the bottom centre, radius R pixels) from the grid.
// (ge, gn): grid centre relative to the aircraft, nm east / north; s, c: true heading.
static void raster(lv_coord_t iw, lv_coord_t ih, float R, float ppn, float s, float c, float ge, float gn) {
    const int PX = LV_IMG_PX_SIZE_ALPHA_BYTE;
    memset(s_img, 0, (size_t)iw * ih * PX);
    const WxGrid &g = g_t.grid;
    const int N = g.n;
    const float inv = N / (2.0f * g.half_nm), mid = N / 2.0f;
    uint8_t col[5][4] = {};
    for (int k = 1; k <= 4; k++) {
        lv_color_t lc = level_color(k);
        memcpy(col[k], &lc, sizeof(lv_color_t));
        col[k][PX - 1] = 0xFF;
    }
    const float hw = (iw - 1) / 2.0f, ddx = 1.0f / (XS * ppn);
    // One pixel to the right: east += ddx * c, north -= ddx * s.
    const float dcol = ddx * c * inv, drow = ddx * s * inv;
    for (int v = 0; v < ih; v++) {
        float h = (float)(ih - 1 - v);              // pixels ahead of the apex
        if (h > R) continue;
        float half = XS * LV_MIN(h * 1.7320508f, sqrtf(R * R - h * h));
        int u0 = LV_MAX(0, (int)ceilf(hw - half)), u1 = LV_MIN(iw - 1, (int)floorf(hw + half));
        float dx = (u0 - hw) * ddx, dy = h / ppn;
        float colf = (dx * c + dy * s - ge) * inv + mid;
        float rowf = mid - (-dx * s + dy * c - gn) * inv;
        uint8_t *p = s_img + ((size_t)v * iw + u0) * PX;
        for (int u = u0; u <= u1; u++, p += PX, colf += dcol, rowf += drow) {
            if (colf < 0 || rowf < 0) continue;
            int ci = (int)colf, ri = (int)rowf;
            if (ci >= N || ri >= N) continue;
            uint8_t k = g.lv[ri * N + ci];
            if (k) memcpy(p, col[k > 4 ? 4 : k], PX);
        }
    }
}

// Draw the grid into the sector; the image's bottom centre pixel is the apex (cx, cy).
static void draw_grid(lv_draw_ctx_t *dc, float cx, float cy, float R, float ppn, float hdg, float s, float c) {
    lv_coord_t hwi = (lv_coord_t)ceilf(R * XS * 0.8660254f) + 1;
    lv_coord_t iw = 2 * hwi + 1, ih = (lv_coord_t)ceilf(R) + 1;
    size_t need = (size_t)iw * ih * LV_IMG_PX_SIZE_ALPHA_BYTE;
    if (need > s_img_cap) {
        free(s_img);
        s_img = (uint8_t *)img_alloc(need);
        s_img_cap = s_img ? need : 0;
        s_key_valid = false;
        if (!s_img) return;
    }
    float ge, gn;
    geo_offset_nm(g_t.nav.lat, g_t.nav.lon, g_t.grid.lat, g_t.grid.lon, ge, gn);
    float range = range_nm();
    bool redo = !s_key_valid || s_key.seq != g_t.grid_seq || s_key.range != range || s_key.R != R ||
                fabsf(wrap180(hdg - s_key.hdg)) > 0.7f || hypotf(ge - s_key.ge, gn - s_key.gn) * ppn > 1.5f ||
                s_img_dsc.header.w != (uint32_t)iw || s_img_dsc.header.h != (uint32_t)ih;
    if (redo) {
        raster(iw, ih, R, ppn, s, c, ge, gn);
        s_key = {g_t.grid_seq, hdg, range, R, ge, gn};
        s_key_valid = true;
        s_img_dsc.header.always_zero = 0;
        s_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
        s_img_dsc.header.w = iw;
        s_img_dsc.header.h = ih;
        s_img_dsc.data_size = need;
        s_img_dsc.data = s_img;
        lv_img_cache_invalidate_src(&s_img_dsc);
    }
    lv_draw_img_dsc_t d;
    lv_draw_img_dsc_init(&d);
    lv_coord_t x0 = (lv_coord_t)lroundf(cx) - hwi, y0 = (lv_coord_t)lroundf(cy) - (ih - 1);
    lv_area_t area = {x0, y0, (lv_coord_t)(x0 + iw - 1), (lv_coord_t)(y0 + ih - 1)};
    lv_draw_img(dc, &d, &area, &s_img_dsc);
}

static bool wx_fresh() { return g_t.wx_valid && g_now - g_t.wx_rx_ms <= 5000; }

static void draw_strikes(lv_draw_ctx_t *dc, float cx, float cy, float ppn, float s, float c) {
    // Ages are as of the last weather packet; add the time since.
    int extra = (int)((g_now - g_t.wx_rx_ms) / 1000);
    for (int i = 0; i < g_t.n_strikes; i++) {
        const Strike &st = g_t.strikes[i];
        int age = st.age + extra;
        if (age > 180) continue;
        float x, y;
        place(st.lat, st.lon, cx, cy, ppn, s, c, x, y);
        lv_color_t col = age < 40 ? lv_color_hex(0xFFFFA0) : age < 100 ? COL_YELLOW : lv_color_hex(0xA08020);
        lv_coord_t sz = age < 40 ? 7 : 5;
        dr_line(dc, x - sz, y, x + sz, y, 3, col);
        dr_line(dc, x, y - sz, x, y + sz, 3, col);
    }
}

static void own_ship(lv_draw_ctx_t *dc, float x, float y) {
    lv_point_t p[3] = {{(lv_coord_t)x, (lv_coord_t)(y - 12)}, {(lv_coord_t)(x + 8), (lv_coord_t)(y + 8)},
                       {(lv_coord_t)(x - 8), (lv_coord_t)(y + 8)}};
    dr_poly(dc, p, 3, COL_TEXT);
}

static void draw_radar(lv_draw_ctx_t *dc, const lv_area_t &a, float hdg, float s, float c) {
    float R = LV_MIN(s_h - 44.0f, (s_w / 2.0f - 16) / (0.866f * XS));
    float cx = a.x1 + s_w / 2.0f, cy = a.y2 - 14;
    float range = range_nm(), ppn = R / range;

    // Sector background, masked by one convex polygon: the apex plus points along the
    // (aspect-corrected) outer arc. The grid image is already cut to the sector.
    lv_point_t sector[26];
    sector[0] = {(lv_coord_t)cx, (lv_coord_t)cy};
    for (int i = 0; i <= 24; i++) {
        float aa = (-60 + i * 5) * DEG2RAD;
        sector[i + 1] = {(lv_coord_t)lroundf(cx + sinf(aa) * R * XS), (lv_coord_t)lroundf(cy - cosf(aa) * R)};
    }
    lv_draw_mask_polygon_param_t poly;
    lv_draw_mask_polygon_init(&poly, sector, 26);
    int16_t id1 = lv_draw_mask_add(&poly, nullptr);
    dr_rect(dc, (lv_coord_t)(cx - R), (lv_coord_t)(cy - R), (lv_coord_t)(cx + R), (lv_coord_t)cy, lv_color_hex(0x00140A));
    lv_draw_mask_remove_id(id1);
    lv_draw_mask_free_param(&poly);
    if (g_live && g_t.has_grid && wx_fresh()) draw_grid(dc, cx, cy, R, ppn, hdg, s, c);
    // Sweep line with a fading trail
    float ph = (g_now % 4000) / 4000.0f;
    float sw = ph < 0.5f ? -60 + 240 * ph : 60 - 240 * (ph - 0.5f);
    float dir = ph < 0.5f ? 1 : -1;
    for (int k = 6; k >= 0; k--) {
        float aa = (sw - dir * k * 1.5f) * DEG2RAD;
        dr_line(dc, cx, cy, cx + sinf(aa) * R * XS, cy - cosf(aa) * R, 3, lv_color_hex(0x40FF80), (lv_opa_t)(220 - k * 30));
    }

    // Range arcs, sector edges, labels
    for (int q = 1; q <= 4; q++) {
        lv_coord_t r = (lv_coord_t)(R * q / 4);
        dr_arc(dc, (lv_coord_t)cx, (lv_coord_t)cy, r, 1, 210, 330, q == 4 ? COL_TEXT : COL_DIM);
        char t[12];
        float v = range * q / 4;
        snprintf(t, sizeof(t), v < 10 && v != (int)v ? "%.1f" : "%.0f", v);
        float la = 60 * DEG2RAD;
        dr_text(dc, (lv_coord_t)(cx + sinf(la) * r * XS + 6), (lv_coord_t)(cy - cosf(la) * r - 8), t, FONT_14, COL_DIM);
    }
    for (int side = -1; side <= 1; side += 2) {
        float aa = side * 60 * DEG2RAD;
        dr_line(dc, cx, cy, cx + sinf(aa) * R * XS, cy - cosf(aa) * R, 1, COL_TEXT);
    }
    dr_line(dc, cx, cy - R * 0.25f, cx, cy - R, 1, COL_DIM);
    own_ship(dc, cx, cy - 2);
}

static void draw_ltng(lv_draw_ctx_t *dc, const lv_area_t &a, float s, float c) {
    float R = LV_MIN(s_h / 2.0f - 26, (s_w / 2.0f - 44) / XS);
    float cx = a.x1 + s_w / 2.0f, cy = a.y1 + s_h / 2.0f + 2;
    float range = range_nm(), ppn = R / range;
    dr_circle(dc, (lv_coord_t)cx, (lv_coord_t)cy, (lv_coord_t)R, lv_color_hex(0x080C10), 0);
    dr_circle(dc, (lv_coord_t)cx, (lv_coord_t)cy, (lv_coord_t)R, COL_TEXT, 1);
    dr_circle(dc, (lv_coord_t)cx, (lv_coord_t)cy, (lv_coord_t)(R / 2), COL_DIM, 1);
    // Compass ticks around the ring, heading up
    float hdg = g_t.pfd.hdg;
    for (int d = 0; d < 360; d += 30) {
        float aa = (d - hdg) * DEG2RAD;
        float sx = sinf(aa) * XS, cy2 = -cosf(aa);
        dr_line(dc, cx + sx * R, cy + cy2 * R, cx + sx * (R - 10), cy + cy2 * (R - 10), 2, COL_TEXT);
        char t[4];
        const char *lbl = t;
        if (d == 0) lbl = "N";
        else if (d == 90) lbl = "E";
        else if (d == 180) lbl = "S";
        else if (d == 270) lbl = "W";
        else snprintf(t, sizeof(t), "%d", d / 10);
        dr_text(dc, (lv_coord_t)(cx + sx * (R + 14)), (lv_coord_t)(cy + cy2 * (R + 14)), lbl, FONT_14, COL_TEXT, TA_CENTER);
    }
    char t[12];
    snprintf(t, sizeof(t), "%.0f", range / 2);
    dr_text(dc, (lv_coord_t)(cx + R / 2 * 0.707f * XS + 4), (lv_coord_t)(cy - R / 2 * 0.707f - 8), t, FONT_14, COL_DIM);
    if (g_live && wx_fresh()) draw_strikes(dc, cx, cy, ppn, s, c);
    own_ship(dc, cx, cy);
}

static void draw_cb(lv_event_t *ev) {
    lv_draw_ctx_t *dc = lv_event_get_draw_ctx(ev);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target(ev), &a);
    float hdg_true = g_t.pfd.hdg + g_t.nav.magvar;
    float s = sinf(hdg_true * DEG2RAD), c = cosf(hdg_true * DEG2RAD);
    if (s_ltng) draw_ltng(dc, a, s, c);
    else draw_radar(dc, a, hdg_true, s, c);

    char t[24];
    snprintf(t, sizeof(t), "%.0f NM", range_nm());
    dr_text(dc, a.x2 - 40, a.y2 - 140, t, FONT_18, COL_CYAN, TA_CENTER);
    // Mode and data source, bottom left (clear of the sector and the ring).
    lv_coord_t ty = a.y2 - 52;
    dr_text(dc, a.x1 + 10, ty, s_ltng ? "LIGHTNING" : "WX RADAR", FONT_16, COL_TEXT);
    if (!g_live || !wx_fresh()) {
        dr_text(dc, a.x1 + 10, ty + 20, "NO WX DATA", FONT_14, COL_YELLOW);
    } else {
        const char *st = s_ltng ? g_t.ltg_txt : g_t.rdr_txt;
        bool ok = s_ltng ? g_t.ltg_ok : g_t.rdr_ok;
        dr_text(dc, a.x1 + 10, ty + 20, st, FONT_14, ok ? COL_DIM : COL_YELLOW);
    }
    if (s_ltng && g_live && wx_fresh()) {
        snprintf(t, sizeof(t), "%d STRIKES", g_t.n_strikes);
        dr_text(dc, a.x1 + 10, ty + 40, t, FONT_14, COL_DIM);
    }
}

static void mode_looks() {
    ui_button_look(s_btn_radar, s_ltng ? BTN_NORMAL : BTN_SELECTED);
    ui_button_look(s_btn_ltng, s_ltng ? BTN_SELECTED : BTN_NORMAL);
}

static void mode_cb(lv_event_t *e) {
    s_ltng = lv_event_get_user_data(e) != nullptr;
    mode_looks();
    lv_obj_invalidate(s_obj);
}

// The bridge sizes the radar grid to the range shown.
static void send_range() {
    cmd_wx_range(RADAR_RANGES[s_radar_rng]);
    s_range_sent_ms = g_now;
}

static void zoom_cb(lv_event_t *e) {
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_ltng) {
        s_ltng_rng = LV_CLAMP(0, s_ltng_rng + d, 3);
    } else {
        s_radar_rng = LV_CLAMP(0, s_radar_rng + d, 4);
        send_range();
    }
    lv_obj_invalidate(s_obj);
}

static void wx_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h) {
    s_w = w;
    s_h = h;
    s_obj = ui_canvas(parent, 0, 0, w, h, draw_cb);
    s_btn_radar = ui_button(parent, "RADAR", FONT_16, 92, 42, mode_cb, nullptr);
    lv_obj_set_pos(s_btn_radar, 8, 8);
    s_btn_ltng = ui_button(parent, "LTNG", FONT_16, 92, 42, mode_cb, (void *)1);
    lv_obj_set_pos(s_btn_ltng, 106, 8);
    lv_obj_t *zin = ui_button(parent, "+", FONT_32, 64, 56, zoom_cb, (void *)(intptr_t)-1);
    lv_obj_set_pos(zin, w - 72, h - 124);
    lv_obj_t *zout = ui_button(parent, "-", FONT_32, 64, 56, zoom_cb, (void *)(intptr_t)1);
    lv_obj_set_pos(zout, w - 72, h - 62);
    mode_looks();
    send_range();
}

static void wx_update() {
    if (!s_obj) return;
    if (g_now - s_range_sent_ms >= 10000) send_range();      // the bridge may have restarted
    if (g_now - s_last_draw >= (s_ltng ? 200u : 50u)) {   // sweep animation at 20 Hz
        s_last_draw = g_now;
        lv_obj_invalidate(s_obj);
    }
}

static void wx_destroy() { s_obj = nullptr; }

const PageDef PAGE_WX = {"WX", wx_create, wx_update, wx_destroy};
