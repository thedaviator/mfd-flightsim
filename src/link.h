#pragma once
// Data link to the PC bridge: WebSocket over WiFi and/or newline-delimited JSON over
// the UART serial port. Both run at once; whichever delivers data is used.
#include <stddef.h>
#include <stdint.h>

#include "netcfg.h"

enum LinkKind { LINK_NONE, LINK_WIFI_SEARCH, LINK_WS, LINK_SERIAL };

struct LinkStatus {
    LinkKind kind;
    bool wifi_configured;
    bool wifi_up;
    char ssid[33];
    char ip[16];            // this display's address
    char bridge[48];        // bridge host:port when known
    uint32_t packets;       // total packets received
    uint32_t bad_packets;
};

void link_begin();                  // loads saved settings, starts the network task on core 0
void link_status(LinkStatus &out);

// Settings are stored in flash (NVS). Saving applies them immediately.
void link_get_settings(NetSettings &out);
void link_save_settings(const NetSettings &s);

// Firmware update over WiFi from the connected bridge (HTTP). The display restarts into
// the new firmware when it has been written and verified.
enum OtaState { OTA_IDLE, OTA_RUNNING, OTA_FAILED, OTA_DONE };
void link_ota_start(uint16_t port, const char *path);
void link_ota_status(int *state, int *pct, char *msg, size_t n);

// WiFi scan: start, then poll. Returns the number of networks, or -1 while scanning.
void link_scan_start();
int link_scan_results(WifiNet *out, int max);
