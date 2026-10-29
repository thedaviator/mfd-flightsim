#pragma once
// Public UI entry points. The UI is portable LVGL code: the firmware and the PC preview
// both compile it.
#include <stddef.h>
#include <stdint.h>

#include "netcfg.h"

struct UiPlatform {
    // Short link label for the top bar ("WIFI", "USB", "SEARCH", "NO WIFI") and whether
    // the link is up.
    void (*link_label)(char *buf, size_t n, bool *ok);
    // Multi-line text for the system status window (long-press the XPDR box).
    void (*sys_info)(char *buf, size_t n);
    // Settings page
    void (*get_settings)(NetSettings &out);
    void (*save_settings)(const NetSettings &s);
    void (*scan_start)();
    int (*scan_results)(WifiNet *out, int max);     // -1 while scanning
    // Two short lines for the Settings page: WiFi state, bridge state.
    void (*net_status)(char *wifi, size_t nw, char *bridge, size_t nb, bool *wifi_ok, bool *bridge_ok);
    // Firmware update over WiFi (from the bridge).
    const char *fw_version;
    void (*ota_start)(uint16_t port, const char *path);
    void (*ota_status)(int *state, int *pct, char *msg, size_t n);    // state: 0 idle 1 running 2 failed 3 done
    // Screen sleep: backlight switch, and the saved timeout in minutes (0 = never).
    void (*backlight)(bool on);
    int (*load_sleep_min)();
    void (*save_sleep_min)(int minutes);
    // Small UI preferences kept in flash (e.g. "top_nav": which NAV radio the top bar shows).
    int (*load_pref)(const char *key, int def);
    void (*save_pref)(const char *key, int value);
};

// Boot splash (before ui_init): show it, then hide it when the UI is about to start.
void ui_splash_show(const char *fw_version);
void ui_splash_hide();

void ui_init(const UiPlatform &platform);
// Call every few ms: takes a telemetry snapshot and updates the visible pages.
void ui_tick(uint32_t now_ms);

// Pages, in softkey order.
enum PageId { PG_PFD, PG_EIS, PG_RADIO, PG_MAP, PG_NRST, PG_WX, PG_AP, PG_SETUP, PG_COUNT };
void ui_show_page(PageId id);
const char *ui_page_key();          // page shown (right one in split view), e.g. "NRST"
void ui_set_split(bool split);
