// MOVING MAP
// Aircraft-centred map, north-up or track-up. Flight plan (active leg magenta), airports,
// VORs and NDBs from the bridge's database, projected locally with flat-earth vector
// math around the aircraft (accurate to well under a pixel at the ranges shown).
// Terrain from the SD card underneath: topographic colours, or relative to the
// aircraft's altitude (red: within 100 ft below or above, yellow: within 1000 ft below).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nav_dct.h"
#include "terrain.h"
#include "ui_priv.h"

#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif

static const float RANGES[] = {1, 2, 3, 5, 10, 15, 20, 30, 50, 80, 120, 200};
static const int N_RANGES = sizeof(RANGES) / sizeof(RANGES[0]);
static int s_range = 6;                 // 20 nm (kept across page switches)
static bool s_track_up = false;

static lv_obj_t *s_obj, *s_orient_btn, *s_terr_btn;
static lv_coord_t s_w, s_h;
static const lv_coord_t BAR_H = 32;
static uint32_t s_seq, s_last_draw, s_dct_gen;
static bool s_was_live;

struct Proj {
    double lat0, lon0;
    float cx, cy, ppn, s, c;            // centre, pixels per nm, sin/cos of map rotation
    void to_xy(float lat, float lon, float &x, float &y) const {
        float e, n;
        geo_offset_nm(lat0, lon0, lat, lon, e, n);
        x = cx + (e * c - n * s) * ppn * XS;
        y = cy - (e * s + n * c) * ppn;
    }
};

static float map_rotation() { return s_track_up ? g_t.pfd.trk + g_t.nav.magvar : 0.0f; }

// ---------------------------------------------------------------- terrain

enum { TERR_OFF, TERR_TOPO, TERR_REL };
static int s_terr = TERR_OFF;
static const char *TERR_NAMES[] = {"TERRAIN", "TOPO", "REL ALT"};

// The terrain is drawn into an image MARGIN pixels larger than the map on every side,
// centred where the aircraft was. Small moves only shift it; it is redrawn when the
// shift gets large or anything else changes.
#define MARGIN 48
static uint16_t *s_timg;
static lv_coord_t s_tw, s_th;
static lv_img_dsc_t s_tdsc;
struct TerrKey {
    double lat, lon;
    float rot, ppn;
    int mode, alt_band, level;
    uint32_t gen;
    bool pending, valid;
    uint32_t ms;
};
static TerrKey s_tk;

static uint16_t rgb565(uint32_t c) { return (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F)); }

// Topographic colours by height (metres), muted so the map symbols stay readable.
static uint16_t s_topo[200];                // 50 m steps from 0 m
static void build_topo_lut() {
    static const struct { int m; uint32_t c; } K[] = {
        {0, 0x1E3A1E}, {150, 0x2C4A22}, {300, 0x3E5426}, {600, 0x58582A}, {1000, 0x6A5430},
        {1500, 0x6E4A30}, {2200, 0x6A4636}, {3000, 0x7A6A62}, {4000, 0x9A9A9A}, {6000, 0xC8C8C8}};
    const int nk = sizeof(K) / sizeof(K[0]);
    for (int i = 0; i < 200; i++) {
        int m = i * 50, k = 0;
        while (k < nk - 2 && m > K[k + 1].m) k++;
        float t = LV_CLAMP(0.0f, (float)(m - K[k].m) / (K[k + 1].m - K[k].m), 1.0f);
        uint32_t a = K[k].c, b = K[k + 1].c, c = 0;
        for (int sh = 0; sh <= 16; sh += 8) {
            int ca = (a >> sh) & 0xFF, cb = (b >> sh) & 0xFF;
            c |= (uint32_t)(ca + (cb - ca) * t) << sh;
        }
        s_topo[i] = rgb565(c);
    }
}

static uint16_t terr_color(int16_t m, int mode, int alt_m) {
    if (m == TERRAIN_NONE) return 0;
    if (mode == TERR_TOPO) {
        if (m == TERRAIN_WATER || m <= 0) return rgb565(0x0E2440);
        return s_topo[LV_MIN(m / 50, 199)];
    }
    int e = m == TERRAIN_WATER ? 0 : m;
    if (e > alt_m - 30) return rgb565(0xB01818);            // within 100 ft below, or above
    if (e > alt_m - 305) return rgb565(0xA89000);           // within 1000 ft below
    return m == TERRAIN_WATER || m <= 0 ? rgb565(0x08162A) : rgb565(0x0C140C);
}

