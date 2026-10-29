// Host-side renderer: runs the firmware's UI and telemetry parser on the PC, feeds it a
// recorded bridge session (packets.jsonl, written by preview.py from the bridge's demo
// flight), taps the screen like a user would, and saves screenshots as PPM files.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "lvgl.h"
#include "telemetry.h"
#include "terrain.h"
#include "ui.h"

static const int W = 800, H = 480;
static uint16_t s_fb[W * H];
static uint32_t s_now = 0;

struct Packet { uint32_t t; std::string json; };
static std::vector<Packet> s_packets;
static size_t s_next = 0;
static bool s_feed = true;
static std::vector<std::string> s_cmds;
// The bridge's answers to airport lookups (apt.jsonl, written by preview.py), and the
// ones on their way back to the display.
static std::map<std::string, std::string> s_apt;
static std::vector<Packet> s_replies;

static int s_touch_x = 0, s_touch_y = 0;
static bool s_touch = false;
static int s_ota_state = 0;

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *px) {
    for (int y = a->y1; y <= a->y2; y++)
        for (int x = a->x1; x <= a->x2; x++) s_fb[y * W + x] = (px++)->full;
    lv_disp_flush_ready(drv);
}

static void touch_cb(lv_indev_drv_t *, lv_indev_data_t *d) {
    d->point.x = s_touch_x;
    d->point.y = s_touch_y;
    d->state = s_touch ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static bool s_ui_ready = false;

static void run(uint32_t ms) {
    uint32_t end = s_now + ms;
    while (s_now < end) {
        s_now += 5;
        lv_tick_inc(5);
        while (s_feed && s_next < s_packets.size() && s_packets[s_next].t <= s_now) {
            const std::string &j = s_packets[s_next++].json;
            telemetry_ingest(j.c_str(), j.size(), s_now);
        }
        for (size_t i = 0; i < s_replies.size();)
            if (s_replies[i].t <= s_now) {
                telemetry_ingest(s_replies[i].json.c_str(), s_replies[i].json.size(), s_now);
                s_replies.erase(s_replies.begin() + i);
            } else {
                i++;
            }
        if (s_ui_ready) ui_tick(s_now);         // not during the splash
        while (terrain_service()) {}            // the display loads tiles in the background
        lv_timer_handler();
    }
}

static void click(int x, int y, uint32_t hold_ms = 60) {
    s_touch_x = x;
    s_touch_y = y;
    s_touch = true;
    run(hold_ms);
    s_touch = false;
    run(80);
}

static void shot(const char *name) {
    run(40);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(nullptr);
    std::string fn = std::string(name) + ".ppm";
    FILE *f = fopen(fn.c_str(), "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        uint16_t c = s_fb[i];
        unsigned char rgb[3] = {(unsigned char)(((c >> 11) & 0x1F) * 255 / 31), (unsigned char)(((c >> 5) & 0x3F) * 255 / 63),
                                (unsigned char)((c & 0x1F) * 255 / 31)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("  %s\n", fn.c_str());
}

static void expect_cmd(const char *needle) {
    for (auto &c : s_cmds)
        if (c.find(needle) != std::string::npos) {
            printf("  ok: sent %s\n", c.c_str());
            s_cmds.clear();
            return;
        }
    printf("  FAIL: expected a command containing %s; got %zu command(s)\n", needle, s_cmds.size());
    for (auto &c : s_cmds) printf("    %s\n", c.c_str());
    s_cmds.clear();
}

static void load_packets(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(1);
    }
    static char line[64 * 1024];
    while (fgets(line, sizeof(line), f)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        s_packets.push_back({(uint32_t)atol(line), std::string(tab + 1)});
    }
    fclose(f);
}

static void load_apt(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    static char line[16 * 1024];
    while (fgets(line, sizeof(line), f)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        s_apt[line] = tab + 1;
    }
    fclose(f);
}

// For the touch script: a packet of its own, as if the bridge had sent it now.
void preview_inject(const char *json) { telemetry_ingest(json, strlen(json), s_now); }

// Like the bridge: answer {"cmd":"apt_find","q":"KAB"} about 40 ms later.
static void cmd_sink(const char *j) {
    s_cmds.push_back(j);
    const char *q = strstr(j, "\"apt_find\",\"q\":\"");
    if (!q) return;
    q += strlen("\"apt_find\",\"q\":\"");
    std::string id(q, strcspn(q, "\""));
    auto it = s_apt.find(id);
    std::string reply = it != s_apt.end() ? it->second : "{\"apt\":{\"q\":\"" + id + "\",\"list\":[]}}";
    s_replies.push_back({s_now + 40, reply});
}

// Key positions are the ones the pages lay out (see ui_radio.cpp / ui_ap.cpp).
int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);       // keep the output if it crashes
    // Terrain tiles around the demo flight (bridge/terrain_sd.py --area, run by preview.py).
    printf("terrain: %s\n", terrain_init("../terrain") ? "tiles found" : "none");
    load_packets(argc > 1 ? argv[1] : "packets.jsonl");
    printf("%zu packets\n", s_packets.size());

    lv_init();
    static lv_color_t buf[W * 60];
    static lv_disp_draw_buf_t db;
    lv_disp_draw_buf_init(&db, buf, nullptr, W * 60);
    static lv_disp_drv_t dd;
    lv_disp_drv_init(&dd);
    dd.hor_res = W;
    dd.ver_res = H;
    dd.flush_cb = flush_cb;
    dd.draw_buf = &db;
    lv_disp_drv_register(&dd);
    static lv_indev_drv_t id;
    lv_indev_drv_init(&id);
    id.type = LV_INDEV_TYPE_POINTER;
    id.read_cb = touch_cb;
    lv_indev_drv_register(&id);

    load_apt("apt.jsonl");
    printf("%zu airport lookups\n", s_apt.size());
    g_cmd_sink = cmd_sink;
    telemetry_init();
    UiPlatform plat = {};
    plat.link_label = [](char *b, size_t n, bool *ok) { snprintf(b, n, "WIFI"); *ok = true; };
    plat.sys_info = [](char *b, size_t n) {
        snprintf(b, n, "Firmware: preview\nWiFi: TestNet  IP 192.168.1.50\nBridge: 192.168.1.10:8080 (WebSocket)\n"
                       "Packets: 1234 ok, 0 bad\nDisplay: 30.0 fps, 21 ms/frame\nHeap: 150 KB internal, 7600 KB PSRAM");
    };

    plat.get_settings = [](NetSettings &s) {
        memset(&s, 0, sizeof(s));
        strcpy(s.ssid, "HangarNet");
        strcpy(s.pass, "x");
    };
    plat.save_settings = [](const NetSettings &s) { printf("  settings saved: ssid=%s bridge=%s\n", s.ssid, s.bridge); };
    plat.scan_start = []() {};
    plat.scan_results = [](WifiNet *out, int max) {
        static const WifiNet nets[] = {{"HangarNet", -48, true}, {"HangarNet-Guest", -60, false},
                                       {"FBO Lounge", -70, true}, {"NETGEAR42", -81, true}};
        int n = 4 < max ? 4 : max;
        memcpy(out, nets, sizeof(WifiNet) * n);
        return n;
    };
    plat.fw_version = "mfd7 1.0.3";
    plat.backlight = [](bool on) { printf("  backlight %s\n", on ? "on" : "off"); };
    plat.load_sleep_min = []() { return 10; };
    plat.save_sleep_min = [](int m) { printf("  sleep timeout saved: %d min\n", m); };
    plat.load_pref = [](const char *, int def) { return def; };
    plat.save_pref = [](const char *key, int v) { printf("  pref saved: %s=%d\n", key, v); };
    plat.ota_start = [](uint16_t port, const char *path) {
        printf("  ota requested: port %u path %s\n", port, path);
        s_ota_state = 1;
    };
    plat.ota_status = [](int *state, int *pct, char *msg, size_t n) {
        *state = s_ota_state;
        *pct = 42;
        snprintf(msg, n, "");
    };
    plat.net_status = [](char *w, size_t nw, char *b, size_t nb, bool *wok, bool *bok) {
        snprintf(w, nw, "WiFi: HangarNet   IP 192.168.1.50");
        snprintf(b, nb, "Bridge: connected  192.168.1.10:8080");
        *wok = *bok = true;
    };
    s_feed = false;
    ui_splash_show(plat.fw_version);
    run(100);
    shot("splash");
    ui_splash_hide();
    ui_init(plat);
    s_ui_ready = true;
    run(300);
    shot("nodata");
    // Every page must also cope with no data at all (bridge or sim not there yet).
    for (int p = PG_EIS; p < PG_COUNT; p++) {
        ui_show_page((PageId)p);
        run(300);
    }
    ui_show_page(PG_NRST);
    run(300);
    shot("nrst_nodata");
    ui_show_page(PG_PFD);
    run(100);
    s_feed = true;
    run(15000);

    shot("pfd");
    ui_show_page(PG_EIS); shot("eis");
    ui_show_page(PG_MAP); shot("map");
    ui_show_page(PG_NRST); shot("nrst");
    ui_show_page(PG_WX); run(1200); shot("wx_radar");
    ui_show_page(PG_AP); shot("ap");
    ui_show_page(PG_RADIO); shot("radio");

    extern void preview_script(void (*click)(int, int, uint32_t), void (*run)(uint32_t), void (*shot)(const char *),
                               void (*expect)(const char *));
    preview_script(click, run, shot, expect_cmd);
    return 0;
}
