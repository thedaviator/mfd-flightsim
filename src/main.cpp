// Flight-sim MFD for the Waveshare ESP32-S3-Touch-LCD-7.
// Core 1 (Arduino loop): LVGL and the UI. Core 0: WiFi / WebSocket / serial link.
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include "board.h"
#include "config.h"
#include "crash.h"
#include "link.h"
#include "telemetry.h"
#include "terrain.h"
#include "ui.h"

// Terrain tiles are read from the SD card here, on core 0, so the UI never waits for them.
static void terrain_task(void *) {
    for (;;)
        if (!terrain_service()) vTaskDelay(pdMS_TO_TICKS(20));
}

static void link_label(char *buf, size_t n, bool *ok) {
    LinkStatus st;
    link_status(st);
    switch (st.kind) {
    case LINK_WS: snprintf(buf, n, "WIFI"); *ok = true; break;
    case LINK_SERIAL: snprintf(buf, n, "USB"); *ok = true; break;
    case LINK_WIFI_SEARCH: snprintf(buf, n, "NO BRIDGE"); *ok = false; break;
    default: snprintf(buf, n, st.wifi_configured ? "NO WIFI" : "SETUP WIFI"); *ok = false; break;
    }
}

static void net_status(char *wifi, size_t nw, char *bridge, size_t nb, bool *wok, bool *bok) {
    LinkStatus st;
    link_status(st);
    *wok = st.wifi_up;
    if (!st.wifi_configured) snprintf(wifi, nw, "WiFi: not set up");
    else if (st.wifi_up) snprintf(wifi, nw, "WiFi: %s   IP %s", st.ssid, st.ip);
    else snprintf(wifi, nw, "WiFi: connecting to %s ...", st.ssid);
    *bok = st.kind == LINK_WS || st.kind == LINK_SERIAL;
    if (st.kind == LINK_WS) snprintf(bridge, nb, "Bridge: connected  %s", st.bridge);
    else if (st.kind == LINK_SERIAL) snprintf(bridge, nb, "Bridge: connected over USB serial");
    else if (st.wifi_up) snprintf(bridge, nb, st.bridge[0] ? "Bridge: trying %s ..." : "Bridge: searching ...", st.bridge);
    else snprintf(bridge, nb, "Bridge: waiting for WiFi");
}

static char s_sd_info[64] = "SD card: not checked";

static void sys_info(char *buf, size_t n) {
    LinkStatus st;
    link_status(st);
    static const char *kinds[] = {"none", "WiFi, looking for the bridge", "WebSocket", "USB serial"};
    snprintf(buf, n,
             "Firmware: " FW_VERSION "\n"
             "WiFi: %s  %s\n"
             "Link: %s  %s\n"
             "Packets: %lu ok, %lu bad\n"
             "Display: %.1f fps, %.0f ms/frame\n"
             "Heap: %u KB internal, %u KB PSRAM free\n"
             "Touch: %s\n"
             "%s\n%s\n"
             "Last restart: %s",
             st.wifi_configured ? st.ssid : "(not set up)", st.wifi_up ? st.ip : "not connected",
             kinds[st.kind], st.bridge, (unsigned long)st.packets, (unsigned long)st.bad_packets, board_fps(),
             board_render_ms(), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), board_touch_ok() ? "GT911 OK" : "NOT FOUND",
             s_sd_info, terrain_status(), crash_report()[0] ? crash_report() : "normal");
}

// Screen sleep timeout, saved in flash next to (not with) the WiFi settings.
static int load_sleep_min() {
    Preferences p;
    p.begin("ui", true);
    int m = p.getUChar("sleep_min", 10);
    p.end();
    return m;
}

static void save_sleep_min(int m) {
    Preferences p;
    p.begin("ui", false);
    p.putUChar("sleep_min", (uint8_t)m);
    p.end();
}

// Other small UI choices, same namespace.
static int load_pref(const char *key, int def) {
    Preferences p;
    p.begin("ui", true);
    int v = p.getInt(key, def);
    p.end();
    return v;
}

static void save_pref(const char *key, int value) {
    Preferences p;
    p.begin("ui", false);
    p.putInt(key, value);
    p.end();
}

void setup() {
    Serial.setRxBufferSize(16384);
    Serial.begin(SERIAL_BAUD);
    delay(50);
    Serial.printf("\n%s  PSRAM %u KB free\n", FW_VERSION, (unsigned)(ESP.getFreePsram() / 1024));
    crash_begin();              // report a crash before this restart (sent with the hello)

    telemetry_init();
    if (!board_init()) {
        Serial.println("Display init failed; halting");
        for (;;) delay(1000);
    }
    // Splash for SPLASH_MS; WiFi starts connecting meanwhile.
    uint32_t t0 = millis();
    ui_splash_show(FW_VERSION);
    lv_timer_handler();         // draw the splash before the backlight comes on
    board_backlight(true);
    link_begin();               // loads the saved WiFi settings; the UI needs them
    char sd[64];
    bool card = board_sd_begin("/sd", sd, sizeof(sd));
    snprintf(s_sd_info, sizeof(s_sd_info), "%s", sd);
    if (!card) {
        // Upper case for the MAP page, like its other notices.
        for (char *p = sd; *p; p++) *p = (char)toupper((unsigned char)*p);
        terrain_set_status(sd);
    } else if (terrain_init("/sd")) {
        xTaskCreatePinnedToCore(terrain_task, "terrain", 6144, nullptr, 1, nullptr, 0);
    }
    Serial.printf("%s; %s\n", s_sd_info, terrain_status());
    while (millis() - t0 < SPLASH_MS) {
        lv_timer_handler();
        delay(10);
    }
    ui_splash_hide();
    UiPlatform plat = {link_label, sys_info, link_get_settings, link_save_settings,
                       link_scan_start, link_scan_results, net_status,
                       FW_VERSION, link_ota_start, link_ota_status,
                       board_backlight, load_sleep_min, save_sleep_min, load_pref, save_pref};
    ui_init(plat);
    lv_timer_handler();
    Serial.println("Ready");
}

void loop() {
    static uint32_t last_log = 0;
    uint32_t now = millis();
    static uint32_t last_note = 0;
    if (now - last_note >= 250) {
        last_note = now;
        crash_note(ui_page_key());
    }
    ui_tick(now);
    lv_timer_handler();
    if (now - last_log > 10000) {
        last_log = now;
        LinkStatus st;
        link_status(st);
        Serial.printf("fps %.1f  render %.0f ms  packets %lu  heap %u/%u KB\n", board_fps(), board_render_ms(),
                      (unsigned long)st.packets, (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                      (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
    delay(2);
}