static void *terr_alloc(size_t n) {
#ifdef ARDUINO
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(n);
#endif
}

// Render the terrain image centred on (lat0, lon0). Mercator coordinates are computed
// exactly every 16 pixels and interpolated in between.
static void terr_render(double lat0, double lon0, float ppn, float rs, float rc, int level, int mode, int alt_m) {
    const int G = 16;
    const double scale = 256.0 * (double)(1 << level);
    const double coslat = cos(lat0 * 0.017453292519943295);
    int gw = s_tw / G + 2, gh = s_th / G + 2;
    static double *gx, *gy;
    static int gcap;
    if (gw * gh > gcap) {
        free(gx);
        free(gy);
        gcap = gw * gh;
        gx = (double *)malloc(sizeof(double) * gcap);
        gy = (double *)malloc(sizeof(double) * gcap);
    }
    for (int j = 0; j < gh; j++)
        for (int i = 0; i < gw; i++) {
            float X = (i * G - s_tw / 2.0f) / (ppn * XS), Y = -(j * G - s_th / 2.0f) / ppn;
            float e = X * rc + Y * rs, n = -X * rs + Y * rc;      // nm east / north
            double lat = lat0 + n / 60.0, lon = lon0 + e / (60.0 * coslat);
            if (lat > 85.0) lat = 85.0;
            if (lat < -85.0) lat = -85.0;
            gx[j * gw + i] = (lon + 180.0) / 360.0 * scale;
            gy[j * gw + i] = (1.0 - asinh(tan(lat * 0.017453292519943295)) / 3.141592653589793) / 2.0 * scale;
        }
    terrain_begin();
    for (int bj = 0; bj < gh - 1; bj++)
        for (int bi = 0; bi < gw - 1; bi++) {
            const double *x0 = &gx[bj * gw + bi], *y0 = &gy[bj * gw + bi];
            double bx = x0[0], by = y0[0];
            // Corner offsets relative to the block's top-left corner (small: float is fine).
            float ax = 0, ay = 0, bxx = (float)(x0[1] - bx), bxy = (float)(y0[1] - by);
            float cx = (float)(x0[gw] - bx), cy = (float)(y0[gw] - by);
            float dx = (float)(x0[gw + 1] - bx), dy = (float)(y0[gw + 1] - by);
            int32_t ibx = (int32_t)floor(bx), iby = (int32_t)floor(by);
            float fbx = (float)(bx - ibx), fby = (float)(by - iby);
            for (int v = 0; v < G; v++) {
                int yy = bj * G + v;
                if (yy >= s_th) break;
                float t = (v + 0.5f) / G;
                float lx = ax + (cx - ax) * t, ly = ay + (cy - ay) * t;     // left edge
                float rx = bxx + (dx - bxx) * t, ry = bxy + (dy - bxy) * t; // right edge
                uint16_t *row = s_timg + (size_t)yy * s_tw + bi * G;
                for (int u = 0; u < G; u++) {
                    if (bi * G + u >= s_tw) break;
                    float s2 = (u + 0.5f) / G;
                    float px = fbx + lx + (rx - lx) * s2, py = fby + ly + (ry - ly) * s2;
                    int16_t m = terrain_sample(level, ibx + (int32_t)floorf(px), iby + (int32_t)floorf(py));
                    row[u] = terr_color(m, mode, alt_m);
                }
            }
        }
    s_tk.pending = terrain_end();
}

