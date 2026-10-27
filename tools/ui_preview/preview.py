#!/usr/bin/env python3
"""Render every MFD screen on the PC, without hardware.

1. Records 20 seconds of the bridge's demo flight (the exact JSON the display receives).
2. Compiles the firmware's UI and telemetry parser with LVGL for the PC.
3. Plays the recording into it, taps through the touch flows (frequency entry, squawk,
   autopilot, nearest-airport tuning, split screen) and checks the commands the display
   sends back, then writes PNG screenshots to tools/ui_preview/out/.

    pip install ziglang pillow
    python tools/ui_preview/preview.py
    python tools/ui_preview/preview.py --live    # real radar, lightning, METARs (internet)

Run `pio pkg install` (or a firmware build) once first so LVGL and ArduinoJson are downloaded.
"""
import glob
import json
import os
import subprocess
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.normpath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(HERE, "out")
UI_SOURCES = ["telemetry.cpp", "nav_dct.cpp", "ui_common.cpp", "ui_shell.cpp", "ui_pfd.cpp", "ui_eis.cpp", "ui_radio.cpp",
              "ui_map.cpp", "ui_nrst.cpp", "ui_wx.cpp", "ui_ap.cpp", "ui_dct.cpp", "ui_setup.cpp", "ui_splash.cpp",
              "splash_img.cpp", "terrain.cpp"]
# Idents the touch script types into the Direct-To window (every prefix is looked up).
DCT_IDENTS = ["KABE", "KPHL", "KJFK"]


class FakeMetars:
    """METARs for the NEAREST page: one of each flight category, and some airports without."""
    REPORTS = [("081454Z 29010KT 10SM FEW070 SCT150 22/10 A2991", "VFR"),
               ("081454Z 32008KT 5SM BR OVC025 14/12 A2985", "MVFR"),
               ("081454Z 00000KT 2SM -RA BR OVC007 12/11 A2979", "IFR"),
               ("081454Z 18004KT 1/4SM FG VV002 11/11 A2980", "LIFR")]

    def request(self, idents):
        self.idents = idents

    def get(self, ident):
        i = sum(map(ord, ident))
        return self.REPORTS[i % 4] if i % 5 else None

    def close(self):
        pass


def record_packets(path, seconds=60.0, hz=30, live=False):
    sys.path.insert(0, os.path.join(PROJECT, "bridge"))
    import navdb
    from mfd_bridge import FAST_HZ, MID_EVERY, Packets, apt_packet
    from sim_demo import DemoBackend
    from simstate import SimState
    from wx import SyntheticWeather

    db = navdb.NavDB(log=lambda m: None)
    args = types.SimpleNamespace(wx="synthetic", plan=None, min_rwy=2500, metar=False)
    packets = Packets(args, db)
    packets.metar = FakeMetars()          # no internet needed
    # Pretend the bridge offers newer firmware, to exercise the WiFi update UI.
    packets.fw_offer = {"ver": "mfd7 9.9.9", "port": 8082, "path": "/firmware.bin", "size": 1600000}
    clock = [0.0]
    packets.wx = SyntheticWeather(seed=7, clock=lambda: clock[0])
    sim = DemoBackend(db, log=lambda m: None)
    state = SimState()
    dt = 1.0 / hz
    if live:
        # Real radar, lightning and METARs around the demo flight; wait until they are in.
        import time
        from metar import MetarService
        from wx import LiveWeather
        packets.wx, packets.metar = LiveWeather(print), MetarService(print)
        sim.poll(state, dt)
        for _ in range(45):
            pkt = packets.fast(state, "DEMO")
            plan, act = packets.nav(state, sim, pkt)
            wx = packets.slow(state, plan, act, pkt)[0]["wx"]
            if "grid" in wx and packets.wx.ltng.sat and any("metar" in a for a in pkt["nrst"]):
                break
            time.sleep(1)
        print(f"  radar: {wx['rdr_txt']}, lightning: {wx['ltg_txt']}, {len(wx['strikes'])} strikes")
    nrst_ids = set()
    with open(path, "w") as f:
        for tick in range(int(seconds * hz)):
            sim.poll(state, dt)
            clock[0] += dt * 6       # weather runs 6x fast so strikes build up in 20 s
            pkt = packets.fast(state, "DEMO")
            plan, act = packets.nav(state, sim, pkt)
            if tick % MID_EVERY == 0:
                packets.mid(state, pkt)
            extra = packets.slow(state, plan, act, pkt) if tick % FAST_HZ == 0 else []
            nrst_ids.update(a["icao"] for a in pkt.get("nrst", [])[:5])
            if tick == 0:
                near = (state.lat, state.lon)
            for p in [pkt] + extra:
                f.write(f"{int(tick * dt * 1000)}\t{json.dumps(p, separators=(',', ':'))}\n")
    # The bridge's answers to the Direct-To window's lookups (the preview plays them back).
    queries = {ident[:n] for ident in DCT_IDENTS for n in range(1, len(ident) + 1)} | nrst_ids
    with open(os.path.join(os.path.dirname(path), "apt.jsonl"), "w") as f:
        for q in sorted(queries):
            f.write(f"{q}\t{json.dumps(apt_packet(db, q, near), separators=(',', ':'))}\n")


