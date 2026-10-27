// SETTINGS
// WiFi network (scan list or typed name), password, optional bridge PC address, and the
// live connection state. Settings are saved in the display's flash; nothing needs to be
// recompiled. Opens by itself on first start when no WiFi is configured.
#include <stdio.h>
#include <string.h>

#include <initializer_list>

#include "ui_priv.h"

#define MAX_NETS 24

static lv_obj_t *s_root, *s_list, *s_scan_btn, *s_scan_lbl;
static lv_obj_t *s_ta_ssid, *s_ta_pass, *s_ta_bridge, *s_show_btn;
static lv_obj_t *s_kb;
static lv_obj_t *s_st_wifi, *s_st_bridge, *s_st_fw, *s_fw_btn;
static const int SLEEP_CHOICES[] = {0, 5, 10, 30};
static lv_obj_t *s_sleep_btn[4];
static NetSettings s_saved;
static WifiNet s_nets[MAX_NETS];
static bool s_scanning;
static uint32_t s_status_ms;

// ---------------------------------------------------------------- styling (no LVGL theme is used)

static void style_textarea(lv_obj_t *ta) {
    lv_obj_set_style_bg_color(ta, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ta, COL_BORDER, 0);
    lv_obj_set_style_border_color(ta, COL_CYAN, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(ta, 2, 0);
    lv_obj_set_style_radius(ta, 5, 0);
    lv_obj_set_style_pad_all(ta, 8, 0);
    lv_obj_set_style_text_color(ta, COL_TEXT, 0);
    lv_obj_set_style_text_font(ta, FONT_20, 0);
    lv_obj_set_style_text_color(ta, COL_DIM, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_border_side(ta, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
    lv_obj_set_style_border_width(ta, 2, LV_PART_CURSOR);
    lv_obj_set_style_border_color(ta, COL_CYAN, LV_PART_CURSOR);
    lv_obj_set_style_anim_time(ta, 500, LV_PART_CURSOR);
    lv_obj_set_style_opa(ta, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_set_style_opa(ta, LV_OPA_COVER, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_clear_flag(ta, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
}

static void style_keyboard(lv_obj_t *kb) {
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x0A0D10), 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(kb, COL_CYAN, 0);
    lv_obj_set_style_border_side(kb, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(kb, 2, 0);
    lv_obj_set_style_pad_all(kb, 4, 0);
    lv_obj_set_style_pad_gap(kb, 4, 0);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x2A3138), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x56626E), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x1A2A36), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, COL_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, COL_CYAN, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_font(kb, FONT_20, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 5, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_color(kb, COL_BORDER, LV_PART_ITEMS);
}

// ---------------------------------------------------------------- keyboard

static void hide_kb() {
    if (!s_kb) return;
    lv_keyboard_set_textarea(s_kb, nullptr);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    for (lv_obj_t *ta : {s_ta_ssid, s_ta_pass, s_ta_bridge}) lv_obj_clear_state(ta, LV_STATE_FOCUSED);
}

static void ta_cb(lv_event_t *e) {
    lv_obj_t *ta = lv_event_get_target(e);
    for (lv_obj_t *o : {s_ta_ssid, s_ta_pass, s_ta_bridge})
        if (o != ta) lv_obj_clear_state(o, LV_STATE_FOCUSED);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    lv_keyboard_set_mode(s_kb, ta == s_ta_bridge ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_kb, ta);
    lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_kb);
}

static void kb_cb(lv_event_t *e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY || c == LV_EVENT_CANCEL) hide_kb();
}

// ---------------------------------------------------------------- network list

static void net_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    lv_textarea_set_text(s_ta_ssid, s_nets[i].ssid);
    lv_textarea_set_text(s_ta_pass, "");
    lv_textarea_set_placeholder_text(s_ta_pass, s_nets[i].secure ? "enter password" : "(open network)");
    // Go straight to the password.
    lv_event_send(s_ta_pass, LV_EVENT_CLICKED, nullptr);
}

static const char *strength(int rssi) {
    return rssi > -55 ? "strong" : rssi > -67 ? "good" : rssi > -78 ? "fair" : "weak";
}

static void fill_list(int n) {
    lv_obj_clean(s_list);
    if (n == 0) {
        lv_obj_t *l = ui_label(s_list, FONT_16, COL_DIM, "No networks found");
        lv_obj_set_pos(l, 10, 10);
        return;
    }
    lv_coord_t row_w = lv_obj_get_width(s_list) - 10;
    for (int i = 0; i < n; i++) {
        lv_obj_t *row = ui_box(s_list, 0, i * 48, row_w, 44);
        lv_obj_set_style_bg_color(row, i % 2 ? COL_PANEL : COL_PANEL2, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x30404E), LV_STATE_PRESSED);
        lv_obj_set_style_radius(row, 5, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_CHAIN);
        lv_obj_add_event_cb(row, net_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *name = ui_label(row, FONT_18, COL_TEXT, s_nets[i].ssid);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_size(name, row_w - 110, lv_font_get_line_height(FONT_18));
        if (!strcmp(s_nets[i].ssid, s_saved.ssid)) ui_set_color(name, COL_CYAN);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 10, 0);
        char t[24];
        snprintf(t, sizeof(t), LV_SYMBOL_WIFI " %s%s", strength(s_nets[i].rssi), s_nets[i].secure ? "" : " open");
        lv_obj_t *sig = ui_label(row, FONT_12, s_nets[i].rssi > -67 ? COL_GREEN : COL_YELLOW, t);
        lv_obj_align(sig, LV_ALIGN_RIGHT_MID, -8, 0);
    }
}