// Draw the terrain under the map; re-render the image when needed.
static void draw_terrain(lv_draw_ctx_t *dc, const lv_area_t &a, float cx, float cy, float ppn, float rot_deg,
                         float rs, float rc) {
    if (s_terr == TERR_OFF || !terrain_available()) return;
    lv_coord_t tw = s_w + 2 * MARGIN, th = (s_h - BAR_H) + 2 * MARGIN;
    if (!s_timg || tw != s_tw || th != s_th) {
        free(s_timg);
        s_timg = (uint16_t *)terr_alloc((size_t)tw * th * 2);
        s_tw = tw;
        s_th = th;
        s_tk.valid = false;
        if (!s_timg) return;
    }
    double lat = g_t.nav.lat, lon = g_t.nav.lon;
    int alt_m = (int)(g_t.pfd.alt * 0.3048f);
    int level = terrain_pick_level(1.0f / ppn, lat);
    float ox = 0, oy = 0;                       // where the image centre is now, relative to (cx, cy)
    if (s_tk.valid) {
        float e, n;
        geo_offset_nm(lat, lon, s_tk.lat, s_tk.lon, e, n);
        ox = (e * rc - n * rs) * ppn * XS;
        oy = -(e * rs + n * rc) * ppn;
    }
    bool moved = fabsf(ox) > MARGIN - 6 || fabsf(oy) > MARGIN - 6;
    bool stale = !s_tk.valid || s_tk.mode != s_terr || s_tk.ppn != ppn || fabsf(wrap180(rot_deg - s_tk.rot)) > 0.7f ||
                 s_tk.level != level || (s_terr == TERR_REL && abs(alt_m / 15 - s_tk.alt_band) > 1) || moved;
    bool more = s_tk.pending && s_tk.gen != terrain_generation() && g_now - s_tk.ms > 300;
    if (stale || more) {
        terr_render(lat, lon, ppn, rs, rc, level, s_terr, alt_m);
        s_tk.lat = lat;
        s_tk.lon = lon;
        s_tk.rot = rot_deg;
        s_tk.ppn = ppn;
        s_tk.mode = s_terr;
        s_tk.alt_band = alt_m / 15;
        s_tk.level = level;
        s_tk.gen = terrain_generation();
        s_tk.valid = true;
        s_tk.ms = g_now;
        ox = oy = 0;
        s_tdsc.header.always_zero = 0;
        s_tdsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        s_tdsc.header.w = tw;
        s_tdsc.header.h = th;
        s_tdsc.data_size = (uint32_t)tw * th * 2;
        s_tdsc.data = (const uint8_t *)s_timg;
        lv_img_cache_invalidate_src(&s_tdsc);
    }
    lv_draw_img_dsc_t d;
    lv_draw_img_dsc_init(&d);
    lv_coord_t x0 = (lv_coord_t)lroundf(cx + ox) - tw / 2, y0 = (lv_coord_t)lroundf(cy + oy) - th / 2;
    lv_area_t area = {x0, y0, (lv_coord_t)(x0 + tw - 1), (lv_coord_t)(y0 + th - 1)};
    lv_draw_img(dc, &d, &area, &s_tdsc);
}

static void terrain_legend(lv_draw_ctx_t *dc, const lv_area_t &a) {
    if (s_terr == TERR_OFF) return;
    lv_coord_t x = a.x1 + 10, y = a.y2 - 16;
    if (!terrain_available()) {
        dr_text(dc, x, y, terrain_status(), FONT_14, COL_YELLOW);
        return;
    }
    if (s_terr == TERR_REL) {
        dr_rect(dc, x, y - 6, x + 12, y + 6, lv_color_hex(0xB01818));
        dr_text(dc, x + 18, y, "-100FT", FONT_12, COL_TEXT);
        dr_rect(dc, x + 76, y - 6, x + 88, y + 6, lv_color_hex(0xA89000));
        dr_text(dc, x + 94, y, "-1000FT", FONT_12, COL_TEXT);
    }
    if (s_tk.pending) dr_text(dc, x, y - 20, "TERRAIN LOADING", FONT_12, COL_DIM);
}

static void send_range() {
    // Ask the bridge for everything out to the screen corners.
    float ppn = (s_h - BAR_H) / 2.0f / RANGES[s_range];
    float half_w = s_w / 2.0f / XS;         // in vertical-pixel units
    float half_diag = sqrtf(half_w * half_w + ((s_h - BAR_H) / 2.0f) * ((s_h - BAR_H) / 2.0f));
    cmd_map_range(half_diag / ppn);
}

// ---------------------------------------------------------------- symbols

static void sym_vor(lv_draw_ctx_t *dc, float x, float y) {
    lv_point_t p[6];
    for (int i = 0; i < 6; i++) {
        float a = (i * 60 + 30) * DEG2RAD;
        p[i] = {(lv_coord_t)(x + cosf(a) * 8 * XS), (lv_coord_t)(y + sinf(a) * 8)};
    }
    for (int i = 0; i < 6; i++) dr_line(dc, p[i].x, p[i].y, p[(i + 1) % 6].x, p[(i + 1) % 6].y, 2, COL_CYAN);
    dr_circle(dc, (lv_coord_t)x, (lv_coord_t)y, 2, COL_CYAN, 0);
}

