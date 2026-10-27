#pragma once
// Network settings and WiFi scan results, shared by the link layer and the Settings page.
#include <stdint.h>

struct NetSettings {
    char ssid[33];
    char pass[65];
    char bridge[40];        // "" = find the bridge automatically, else "host" or "host:port"
};

struct WifiNet {
    char ssid[33];
    int8_t rssi;
    bool secure;
};