static void scan_cb(lv_event_t *) {
    if (!g_platform.scan_start || s_scanning) return;
    g_platform.scan_start();
    s_scanning = true;
    ui_set_text(s_scan_lbl, "SCANNING...");
}

// ---------------------------------------------------------------- save

static void show_cb(lv_event_t *) {
    bool pw = !lv_textarea_get_password_mode(s_ta_pass);
    lv_textarea_set_password_mode(s_ta_pass, pw);
    ui_set_text(ui_button_label(s_show_btn), pw ? "SHOW" : "HIDE");
}

static void save_cb(lv_event_t *) {
    NetSettings ns;
    memset(&ns, 0, sizeof(ns));
    strncpy(ns.ssid, lv_textarea_get_text(s_ta_ssid), sizeof(ns.ssid) - 1);
    const char *pw = lv_textarea_get_text(s_ta_pass);
    // An empty password field keeps the saved password for the same network.
    if (!pw[0] && !strcmp(ns.ssid, s_saved.ssid)) strncpy(ns.pass, s_saved.pass, sizeof(ns.pass) - 1);
    else strncpy(ns.pass, pw, sizeof(ns.pass) - 1);
    strncpy(ns.bridge, lv_textarea_get_text(s_ta_bridge), sizeof(ns.bridge) - 1);
    hide_kb();
    if (!ns.ssid[0]) {
        ui_toast("ENTER OR PICK A WIFI NETWORK", COL_YELLOW);
        return;
    }
    if (g_platform.save_settings) g_platform.save_settings(ns);
    s_saved = ns;
    lv_textarea_set_text(s_ta_pass, "");
    lv_textarea_set_placeholder_text(s_ta_pass, ns.pass[0] ? "(saved)" : "(none)");
    char msg[64];
    snprintf(msg, sizeof(msg), "SAVED - CONNECTING TO %s", ns.ssid);
    ui_toast(msg, COL_CYAN);
}

// ---------------------------------------------------------------- page

static lv_obj_t *field(lv_obj_t *parent, const char *label, lv_coord_t y, lv_coord_t w, const char *placeholder) {
    lv_obj_t *l = ui_label(parent, FONT_14, COL_DIM, label);
    lv_obj_set_pos(l, 0, y + 14);
    lv_obj_t *ta = lv_textarea_create(parent);
    style_textarea(ta);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_obj_set_pos(ta, 92, y);
    lv_obj_set_size(ta, w - 92, 46);
    lv_obj_add_event_cb(ta, ta_cb, LV_EVENT_CLICKED, nullptr);
    return ta;
}

// ---------------------------------------------------------------- screen sleep

static void sleep_looks() {
    for (int i = 0; i < 4; i++)
        ui_button_look(s_sleep_btn[i], SLEEP_CHOICES[i] == ui_sleep_min() ? BTN_SELECTED : BTN_NORMAL);
}

static void sleep_cb(lv_event_t *e) {
    int m = SLEEP_CHOICES[(intptr_t)lv_event_get_user_data(e)];
    if (m == ui_sleep_min()) return;
    ui_set_sleep_min(m);
    sleep_looks();
    char t[64];
    if (m) snprintf(t, sizeof(t), "SCREEN OFF AFTER %d MIN WITHOUT FLIGHT DATA", m);
    else snprintf(t, sizeof(t), "SCREEN STAYS ON");
    ui_toast(t, COL_CYAN);
}

