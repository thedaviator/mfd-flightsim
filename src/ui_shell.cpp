// Screen frame: top bar (radios, autopilot, transponder, link), page area with optional
// split screen, bottom softkeys, toast messages and the system status window.
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "nav_dct.h"
#include "ui_priv.h"

Telemetry g_t;
uint32_t g_now;
bool g_live;

UiPlatform g_platform;
static const PageDef *PAGES[PG_COUNT] = {&PAGE_PFD, &PAGE_EIS, &PAGE_RADIO, &PAGE_MAP, &PAGE_NRST, &PAGE_WX, &PAGE_AP, &PAGE_SETUP};

static lv_obj_t *s_main;                 // page area
static lv_obj_t *s_pane[2];              // [0] full or left, [1] right (split)
static const PageDef *s_shown[2];
static PageId s_page = PG_PFD;           // full-screen page, or right page in split mode
static bool s_split = false;
static lv_obj_t *s_keys[PG_COUNT + 1];
static lv_obj_t *s_dct_key;
static lv_obj_t *s_toast;
static uint32_t s_toast_until;
static lv_obj_t *s_ota, *s_ota_title, *s_ota_msg, *s_ota_bar, *s_ota_close;
static int s_ota_last = 0;
static void build_ota_overlay(lv_obj_t *scr);
static lv_obj_t *s_sysinfo;
static lv_obj_t *s_sysinfo_label;

// Screen sleep. With no flight data and no touch for the chosen time, the screen dims
// for DIM_MS with a notice, then the backlight goes off (it has no brightness control).
// Flight data or a touch wakes it. An overlay catches the waking touch, so it doesn't
// press whatever is under the finger.
#define DIM_MS 30000
enum { AWAKE, DIMMED, ASLEEP };
static lv_obj_t *s_sleep, *s_sleep_msg;
static int s_sleep_min = 10, s_sleep_state = AWAKE;
static bool s_sleep_swallow;                 // the finger that woke the screen is still down
static uint32_t s_last_live;
static void build_sleep_overlay(lv_obj_t *scr);
static void update_sleep();

// Top bar labels
static lv_obj_t *s_com_act, *s_com_stby;
static lv_obj_t *s_nav_act, *s_nav_stby;
static lv_obj_t *s_ap_main, *s_ap_lat, *s_ap_vert, *s_ap_arm;
static lv_obj_t *s_xpdr_code, *s_xpdr_mode, *s_link;

// ---------------------------------------------------------------- page management

