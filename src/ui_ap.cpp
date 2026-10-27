// AUTOPILOT
// Mode keys (lit green when active, white border when armed) and selectors for heading
// bug, altitude, vertical speed and airspeed. Selector keys repeat while held. Values
// are sent as absolute targets; the last value sent is used as the base for further
// presses until the simulator has reported it back.
#include <stdio.h>
#include <string.h>

#include "ui_priv.h"

static const char *MODE_KEYS[10] = {"AP", "FD", "YD", "HDG", "NAV", "APR", "BC", "ALT", "VS", "FLC"};

enum Sel { S_HDG, S_ALT, S_VS, S_IAS, S_COUNT };
static const char *SEL_VAR[S_COUNT] = {"hdg", "alt", "vs", "ias"};
static const char *SEL_NAME[S_COUNT] = {"HDG", "ALT", "VS", "IAS"};
struct Step { const char *label; float delta; };   // delta 0 = SYNC
static const Step STEPS[S_COUNT][5] = {
    {{"-10", -10}, {"-1", -1}, {"SYNC", 0}, {"+1", 1}, {"+10", 10}},
    {{"-1000", -1000}, {"-100", -100}, {"SYNC", 0}, {"+100", 100}, {"+1000", 1000}},
    {{"-500", -500}, {"-100", -100}, {"SYNC", 0}, {"+100", 100}, {"+500", 500}},
    {{"-10", -10}, {"-1", -1}, {"SYNC", 0}, {"+1", 1}, {"+10", 10}},
};

static lv_obj_t *s_root;
static lv_obj_t *s_mode_btn[10];
static lv_obj_t *s_val[S_COUNT];
static float s_pending[S_COUNT];
static uint32_t s_pending_ms[S_COUNT];
static uint32_t s_seq;
static bool s_was_live;

static float reported(Sel s) {
    const Autopilot &a = g_t.ap;
    switch (s) {
    case S_HDG: return a.hdg;
    case S_ALT: return a.alt;
    case S_VS: return a.vs;
    default: return a.ias;
    }
}

static float current(Sel s) {
    return g_now - s_pending_ms[s] < 1500 ? s_pending[s] : reported(s);
}

static void format_val(Sel s, float v, char *t, size_t n) {
    switch (s) {
    case S_HDG: snprintf(t, n, "%03d\xC2\xB0", ((int)lroundf(v) % 360 + 360) % 360); break;
    case S_ALT: snprintf(t, n, "%.0fFT", v); break;
    case S_VS: snprintf(t, n, "%+.0fFPM", v); break;
    default: snprintf(t, n, "%.0fKT", v); break;
    }
}

static void step_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_LONG_PRESSED_REPEAT) return;
    if (!g_live) return;
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    Sel s = (Sel)(id / 5);
    const Step &st = STEPS[s][id % 5];
    if (st.delta == 0 && code == LV_EVENT_LONG_PRESSED_REPEAT) return;
    float v;
    if (st.delta == 0) {                    // SYNC to the aircraft's present value
        switch (s) {
        case S_HDG: v = roundf(g_t.pfd.hdg); break;
        case S_ALT: v = roundf(g_t.pfd.alt / 100) * 100; break;
        case S_VS: v = roundf(g_t.pfd.vsi / 100) * 100; break;
        default: v = roundf(g_t.pfd.ias); break;
        }
    } else {
        v = current(s) + st.delta;
        // Big altitude steps land on whole thousands, like a real selector.
        if (s == S_ALT && fabsf(st.delta) >= 1000) v = roundf(v / 1000) * 1000;
    }
    switch (s) {
    case S_HDG: v = wrap360(v); break;
    case S_ALT: v = LV_CLAMP(0.0f, v, 50000.0f); break;
    case S_VS: v = LV_CLAMP(-3000.0f, v, 3000.0f); break;
    default: v = LV_CLAMP(40.0f, v, 300.0f); break;
    }
    s_pending[s] = v;
    s_pending_ms[s] = g_now;
    cmd_ap_set(SEL_VAR[s], v);
    char t[16];
    format_val(s, v, t, sizeof(t));
    ui_set_text(s_val[s], t);
}

