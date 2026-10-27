# esp32-mfd7: notes for Claude

Garmin-style flight-sim MFD on a **Waveshare ESP32-S3-Touch-LCD-7 Rev1.2** (800x480 RGB, GT911,
CH422G expander, ESP32-S3 16 MB flash + 8 MB octal PSRAM), fed by a Windows bridge app
(MSFS 2024 via Python-SimConnect, X-Plane 12 via UDP). README.md describes the product,
protocol and layout; this file is what you need to know to work on it safely.

## Hardware facts learned the hard way

- **Two USB-C ports.** `USB` = power for normal use (the native USB never enumerates on the PC).
  `UART1` (CH343, shows as "USB-Enhanced-SERIAL CH343") = flashing only.
- **On the UART1 port the board always boots into download mode** (the CH343 control lines hold
  GPIO0 low): the screen stays dark there. This is normal. Run it from the USB port.
- **USB flashing:** slide switch next to BOOT to `UART1` (on `UART2` nothing gets through),
  cable in UART1, hold BOOT, tap RESET, release BOOT, then flash. Use `--before no-reset`.
- **3D-printer slicers (UltiMaker Cura) grab every COM port** and break esptool. Close them.
- The panel's pixels are **7.7 % wider than tall** (154.2 x 85.9 mm for 800x480). All round /
  rotated drawing multiplies horizontal offsets by `XS` (src/ui_priv.h); `dr_circle` / `dr_arc`
  draw aspect-corrected ellipses. Keep doing this for any new round graphics.
- Backlight is on/off only (CH422G EXIO2), no PWM: screen sleep "dims" with a 60 % black overlay
  for 30 s, then switches the backlight off (ui_shell.cpp). Sleep timeout (no flight data and no
  touch; OFF/5/10/30 min, default 10) is in NVS namespace "ui", key "sleep_min" (main.cpp).
  Touch is GT911 at 0x5D (reset via EXIO1); it keeps working with the backlight off.
- Build from a local disk: on a network share the ESP-IDF build cannot find its own files.
  Copy the project to a local folder first (robocopy /E /XD .pio pyi-work).

## Display stability (do not regress)

The frame buffer is in PSRAM. On Arduino-ESP32 2.x (IDF 4.4) the picture jumped and colours
swapped (DMA starvation). The fix, all of which is needed:
- Arduino-ESP32 **3.x via pioarduino** (`platform = ...pioarduino.../55.03.312-1/...` in platformio.ini)
- RGB **bounce buffers** (board.cpp, `LCD_BOUNCE_LINES` 20) at 14 MHz with wide h-porches (config.h)
- `custom_sdkconfig` in platformio.ini: 64-byte data cache lines, XIP from PSRAM, LCD ISR in IRAM.
  pioarduino rebuilds the Arduino core with these (first build 20-40 min, then ~1 min).
- `LV_DISP_DEF_REFR_PERIOD` 33 (30 fps cap).

## Building (Windows)

- **Run PlatformIO from PowerShell, not Git Bash/MSYS** (pioarduino refuses MSYS). From Bash,
  strip `MSYSTEM*` env vars (tools/make_release.py does this).
