#include "link.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_heap_caps.h>

#include "config.h"
#include "crash.h"
#include "telemetry.h"

// Everything network-related runs in one task on core 0, so the WebSocket client and
// the serial reader are never touched from the UI task. The UI hands commands and
// settings over through a queue and flags.

#define SERIAL_LINE_MAX (32 * 1024)
#define CMD_QUEUE_LEN 16
#define CMD_MAX 128
#define MAX_SCAN 24

static WebSocketsClient s_ws;
static WiFiUDP s_beacon;
static QueueHandle_t s_cmd_queue;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static LinkStatus s_status;

// Settings: s_cfg is owned by the net task; s_pending is handed over by the UI.
static NetSettings s_cfg, s_pending;
static volatile bool s_reconfig = false;

static volatile bool s_scan_req = false;
static volatile int s_scan_n = -2;              // -2 none, -1 scanning, >= 0 results
static WifiNet s_scan[MAX_SCAN];

static bool s_ws_started = false;
static bool s_ws_connected = false;
static uint32_t s_ws_rx_ms = 0, s_serial_rx_ms = 0;
static char s_bridge_host[40] = "";
static uint16_t s_bridge_port = BRIDGE_PORT;

static volatile int s_ota_state = OTA_IDLE, s_ota_pct = 0;
static volatile bool s_ota_req = false;
static uint16_t s_ota_port;
static char s_ota_path[40], s_ota_msg[64];

static char *s_line;            // serial line buffer (PSRAM)
static size_t s_line_len = 0;
static bool s_line_overflow = false;

// ---------------------------------------------------------------- settings storage

static void load_settings(NetSettings &s) {
    memset(&s, 0, sizeof(s));
    Preferences p;
    p.begin("mfd", true);
    bool saved = p.isKey("ssid");
    if (saved) {
        p.getString("ssid", s.ssid, sizeof(s.ssid));
        p.getString("pass", s.pass, sizeof(s.pass));
        p.getString("bridge", s.bridge, sizeof(s.bridge));
    }
    p.end();
    if (!saved) {               // first boot: compiled-in defaults (secrets.h), if any
        strncpy(s.ssid, WIFI_SSID, sizeof(s.ssid) - 1);
        strncpy(s.pass, WIFI_PASSWORD, sizeof(s.pass) - 1);
        strncpy(s.bridge, BRIDGE_HOST, sizeof(s.bridge) - 1);
    }
}

static void store_settings(const NetSettings &s) {
    Preferences p;
    p.begin("mfd", false);
    p.putString("ssid", s.ssid);
    p.putString("pass", s.pass);
    p.putString("bridge", s.bridge);
    p.end();
}

void link_get_settings(NetSettings &out) {
    portENTER_CRITICAL(&s_mux);
    out = s_reconfig ? s_pending : s_cfg;
    portEXIT_CRITICAL(&s_mux);
}

void link_save_settings(const NetSettings &s) {
    portENTER_CRITICAL(&s_mux);
    s_pending = s;
    s_reconfig = true;
    portEXIT_CRITICAL(&s_mux);
}

void link_scan_start() {
    s_scan_n = -1;
    s_scan_req = true;
}

int link_scan_results(WifiNet *out, int max) {
    portENTER_CRITICAL(&s_mux);
    int n = s_scan_n;
    if (n > 0) memcpy(out, s_scan, sizeof(WifiNet) * (n < max ? n : max));
    portEXIT_CRITICAL(&s_mux);
    return n < 0 ? -1 : (n < max ? n : max);
}

// ---------------------------------------------------------------- packets and commands

static void count_packet(bool ok) {
    portENTER_CRITICAL(&s_mux);
    if (ok) s_status.packets++;
    else s_status.bad_packets++;
    portEXIT_CRITICAL(&s_mux);
}

// Called from the UI task.
static void queue_command(const char *json) {
    char buf[CMD_MAX];
    strncpy(buf, json, CMD_MAX - 1);
    buf[CMD_MAX - 1] = 0;
    xQueueSend(s_cmd_queue, buf, 0);
}