static void sym_ndb(lv_draw_ctx_t *dc, float x, float y) {
    lv_color_t c = lv_color_hex(0xC890FF);
    for (int i = 0; i < 10; i++) {
        float a = i * 36 * DEG2RAD;
        dr_circle(dc, (lv_coord_t)(x + cosf(a) * 8 * XS), (lv_coord_t)(y + sinf(a) * 8), 1, c, 0);
    }
    dr_circle(dc, (lv_coord_t)x, (lv_coord_t)y, 3, c, 0);
}

static void sym_airport(lv_draw_ctx_t *dc, float x, float y, bool big) {
    if (big) {
        dr_circle(dc, (lv_coord_t)x, (lv_coord_t)y, 7, lv_color_hex(0x2F6BFF), 0);
        for (int i = 0; i < 4; i++) {
            float a = i * 90 * DEG2RAD;
            dr_line(dc, x + cosf(a) * 7 * XS, y + sinf(a) * 7, x + cosf(a) * 11 * XS, y + sinf(a) * 11, 3, lv_color_hex(0x2F6BFF));
        }
        dr_circle(dc, (lv_coord_t)x, (lv_coord_t)y, 3, COL_TEXT, 0);
    } else {
        dr_circle(dc, (lv_coord_t)x, (lv_coord_t)y, 5, COL_MAGENTA, 2);
    }
}

static void sym_aircraft(lv_draw_ctx_t *dc, float cx, float cy, float hdg_deg) {
    // Outline in local units (x right, y forward), rotated to the heading.
    static const float fus[][2] = {{0, 15}, {2.5f, 10}, {2.5f, -12}, {0, -14}, {-2.5f, -12}, {-2.5f, 10}};
    static const float wing[][2] = {{-15, 1}, {15, 1}, {15, -3}, {-15, -3}};
    static const float tail[][2] = {{-6, -10}, {6, -10}, {6, -13}, {-6, -13}};
    float a = hdg_deg * DEG2RAD, s = sinf(a), c = cosf(a);
    auto xf = [&](const float (*pts)[2], int n, lv_point_t *out, float scale) {
        for (int i = 0; i < n; i++) {
            float lx = pts[i][0] * scale, ly = pts[i][1] * scale;
            out[i] = {(lv_coord_t)(cx + (lx * c + ly * s) * XS), (lv_coord_t)(cy + lx * s - ly * c)};
        }
    };
    lv_point_t p[6];
    // Black halo first, then white.
    for (int pass = 0; pass < 2; pass++) {
        float sc = pass == 0 ? 1.25f : 1.0f;
        lv_color_t col = pass == 0 ? lv_color_hex(0x000000) : COL_TEXT;
        xf(wing, 4, p, sc);
        dr_poly(dc, p, 4, col);
        xf(tail, 4, p, sc);
        dr_poly(dc, p, 4, col);
        xf(fus, 6, p, sc);
        dr_poly(dc, p, 6, col);
    }
}

// ---------------------------------------------------------------- drawing

static void label_box(lv_draw_ctx_t *dc, float x, float y, const char *t, lv_color_t c) {
    lv_coord_t tw = text_width(t, FONT_14);
    dr_rect(dc, (lv_coord_t)x - 3, (lv_coord_t)y - 9, (lv_coord_t)x + tw + 3, (lv_coord_t)y + 9, lv_color_hex(0x000000),
            LV_OPA_70, 3);
    dr_text(dc, (lv_coord_t)x, (lv_coord_t)y, t, FONT_14, c);
}

// ETE as MM:SS under an hour, H:MM with an "H" above.
static void fmt_ete(char *out, size_t n, int s) {
    if (s < 0) snprintf(out, n, "--:--");
    else if (s < 3600) snprintf(out, n, "%02d:%02d", s / 60, s % 60);
    else snprintf(out, n, "%d:%02dH", s / 3600, (s / 60) % 60);
}