def main():
    libdeps = glob.glob(os.path.join(PROJECT, ".pio", "libdeps", "*"))
    lvgl = next(iter(glob.glob(os.path.join(PROJECT, ".pio", "libdeps", "*", "lvgl"))), None)
    ajson = next(iter(glob.glob(os.path.join(PROJECT, ".pio", "libdeps", "*", "ArduinoJson", "src"))), None)
    if not (libdeps and lvgl and ajson):
        sys.exit("LVGL / ArduinoJson not found under .pio/libdeps. Run `pio pkg install` first.")
    os.makedirs(OUT, exist_ok=True)
    pk = os.path.join(OUT, "packets.jsonl")
    terrain = os.path.join(HERE, "terrain", "TERRAIN", "INFO.TXT")
    if not os.path.exists(terrain):
        print("Fetching terrain tiles around the demo flight ...")
        subprocess.run([sys.executable, os.path.join(PROJECT, "bridge", "terrain_sd.py"), os.path.join(HERE, "terrain"),
                        "--area", "40.08", "-75.01", "70"], check=True)
    live = "--live" in sys.argv
    print("Recording the bridge's demo flight" + (" with live weather" if live else "") + " ...")
    record_packets(pk, live=live)

    exe = os.path.join(OUT, "preview.exe" if os.name == "nt" else "preview")
    sources = [os.path.join(HERE, "main.cpp"), os.path.join(HERE, "script.cpp")]
    sources += [os.path.join(PROJECT, "src", s) for s in UI_SOURCES]
    sources += glob.glob(os.path.join(lvgl, "src", "**", "*.c"), recursive=True)
    cmd = [sys.executable, "-m", "ziglang", "c++", "-O1", "-w", "-DLV_CONF_INCLUDE_SIMPLE",
           "-I" + os.path.join(PROJECT, "include"), "-I" + lvgl, "-I" + ajson, "-I" + os.path.join(PROJECT, "src"),
           *sources, "-o", exe]
    print("Compiling (the first run takes a minute) ...")
    build = subprocess.run(cmd, capture_output=True, text=True)
    if build.returncode != 0:
        sys.exit(build.stderr[-6000:])
    for old in glob.glob(os.path.join(OUT, "*.png")):
        os.remove(old)
    run = subprocess.run([exe, pk], cwd=OUT)
    if run.returncode != 0:
        sys.exit(f"preview exited with {run.returncode}")

    try:
        from PIL import Image
    except ImportError:
        print(f"Wrote PPM images to {OUT} (pip install pillow to get PNGs)")
        return
    for ppm in glob.glob(os.path.join(OUT, "*.ppm")):
        Image.open(ppm).save(ppm[:-4] + ".png")
        os.remove(ppm)
    print(f"Wrote {len(glob.glob(os.path.join(OUT, '*.png')))} screenshots to {OUT}")


if __name__ == "__main__":
    main()
