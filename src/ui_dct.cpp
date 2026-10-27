// DIRECT-TO window over the page area, opened and closed with the D-> softkey.
// Type an airport ident on the on-screen keyboard (or tap one of the nearest airports).
// Every keystroke asks the bridge's airport database (cmd_apt_find); the best match
// fills in the rest of the ident, Garmin style, with its name, city, bearing and
// distance. ACTIVATE starts a Direct-To course from the present position (nav_dct).
#include <stdio.h>
#include <string.h>

#include "nav_dct.h"
#include "ui_priv.h"

#define WIN_Y   TOPBAR_H
#define WIN_H   MAIN_H
#define ENTRY_MAX 6
#define RETRY_MS 1500
#define N_NRST_KEYS 5

static lv_obj_t *s_win, *s_typed, *s_compl, *s_cursor;
static lv_obj_t *s_name, *s_city, *s_brgdis;
static lv_obj_t *s_activate, *s_cancel_dct;
static lv_obj_t *s_nrst_btn[N_NRST_KEYS];
static char s_nrst_id[N_NRST_KEYS][8];
static lv_obj_t *s_softkey;                  // the D-> softkey (shell), shown selected while open

static char s_entry[ENTRY_MAX + 1];
static AptHit s_hit;
static bool s_has_hit, s_answered;
static uint32_t s_sent_ms, s_seen_seq, s_nrst_seq;
static int s_retries;

// ---------------------------------------------------------------- D-> symbol

void dr_dct_icon(lv_draw_ctx_t *dc, lv_coord_t cx, lv_coord_t cy, lv_color_t c) {
    // A "D" with an arrow through it, pointing right.
    dr_text(dc, cx - 2, cy, "D", FONT_24, c, TA_CENTER);
    dr_line(dc, cx - 22, cy, cx + 14, cy, 3, c);
    lv_point_t head[3] = {{(lv_coord_t)(cx + 22), cy}, {(lv_coord_t)(cx + 11), (lv_coord_t)(cy - 7)},
                          {(lv_coord_t)(cx + 11), (lv_coord_t)(cy + 7)}};
    dr_poly(dc, head, 3, c);
}

// ---------------------------------------------------------------- state

static void query() {
    s_has_hit = s_answered = false;
    s_retries = 0;
    if (s_entry[0]) {
        cmd_apt_find(s_entry);
        s_sent_ms = g_now;
    }
}

static bool hit_completes_entry() { return s_has_hit && !strncmp(s_hit.id, s_entry, strlen(s_entry)); }

