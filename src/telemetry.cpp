#include "telemetry.h"

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

#ifdef ARDUINO
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
static SemaphoreHandle_t s_mutex;
static void lock() { xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void unlock() { xSemaphoreGive(s_mutex); }
#else
static void lock() {}
static void unlock() {}
#endif

static Telemetry s_tel;

void (*g_cmd_sink)(const char *json) = nullptr;

void telemetry_init() {
#ifdef ARDUINO
    s_mutex = xSemaphoreCreateMutex();
#endif
    memset(&s_tel, 0, sizeof(s_tel));
    s_tel.pfd.baro = 29.92f;
    s_tel.nav.ete = -1;
}

static void copy_str(char *dst, size_t n, JsonVariantConst v) {
    const char *s = v.as<const char *>();
    if (!s) return;
    strncpy(dst, s, n - 1);
    dst[n - 1] = 0;
}
#define COPY(dst, v) copy_str(dst, sizeof(dst), v)

static void get_f(float &dst, JsonObjectConst o, const char *key) {
    JsonVariantConst v = o[key];
    if (!v.isNull()) dst = v.as<float>();
}

static void parse_pfd(Pfd &p, JsonObjectConst o) {
    get_f(p.pitch, o, "pitch");
    get_f(p.roll, o, "roll");
    get_f(p.ias, o, "ias");
    get_f(p.tas, o, "tas");
    get_f(p.gs, o, "gs");
    get_f(p.alt, o, "alt");
    get_f(p.agl, o, "agl");
    get_f(p.hdg, o, "hdg");
    get_f(p.trk, o, "trk");
    get_f(p.vsi, o, "vsi");
    get_f(p.slip, o, "slip");
    get_f(p.turn, o, "turn");
    get_f(p.baro, o, "baro");
    get_f(p.oat, o, "oat");
    if (!o["pbrk"].isNull()) p.park_brake = o["pbrk"].as<int>() != 0;
    if (!o["gnd"].isNull()) p.on_ground = o["gnd"].as<int>() != 0;
}

static void parse_nav(NavInfo &n, JsonObjectConst o) {
    if (!o["lat"].isNull()) n.lat = o["lat"].as<double>();
    if (!o["lon"].isNull()) n.lon = o["lon"].as<double>();
    get_f(n.magvar, o, "magvar");
    get_f(n.wind_dir, o, "wind_dir");
    get_f(n.wind_spd, o, "wind_spd");
    n.has_to = !o["to"].isNull();
    if (n.has_to) {
        COPY(n.to, o["to"]);
        get_f(n.to_dist, o, "to_dist");
        get_f(n.to_brg, o, "to_brg");
    } else {
        n.to[0] = 0;
    }
    n.has_dtk = !o["dtk"].isNull();
    if (n.has_dtk) {
        get_f(n.dtk, o, "dtk");
        get_f(n.xtk, o, "xtk");
    }
    n.ete = o["ete"].isNull() ? -1 : o["ete"].as<int>();
    n.cdi_fs = 2.0f;
    n.from = false;
    n.dct = false;
}

static void parse_apt(AptFind &f, JsonObjectConst o) {
    COPY(f.q, o["q"]);
    uint8_t n = 0;
    for (JsonArrayConst h : o["list"].as<JsonArrayConst>()) {
        if (n >= MAX_APT_HITS) break;
        AptHit &a = f.hit[n++];
        memset(&a, 0, sizeof(a));
        COPY(a.id, h[0]);
        COPY(a.name, h[1]);
        COPY(a.city, h[2]);
        a.lat = h[3].as<double>();
        a.lon = h[4].as<double>();
        a.elev = h[5] | 0;
    }
    f.n = n;
    f.seq++;
}

static void parse_eis(Eis &e, JsonObjectConst o) {
    get_f(e.rpm, o, "rpm");
    get_f(e.mrp, o, "mrp");
    get_f(e.ff, o, "ff");
    get_f(e.oil_t, o, "oil_t");
    get_f(e.oil_p, o, "oil_p");
    get_f(e.fuel_p, o, "fuel_p");
    get_f(e.fuel_l, o, "fuel_l");
    get_f(e.fuel_r, o, "fuel_r");
    get_f(e.fuel_cap, o, "fuel_cap");
    get_f(e.volts, o, "volts");
    get_f(e.amps, o, "amps");
    JsonArrayConst cht = o["cht"], egt = o["egt"];
    if (!cht.isNull()) {
        uint8_t n = 0;
        for (JsonVariantConst v : cht) {
            if (n >= MAX_CYL) break;
            e.cht[n++] = v.as<float>();
        }
        e.ncyl = n;
    }
    if (!egt.isNull()) {
        uint8_t n = 0;
        for (JsonVariantConst v : egt) {
            if (n >= MAX_CYL) break;
            e.egt[n++] = v.as<float>();
        }
        if (n < e.ncyl || !e.ncyl) e.ncyl = n;
    }
}

static void parse_rad(Radios &r, JsonObjectConst o) {
    COPY(r.com1_act, o["com1_act"]);
    COPY(r.com1_stby, o["com1_stby"]);
    COPY(r.com2_act, o["com2_act"]);
    COPY(r.com2_stby, o["com2_stby"]);
    COPY(r.nav1_act, o["nav1_act"]);
    COPY(r.nav1_stby, o["nav1_stby"]);
    COPY(r.nav2_act, o["nav2_act"]);
    COPY(r.nav2_stby, o["nav2_stby"]);
    COPY(r.xpdr, o["xpdr"]);
    COPY(r.xpdr_mode, o["xpdr_mode"]);
}

static void parse_ap(Autopilot &a, JsonObjectConst o) {
    if (!o["master"].isNull()) a.master = o["master"].as<int>() != 0;
    if (!o["fd"].isNull()) a.fd = o["fd"].as<int>() != 0;
    if (!o["yd"].isNull()) a.yd = o["yd"].as<int>() != 0;
    COPY(a.lat, o["lat"]);
    COPY(a.lat_arm, o["lat_arm"]);
    COPY(a.vert, o["vert"]);
    COPY(a.vert_arm, o["vert_arm"]);
    get_f(a.hdg, o, "hdg");
    get_f(a.alt, o, "alt");
    get_f(a.vs, o, "vs");
    get_f(a.ias, o, "ias");
}

static void parse_nrst(Telemetry &t, JsonArrayConst arr) {
    uint8_t n = 0;
    for (JsonObjectConst o : arr) {
        if (n >= MAX_NRST) break;
        NrstApt &a = t.nrst[n++];
        memset(&a, 0, sizeof(a));
        COPY(a.icao, o["icao"]);
        COPY(a.name, o["name"]);
        a.dist = o["dist"] | 0.0f;
        a.brg = o["brg"] | 0;
        a.rwy = o["rwy"] | 0;
        a.elev = o["elev"] | 0;
        COPY(a.freq, o["freq"]);
        COPY(a.ftype, o["ftype"]);
        COPY(a.metar, o["metar"]);
        COPY(a.cat, o["cat"]);
    }
    t.n_nrst = n;
}

static void parse_map(Telemetry &t, JsonObjectConst o) {
    uint8_t n = 0;
    for (JsonArrayConst w : o["fpl"].as<JsonArrayConst>()) {
        if (n >= MAX_FPL) break;
        FplWpt &p = t.fpl[n++];
        COPY(p.id, w[0]);
        p.lat = w[1].as<float>();
        p.lon = w[2].as<float>();
    }
    t.n_fpl = n;
    t.fpl_act = o["act"] | 0;
    uint16_t m = 0;
    for (JsonArrayConst a : o["pts"].as<JsonArrayConst>()) {
        if (m >= MAX_MAP_PTS) break;
        MapPt &p = t.pts[m++];
        const char *k = a[0].as<const char *>();
        p.kind = k ? k[0] : '?';
        COPY(p.id, a[1]);
        p.lat = a[2].as<float>();
        p.lon = a[3].as<float>();
    }
    t.n_pts = m;
}

// Radar grid: run-length text, a level character (".gyrm") and the run length if > 1.
static bool parse_grid(WxGrid &g, JsonObjectConst o) {
    int n = o["n"] | 0;
    const char *p = o["rle"].as<const char *>();
    if (n < 2 || n > WX_GRID_MAX || !p) return false;
    int total = n * n, i = 0;
    while (*p && i < total) {
        const char *lv = strchr(".gyrm", *p++);
        if (!lv) return false;
        long run = 1;
        if (*p >= '0' && *p <= '9') run = strtol(p, (char **)&p, 10);
        if (run > total - i) run = total - i;
        memset(g.lv + i, (int)(lv - ".gyrm"), run);
        i += run;
    }
    if (i < total) return false;
    g.n = (uint8_t)n;
    g.lat = o["lat"].as<double>();
    g.lon = o["lon"].as<double>();
    g.half_nm = o["nm"] | 40.0f;
    return true;
}

static void parse_wx(Telemetry &t, JsonObjectConst o, uint32_t now_ms) {
    t.wx_valid = true;
    t.wx_rx_ms = now_ms;
    JsonObjectConst g = o["grid"];
    t.has_grid = !g.isNull() && parse_grid(t.grid, g);
    if (t.has_grid) t.grid_seq++;
    uint16_t n = 0;
    for (JsonArrayConst s : o["strikes"].as<JsonArrayConst>()) {
        if (n >= MAX_STRIKES) break;
        t.strikes[n++] = {s[0] | 0.0f, s[1] | 0.0f, (int16_t)(s[2] | 0)};
    }
    t.n_strikes = n;
    COPY(t.rdr_txt, o["rdr_txt"]);
    COPY(t.ltg_txt, o["ltg_txt"]);
    t.rdr_ok = (o["rdr_ok"] | 0) != 0;
    t.ltg_ok = (o["ltg_ok"] | 0) != 0;
}

bool telemetry_ingest(const char *json, size_t len, uint32_t now_ms) {
    JsonDocument doc;
    if (deserializeJson(doc, json, len) != DeserializationError::Ok) return false;
    JsonObjectConst root = doc.as<JsonObjectConst>();
    if (root.isNull()) return false;

    lock();
    Telemetry &t = s_tel;
    t.any_rx_ms = now_ms;
    if (!root["src"].isNull()) COPY(t.src, root["src"]);
    JsonObjectConst o;
    if (!(o = root["pfd"]).isNull()) {
        parse_pfd(t.pfd, o);
        t.rx_ms = now_ms;
        t.seq_fast++;
    }
    if (!(o = root["nav"]).isNull()) parse_nav(t.nav, o);
    if (!(o = root["apt"]).isNull()) parse_apt(t.apt, o);
    bool mid = false;
    if (!(o = root["eis"]).isNull()) { parse_eis(t.eis, o); mid = true; }
    if (!(o = root["rad"]).isNull()) { parse_rad(t.rad, o); mid = true; }
    if (!(o = root["ap"]).isNull()) { parse_ap(t.ap, o); mid = true; }
    if (mid) t.seq_mid++;
    bool slow = false;
    JsonArrayConst a = root["nrst"];
    if (!a.isNull()) { parse_nrst(t, a); slow = true; }
    if (!(o = root["map"]).isNull()) { parse_map(t, o); slow = true; }
    if (!(o = root["wx"]).isNull()) { parse_wx(t, o, now_ms); slow = true; }
    if (!(o = root["fwu"]).isNull()) {
        COPY(t.fwu_ver, o["ver"]);
        t.fwu_port = o["port"] | 0;
        COPY(t.fwu_path, o["path"]);
        t.fwu_size = o["size"] | 0;
    }
    if (slow) t.seq_slow++;
    unlock();
    return true;
}

void telemetry_snapshot(Telemetry &out) {
    const size_t fast = offsetof(Telemetry, nrst);
    lock();
    bool slow = out.seq_slow != s_tel.seq_slow;
    memcpy(&out, &s_tel, fast);
    if (slow) memcpy((char *)&out + fast, (const char *)&s_tel + fast, sizeof(Telemetry) - fast);
    unlock();
}

int version_cmp(const char *a, const char *b) {
    // Walk both strings number by number.
    for (;;) {
        while (*a && (*a < '0' || *a > '9')) a++;
        while (*b && (*b < '0' || *b > '9')) b++;
        if (!*a || !*b) return (*a ? 1 : 0) - (*b ? 1 : 0);
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
    }
}

bool telemetry_live(const Telemetry &t, uint32_t now_ms) {
    return t.rx_ms != 0 && now_ms - t.rx_ms < 2000 && strcmp(t.src, "NONE") != 0;
}

// ---------------------------------------------------------------- commands

static void send(const char *json) {
    if (g_cmd_sink) g_cmd_sink(json);
}

void cmd_freq(const char *radio, float mhz) {
    char b[96];
    snprintf(b, sizeof(b), "{\"cmd\":\"freq\",\"radio\":\"%s\",\"mhz\":%.3f}", radio, mhz);
    send(b);
}

void cmd_swap(const char *radio) {
    char b[64];
    snprintf(b, sizeof(b), "{\"cmd\":\"swap\",\"radio\":\"%s\"}", radio);
    send(b);
}

void cmd_xpdr(const char *code) {
    char b[64];
    snprintf(b, sizeof(b), "{\"cmd\":\"xpdr\",\"code\":\"%.4s\"}", code);
    send(b);
}

void cmd_xpdr_mode(const char *mode) {
    char b[64];
    snprintf(b, sizeof(b), "{\"cmd\":\"xpdr_mode\",\"mode\":\"%s\"}", mode);
    send(b);
}

void cmd_ap(const char *key) {
    char b[64];
    snprintf(b, sizeof(b), "{\"cmd\":\"ap\",\"key\":\"%s\"}", key);
    send(b);
}

void cmd_ap_set(const char *var, float val) {
    char b[80];
    snprintf(b, sizeof(b), "{\"cmd\":\"ap_set\",\"var\":\"%s\",\"val\":%.0f}", var, val);
    send(b);
}

void cmd_map_range(float nm) {
    char b[64];
    snprintf(b, sizeof(b), "{\"cmd\":\"map_range\",\"nm\":%.1f}", nm);
    send(b);
}

void cmd_wx_range(float nm) {
    char b[64];
    snprintf(b, sizeof(b), "{\"cmd\":\"wx_range\",\"nm\":%.1f}", nm);
    send(b);
}

void cmd_apt_find(const char *q) {
    char id[8];
    size_t n = 0;
    for (; *q && n < sizeof(id) - 1; q++)          // idents are letters and digits only
        if ((*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9')) id[n++] = *q;
    id[n] = 0;
    char b[64];
    snprintf(b, sizeof(b), "{\"cmd\":\"apt_find\",\"q\":\"%s\"}", id);
    send(b);
}

void cmd_hello() {
    send("{\"cmd\":\"hello\",\"fw\":\"" FW_VERSION "\"}");
}