// Nav data strip: [WPT KABE] [DIS 24.5NM] [DTK 045°] [TRK 043°] [ETE 09:12]. Labels white,
// values magenta (Garmin). The split-screen map shows WPT, DIS and ETE.
static void draw_data_bar(lv_draw_ctx_t *dc, lv_coord_t x, lv_coord_t y) {
    dr_rect(dc, x, y, x + s_w - 1, y + BAR_H - 1, lv_color_hex(0x0A0D10));
    dr_line(dc, x, y + BAR_H - 1, x + s_w, y + BAR_H - 1, 1, COL_BORDER);
    const NavInfo &n = g_t.nav;
    const char *lab[5] = {"WPT", "DIS", "DTK", "TRK", "ETE"};
    char val[5][16];
    snprintf(val[0], 16, "%s", n.has_to ? n.to : "____");
    if (n.has_to) snprintf(val[1], 16, n.to_dist < 100 ? "%.1fNM" : "%.0fNM", n.to_dist);
    else snprintf(val[1], 16, "---NM");
    if (n.has_dtk) snprintf(val[2], 16, "%03d\xC2\xB0", (int)lroundf(wrap360(n.dtk)) % 360);
    else snprintf(val[2], 16, "---\xC2\xB0");
    if (g_t.pfd.gs >= 5) snprintf(val[3], 16, "%03d\xC2\xB0", (int)lroundf(wrap360(g_t.pfd.trk)) % 360);
    else snprintf(val[3], 16, "---\xC2\xB0");
    fmt_ete(val[4], 16, n.has_to ? n.ete : -1);
    static const int wide[] = {0, 1, 2, 3, 4}, narrow[] = {0, 1, 4};
    const int *order = s_w >= 600 ? wide : narrow;
    int nf = s_w >= 600 ? 5 : 3;
    lv_coord_t cw = s_w / nf, cy = y + BAR_H / 2;
    for (int i = 0; i < nf; i++) {
        int k = order[i];
        lv_coord_t lw = text_width(lab[k], FONT_14), vw = text_width(val[k], FONT_20);
        lv_coord_t x0 = x + cw * i + (cw - lw - 6 - vw) / 2;
        dr_text(dc, x0, cy + 1, lab[k], FONT_14, COL_TEXT);
        bool have = k == 3 ? g_t.pfd.gs >= 5 : k == 2 ? n.has_dtk : n.has_to;
        dr_text(dc, x0 + lw + 6, cy, val[k], FONT_20, have ? COL_MAGENTA : COL_DIM);
        if (i) dr_line(dc, x + cw * i, y + 6, x + cw * i, y + BAR_H - 7, 1, COL_BORDER);
    }
}

// ---------------------------------------------------------------- Direct-To course

// The course from the Direct-To origin to the waypoint, drawn along the great circle in
// short pieces, so it stays where it really is when the aircraft is off it (the pilot
// sees which way and how far to turn to intercept it).
static void draw_dct_course(lv_draw_ctx_t *dc, const Proj &P) {
    const DctNav &D = dct();
    if (!D.active) return;
    int n = LV_CLAMP((int)(D.leg_nm / 4), 1, 96);
    float px = 0, py = 0;
    for (int pass = 0; pass < 2; pass++) {       // dark casing, then the magenta line
        for (int i = 0; i <= n; i++) {
            double la, lo;
            gc_interp(D.org_lat, D.org_lon, D.wpt.lat, D.wpt.lon, (double)i / n, la, lo);
            float x, y;
            P.to_xy((float)la, (float)lo, x, y);
            if (i) {
                if (pass == 0) dr_line(dc, px, py, x, y, 9, lv_color_hex(0x000000), LV_OPA_50);
                else dr_line(dc, px, py, x, y, 5, COL_MAGENTA);
            }
            px = x;
            py = y;
        }
    }
    // Waypoint: magenta airport symbol and ident
    float x, y;
    P.to_xy((float)D.wpt.lat, (float)D.wpt.lon, x, y);
    dr_circle(dc, (lv_coord_t)x, (lv_coord_t)y, 9, lv_color_hex(0x000000), 0);
    dr_circle(dc, (lv_coord_t)x, (lv_coord_t)y, 8, COL_MAGENTA, 3);
    dr_circle(dc, (lv_coord_t)x, (lv_coord_t)y, 3, COL_MAGENTA, 0);
    label_box(dc, x + 14, y - 14, D.wpt.id, COL_MAGENTA);
}

// Where the aircraft is going: a dashed line along the ground track, 3 minutes ahead but
// at least 40 % of the way to the map edge, so the pilot sees where it meets the course.
static void draw_track_vector(lv_draw_ctx_t *dc, const Proj &P) {
    if (g_t.pfd.gs < 30) return;
    float len = LV_MAX(g_t.pfd.gs / 20.0f * P.ppn, 0.4f * (s_h - BAR_H) / 2);
    float a = (wrap360(g_t.pfd.trk + g_t.nav.magvar) - map_rotation()) * DEG2RAD;
    float dx = sinf(a) * XS, dy = -cosf(a);
    for (float d = 18; d < len; d += 16) {
        float e = LV_MIN(d + 9, len);
        dr_line(dc, P.cx + dx * d, P.cy + dy * d, P.cx + dx * e, P.cy + dy * e, 2, COL_TEXT);
    }
}