static void refresh() {
    if (!s_win) return;
    char t[64];
    // Ident: typed part in cyan, the database's completion dimmed, cursor under the next character.
    ui_set_text(s_typed, s_entry);
    const char *rest = hit_completes_entry() ? s_hit.id + strlen(s_entry) : "";
    ui_set_text(s_compl, rest);
    lv_coord_t x = 16 + text_width(s_entry, FONT_40);
    lv_obj_set_x(s_compl, x);
    lv_obj_set_x(s_cursor, x + 1);
    char next[2] = {rest[0], 0};
    lv_obj_set_width(s_cursor, next[0] ? LV_MAX(10, text_width(next, FONT_40) - 2) : 24);

    lv_color_t name_col = COL_TEXT;
    t[0] = 0;
    if (!s_entry[0]) {
        ui_set_text(s_name, "ENTER AN AIRPORT IDENT");
        ui_set_text(s_city, "e.g. KABE  KPHL  KJFK");
        ui_set_text(s_brgdis, "");
        name_col = COL_DIM;
    } else if (!s_answered) {
        ui_set_text(s_name, s_retries ? "NO ANSWER FROM MFD BRIDGE" : "SEARCHING ...");
        ui_set_text(s_city, s_retries ? "The airport database is on the sim PC" : "");
        ui_set_text(s_brgdis, "");
        name_col = s_retries ? COL_YELLOW : COL_DIM;
    } else if (!s_has_hit) {
        ui_set_text(s_name, "NO MATCH");
        ui_set_text(s_city, "Check the ident");
        ui_set_text(s_brgdis, "");
        name_col = COL_YELLOW;
    } else {
        ui_set_text(s_name, s_hit.name);
        if (!hit_completes_entry()) snprintf(t, sizeof(t), "%s = %s   ELEV %dFT", s_entry, s_hit.id, (int)s_hit.elev);
        else if (s_hit.city[0]) snprintf(t, sizeof(t), "%s   ELEV %dFT", s_hit.city, (int)s_hit.elev);
        else snprintf(t, sizeof(t), "ELEV %dFT", (int)s_hit.elev);
        ui_set_text(s_city, t);
        if (g_live) {
            double d, b;
            gc_dist_brg(g_t.nav.lat, g_t.nav.lon, s_hit.lat, s_hit.lon, d, b);
            snprintf(t, sizeof(t), "BRG %03d\xC2\xB0   DIS %.1fNM", (int)lroundf(wrap360((float)b - g_t.nav.magvar)) % 360, d);
        } else {
            snprintf(t, sizeof(t), "NO GPS POSITION");
        }
        ui_set_text(s_brgdis, t);
    }
    ui_set_color(s_name, name_col);
    ui_set_color(s_brgdis, g_live ? COL_CYAN : COL_YELLOW);
    ui_button_look(s_activate, s_has_hit && g_live ? BTN_ACTIVE : BTN_DISABLED);
    ui_button_look(s_cancel_dct, dct().active ? BTN_NORMAL : BTN_DISABLED);
}