static void ws_event(WStype_t type, uint8_t *payload, size_t len) {
    switch (type) {
    case WStype_CONNECTED:
        s_ws_connected = true;
        Serial.printf("Bridge connected: %s:%u\n", s_bridge_host, s_bridge_port);
        if (crash_report()[0]) {
            // Tell the bridge why the display restarted (it logs it).
            char hello[400];
            snprintf(hello, sizeof(hello), "{\"cmd\":\"hello\",\"fw\":\"" FW_VERSION "\",\"crash\":\"%s\"}", crash_report());
            s_ws.sendTXT(hello);
        } else {
            s_ws.sendTXT("{\"cmd\":\"hello\",\"fw\":\"" FW_VERSION "\"}");
        }
        break;
    case WStype_DISCONNECTED:
        if (s_ws_connected) Serial.println("Bridge disconnected");
        s_ws_connected = false;
        break;
    case WStype_TEXT:
        s_ws_rx_ms = millis();
        count_packet(telemetry_ingest((const char *)payload, len, s_ws_rx_ms));
        break;
    default:
        break;
    }
}

static void stop_ws() {
    if (s_ws_started) s_ws.disconnect();
    s_ws_started = false;
    s_ws_connected = false;
    s_bridge_host[0] = 0;
}

static void start_ws(const char *host, uint16_t port) {
    if (s_ws_started && !strcmp(host, s_bridge_host) && port == s_bridge_port) return;
    stop_ws();
    strncpy(s_bridge_host, host, sizeof(s_bridge_host) - 1);
    s_bridge_port = port;
    Serial.printf("Bridge at %s:%u\n", host, port);
    s_ws.begin(s_bridge_host, s_bridge_port, "/");
    s_ws.onEvent(ws_event);
    s_ws.setReconnectInterval(2000);
    s_ws.enableHeartbeat(5000, 3000, 2);
    s_ws_started = true;
}

// A configured address: "host" or "host:port".
static void start_ws_configured(const char *addr) {
    char host[40];
    strncpy(host, addr, sizeof(host) - 1);
    host[sizeof(host) - 1] = 0;
    uint16_t port = BRIDGE_PORT;
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = 0;
        port = atoi(colon + 1) ?: BRIDGE_PORT;
    }
    start_ws(host, port);
}