static void build_panes() {
    for (int i = 0; i < 2; i++) {
        if (s_shown[i] && s_shown[i]->destroy) s_shown[i]->destroy();
        s_shown[i] = nullptr;
        if (s_pane[i]) lv_obj_del(s_pane[i]);
        s_pane[i] = nullptr;
    }
    if (s_split) {
        s_pane[0] = ui_box(s_main, 0, 0, SPLIT_LEFT_W, MAIN_H);
        s_pane[1] = ui_box(s_main, SPLIT_LEFT_W, 0, 800 - SPLIT_LEFT_W, MAIN_H);
        // Divider: a left border drawn over the right page's content.
        lv_obj_set_style_border_side(s_pane[1], LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(s_pane[1], 2, 0);
        lv_obj_set_style_border_color(s_pane[1], COL_BORDER, 0);
        lv_obj_set_style_border_post(s_pane[1], true, 0);
        s_shown[0] = &PAGE_PFD;
        s_shown[1] = PAGES[s_page];
    } else {
        s_pane[0] = ui_box(s_main, 0, 0, 800, MAIN_H);
        s_shown[0] = PAGES[s_page];
    }
    for (int i = 0; i < 2; i++)
        if (s_shown[i]) s_shown[i]->create(s_pane[i], !s_split ? 800 : i == 0 ? SPLIT_LEFT_W : 800 - SPLIT_LEFT_W, MAIN_H);

    for (int i = 0; i < PG_COUNT; i++) {
        bool on = s_split ? (i == PG_PFD || i == s_page) : i == s_page;
        ui_button_look(s_keys[i], on ? BTN_SELECTED : BTN_NORMAL);
    }
    ui_button_look(s_keys[PG_COUNT], s_split ? BTN_ACTIVE : BTN_NORMAL);
}

void ui_show_page(PageId id) {
    dct_window_close();
    if (id == PG_SETUP && s_split) {        // settings always use the full screen
        s_split = false;
        s_page = PG_SETUP;
        build_panes();
        return;
    }
    if (s_split) {
        if (id == PG_PFD) {
            s_split = false;
            s_page = PG_PFD;
        } else if (id == s_page) {
            return;
        } else {
            s_page = id;
        }
    } else {
        if (id == s_page) return;
        s_page = id;
    }
    build_panes();
}

void ui_set_split(bool split) {
    dct_window_close();
    if (split == s_split) return;
    s_split = split;
    if (split && (s_page == PG_PFD || s_page == PG_SETUP)) s_page = PG_MAP;
    build_panes();
}

static void softkey_cb(lv_event_t *e) {
    intptr_t i = (intptr_t)lv_event_get_user_data(e);
    if (i == PG_COUNT) ui_set_split(!s_split);
    else ui_show_page((PageId)i);
}

// The D-> key draws its symbol over an empty label, in the label's colour (set by ui_button_look).
static void dct_key_draw(lv_event_t *e) {
    lv_obj_t *b = lv_event_get_target(e);
    lv_area_t a;
    lv_obj_get_coords(b, &a);
    lv_color_t c = lv_obj_get_style_text_color(ui_button_label(b), 0);
    if (dct().active && !dct_window_visible()) c = COL_MAGENTA;
    dr_dct_icon(lv_event_get_draw_ctx(e), (a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2, c);
}

// ---------------------------------------------------------------- toast and status window

void ui_toast(const char *msg, lv_color_t color) {
    lv_label_set_text(s_toast, msg);
    lv_obj_set_style_text_color(s_toast, color, 0);
    lv_obj_set_style_border_color(s_toast, color, 0);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    s_toast_until = g_now + 2500;
}

static void sysinfo_refresh() {
    static char buf[768];
    char lines[512] = "";
    if (g_platform.sys_info) g_platform.sys_info(lines, sizeof(lines));
    snprintf(buf, sizeof(buf),
             "%s\nSource: %s   %s\n\n(c) 2026 AeroRelics - David Eckert\n"
             "Open source, MIT License. For flight simulation only,\nnot for real-world navigation.",
             lines, g_t.src[0] ? g_t.src : "-", g_live ? "data OK" : "no flight data");
    ui_set_text(s_sysinfo_label, buf);
}

static void sysinfo_show(lv_event_t *) {
    lv_obj_clear_flag(s_sysinfo, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_sysinfo);
    sysinfo_refresh();
}

// ---------------------------------------------------------------- top bar

static lv_obj_t *topcell(lv_obj_t *bar, lv_coord_t x, lv_coord_t w, lv_event_cb_t cb, void *user) {
    lv_obj_t *c = ui_box(bar, x, 0, w, TOPBAR_H);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x26303A), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(c, cb, LV_EVENT_SHORT_CLICKED, user);
    lv_obj_add_event_cb(c, cb, LV_EVENT_LONG_PRESSED, user);
    // Right-hand separator
    lv_obj_t *sep = ui_box(c, w - 1, 6, 1, TOPBAR_H - 12);
    lv_obj_set_style_bg_color(sep, COL_BORDER, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    return c;
}

// Tap: open the radio page on that field. Long press: swap active and standby.
static void radio_cell_cb(lv_event_t *e) {
    const char *radio = lv_event_get_user_data(e) ? "com1" : "nav1";
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        cmd_swap(radio);
        char m[32];
        snprintf(m, sizeof(m), "%s SWAP", radio);
        for (char *p = m; *p; p++) *p = (char)toupper(*p);
        ui_toast(m, COL_CYAN);
        return;
    }
    radio_preselect(radio);
    ui_show_page(PG_RADIO);
}

static void ap_cell_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_SHORT_CLICKED) ui_show_page(PG_AP);
}

static void xpdr_cell_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        sysinfo_show(e);
        return;
    }
    radio_preselect("xpdr");
    ui_show_page(PG_RADIO);
}