// Course deviation indicator: two dots each side, full scale at the outer dot (ENR 2 nm,
// TERM 1 nm). The bar shows where the course is: fly towards it.
static void draw_cdi(lv_draw_ctx_t *dc, const lv_area_t &a) {
    const NavInfo &n = g_t.nav;
    if (!n.has_dtk) return;
    // Clear of the zoom keys (right) and, in the split view, above the terrain legend.
    lv_coord_t half = LV_MIN(130, (s_w - 260) / 2);
    lv_coord_t cx = a.x1 + s_w / 2, y = a.y2 - (s_w < 600 ? 58 : 26);
    dr_rect(dc, cx - half - 50, y - 44, cx + half + 50, y + 22, lv_color_hex(0x000000), LV_OPA_70, 6, 1, COL_BORDER);
    float fs = n.cdi_fs > 0 ? n.cdi_fs : 2.0f;
    dr_text(dc, cx - half - 26, y - 10, fs < 1.5f ? "TERM" : "ENR", FONT_12, COL_GREEN, TA_CENTER);
    for (int i = -2; i <= 2; i++)
        if (i) dr_circle(dc, cx + i * half / 2, y, 5, COL_TEXT, 2);
    dr_line(dc, cx, y - 14, cx, y + 14, 2, COL_TEXT);
    float dev = LV_CLAMP(-1.0f, -n.xtk / fs, 1.0f);
    lv_coord_t bx = (lv_coord_t)(cx + dev * half);
    dr_rect(dc, bx - 3, y - 15, bx + 3, y + 15, COL_MAGENTA, LV_OPA_COVER, 1);
    // TO / FROM: a triangle pointing up (to) or down (from) by the centre line.
    lv_coord_t tx = cx + half + 26;
    if (n.from) {
        lv_point_t t[3] = {{(lv_coord_t)(tx - 9), (lv_coord_t)(y - 5)}, {(lv_coord_t)(tx + 9), (lv_coord_t)(y - 5)}, {tx, (lv_coord_t)(y + 7)}};
        dr_poly(dc, t, 3, COL_MAGENTA);
    } else {
        lv_point_t t[3] = {{(lv_coord_t)(tx - 9), (lv_coord_t)(y + 5)}, {(lv_coord_t)(tx + 9), (lv_coord_t)(y + 5)}, {tx, (lv_coord_t)(y - 7)}};
        dr_poly(dc, t, 3, COL_MAGENTA);
    }
    dr_text(dc, tx, y + 15, n.from ? "FROM" : "TO", FONT_12, COL_TEXT, TA_CENTER);
    char t[24];
    float ax = fabsf(n.xtk);
    if (ax >= 0.01f) snprintf(t, sizeof(t), ax < 10 ? "XTK %.2fNM %c" : "XTK %.1fNM %c", ax, n.xtk > 0 ? 'R' : 'L');
    else snprintf(t, sizeof(t), "XTK 0.00NM");
    dr_text(dc, cx - half - 26, y + 9, n.dct ? "DCT" : "GPS", FONT_12, COL_MAGENTA, TA_CENTER);
    dr_text(dc, cx, y - 30, t, FONT_14, ax > fs ? COL_YELLOW : COL_MAGENTA, TA_CENTER);
}

