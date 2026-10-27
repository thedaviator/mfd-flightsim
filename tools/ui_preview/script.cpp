// Touch flows exercised by the preview, with the commands each one must send.
// Coordinates are screen pixels of the layouts in src/ui_*.cpp (main area starts at y=44).
#include <stdint.h>
#include <stdio.h>

#include "nav_dct.h"
#include "telemetry.h"
#include "ui.h"

void preview_script(void (*click)(int, int, uint32_t), void (*run)(uint32_t), void (*shot)(const char *),
                    void (*expect)(const char *)) {
    auto tap = [&](int x, int y) { click(x, y, 60); };
    // Keypad (full width): keys 103 px apart from x=536, 73 px apart from y=167.
    auto key = [&](int k) { tap(536 + (k % 3) * 103, 167 + (k / 3) * 73); };
    auto digit = [&](int d) { key(d == 0 ? 10 : d - 1); };

    printf("Radio page: COM1 standby entry\n");
    ui_show_page(PG_RADIO);
    run(200);
    digit(1); digit(1); digit(8); digit(3);
    shot("radio_entry");
    key(11);                                    // ENT
    expect("\"radio\":\"com1\",\"mhz\":118.300");
    tap(258, 87);                               // COM1 swap
    expect("\"cmd\":\"swap\",\"radio\":\"com1\"");

    printf("Radio page: squawk 7000\n");
    tap(128, 383);                              // XPDR code field
    digit(7); digit(0); digit(0); digit(0);
    expect("\"code\":\"7000\"");
    run(400);
    shot("radio_squawk");

    printf("Nearest: tap the first airport\n");
    ui_show_page(PG_NRST);
    run(300);
    tap(400, 99);
    expect("\"radio\":\"com1\"");
    shot("nrst_tuned");

    printf("Autopilot: HDG mode, heading bug +10\n");
    ui_show_page(PG_AP);
    run(300);
    tap(556, 78);
    expect("\"key\":\"HDG\"");
    tap(735, 208);
    expect("\"var\":\"hdg\"");
    run(600);
    shot("ap_hdg");

    printf("Map: track up, zoom in\n");
    ui_show_page(PG_MAP);
    run(300);
    tap(56, 104);
    tap(768, 44 + 384 - 96);                    // "+"
    expect("map_range");
    run(300);
    shot("map_trackup");
    tap(160, 44 + 32 + 28);                     // TERRAIN -> TOPO
    run(600);
    shot("map_topo");
    tap(768, 44 + 384 - 32);                    // "-": wider range
    tap(768, 44 + 384 - 32);
    run(600);
    shot("map_topo_wide");
    tap(160, 44 + 32 + 28);                     // TOPO -> REL ALT
    run(600);
    shot("map_rel");
    tap(160, 44 + 32 + 28);                     // REL ALT -> off
    tap(768, 44 + 384 - 96);
    tap(768, 44 + 384 - 96);
    run(200);

    // Direct-To window (ui_dct.cpp): keys 52 px apart from x=12, rows 64 px apart from y=164.
    auto dct_key = [&](int row, int col) { tap(12 + col * 52 + 24, 44 + 120 + row * 64 + 29); };
    const int DCT_SOFTKEY_X = 604, SOFTKEY_Y = 455;
    printf("Direct-To: type KABE, activate\n");
    tap(DCT_SOFTKEY_X, SOFTKEY_Y);              // D-> softkey
    run(200);
    dct_key(2, 7);                              // K
    dct_key(2, 0);                              // A
    dct_key(3, 4);                              // B
    expect("\"cmd\":\"apt_find\",\"q\":\"KAB\"");
    run(200);
    shot("dct_entry");                          // the database fills in the E
    dct_key(1, 2);                              // E
    expect("\"q\":\"KABE\"");
    run(200);
    shot("dct_kabe");
    tap(667, 44 + 260 + 27);                    // ACTIVATE
    run(4000);                                  // the demo flight keeps its own heading: XTK builds up
    shot("map_dct");
    // The recorded flight can't be steered, so restart the course 1.2 nm left of the
    // aircraft: it is then off course to the right, as if it had drifted.
    {
        static Telemetry t;
        telemetry_snapshot(t);
        double d, brg, olat, olon;
        gc_dist_brg(t.nav.lat, t.nav.lon, dct().wpt.lat, dct().wpt.lon, d, brg);
        gc_offset(t.nav.lat, t.nav.lon, 1.2, brg - 90, olat, olon);
        DctTarget wpt = dct().wpt;
        dct_activate(wpt, olat, olon, 0);
    }
    run(500);
    shot("map_dct_offcourse");
    ui_show_page(PG_PFD);
    run(300);
    shot("pfd_dct");
    ui_set_split(true);
    ui_show_page(PG_MAP);
    run(300);
    shot("split_map_dct");
    ui_set_split(false);

    printf("Direct-To: nearest airport, then cancel\n");
    ui_show_page(PG_MAP);
    run(200);
    tap(DCT_SOFTKEY_X, SOFTKEY_Y);
    run(200);
    shot("dct_reopen");                         // opens on the active waypoint
    expect("\"q\":\"KABE\"");                   // ... and refreshes its details
    tap(667, 44 + 36 + 20);                     // first nearest airport (KPNE on the demo flight)
    expect("\"q\":\"KPNE\"");
    run(200);
    tap(667, 44 + 260 + 27);                    // ACTIVATE
    run(1500);
    shot("map_dct_nrst");
    tap(DCT_SOFTKEY_X, SOFTKEY_Y);
    run(200);
    tap(729, 44 + 322 + 27);                    // CANCEL DIRECT-TO
    run(300);
    shot("map_dct_cancelled");

    printf("Weather: radar range, lightning\n");
    ui_show_page(PG_WX);
    run(200);
    tap(760, 44 + 384 - 96);                    // "+": 20 nm
    expect("\"cmd\":\"wx_range\",\"nm\":20.0");
    run(1500);
    shot("wx_radar_20");
    tap(152, 73);
    run(600);
    shot("wx_ltng");

    printf("Split screen\n");
    ui_show_page(PG_MAP);
    ui_set_split(true);
    run(300);
    shot("split_map");
    ui_show_page(PG_EIS); run(300); shot("split_eis");
    ui_show_page(PG_NRST); run(300); shot("split_nrst");
    ui_show_page(PG_AP); run(300); shot("split_ap");
    ui_show_page(PG_RADIO); run(300); shot("split_radio");
    tap(705, 161);                              // COM2 standby: keypad pops up
    run(200);
    shot("split_radio_keypad");
    ui_show_page(PG_WX); run(600); shot("split_wx");

    printf("Screen sleep: 5 min without flight data\n");
    ui_set_split(false);
    ui_show_page(PG_SETUP);
    run(40000);                                 // the recording ends: no more flight data
    tap(163, 44 + 384 - 30);                    // "5 MIN" (the last touch: the timer starts here)
    run(300);
    shot("setup_sleep");
    ui_show_page(PG_PFD);
    run(275000);                                // 4:35: dimmed, with the notice
    shot("sleep_dim");
    run(30000);                                 // 5:05: backlight off
    shot("sleep_off");
    tap(700, 22);                               // wakes it; must not open the XPDR entry
    run(300);
    shot("sleep_woken");
    ui_show_page(PG_SETUP);
    run(300);
    tap(101, 44 + 384 - 30);                    // "OFF"
    run(300);
    ui_set_split(true);

    // The recording has ended; feed parking-brake packets by hand (the rest of the state stays).
    extern void preview_inject(const char *json);
    auto hold = [&](const char *json, int ms) {
        for (int t = 0; t < ms; t += 100) {
            preview_inject(json);
            run(100);
        }
    };
    const char *parked = "{\"src\":\"DEMO\",\"pfd\":{\"gs\":0,\"pbrk\":1,\"gnd\":1}}";
    const char *rolling = "{\"src\":\"DEMO\",\"pfd\":{\"gs\":42,\"pbrk\":1,\"gnd\":1}}";
    printf("Parking brake: caution when parked, flashing warning when rolling\n");
    ui_set_split(false);
    ui_show_page(PG_PFD);
    hold(parked, 600);
    shot("pfd_pbrk_caution");
    hold(rolling, 300);
    shot("pfd_pbrk_warn_a");                    // one of the two is in the "on" half of the flash
    hold(rolling, 500);
    shot("pfd_pbrk_warn_b");
    ui_set_split(true);
    ui_show_page(PG_MAP);
    hold(parked, 400);
    shot("split_pbrk");
    hold("{\"src\":\"DEMO\",\"pfd\":{\"pbrk\":0}}", 300);
    shot("pfd_pbrk_off");
    // Status window (before the update overlay below covers the screen): license line.
    click(700, 22, 700);
    run(300);
    shot("sysinfo_license");
    tap(400, 240);                              // tap closes it

    printf("Settings page\n");
    ui_show_page(PG_SETUP);
    run(300);
    shot("setup");
    tap(200, 44 + 50 + 70);                     // second network: keyboard opens on the password
    run(200);
    shot("setup_keyboard");
    ui_show_page(PG_PFD);
    run(300);
    ui_show_page(PG_SETUP);                     // keyboard closed again
    run(800);
    shot("setup_update");
    tap(707, 361);                              // INSTALL 9.9.9
    run(400);
    shot("ota_progress");

    printf("System status window\n");
    ui_set_split(false);
    ui_show_page(PG_PFD);
    click(700, 22, 700);                        // long press on the XPDR box
    run(300);
    shot("sysinfo");
}