static void build_topbar(lv_obj_t *scr) {
    lv_obj_t *bar = ui_box(scr, 0, 0, 800, TOPBAR_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x0A0D10), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, COL_BORDER, 0);

    lv_obj_t *c = topcell(bar, 0, 224, radio_cell_cb, (void *)1);
    lv_obj_set_pos(ui_label(c, FONT_12, COL_DIM, "COM1"), 8, 1);
    s_com_act = ui_label(c, FONT_24, COL_TEXT, "---.---");
    lv_obj_set_pos(s_com_act, 8, 14);
    s_com_stby = ui_label(c, FONT_20, COL_CYAN, "---.---");
    lv_obj_set_pos(s_com_stby, 124, 17);

    c = topcell(bar, 224, 196, radio_cell_cb, nullptr);
    lv_obj_set_pos(ui_label(c, FONT_12, COL_DIM, "NAV1"), 8, 1);
    s_nav_act = ui_label(c, FONT_24, COL_TEXT, "---.--");
    lv_obj_set_pos(s_nav_act, 8, 14);
    s_nav_stby = ui_label(c, FONT_20, COL_CYAN, "---.--");
    lv_obj_set_pos(s_nav_stby, 108, 17);

    c = topcell(bar, 420, 222, ap_cell_cb, nullptr);
    s_ap_main = ui_label(c, FONT_16, COL_GREEN, "");
    lv_obj_set_pos(s_ap_main, 8, 4);
    s_ap_lat = ui_label(c, FONT_16, COL_GREEN, "");
    lv_obj_set_pos(s_ap_lat, 44, 4);
    s_ap_vert = ui_label(c, FONT_16, COL_GREEN, "");
    lv_obj_set_pos(s_ap_vert, 96, 4);
    s_ap_arm = ui_label(c, FONT_14, COL_TEXT, "");
    lv_obj_set_pos(s_ap_arm, 44, 25);

    c = topcell(bar, 642, 158, xpdr_cell_cb, nullptr);
    lv_obj_t *cap = ui_label(c, FONT_12, COL_DIM, "XPDR");
    lv_obj_set_pos(cap, 8, 1);
    s_xpdr_code = ui_label(c, FONT_24, COL_TEXT, "----");
    lv_obj_set_pos(s_xpdr_code, 8, 14);
    s_xpdr_mode = ui_label(c, FONT_14, COL_TEXT, "");
    lv_obj_set_pos(s_xpdr_mode, 74, 22);
    s_link = ui_label(c, FONT_12, COL_RED, "");
    lv_obj_set_style_text_align(s_link, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_link, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_link, 104);
    lv_obj_set_pos(s_link, 46, 1);
}

static void update_topbar() {
    const Radios &r = g_t.rad;
    bool have = g_live && r.com1_act[0];
    ui_set_text(s_com_act, have ? r.com1_act : "---.---");
    ui_set_text(s_com_stby, have ? r.com1_stby : "---.---");
    ui_set_text(s_nav_act, have ? r.nav1_act : "---.--");
    ui_set_text(s_nav_stby, have ? r.nav1_stby : "---.--");

    const Autopilot &a = g_t.ap;
    char buf[40];
    if (!g_live) {
        ui_set_text(s_ap_main, "");
        ui_set_text(s_ap_lat, "");
        ui_set_text(s_ap_vert, "");
        ui_set_text(s_ap_arm, "");
    } else if (!a.master && !a.fd) {
        ui_set_text(s_ap_main, "");
        ui_set_text(s_ap_lat, "AP OFF");
        ui_set_color(s_ap_lat, COL_DIM);
        ui_set_text(s_ap_vert, "");
        ui_set_text(s_ap_arm, a.yd ? "YD" : "");
    } else {
        ui_set_text(s_ap_main, a.master ? "AP" : "FD");
        ui_set_color(s_ap_main, a.master ? COL_GREEN : COL_TEXT);
        ui_set_text(s_ap_lat, a.lat);
        ui_set_color(s_ap_lat, COL_GREEN);
        if (!strcmp(a.vert, "ALT")) snprintf(buf, sizeof(buf), "ALT %.0fFT", a.alt);
        else if (!strcmp(a.vert, "VS")) snprintf(buf, sizeof(buf), "VS %+.0fFPM", a.vs);
        else if (!strcmp(a.vert, "IAS") || !strcmp(a.vert, "FLC")) snprintf(buf, sizeof(buf), "IAS %.0fKT", a.ias);
        else snprintf(buf, sizeof(buf), "%s", a.vert);
        ui_set_text(s_ap_vert, buf);
        snprintf(buf, sizeof(buf), "%s%s%s%s", a.lat_arm, a.lat_arm[0] ? "     " : "", a.vert_arm,
                 a.yd ? "     YD" : "");
        ui_set_text(s_ap_arm, buf);
    }

    ui_set_text(s_xpdr_code, have ? r.xpdr : "----");
    ui_set_text(s_xpdr_mode, have ? r.xpdr_mode : "");
    bool xon = have && (!strcmp(r.xpdr_mode, "ALT") || !strcmp(r.xpdr_mode, "ON") || !strcmp(r.xpdr_mode, "GND"));
    ui_set_color(s_xpdr_code, xon ? COL_GREEN : COL_TEXT);
    ui_set_color(s_xpdr_mode, xon ? COL_GREEN : COL_TEXT);

    char link[24];
    bool link_ok = false;
    if (g_platform.link_label) g_platform.link_label(link, sizeof(link), &link_ok);
    else strcpy(link, "");
    if (link_ok && g_live) snprintf(buf, sizeof(buf), "%s %s", g_t.src, link);
    else if (link_ok) snprintf(buf, sizeof(buf), "NO SIM %s", link);
    else snprintf(buf, sizeof(buf), "%s", link);
    ui_set_text(s_link, buf);
    ui_set_color(s_link, link_ok && g_live ? COL_GREEN : link_ok ? COL_YELLOW : COL_RED);
}