- First build after a fresh clone downloads ESP-IDF 5.5 and recompiles the core. If CMake fails in
  "git-data/grabRef.cmake", a parent folder (e.g. the user's home) is an empty git repo: run
  `git init` + an empty commit inside `~/.platformio/packages/framework-espidf` to stop the search.
- `partitions.csv` is the standard 16 MB table (NVS at 0x9000 holds the WiFi settings).
- Firmware: `python -m platformio run` (PowerShell). Preview: `python tools/ui_preview/preview.py`
  renders every page on the PC and checks touch flows (needs `pip install ziglang pillow`).
  Use it before flashing; it compiles the same UI code.
- Shell heredocs in the Bash tool mangle `\n` inside Python strings: write edit scripts to a file.

## Releases and updates

- Versions: `FW_VERSION` in src/config.h ("mfd7 x.y.z") and `APP_VERSION` in
  bridge/mfd_bridge_app.py. Bump both for a release.
- `python tools/make_release.py` (PowerShell) -> release/MFD-Bridge-Setup-<ver>.exe (Inno Setup;
  bundles the app, firmware parts, airport DB, Quick Start, VC++ runtime check, firewall rule for
  TCP 8080/8082), release/firmware/ (parts + parts.json with version), release/webflash/.
- **Never flash a merged image** (it would erase NVS = WiFi settings). Flash parts at
  0x0 bootloader, 0x8000 partitions, 0xe000 boot_app0, 0x10000 firmware.
- **Updates over WiFi (normal path):** the bridge serves the bundled firmware on HTTP :8082 with
  x-MD5 and puts `"fwu"` in its packets (also in the no-sim heartbeat). The display shows
  INSTALL on its Settings page when `version_cmp` says it's newer, downloads into the other OTA
  slot (HTTPClient + Update in link.cpp `ota_download`), verifies the MD5, restarts.
- Do not go back to HTTPUpdate: it peeks the first byte right after the headers without waiting
  and fails at random with "Verify Bin Header Failed" (hit on 1.1.6 -> 1.1.7). Firmware up to
  1.1.7 still uses it, so the bridge sends the headers and the first 8 KB in one write.
- Installing a new MFD-Bridge-Setup on the sim PC is all an end user does; the display then
  offers the update.

## Network

- Bridge: WebSocket :8080, UDP beacon :8081 ("MFDBRIDGE <port> <host>"), firmware HTTP :8082.
- The display follows the first beacon it hears; with MFD Bridge running on two PCs it can
  attach to either (Settings -> BRIDGE PC pins one). Windows marks home networks Public; the
  installer's firewall rule covers all profiles.

## Branding (AeroRelics)

- Artwork in branding/. `python tools/make_branding.py` regenerates src/splash_img.cpp (800x480
  RGB565, squeezed by XS for the pixel aspect, "Version 2.5" painted out; ui_splash.cpp draws
  the real version there, right-aligned to x=371). Re-run it when the splash art changes.
- make_release builds the icon, the app banner and the setup bitmaps from the same artwork
  (release/build/branding/). The setup welcome page is the full artwork, picked by page height
  from welcome_314/392/471/628.bmp in [Code] InitializeWizard. AppName/AppId/APPDATA stay
  "MFD Bridge" (renaming would break upgrades and settings); AppPublisher is AeroRelics.
- The splash shows for SPLASH_MS (config.h) while WiFi connects. The preview's run() must not
  call ui_tick() before ui_init() (it crashes): s_ui_ready in tools/ui_preview/main.cpp.

## Terrain (src/terrain.*, ui_map.cpp, bridge/terrain_sd.py)

- SD card: SPI2 on GPIO 11 (MOSI) / 12 (CLK) / 13 (MISO); chip select is CH422G EXIO4, held low,
  driver runs with SDSPI_SLOT_NO_CS (board_sd_begin). FAT32 only (exFAT is not enabled).
- Card layout: TERRAIN/INFO.TXT ("level z x0 y0 x1 y1" per covered area), TERRAIN/z/x/y.BIN =
  256x256 int16 LE metres (Web Mercator, row 0 north). All-water tiles are not written (listed
  in z/OCEAN.TXT so a rerun skips them); the display treats a missing covered tile as water.
  Levels: 5, 7 world; 8, 10 US regions. ~50k tiles / 4.1 GB for world + all US.
- Display: 24 tile buffers (3 MB PSRAM), loader task on core 0 reads in 16 KB chunks through
  internal RAM (sdspi into PSRAM is per-sector). Map terrain is an image with a 48 px margin,
  shifted for small moves, re-rendered on bigger changes (terr_render, mercator exact every
  16 px). terrain_pick_level: coarsest level with pixels <= 3 screen px (cache limit).
  Not covered / still loading: falls back to coarser levels.
- Preview: tools/ui_preview/terrain/ (made by terrain_sd.py --area around the demo flight).
- Not yet run on the hardware (as of 2026-10-08): SD mount, read speed, render time.

## Weather (bridge/wx.py, bridge/metar.py)

- `--wx live` (default): RainViewer radar + NOAA GOES GLM lightning; `synthetic`; `off`. Both
  weather modes send the same `wx` packet: a 96x96 run-length radar grid + strikes + status
  lines. It goes as its own WebSocket message: the display's WebSockets library drops frames
  over 15 KB (`WEBSOCKETS_MAX_DATA_SIZE`).
- RainViewer's free API: zoom 7 max (the bridge uses z6 x 512 px tiles), only the "Universal
  Blue" colour scheme whatever you ask for, no nowcast. Colours are mapped back to dBZ with the
  table in wx.py (from rainviewer.com/files/rainviewer_api_colors_table.csv).
- GLM: `noaa-goes19` / `noaa-goes18` S3 buckets, `GLM-L2-LCFA/YYYY/DDD/HH/`, read with h5py.
  `flash_time_offset_of_first_event` is a scaled, _Unsigned int16 (h5py does not apply that).
  Quality flags 1/3/5 are big flashes in strong storms: keep them, drop only -1.
- METARs: aviationweather.gov `api/data/metar?ids=...&format=json`, refreshed every 10 min.
- The display draws the grid into a TRUE_COLOR_ALPHA image in PSRAM (ui_wx.cpp `raster`) and
  redraws it only when the grid, heading, range or position changes.
  `lv_draw_polygon` mis-fills the radar sector (fills outside the straight edges): use a mask.
- Preview: `python tools/ui_preview/preview.py --live` records with real weather and METARs.
- Check which `python` is on PATH: other programs (Inkscape, for one) install their own Python
  without pip or these packages. Call the right interpreter by its full path if needed.

## Direct-To (src/nav_dct.*, ui_dct.cpp, bridge navdb.find_airports)

- The airport database stays on the bridge: the D-> window sends `apt_find` per keystroke and
  the bridge answers with an `apt` packet (mfd_bridge.apt_packet). Ranking: exact ident, then
  IATA/FAA/GPS codes and ident prefixes together, nearest first (so "KAB" -> KABE, not Kariba).
- navdb.pkl.gz now holds city + other codes per airport (11-field tuples; the loader still reads
  old 9-field caches, which then have no city/codes). Rebuild with `python bridge/navdb.py`
  after changing what it stores (uses the CSVs in bridge/data, no download when present).
- Guidance runs on the display (ui_tick -> dct_update at 10 Hz) and dct_apply() overwrites
  g_t.nav after every snapshot, so PFD / map read Direct-To values through the usual NavInfo.
  Conventions match the bridge: DTK/BRG magnetic (true - magvar), XTK + = right of course.
- The shell's softkeys were narrowed (76 px) to fit the D-> key between AP and SPLIT; the
  window covers only the page area so D-> closes it again.
- Preview: preview.py writes out/apt.jsonl (the bridge's answers for the idents the script types
  and the nearest airports); main.cpp replays them 40 ms after each apt_find.
- Math check: scratchpad test compared nav_dct against navdb.distance_bearing/offset (22 cases,
  incl. JFK-LHR great circle). Not yet run on the hardware (as of 2026-10-20).

## Status / next ideas (as of 2026-10-08)

Working on hardware: all pages, split screen, WiFi setup page, demo + MSFS 2024 data,
WiFi firmware updates, stable picture, aspect correction (user to confirm roundness).
Real weather (live radar, lightning, METARs) added 2026-10-08 in 1.1.0; display updated to 1.1.0
and running with MSFS. Screen sleep added in 1.1.1. AeroRelics splash and branded app/setup in 1.1.2. Terrain from SD (TOPO / REL ALT) in 1.1.3.
Direct-To (D-> softkey, map CDI / track vector / WPT-DIS-DTK-TRK-ETE strip) in 1.3.0
(2026-10-20). 1.3.1 (2026-10-21): softkeys reordered by task, PFD MAP NRST WX RADIO AP EIS,
then D-> SPLIT settings; flashed over UART1 and running on the board, not yet flown.
MFD-Bridge-Setup-1.3.1.exe built (bundles fw 1.3.1); the bridge needs >= 1.3.0 for lookups.
1.3.2 (2026-10-27): PARK BRAKE annunciation on the PFD (pfd.pbrk / pfd.gnd from the bridge:
MSFS BRAKE PARKING POSITION, X-Plane parking_brake_ratio >= 0.5; yellow when set, red flashing
when set and GS > 10 kt or airborne). Previewed (script injects packets via preview_inject);
installer built, not yet on the display or tried in a sim.
Licensing (2026-10-27): MIT, "Copyright (c) 2026 AeroRelics - David Eckert" (LICENSE).
THIRD_PARTY_NOTICES.txt lists bundled components + data sources (keep it current when adding
dependencies; Python-SimConnect is AGPL-3.0, esptool GPL-2.0+, Arduino core / WebSockets LGPL).
Setup shows installer/LICENSE_AGREEMENT.txt ("use at your own risk" + MIT) with an agree tick
box replacing the radios (mfd_bridge.iss BuildAgreeBox), installs LICENSE.txt, TERMS.txt and the
notices, Start menu "License and notices". App: About button. Display: status window lines.
New boards: esptool's "No serial data received" on the CH343 port means the UART1/UART2 slide
switch is in the wrong position; move it, then repeat BOOT + RESET.
Ideas: keep the Direct-To in NVS across restarts; Direct-To to VORs/NDBs; a full HSI page.