static void setup_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h) {
    s_root = parent;
    memset(&s_saved, 0, sizeof(s_saved));
    if (g_platform.get_settings) g_platform.get_settings(s_saved);

    // Left: networks
    lv_coord_t lw = 310;
    lv_obj_t *t = ui_label(parent, FONT_16, COL_CYAN, "WIFI NETWORKS");
    lv_obj_set_pos(t, 10, 14);
    s_scan_btn = ui_button(parent, "SCAN", FONT_16, 130, 38, scan_cb, nullptr);
    lv_obj_set_pos(s_scan_btn, lw - 130, 4);
    s_scan_lbl = ui_button_label(s_scan_btn);
    s_list = ui_box(parent, 6, 50, lw, h - 108);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_style_bg_color(s_list, COL_CYAN, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_list, 5, LV_PART_SCROLLBAR);

    // Bottom left: screen sleep (backlight off after this long without flight data)
    lv_obj_t *sl = ui_label(parent, FONT_14, COL_CYAN, "SCREEN\nSLEEP");
    lv_obj_set_pos(sl, 10, h - 48);
    static const char *labels[] = {"OFF", "5 MIN", "10 MIN", "30 MIN"};
    for (int i = 0; i < 4; i++) {
        s_sleep_btn[i] = ui_button(parent, labels[i], FONT_14, 58, 44, sleep_cb, (void *)(intptr_t)i);
        lv_obj_set_pos(s_sleep_btn[i], 72 + i * 62, h - 52);
    }
    sleep_looks();

    // Right: fields, buttons, status
    lv_coord_t rx = lw + 24, rw = w - rx - 10;
    lv_obj_t *right = ui_box(parent, rx, 0, rw, h);
    s_ta_ssid = field(right, "NETWORK", 6, rw, "pick from the list or type");
    lv_textarea_set_text(s_ta_ssid, s_saved.ssid);
    s_ta_pass = field(right, "PASSWORD", 60, rw - 86, s_saved.pass[0] ? "(saved)" : "enter password");
    lv_textarea_set_password_mode(s_ta_pass, true);
    s_show_btn = ui_button(right, "SHOW", FONT_14, 78, 46, show_cb, nullptr);
    lv_obj_set_pos(s_show_btn, rw - 78, 60);
    s_ta_bridge = field(right, "BRIDGE PC", 114, rw, "automatic (leave empty)");
    lv_textarea_set_text(s_ta_bridge, s_saved.bridge);
    lv_textarea_set_accepted_chars(s_ta_bridge, "0123456789.:abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-");

    lv_obj_t *save = ui_button(right, "SAVE & CONNECT", FONT_20, rw, 52, save_cb, nullptr);
    lv_obj_set_pos(save, 0, 176);
    ui_button_look(save, BTN_SELECTED);

    lv_obj_t *box = ui_box(right, 0, 240, rw, h - 246);
    lv_obj_set_style_bg_color(box, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 6, 0);
    s_st_wifi = ui_label(box, FONT_16, COL_TEXT, "");
    lv_obj_set_pos(s_st_wifi, 12, 12);
    s_st_bridge = ui_label(box, FONT_16, COL_TEXT, "");
    lv_obj_set_pos(s_st_bridge, 12, 40);
    s_st_fw = ui_label(box, FONT_16, COL_DIM, "");
    lv_obj_set_pos(s_st_fw, 12, 68);
    s_fw_btn = ui_button(box, "INSTALL", FONT_16, 150, 38, [](lv_event_t *) { ui_start_update(); }, nullptr);
    lv_obj_align(s_fw_btn, LV_ALIGN_TOP_RIGHT, -8, 58);
    ui_button_look(s_fw_btn, BTN_ACTIVE);
    lv_obj_add_flag(s_fw_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *hint = ui_label(box, FONT_12, COL_DIM,
                              "The bridge PC is found automatically. Enter its address only\n"
                              "if your network blocks broadcasts (guest WiFi).");
    lv_obj_set_pos(hint, 12, 102);

    // On-screen keyboard over the lower part of the page
    s_kb = lv_keyboard_create(parent);
    style_keyboard(s_kb);
    lv_obj_set_size(s_kb, w, 196);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(s_kb, kb_cb, LV_EVENT_ALL, nullptr);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);

    s_scanning = false;
    s_status_ms = 0;
    scan_cb(nullptr);
}

static void setup_update() {
    if (!s_root) return;
    if (s_scanning && g_platform.scan_results) {
        int n = g_platform.scan_results(s_nets, MAX_NETS);
        if (n >= 0) {
            s_scanning = false;
            ui_set_text(s_scan_lbl, LV_SYMBOL_REFRESH " SCAN");
            fill_list(n);
        }
    }
    if (g_now - s_status_ms >= 500 && g_platform.net_status) {
        s_status_ms = g_now;
        char a[64], b[64];
        bool wok = false, bok = false;
        g_platform.net_status(a, sizeof(a), b, sizeof(b), &wok, &bok);
        ui_set_text(s_st_wifi, a);
        ui_set_color(s_st_wifi, wok ? COL_GREEN : COL_YELLOW);
        ui_set_text(s_st_bridge, b);
        ui_set_color(s_st_bridge, bok ? COL_GREEN : COL_YELLOW);
        char f[48];
        snprintf(f, sizeof(f), "Firmware %s", g_platform.fw_version ? g_platform.fw_version : "?");
        ui_set_text(s_st_fw, f);
        if (ui_update_available()) {
            const char *v = g_t.fwu_ver;
            if (!strncmp(v, "mfd7 ", 5)) v += 5;
            snprintf(f, sizeof(f), "INSTALL %s", v);
            ui_set_text(ui_button_label(s_fw_btn), f);
            lv_obj_clear_flag(s_fw_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_fw_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void setup_destroy() {
    s_root = nullptr;
    s_kb = nullptr;
}

const PageDef PAGE_SETUP = {"SETUP", setup_create, setup_update, setup_destroy};