static void draw_cb(lv_event_t *ev) {
    lv_draw_ctx_t *dc = lv_event_get_draw_ctx(ev);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target(ev), &a);
    if (!g_live) {
        dr_text(dc, a.x1 + s_w / 2, a.y1 + s_h / 2, "NO POSITION DATA", FONT_24, COL_YELLOW, TA_CENTER);
        return;
    }
    Proj P;
    P.lat0 = g_t.nav.lat;
    P.lon0 = g_t.nav.lon;
    P.cx = a.x1 + s_w / 2.0f;
    P.cy = a.y1 + BAR_H + (s_h - BAR_H) / 2.0f;
    float range = RANGES[s_range];
    P.ppn = ((s_h - BAR_H) / 2.0f - 6) / range;
    float rot = map_rotation() * DEG2RAD;
    P.s = sinf(rot);
    P.c = cosf(rot);

    const lv_area_t *saved;
    lv_area_t clip;
    if (clip_push(dc, a.x1, a.y1 + BAR_H, a.x2, a.y2, &saved, &clip)) {
        draw_terrain(dc, a, P.cx, P.cy, P.ppn, map_rotation(), P.s, P.c);
        // Range ring at half range, dashed, with its distance.
        lv_coord_t rr = (lv_coord_t)(P.ppn * range / 2);
        for (int d = 0; d < 360; d += 12) dr_arc(dc, (lv_coord_t)P.cx, (lv_coord_t)P.cy, rr, 1, d, d + 6, COL_DIM);
        char t[16];
        snprintf(t, sizeof(t), range / 2 < 1 ? "%.1f" : "%.0f", range / 2);
        dr_text(dc, (lv_coord_t)(P.cx - rr * 0.70f * XS), (lv_coord_t)(P.cy - rr * 0.70f), t, FONT_14, COL_DIM, TA_CENTER);

        // Airports and navaids
        bool labels = range <= 50, small_labels = range <= 15;
        for (int i = 0; i < g_t.n_pts; i++) {
            const MapPt &m = g_t.pts[i];
            float x, y;
            P.to_xy(m.lat, m.lon, x, y);
            if (x < a.x1 - 20 || x > a.x2 + 20 || y < a.y1 + BAR_H - 20 || y > a.y2 + 20) continue;
            lv_color_t lc = COL_TEXT;
            switch (m.kind) {
            case 'V': sym_vor(dc, x, y); lc = COL_CYAN; break;
            case 'N': sym_ndb(dc, x, y); lc = lv_color_hex(0xC890FF); break;
            case 'A': sym_airport(dc, x, y, true); break;
            default: sym_airport(dc, x, y, false); lc = COL_MAGENTA; break;
            }
            bool in_plan = false;
            for (int k = 0; k < g_t.n_fpl && !in_plan; k++) in_plan = !strcmp(g_t.fpl[k].id, m.id);
            if (!in_plan && (m.kind == 'a' ? small_labels : labels)) dr_text(dc, (lv_coord_t)x + 12, (lv_coord_t)y + 1, m.id, FONT_12, lc);
        }

        // Flight plan (a Direct-To replaces its active leg: then every leg is drawn white)
        bool dct_on = dct().active;
        int fpl_act = dct_on ? -1 : g_t.fpl_act;
        for (int i = 1; i < g_t.n_fpl; i++) {
            float x1, y1, x2, y2;
            P.to_xy(g_t.fpl[i - 1].lat, g_t.fpl[i - 1].lon, x1, y1);
            P.to_xy(g_t.fpl[i].lat, g_t.fpl[i].lon, x2, y2);
            if (i == fpl_act) dr_line(dc, x1, y1, x2, y2, 4, COL_MAGENTA);
            else if (i < fpl_act) dr_line(dc, x1, y1, x2, y2, 2, lv_color_hex(0x707880));
            else dr_line(dc, x1, y1, x2, y2, dct_on ? 2 : 3, COL_TEXT);
        }
        for (int i = 0; i < g_t.n_fpl; i++) {
            float x, y;
            P.to_xy(g_t.fpl[i].lat, g_t.fpl[i].lon, x, y);
            bool act = i == fpl_act;
            lv_point_t d[4] = {{(lv_coord_t)x, (lv_coord_t)(y - 6)}, {(lv_coord_t)(x + 6), (lv_coord_t)y},
                               {(lv_coord_t)x, (lv_coord_t)(y + 6)}, {(lv_coord_t)(x - 6), (lv_coord_t)y}};
            dr_poly(dc, d, 4, act ? COL_MAGENTA : COL_TEXT);
            if (g_t.fpl[i].id[0]) label_box(dc, x + 10, y - 12, g_t.fpl[i].id, act ? COL_MAGENTA : COL_TEXT);
        }

        draw_dct_course(dc, P);
        if (g_t.nav.has_dtk) draw_track_vector(dc, P);

        // Aircraft, and a north pointer in track-up mode.
        float hdg_true = g_t.pfd.hdg + g_t.nav.magvar;
        sym_aircraft(dc, P.cx, P.cy, hdg_true - map_rotation());
        if (s_track_up) {
            float nx = a.x2 - 30, ny = a.y1 + BAR_H + 30;
            float s = -P.s, c = P.c;            // screen direction of true north
            dr_circle(dc, (lv_coord_t)nx, (lv_coord_t)ny, 18, lv_color_hex(0x000000), 0, LV_OPA_70);
            lv_point_t tri[3] = {{(lv_coord_t)(nx + s * 14 * XS), (lv_coord_t)(ny - c * 14)},
                                 {(lv_coord_t)(nx + (-c * 6 - s * 4) * XS), (lv_coord_t)(ny - s * 6 + c * 4)},
                                 {(lv_coord_t)(nx + (c * 6 - s * 4) * XS), (lv_coord_t)(ny + s * 6 + c * 4)}};
            dr_poly(dc, tri, 3, COL_TEXT);
            dr_text(dc, (lv_coord_t)(nx - s * 8 * XS), (lv_coord_t)(ny + c * 8), "N", FONT_12, COL_TEXT, TA_CENTER);
        }
        // Range readout above the zoom keys
        snprintf(t, sizeof(t), range < 1 ? "%.1f NM" : "%.0f NM", range);
        dr_text(dc, a.x2 - 40, a.y2 - 140, t, FONT_18, COL_CYAN, TA_CENTER);
        terrain_legend(dc, a);
        draw_cdi(dc, a);
        clip_pop(dc, saved);
    }
    draw_data_bar(dc, a.x1, a.y1);
}