// "MFDBRIDGE <port> <hostname>" broadcast by the bridge every 2 s.
static void poll_beacon() {
    int n = s_beacon.parsePacket();
    if (n <= 0) return;
    char buf[64];
    n = s_beacon.read(buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = 0;
    if (strncmp(buf, "MFDBRIDGE ", 10) != 0) return;
    uint16_t port = atoi(buf + 10);
    if (!port) port = BRIDGE_PORT;
    // Follow the beacon unless a connected bridge is already feeding us.
    if (!s_ws_connected || millis() - s_ws_rx_ms > 5000)
        start_ws(s_beacon.remoteIP().toString().c_str(), port);
}

static void poll_serial() {
    while (Serial.available()) {
        int c = Serial.read();
        if (c < 0) break;
        if (c == '\n') {
            if (!s_line_overflow && s_line_len > 0 && s_line[0] == '{') {
                uint32_t now = millis();
                bool ok = telemetry_ingest(s_line, s_line_len, now);
                if (ok) s_serial_rx_ms = now;
                count_packet(ok);
            }
            s_line_len = 0;
            s_line_overflow = false;
        } else if (c != '\r') {
            if (s_line_len < SERIAL_LINE_MAX - 1) s_line[s_line_len++] = (char)c;
            else s_line_overflow = true;
        }
    }
}

static void send_commands() {
    char buf[CMD_MAX];
    uint32_t now = millis();
    bool serial_live = s_serial_rx_ms && now - s_serial_rx_ms < 3000;
    while (xQueueReceive(s_cmd_queue, buf, 0) == pdTRUE) {
        if (s_ws_connected) s_ws.sendTXT(buf);
        if (serial_live) Serial.println(buf);
    }
}

static void update_status() {
    uint32_t now = millis();
    LinkStatus st;
    memset(&st, 0, sizeof(st));
    st.wifi_configured = s_cfg.ssid[0] != 0;
    st.wifi_up = WiFi.status() == WL_CONNECTED;
    strncpy(st.ssid, s_cfg.ssid, sizeof(st.ssid) - 1);
    if (s_serial_rx_ms && now - s_serial_rx_ms < 3000) st.kind = LINK_SERIAL;
    else if (s_ws_connected) st.kind = LINK_WS;
    else if (st.wifi_up) st.kind = LINK_WIFI_SEARCH;
    else st.kind = LINK_NONE;
    if (st.wifi_up) strncpy(st.ip, WiFi.localIP().toString().c_str(), sizeof(st.ip) - 1);
    if (s_bridge_host[0]) snprintf(st.bridge, sizeof(st.bridge), "%s:%u", s_bridge_host, s_bridge_port);
    portENTER_CRITICAL(&s_mux);
    st.packets = s_status.packets;
    st.bad_packets = s_status.bad_packets;
    s_status = st;
    portEXIT_CRITICAL(&s_mux);
}

static void wifi_connect() {
    stop_ws();
    WiFi.disconnect();
    if (s_cfg.ssid[0]) {
        Serial.printf("WiFi: joining %s\n", s_cfg.ssid);
        WiFi.begin(s_cfg.ssid, s_cfg.pass);
    } else {
        Serial.println("WiFi: not configured (Settings page)");
    }
}

static void poll_scan() {
    if (s_scan_req) {
        s_scan_req = false;
        WiFi.scanDelete();
        WiFi.scanNetworks(true);        // async
        s_scan_n = -1;
    }
    if (s_scan_n != -1) return;
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;
    WifiNet tmp[MAX_SCAN];
    int m = 0;
    for (int i = 0; i < n && m < MAX_SCAN; i++) {
        String ssid = WiFi.SSID(i);
        if (!ssid.length()) continue;
        bool dup = false;               // one entry per name (mesh networks repeat)
        for (int k = 0; k < m && !dup; k++) dup = !strcmp(tmp[k].ssid, ssid.c_str());
        if (dup) continue;
        strncpy(tmp[m].ssid, ssid.c_str(), sizeof(tmp[m].ssid) - 1);
        tmp[m].ssid[sizeof(tmp[m].ssid) - 1] = 0;
        tmp[m].rssi = WiFi.RSSI(i);
        tmp[m].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        m++;
    }
    WiFi.scanDelete();
    portENTER_CRITICAL(&s_mux);
    memcpy(s_scan, tmp, sizeof(WifiNet) * m);
    s_scan_n = n < 0 ? 0 : m;
    portEXIT_CRITICAL(&s_mux);
}

void link_ota_start(uint16_t port, const char *path) {
    if (s_ota_state == OTA_RUNNING) return;
    s_ota_port = port;
    strncpy(s_ota_path, path, sizeof(s_ota_path) - 1);
    s_ota_msg[0] = 0;
    s_ota_pct = 0;
    s_ota_state = OTA_RUNNING;
    s_ota_req = true;
}

void link_ota_status(int *state, int *pct, char *msg, size_t n) {
    *state = s_ota_state;
    *pct = s_ota_pct;
    strncpy(msg, s_ota_msg, n - 1);
    msg[n - 1] = 0;
}

// Download the image into the other OTA slot. False with s_ota_msg set on failure.
static bool ota_download(const char *host) {
    WiFiClient client;
    HTTPClient http;
    http.setTimeout(15000);
    if (!http.begin(client, host, s_ota_port, s_ota_path)) {
        snprintf(s_ota_msg, sizeof(s_ota_msg), "Cannot reach the bridge");
        return false;
    }
    const char *keys[] = {"x-MD5"};
    http.collectHeaders(keys, 1);
    int code = http.GET();
    if (code != 200) {
        snprintf(s_ota_msg, sizeof(s_ota_msg), code < 0 ? "Download failed (%d)" : "Bridge answered HTTP %d", code);
        http.end();
        return false;
    }
    int len = http.getSize();
    String md5 = http.header("x-MD5");
    if (len <= 0 || !Update.begin(len, U_FLASH)) {
        snprintf(s_ota_msg, sizeof(s_ota_msg), len <= 0 ? "No size from the bridge" : "%s", Update.errorString());
        http.end();
        return false;
    }
    if (md5.length() == 32) Update.setMD5(md5.c_str());
    WiFiClient *s = http.getStreamPtr();
    static uint8_t buf[4096];
    int got = 0;
    uint32_t last = millis();
    while (got < len) {
        int avail = s->available();
        if (avail <= 0) {
            if (millis() - last > 15000 || (!s->connected() && !s->available())) {
                snprintf(s_ota_msg, sizeof(s_ota_msg), "Download stopped at %d%%", (int)((int64_t)got * 100 / len));
                Update.abort();
                http.end();
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        int n = s->read(buf, avail < (int)sizeof(buf) ? avail : (int)sizeof(buf));
        if (n <= 0) continue;
        if (got == 0 && buf[0] != 0xE9) {
            snprintf(s_ota_msg, sizeof(s_ota_msg), "Not a firmware image");
            Update.abort();
            http.end();
            return false;
        }
        if ((int)Update.write(buf, n) != n) {
            snprintf(s_ota_msg, sizeof(s_ota_msg), "Flash write failed: %s", Update.errorString());
            Update.abort();
            http.end();
            return false;
        }
        got += n;
        s_ota_pct = (int)((int64_t)got * 100 / len);
        last = millis();
    }
    http.end();
    if (!Update.end()) {                // checks the MD5, then makes the new slot the boot one
        snprintf(s_ota_msg, sizeof(s_ota_msg), "%s", Update.errorString());
        return false;
    }
    return true;
}

// Runs in the net task. Writes the new firmware to the other OTA slot; the bootloader
// switches to it only after the image has been verified (size and MD5 from the bridge).
static void run_ota() {
    s_ota_req = false;
    char host[40];
    strncpy(host, s_bridge_host, sizeof(host) - 1);
    host[sizeof(host) - 1] = 0;
    if (!host[0] || WiFi.status() != WL_CONNECTED) {
        snprintf(s_ota_msg, sizeof(s_ota_msg), "Not connected to the bridge");
        s_ota_state = OTA_FAILED;
        return;
    }
    Serial.printf("OTA: http://%s:%u%s\n", host, s_ota_port, s_ota_path);
    stop_ws();                          // no telemetry while downloading
    // Our own loop instead of HTTPUpdate: that checks the image's first byte the moment the
    // headers are in, without waiting for it, and fails at random ("Verify Bin Header Failed").
    if (ota_download(host)) {
        s_ota_pct = 100;
        s_ota_state = OTA_DONE;
        Serial.println("OTA: done, restarting");
        vTaskDelay(pdMS_TO_TICKS(2000));    // let the screen show "done"
        ESP.restart();
    }
    Serial.printf("OTA failed: %s\n", s_ota_msg);
    s_ota_state = OTA_FAILED;
    start_ws(host, BRIDGE_PORT);        // back to normal
}

static void net_task(void *) {
    WiFi.mode(WIFI_STA);                // always on, so the Settings page can scan
    WiFi.setSleep(false);               // modem sleep adds 100+ ms of latency
    WiFi.setAutoReconnect(true);
    wifi_connect();
    bool was_up = false;
    uint32_t last_status = 0;
    for (;;) {
        if (s_reconfig) {
            portENTER_CRITICAL(&s_mux);
            s_cfg = s_pending;
            s_reconfig = false;
            portEXIT_CRITICAL(&s_mux);
            store_settings(s_cfg);
            was_up = false;
            s_beacon.stop();
            wifi_connect();
        }
        if (s_ota_req) run_ota();
        poll_serial();
        poll_scan();
        bool up = WiFi.status() == WL_CONNECTED;
        if (up && !was_up) {
            Serial.printf("WiFi: connected, IP %s\n", WiFi.localIP().toString().c_str());
            s_beacon.begin(BEACON_PORT);
            if (s_cfg.bridge[0]) start_ws_configured(s_cfg.bridge);
        } else if (!up && was_up) {
            Serial.println("WiFi: connection lost");
            s_beacon.stop();
        }
        was_up = up;
        if (up) {
            if (!s_cfg.bridge[0]) poll_beacon();
            if (s_ws_started) s_ws.loop();
        }
        send_commands();
        if (millis() - last_status > 200) {
            update_status();
            last_status = millis();
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void link_begin() {
    load_settings(s_cfg);
    s_line = (char *)heap_caps_malloc(SERIAL_LINE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_line) s_line = (char *)malloc(SERIAL_LINE_MAX);
    s_cmd_queue = xQueueCreate(CMD_QUEUE_LEN, CMD_MAX);
    g_cmd_sink = queue_command;
    xTaskCreatePinnedToCore(net_task, "net", 8192, nullptr, 3, nullptr, 0);
}

void link_status(LinkStatus &out) {
    portENTER_CRITICAL(&s_mux);
    out = s_status;
    portEXIT_CRITICAL(&s_mux);
}