// ---------------------------------------------------------------- init / tick

void ui_init(const UiPlatform &platform) {
    g_platform = platform;
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_topbar(scr);
    s_main = ui_box(scr, 0, TOPBAR_H, 800, MAIN_H);

    lv_obj_t *keys = ui_box(scr, 0, 480 - SOFTKEY_H, 800, SOFTKEY_H);
    lv_obj_set_style_bg_color(keys, lv_color_hex(0x0A0D10), 0);
    lv_obj_set_style_bg_opa(keys, LV_OPA_COVER, 0);
    static const char *names[PG_COUNT + 1] = {"PFD", "EIS", "RADIO", "MAP", "NRST", "WX", "AP", LV_SYMBOL_SETTINGS, "SPLIT"};
    static const lv_coord_t widths[PG_COUNT + 1] = {76, 76, 76, 76, 76, 76, 76, 64, 76};
    // Left to right, grouped by task as on Garmin units: flying and navigation (PFD, MAP,
    // NRST, WX), settings (RADIO, AP), systems (EIS); then D->, SPLIT and settings.
    static const int order[PG_COUNT + 1] = {PG_PFD, PG_MAP, PG_NRST, PG_WX, PG_RADIO, PG_AP, PG_EIS, PG_COUNT, PG_SETUP};
    lv_coord_t x = 4;
    for (int k = 0; k <= PG_COUNT; k++) {
        int i = order[k];
        if (i == PG_COUNT) {
            s_dct_key = ui_button(keys, "", FONT_18, 80, 44, [](lv_event_t *) { dct_window_toggle(); }, nullptr);
            lv_obj_add_event_cb(s_dct_key, dct_key_draw, LV_EVENT_DRAW_POST, nullptr);
            lv_obj_set_pos(s_dct_key, x, 5);
            x += 80 + 4;
        }
        s_keys[i] = ui_button(keys, names[i], FONT_18, widths[i], 44, softkey_cb, (void *)(intptr_t)i);
        lv_obj_set_pos(s_keys[i], x, 5);
        x += widths[i] + 4;
    }

    s_toast = ui_label(scr, FONT_20, COL_CYAN, "");
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x0A0D10), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_toast, 2, 0);
    lv_obj_set_style_radius(s_toast, 6, 0);
    lv_obj_set_style_pad_hor(s_toast, 16, 0);
    lv_obj_set_style_pad_ver(s_toast, 8, 0);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -SOFTKEY_H - 12);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    s_sysinfo = ui_box(scr, 150, 70, 500, 330);
    lv_obj_set_style_bg_color(s_sysinfo, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(s_sysinfo, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_sysinfo, COL_CYAN, 0);
    lv_obj_set_style_border_width(s_sysinfo, 2, 0);
    lv_obj_set_style_radius(s_sysinfo, 8, 0);
    lv_obj_add_flag(s_sysinfo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_sysinfo, [](lv_event_t *e) { lv_obj_add_flag(lv_event_get_target(e), LV_OBJ_FLAG_HIDDEN); },
                        LV_EVENT_CLICKED, nullptr);
    lv_obj_t *title = ui_label(s_sysinfo, FONT_20, COL_CYAN, "SYSTEM STATUS");
    lv_obj_set_pos(title, 16, 12);
    s_sysinfo_label = ui_label(s_sysinfo, FONT_16, COL_TEXT, "");
    lv_obj_set_pos(s_sysinfo_label, 16, 46);
    lv_obj_t *hint = ui_label(s_sysinfo, FONT_14, COL_DIM, "Tap to close");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_RIGHT, -12, -10);
    lv_obj_add_flag(s_sysinfo, LV_OBJ_FLAG_HIDDEN);

    dct_window_build(scr, s_dct_key);
    build_ota_overlay(scr);
    if (g_platform.load_sleep_min) s_sleep_min = g_platform.load_sleep_min();
    build_sleep_overlay(scr);
    if (g_platform.get_settings) {
        NetSettings ns;
        g_platform.get_settings(ns);
        if (!ns.ssid[0]) s_page = PG_SETUP;     // first run: WiFi setup
    }
    build_panes();
}