// ---------------------------------------------------------------- page

static void zoom_cb(lv_event_t *e) {
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    int r = LV_CLAMP(0, s_range + d, N_RANGES - 1);
    if (r == s_range) return;
    s_range = r;
    send_range();
    lv_obj_invalidate(s_obj);
}

static void orient_cb(lv_event_t *) {
    s_track_up = !s_track_up;
    ui_set_text(ui_button_label(s_orient_btn), s_track_up ? "TRK UP" : "NORTH UP");
    lv_obj_invalidate(s_obj);
}

static void terr_cb(lv_event_t *) {
    s_terr = (s_terr + 1) % 3;
    ui_set_text(ui_button_label(s_terr_btn), TERR_NAMES[s_terr]);
    ui_button_look(s_terr_btn, s_terr == TERR_OFF ? BTN_NORMAL : BTN_SELECTED);
    lv_obj_invalidate(s_obj);
}

static void map_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h) {
    s_w = w;
    s_h = h;
    s_obj = ui_canvas(parent, 0, 0, w, h, draw_cb);
    lv_obj_t *zin = ui_button(parent, "+", FONT_32, 64, 56, zoom_cb, (void *)(intptr_t)-1);
    lv_obj_set_pos(zin, w - 72, h - 124);
    lv_obj_t *zout = ui_button(parent, "-", FONT_32, 64, 56, zoom_cb, (void *)(intptr_t)1);
    lv_obj_set_pos(zout, w - 72, h - 62);
    s_orient_btn = ui_button(parent, s_track_up ? "TRK UP" : "NORTH UP", FONT_14, 96, 40, orient_cb, nullptr);
    lv_obj_set_pos(s_orient_btn, 8, BAR_H + 8);
    s_terr_btn = ui_button(parent, TERR_NAMES[s_terr], FONT_14, 96, 40, terr_cb, nullptr);
    lv_obj_set_pos(s_terr_btn, 112, BAR_H + 8);
    ui_button_look(s_terr_btn, s_terr == TERR_OFF ? BTN_NORMAL : BTN_SELECTED);
    static bool lut;
    if (!lut) build_topo_lut();
    lut = true;
    s_tk.valid = false;
    s_seq = 0;
    s_was_live = !g_live;
    send_range();
}

static void map_update() {
    if (!s_obj) return;
    bool changed = g_t.seq_fast != s_seq || g_live != s_was_live || dct().gen != s_dct_gen ||
                   (s_terr != TERR_OFF && s_tk.pending && s_tk.gen != terrain_generation());
    if (changed && g_now - s_last_draw >= 66) {         // ~15 Hz is plenty for a map
        s_seq = g_t.seq_fast;
        s_dct_gen = dct().gen;
        s_was_live = g_live;
        s_last_draw = g_now;
        lv_obj_invalidate(s_obj);
    }
}

static void map_destroy() { s_obj = nullptr; }

const PageDef PAGE_MAP = {"MAP", map_create, map_update, map_destroy};