static void refresh_nearest() {
    for (int i = 0; i < N_NRST_KEYS; i++) {
        lv_obj_t *b = s_nrst_btn[i];
        if (i < g_t.n_nrst && g_live) {
            const NrstApt &a = g_t.nrst[i];
            char t[32];
            snprintf(t, sizeof(t), "%-5s  %5.1fNM", a.icao, a.dist);
            ui_set_text(ui_button_label(b), t);
            strncpy(s_nrst_id[i], a.icao, sizeof(s_nrst_id[i]) - 1);
            s_nrst_id[i][sizeof(s_nrst_id[i]) - 1] = 0;
            lv_obj_clear_flag(b, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ---------------------------------------------------------------- open / close

bool dct_window_visible() { return s_win && !lv_obj_has_flag(s_win, LV_OBJ_FLAG_HIDDEN); }

void dct_window_close() {
    if (!s_win) return;
    lv_obj_add_flag(s_win, LV_OBJ_FLAG_HIDDEN);
    if (s_softkey) ui_button_look(s_softkey, BTN_NORMAL);
}

void dct_window_open() {
    if (!s_win) return;
    const DctNav &D = dct();
    if (D.active) {
        // Re-selecting the active waypoint and pressing ACTIVATE restarts the course here.
        strncpy(s_entry, D.wpt.id, ENTRY_MAX);
        s_entry[ENTRY_MAX] = 0;
        memset(&s_hit, 0, sizeof(s_hit));
        strncpy(s_hit.id, D.wpt.id, sizeof(s_hit.id) - 1);
        strncpy(s_hit.name, D.wpt.name, sizeof(s_hit.name) - 1);
        s_hit.lat = D.wpt.lat;
        s_hit.lon = D.wpt.lon;
        s_has_hit = s_answered = true;
        query();                                // refresh city / elevation; keeps working without an answer
        s_has_hit = s_answered = true;
    } else {
        s_entry[0] = 0;
        s_has_hit = s_answered = false;
    }
    s_seen_seq = g_t.apt.seq;
    refresh_nearest();
    s_nrst_seq = g_t.seq_slow;
    refresh();
    lv_obj_clear_flag(s_win, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_win);
    if (s_softkey) ui_button_look(s_softkey, BTN_SELECTED);
}

void dct_window_toggle() {
    if (dct_window_visible()) dct_window_close();
    else dct_window_open();
}

// ---------------------------------------------------------------- input

static void key_cb(lv_event_t *e) {
    intptr_t k = (intptr_t)lv_event_get_user_data(e);
    size_t len = strlen(s_entry);
    if (k == 1) {                               // backspace
        if (!len) return;
        s_entry[len - 1] = 0;
    } else if (k == 2) {                        // clear
        if (!len) return;
        s_entry[0] = 0;
    } else {
        if (len >= ENTRY_MAX) return;
        s_entry[len] = (char)k;
        s_entry[len + 1] = 0;
    }
    query();
    refresh();
}

static void nrst_cb(lv_event_t *e) {
    intptr_t i = (intptr_t)lv_event_get_user_data(e);
    strncpy(s_entry, s_nrst_id[i], ENTRY_MAX);
    s_entry[ENTRY_MAX] = 0;
    query();
    refresh();
}

static void activate_cb(lv_event_t *) {
    if (!s_has_hit) {
        ui_toast(s_entry[0] ? "NO MATCHING AIRPORT" : "ENTER AN AIRPORT IDENT", COL_YELLOW);
        return;
    }
    if (!g_live) {
        ui_toast("NO GPS POSITION", COL_YELLOW);
        return;
    }
    DctTarget t;
    memset(&t, 0, sizeof(t));
    strncpy(t.id, s_hit.id, sizeof(t.id) - 1);
    strncpy(t.name, s_hit.name, sizeof(t.name) - 1);
    t.lat = s_hit.lat;
    t.lon = s_hit.lon;
    dct_activate(t, g_t.nav.lat, g_t.nav.lon, g_now);
    char m[40];
    snprintf(m, sizeof(m), "DIRECT TO %s", t.id);
    ui_toast(m, COL_MAGENTA);
    dct_window_close();
}

static void cancel_dct_cb(lv_event_t *) {
    if (!dct().active) return;
    dct_cancel();
    ui_toast("DIRECT-TO CANCELLED", COL_CYAN);
    dct_window_close();
}

// ---------------------------------------------------------------- build

void dct_window_build(lv_obj_t *scr, lv_obj_t *softkey) {
    s_softkey = softkey;
    s_win = ui_box(scr, 0, WIN_Y, 800, WIN_H);
    lv_obj_set_style_bg_color(s_win, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(s_win, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(s_win, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(s_win, 2, 0);
    lv_obj_set_style_border_color(s_win, COL_MAGENTA, 0);
    lv_obj_add_flag(s_win, LV_OBJ_FLAG_CLICKABLE);      // swallow taps on the gaps

    lv_obj_t *title = ui_label(s_win, FONT_20, COL_MAGENTA, "DIRECT TO");
    lv_obj_set_pos(title, 14, 8);

    // Ident field
    lv_obj_t *box = ui_box(s_win, 12, 36, 236, 74);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(box, COL_CYAN, 0);
    lv_obj_set_style_radius(box, 4, 0);
    s_typed = ui_label(box, FONT_40, COL_CYAN, "");
    lv_obj_set_pos(s_typed, 16, 12);
    s_compl = ui_label(box, FONT_40, COL_DIM, "");
    lv_obj_set_pos(s_compl, 16, 12);
    s_cursor = ui_box(box, 16, 60, 24, 4);
    lv_obj_set_style_bg_color(s_cursor, COL_CYAN, 0);
    lv_obj_set_style_bg_opa(s_cursor, LV_OPA_COVER, 0);

    // Match: name, city / elevation, bearing and distance from here
    s_name = ui_label(s_win, FONT_18, COL_TEXT, "");
    lv_label_set_long_mode(s_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_name, 272);
    lv_obj_set_pos(s_name, 260, 36);
    s_city = ui_label(s_win, FONT_14, COL_DIM, "");
    lv_label_set_long_mode(s_city, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_city, 272);
    lv_obj_set_pos(s_city, 260, 62);
    s_brgdis = ui_label(s_win, FONT_18, COL_CYAN, "");
    lv_obj_set_pos(s_brgdis, 260, 84);

    // Keyboard: 4 rows of 10 keys
    static const char *rows[4] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL\x01", "ZXCVBNM\x02"};
    const lv_coord_t kx = 12, ky = 120, kw = 48, kh = 58, gap = 4, vgap = 6;
    for (int r = 0; r < 4; r++) {
        lv_coord_t x = kx;
        for (const char *p = rows[r]; *p; p++) {
            char lab[2] = {*p, 0};
            const char *text = lab;
            lv_coord_t w = kw;
            if (*p == 1) text = LV_SYMBOL_BACKSPACE;
            if (*p == 2) {
                text = "CLR";
                w = kw * 3 + gap * 2;
            }
            lv_obj_t *b = ui_button(s_win, text, *p < 3 ? FONT_20 : FONT_28, w, kh, key_cb, (void *)(intptr_t)*p);
            lv_obj_set_pos(b, x, ky + r * (kh + vgap));
            x += w + gap;
        }
    }

    // Nearest airports: tap to fill in the ident
    lv_obj_t *nl = ui_label(s_win, FONT_14, COL_DIM, "NEAREST");
    lv_obj_set_pos(nl, 546, 12);
    for (int i = 0; i < N_NRST_KEYS; i++) {
        s_nrst_btn[i] = ui_button(s_win, "", FONT_18, 242, 40, nrst_cb, (void *)(intptr_t)i);
        lv_obj_set_pos(s_nrst_btn[i], 546, 36 + i * 44);
    }

    s_activate = ui_button(s_win, "ACTIVATE", FONT_24, 242, 54, activate_cb, nullptr);
    lv_obj_set_pos(s_activate, 546, 260);
    lv_obj_t *close = ui_button(s_win, "CLOSE", FONT_18, 118, 54, [](lv_event_t *) { dct_window_close(); }, nullptr);
    lv_obj_set_pos(close, 546, 322);
    s_cancel_dct = ui_button(s_win, "CANCEL\nDIRECT-TO", FONT_14, 118, 54, cancel_dct_cb, nullptr);
    lv_obj_set_pos(s_cancel_dct, 670, 322);

    lv_obj_add_flag(s_win, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------- tick

void dct_ui_tick() {
    // Arrival alert, once per course: 1 nm out, moving.
    static uint32_t announced_gen = 0;
    const DctNav &D = dct();
    if (D.active && D.valid && D.gen != announced_gen && g_t.pfd.gs > 30 && D.dis < 1.0f) {
        announced_gen = D.gen;
        char m[40];
        snprintf(m, sizeof(m), "ARRIVING %s", D.wpt.id);
        ui_toast(m, COL_MAGENTA);
    }

    if (!dct_window_visible()) return;
    bool changed = false;
    if (g_t.apt.seq != s_seen_seq) {
        s_seen_seq = g_t.apt.seq;
        if (s_entry[0] && !strcmp(g_t.apt.q, s_entry)) {
            s_answered = true;
            s_has_hit = g_t.apt.n > 0;
            if (s_has_hit) s_hit = g_t.apt.hit[0];
            changed = true;
        }
    }
    if (s_entry[0] && !s_answered && g_now - s_sent_ms > RETRY_MS) {
        cmd_apt_find(s_entry);                  // lost, or the bridge was away: ask again
        s_sent_ms = g_now;
        s_retries++;
        changed = true;
    }
    if (g_t.seq_slow != s_nrst_seq) {
        s_nrst_seq = g_t.seq_slow;
        refresh_nearest();
        changed = true;                         // bearing / distance moved on
    }
    // Blinking cursor
    bool on = (g_now / 400) % 2 == 0;
    if (on == lv_obj_has_flag(s_cursor, LV_OBJ_FLAG_HIDDEN)) {
        if (on) lv_obj_clear_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
    }
    if (changed) refresh();
}