// ---------------------------------------------------------------- firmware update over WiFi

bool ui_update_available() {
    // Offered whenever a bridge is talking to us, with or without a simulator running.
    bool bridge = g_t.any_rx_ms && g_now - g_t.any_rx_ms < 5000;
    return bridge && g_t.fwu_ver[0] && g_t.fwu_port && g_platform.fw_version && g_platform.ota_start &&
           version_cmp(g_t.fwu_ver, g_platform.fw_version) > 0;
}

void ui_start_update() {
    if (!ui_update_available()) return;
    g_platform.ota_start(g_t.fwu_port, g_t.fwu_path);
}

static void build_ota_overlay(lv_obj_t *scr) {
    s_ota = ui_box(scr, 0, 0, 800, 480);
    lv_obj_set_style_bg_color(s_ota, lv_color_hex(0x05080B), 0);
    lv_obj_set_style_bg_opa(s_ota, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_ota, LV_OBJ_FLAG_CLICKABLE);          // blocks taps on the pages below
    s_ota_title = ui_label(s_ota, FONT_28, COL_CYAN, "UPDATING FIRMWARE");
    lv_obj_align(s_ota_title, LV_ALIGN_CENTER, 0, -80);
    lv_obj_t *frame = ui_box(s_ota, 146, 216, 508, 36);
    lv_obj_set_style_border_color(frame, COL_BORDER, 0);
    lv_obj_set_style_border_width(frame, 2, 0);
    lv_obj_set_style_radius(frame, 6, 0);
    s_ota_bar = ui_box(s_ota, 150, 220, 0, 28);
    lv_obj_set_style_bg_color(s_ota_bar, COL_CYAN, 0);
    lv_obj_set_style_bg_opa(s_ota_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_ota_bar, 4, 0);
    s_ota_msg = ui_label(s_ota, FONT_18, COL_TEXT, "");
    lv_obj_set_style_text_align(s_ota_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_ota_msg, LV_ALIGN_CENTER, 0, 60);
    s_ota_close = ui_button(s_ota, "CLOSE", FONT_18, 160, 50,
                            [](lv_event_t *) { lv_obj_add_flag(s_ota, LV_OBJ_FLAG_HIDDEN); }, nullptr);
    lv_obj_align(s_ota_close, LV_ALIGN_CENTER, 0, 140);
    lv_obj_add_flag(s_ota, LV_OBJ_FLAG_HIDDEN);
}

static void update_ota() {
    if (!g_platform.ota_status) return;
    int state, pct;
    char msg[64];
    g_platform.ota_status(&state, &pct, msg, sizeof(msg));
    if (state == 0) return;
    if (state != s_ota_last) {          // show the overlay on every state change
        lv_obj_clear_flag(s_ota, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_ota);
        s_ota_last = state;
    }
    lv_obj_set_width(s_ota_bar, 500 * LV_CLAMP(0, pct, 100) / 100);
    char t[112];
    if (state == 1) {
        ui_set_text(s_ota_title, "UPDATING FIRMWARE");
        ui_set_color(s_ota_title, COL_CYAN);
        snprintf(t, sizeof(t), "%d%%\nKeep the display powered. It restarts when done.", pct);
        lv_obj_add_flag(s_ota_close, LV_OBJ_FLAG_HIDDEN);
    } else if (state == 3) {
        ui_set_text(s_ota_title, "UPDATE COMPLETE");
        ui_set_color(s_ota_title, COL_GREEN);
        snprintf(t, sizeof(t), "Restarting ...");
        lv_obj_add_flag(s_ota_close, LV_OBJ_FLAG_HIDDEN);
    } else {
        ui_set_text(s_ota_title, "UPDATE FAILED");
        ui_set_color(s_ota_title, COL_RED);
        snprintf(t, sizeof(t), "%s\nThe current firmware is still installed.", msg[0] ? msg : "Unknown error");
        lv_obj_clear_flag(s_ota_close, LV_OBJ_FLAG_HIDDEN);
    }
    ui_set_text(s_ota_msg, t);
    lv_obj_align(s_ota_msg, LV_ALIGN_CENTER, 0, 60);
}