static void mode_cb(lv_event_t *e) {
    if (!g_live) return;
    cmd_ap(MODE_KEYS[(intptr_t)lv_event_get_user_data(e)]);
}

static BtnLook mode_look(int k) {
    if (!g_live) return BTN_DISABLED;
    const Autopilot &a = g_t.ap;
    const char *key = MODE_KEYS[k];
    switch (k) {
    case 0: return a.master ? BTN_ACTIVE : BTN_NORMAL;
    case 1: return a.fd ? BTN_ACTIVE : BTN_NORMAL;
    case 2: return a.yd ? BTN_ACTIVE : BTN_NORMAL;
    default: break;
    }
    // FLC shows as IAS in the annunciations.
    const char *vert = !strcmp(a.vert, "IAS") ? "FLC" : a.vert;
    if (!strcmp(a.lat, key) || !strcmp(vert, key)) return BTN_ACTIVE;
    if (strstr(a.lat_arm, key) || strstr(a.vert_arm, key)) return BTN_ARMED;
    return BTN_NORMAL;
}

static void ap_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h) {
    s_root = parent;
    bool narrow = w < 600;
    lv_coord_t gap = 6;
    lv_coord_t mw = (w - 12 - gap * 4) / 5, mh = 56;
    for (int k = 0; k < 10; k++) {
        s_mode_btn[k] = ui_button(parent, MODE_KEYS[k], narrow ? FONT_20 : FONT_24, mw, mh, mode_cb, (void *)(intptr_t)k);
        lv_obj_set_pos(s_mode_btn[k], 6 + (k % 5) * (mw + gap), 6 + (k / 5) * (mh + gap));
    }
    lv_coord_t y0 = 6 + 2 * (mh + gap) + 6;
    lv_coord_t rh = (h - y0 - 4) / S_COUNT;
    lv_coord_t name_w = narrow ? 46 : 60, val_w = narrow ? 104 : 190;
    lv_coord_t bx = 6 + name_w + val_w;
    lv_coord_t bw = (w - bx - 6 - gap * 4) / 5;
    for (int s = 0; s < S_COUNT; s++) {
        lv_coord_t y = y0 + s * rh;
        lv_obj_t *row = ui_box(parent, 4, y, w - 8, rh - 4);
        lv_obj_set_style_bg_color(row, COL_PANEL, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_t *n = ui_label(row, narrow ? FONT_14 : FONT_20, COL_DIM, SEL_NAME[s]);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 8, 0);
        s_val[s] = ui_label(row, narrow ? FONT_18 : FONT_32, COL_CYAN, "---");
        lv_obj_align(s_val[s], LV_ALIGN_LEFT_MID, name_w + 4, 0);
        for (int k = 0; k < 5; k++) {
            int id = s * 5 + k;
            lv_obj_t *b = ui_button(row, STEPS[s][k].label, narrow ? FONT_12 : FONT_18, bw, rh - 12, nullptr, nullptr);
            lv_obj_add_event_cb(b, step_cb, LV_EVENT_PRESSED, (void *)(intptr_t)id);
            lv_obj_add_event_cb(b, step_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)id);
            lv_obj_align(b, LV_ALIGN_LEFT_MID, bx - 4 + k * (bw + gap), 0);
        }
    }
    s_seq = 0;
    s_was_live = !g_live;
}

static void ap_update() {
    if (!s_root) return;
    if (g_t.seq_mid == s_seq && g_live == s_was_live) return;
    s_seq = g_t.seq_mid;
    s_was_live = g_live;
    for (int k = 0; k < 10; k++) ui_button_look(s_mode_btn[k], mode_look(k));
    for (int s = 0; s < S_COUNT; s++) {
        char t[16];
        if (g_live) format_val((Sel)s, current((Sel)s), t, sizeof(t));
        else strcpy(t, "---");
        ui_set_text(s_val[s], t);
    }
}

static void ap_destroy() { s_root = nullptr; }

const PageDef PAGE_AP = {"AP", ap_create, ap_update, ap_destroy};