const char *ui_page_key() { return PAGES[s_page]->key; }

// ---------------------------------------------------------------- screen sleep

int ui_sleep_min() { return s_sleep_min; }

void ui_set_sleep_min(int minutes) {
    s_sleep_min = minutes;
    if (g_platform.save_sleep_min) g_platform.save_sleep_min(minutes);
}

static void sleep_apply(int st) {
    if (st == s_sleep_state) return;
    if (g_platform.backlight && (st == ASLEEP) != (s_sleep_state == ASLEEP)) g_platform.backlight(st != ASLEEP);
    s_sleep_state = st;
    if (st == AWAKE) {
        if (!s_sleep_swallow) lv_obj_add_flag(s_sleep, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(s_sleep, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(s_sleep_msg, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_set_style_bg_opa(s_sleep, st == DIMMED ? LV_OPA_60 : LV_OPA_COVER, 0);
    if (st == DIMMED) lv_obj_clear_flag(s_sleep_msg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_sleep, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_sleep);
}

static void sleep_touch_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_sleep_swallow = true;
        sleep_apply(AWAKE);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_sleep_swallow = false;
        if (s_sleep_state == AWAKE) lv_obj_add_flag(s_sleep, LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_sleep_overlay(lv_obj_t *scr) {
    s_sleep = ui_box(scr, 0, 0, 800, 480);
    lv_obj_set_style_bg_color(s_sleep, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_sleep, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_sleep, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_sleep, sleep_touch_cb, LV_EVENT_ALL, nullptr);
    s_sleep_msg = ui_label(s_sleep, FONT_20, COL_TEXT, "NO FLIGHT DATA - SCREEN OFF SOON\nTOUCH TO KEEP IT ON");
    lv_obj_set_style_text_align(s_sleep_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_sleep_msg, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_sleep_msg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_sleep, LV_OBJ_FLAG_HIDDEN);
}

static void update_sleep() {
    if (g_live) s_last_live = g_now;
    uint32_t idle = LV_MIN(lv_disp_get_inactive_time(nullptr), g_now - s_last_live);
    int ota = 0;
    if (g_platform.ota_status) {
        int pct;
        char msg[4];
        g_platform.ota_status(&ota, &pct, msg, sizeof(msg));
    }
    uint32_t limit = (uint32_t)s_sleep_min * 60000u;
    int want = AWAKE;
    if (s_sleep_min > 0 && ota != 1 && !s_sleep_swallow && idle >= limit - DIM_MS)
        want = idle >= limit ? ASLEEP : DIMMED;
    sleep_apply(want);
}

void ui_tick(uint32_t now_ms) {
    g_now = now_ms;
    telemetry_snapshot(g_t);
    g_live = telemetry_live(g_t, now_ms);
    // Direct-To guidance (10 Hz) replaces the simulator's waypoint for every page.
    dct_update(g_t, g_live, now_ms);
    dct_apply(g_t.nav);
    update_sleep();

    static uint32_t last_bar = 0;
    if (now_ms - last_bar >= 100) {
        update_topbar();
        last_bar = now_ms;
        if (!lv_obj_has_flag(s_sysinfo, LV_OBJ_FLAG_HIDDEN)) sysinfo_refresh();
        update_ota();
        // Announce a newer firmware once per version.
        static char announced[20] = "";
        if (ui_update_available() && strcmp(announced, g_t.fwu_ver)) {
            strncpy(announced, g_t.fwu_ver, sizeof(announced) - 1);
            char m[64];
            snprintf(m, sizeof(m), "FIRMWARE %s AVAILABLE - SEE SETTINGS", g_t.fwu_ver);
            ui_toast(m, COL_GREEN);
        }
    }
    for (int i = 0; i < 2; i++)
        if (s_shown[i] && s_shown[i]->update) s_shown[i]->update();
    dct_ui_tick();
    if (s_toast_until && (int32_t)(now_ms - s_toast_until) > 0) {
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        s_toast_until = 0;
    }
}
