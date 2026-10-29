#!/usr/bin/env python3
"""MFD Bridge: the Windows app around mfd_bridge.py.

Shows whether a simulator and a display are connected and this PC's address, keeps the
settings, can start with Windows, and updates the display's firmware over USB.
Packaged as a single .exe by tools/make_release.py; also runs as a plain script.
"""
import contextlib
import io
import json
import os
import queue
import re
import socket
import sys
import threading
import time
import tkinter as tk
import webbrowser
from tkinter import filedialog, messagebox, ttk

import asyncio

import mfd_bridge
import navdb
import terrain_sd

APP_NAME = "MFD Bridge"
APP_VERSION = "1.3.3"
FROZEN = getattr(sys, "frozen", False)
BUNDLE = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
APPDATA = os.path.join(os.environ.get("APPDATA", os.path.expanduser("~")), APP_NAME)
SETTINGS_FILE = os.path.join(APPDATA, "settings.json")
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"

DEFAULTS = {"sim": "auto", "xp_host": "127.0.0.1", "plan": "", "wx": "live", "metar": True, "min_rwy": 2500,
            "autostart": False, "start_minimized": False}
SIM_CHOICES = [("Automatic (MSFS or X-Plane)", "auto"), ("Microsoft Flight Simulator", "msfs"),
               ("X-Plane 12", "xplane"), ("Demo flight (no simulator)", "demo")]
WX_CHOICES = [("Live: RainViewer radar, GOES lightning (internet)", "live"),
              ("Simulated storms (offline)", "synthetic"), ("Off", "off")]

GREEN, AMBER, RED, GREY = "#1a9c3c", "#c98a00", "#c62828", "#808890"


def resource(*parts):
    return os.path.join(BUNDLE, *parts)


def firmware_dir():
    """Folder with the flash parts and parts.json ([{"offset": "0x0", "file": ...}, ...])."""
    for p in (resource("firmware"), os.path.join(os.path.dirname(BUNDLE), "release", "firmware")):
        if os.path.exists(os.path.join(p, "parts.json")):
            return p
    return None


def firmware_parts(fw_dir):
    """esptool write-flash arguments: offset file offset file ...
    The parts go to their own offsets so the NVS partition (WiFi settings) is untouched."""
    with open(os.path.join(fw_dir, "parts.json"), encoding="utf-8") as f:
        info = json.load(f)
    listing = info["parts"] if isinstance(info, dict) else info
    args = []
    for p in listing:
        args += [p["offset"], os.path.join(fw_dir, p["file"])]
    return args


def load_settings():
    s = dict(DEFAULTS)
    try:
        with open(SETTINGS_FILE, encoding="utf-8") as f:
            s.update(json.load(f))
    except (OSError, ValueError):
        pass
    if s["wx"] not in [c[1] for c in WX_CHOICES]:   # up to 1.0.5: a checkbox for simulated weather
        s["wx"] = "live" if s["wx"] else "off"
    return s


def save_settings(s):
    os.makedirs(APPDATA, exist_ok=True)
    with open(SETTINGS_FILE, "w", encoding="utf-8") as f:
        json.dump(s, f, indent=2)


def get_autostart():
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as k:
            winreg.QueryValueEx(k, APP_NAME)
            return True
    except OSError:
        return False


def set_autostart(on):
    import winreg
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE) as k:
        if on:
            if FROZEN:
                cmd = f'"{sys.executable}" --minimized'
            else:
                pyw = sys.executable.replace("python.exe", "pythonw.exe")
                cmd = f'"{pyw}" "{os.path.abspath(__file__)}" --minimized'
            winreg.SetValueEx(k, APP_NAME, 0, winreg.REG_SZ, cmd)
        else:
            try:
                winreg.DeleteValue(k, APP_NAME)
            except FileNotFoundError:
                pass


# ---------------------------------------------------------------- bridge thread

class BridgeRunner:
    """Runs mfd_bridge.Bridge on its own asyncio loop in a background thread."""

    def __init__(self, db):
        self.db = db
        self.bridge = None
        self.thread = None
        self.error = None

    def start(self, settings):
        self.stop()
        args = mfd_bridge.make_parser().parse_args([])
        args.sim = settings["sim"]
        args.xp_host = settings["xp_host"] or "127.0.0.1"
        args.plan = settings["plan"] or None
        args.wx = settings["wx"]
        args.metar = bool(settings["metar"])
        args.min_rwy = int(settings["min_rwy"])
        args.firmware_dir = firmware_dir()
        self.error = None
        try:
            self.bridge = mfd_bridge.Bridge(args, db=self.db)
        except Exception as e:          # e.g. a flight plan file that cannot be read
            self.error = str(e)
            mfd_bridge.log(f"Cannot start: {e}")
            return
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        try:
            asyncio.run(self.bridge.run())
        except OSError as e:
            self.error = str(e)
            if getattr(e, "winerror", None) == 10048 or e.errno in (98, 10048):
                self.error = "Port 8080 is in use - is MFD Bridge already running?"
            mfd_bridge.log(f"Bridge stopped: {self.error}")
        except Exception as e:
            self.error = str(e)
            mfd_bridge.log(f"Bridge stopped: {e}")

    def stop(self):
        if self.bridge and self.thread and self.thread.is_alive():
            self.bridge.stop_requested = True
            self.thread.join(timeout=3)
        self.bridge = None
        self.thread = None


# ---------------------------------------------------------------- firmware update dialog

class QueueWriter(io.TextIOBase):
    def __init__(self, q):
        self.q = q

    def write(self, s):
        if s:
            self.q.put(s)
        return len(s)


class FlashDialog(tk.Toplevel):
    STEPS = (
        "1.  On the back of the display, slide the small switch next to BOOT to UART1.\n"
        "2.  Connect the USB cable to the port marked UART1 (not the one marked USB).\n"
        "3.  Hold BOOT, press and release RESET, then release BOOT.\n"
        "4.  Choose the port below and click Update.\n"
        "5.  When it says Done: move the cable to the port marked USB. The display starts.\n"
        "     Your WiFi settings are kept."
    )

    def __init__(self, app):
        super().__init__(app.root)
        self.app = app
        self.title("Update display firmware")
        self.resizable(False, False)
        self.transient(app.root)
        self.q = queue.Queue()
        self.busy = False
        fw = firmware_dir()
        pad = {"padx": 12, "pady": 4}
        ttk.Label(self, text=self.STEPS, justify="left").grid(row=0, column=0, columnspan=3, sticky="w", padx=12, pady=10)
        ttk.Label(self, text="Firmware:").grid(row=1, column=0, sticky="w", **pad)
        ttk.Label(self, text="included with this version" if fw else "not found in this package").grid(row=1, column=1, sticky="w", **pad)
        ttk.Label(self, text="Port:").grid(row=2, column=0, sticky="w", **pad)
        self.port = ttk.Combobox(self, width=48, state="readonly")
        self.port.grid(row=2, column=1, sticky="w", **pad)
        ttk.Button(self, text="Refresh", command=self.refresh_ports).grid(row=2, column=2, **pad)
        self.progress = ttk.Progressbar(self, length=420, maximum=100)
        self.progress.grid(row=3, column=0, columnspan=3, padx=12, pady=8)
        self.status = ttk.Label(self, text="")
        self.status.grid(row=4, column=0, columnspan=3, sticky="w", padx=12)
        self.go = ttk.Button(self, text="Update", command=self.start, state="normal" if fw else "disabled")
        self.go.grid(row=5, column=2, sticky="e", padx=12, pady=12)
        self.fw = fw
        self.refresh_ports()
        self.after(100, self.pump)

    def refresh_ports(self):
        from serial.tools import list_ports
        ports = list(list_ports.comports())
        self.ports = [p.device for p in ports]
        labels = [f"{p.device}  -  {p.description}" for p in ports]
        self.port["values"] = labels
        # Prefer the board's CH343 (UART1 port), then any ESP32 native USB port.
        best = next((i for i, p in enumerate(ports) if "1A86" in p.hwid.upper()), None)
        if best is None:
            best = next((i for i, p in enumerate(ports) if "303A" in p.hwid.upper()), 0 if ports else None)
        if best is not None:
            self.port.current(best)
        else:
            self.port.set("No serial ports - is the cable in the UART1 port?")

    def start(self):
        if self.busy or not self.ports or self.port.current() < 0:
            return
        port = self.ports[self.port.current()]
        self.busy = True
        self.go.configure(state="disabled")
        self.progress["value"] = 0
        self.status.configure(text=f"Connecting on {port} ...")
        # The bridge may hold the port if it was started with --serial; nothing else should.
        threading.Thread(target=self._flash, args=(port,), daemon=True).start()

    def _flash(self, port):
        import esptool
        w = QueueWriter(self.q)
        argv = ["--chip", "esp32s3", "--port", port, "--baud", "921600", "--before", "default-reset",
                "--after", "no-reset", "write-flash", *firmware_parts(self.fw)]
        try:
            with contextlib.redirect_stdout(w), contextlib.redirect_stderr(w):
                esptool.main(argv)
            self.q.put(("done", None))
        except SystemExit as e:
            self.q.put(("done", None) if not e.code else ("fail", f"esptool exited with {e.code}"))
        except Exception as e:
            self.q.put(("fail", str(e)))

    def pump(self):
        try:
            while True:
                item = self.q.get_nowait()
                if isinstance(item, tuple):
                    kind, msg = item
                    self.busy = False
                    self.go.configure(state="normal")
                    if kind == "done":
                        self.progress["value"] = 100
                        self.status.configure(text="Done. Move the cable to the USB port; the display starts.")
                        mfd_bridge.log("Display firmware updated")
                    else:
                        hint = ""
                        if "connect" in msg.lower() or "No serial data" in msg:
                            hint = "  Check the UART1 switch and cable port, then repeat step 3."
                        self.status.configure(text=f"Failed: {msg}.{hint}")
                        mfd_bridge.log(f"Firmware update failed: {msg}")
                    continue
                for line in re.split(r"[\r\n]+", item):
                    m = re.search(r"(\d+(?:\.\d+)?)\s*%", line)
                    if m and "Writing" in line:
                        self.progress["value"] = float(m.group(1))
                        self.status.configure(text="Writing firmware ...")
                    elif "fatal error" in line.lower() or "Failed to connect" in line:
                        self.status.configure(text=line.strip()[:110])
        except queue.Empty:
            pass
        if self.winfo_exists():
            self.after(100, self.pump)


# ---------------------------------------------------------------- main window

def removable_drives():
    """[(root, label)] of removable drives (SD cards, USB sticks), e.g. ("E:\\", "E:  SDCARD  29.7 GB free  FAT32")."""
    import ctypes
    import string
    k32 = ctypes.windll.kernel32
    out = []
    mask = k32.GetLogicalDrives()
    for i, letter in enumerate(string.ascii_uppercase):
        if not mask & (1 << i):
            continue
        root = f"{letter}:\\"
        if k32.GetDriveTypeW(root) != 2:            # DRIVE_REMOVABLE
            continue
        name, fs = ctypes.create_unicode_buffer(64), ctypes.create_unicode_buffer(16)
        if not k32.GetVolumeInformationW(root, name, 64, None, None, None, fs, 16):
            continue                                 # empty card reader
        free = ctypes.c_ulonglong()
        k32.GetDiskFreeSpaceExW(root, ctypes.byref(free), None, None)
        out.append((root, fs.value, free.value, f"{letter}:  {name.value or 'no label'}   {free.value / 1e9:.1f} GB free   {fs.value}"))
    return out


class TerrainDialog(tk.Toplevel):
    """Fills a microSD card with terrain for the display's MAP page (terrain_sd.py)."""

    def __init__(self, app):
        super().__init__(app.root)
        self.app = app
        self.title("Terrain SD card")
        self.resizable(False, False)
        self.transient(app.root)
        self.q = queue.Queue()
        self.builder = None
        pad = {"padx": 12, "pady": 4}
        ttk.Label(self, justify="left", text=(
            "Terrain for the MAP page: the whole world (about 1 km detail) plus finer detail\n"
            "for the US regions ticked below (about 120 m). Downloaded free from the AWS\n"
            "Terrain Tiles; about 15-45 minutes, mostly set by the card's write speed.\n"
            "If it is interrupted, start it again: it continues where it stopped.\n\n"
            "Use a microSD card of 8-32 GB formatted FAT32, in a card reader on this PC.\n"
            "When it is done, put the card into the display's TF slot and restart the display.")
        ).grid(row=0, column=0, columnspan=3, sticky="w", padx=12, pady=10)
        ttk.Label(self, text="SD card:").grid(row=1, column=0, sticky="w", **pad)
        self.drive = ttk.Combobox(self, width=52, state="readonly")
        self.drive.grid(row=1, column=1, sticky="w", **pad)
        ttk.Button(self, text="Refresh", command=self.refresh).grid(row=1, column=2, **pad)
        regions = ttk.Frame(self)
        regions.grid(row=2, column=0, columnspan=3, sticky="w", padx=12, pady=4)
        ttk.Label(regions, text="US detail:").grid(row=0, column=0, sticky="nw", padx=(0, 8))
        self.regions = {}
        for i, (key, (label, _)) in enumerate(terrain_sd.US_REGIONS.items()):
            v = tk.BooleanVar(value=True)
            ttk.Checkbutton(regions, text=label, variable=v, command=self.update_size).grid(
                row=i // 3, column=1 + i % 3, sticky="w", padx=4, pady=1)
            self.regions[key] = v
        self.size = ttk.Label(self, text="")
        self.size.grid(row=3, column=0, columnspan=3, sticky="w", **pad)
        self.progress = ttk.Progressbar(self, length=520, maximum=100)
        self.progress.grid(row=4, column=0, columnspan=3, padx=12, pady=8)
        self.status = ttk.Label(self, text="")
        self.status.grid(row=5, column=0, columnspan=3, sticky="w", padx=12)
        self.go = ttk.Button(self, text="Start", command=self.start)
        self.go.grid(row=6, column=2, sticky="e", padx=12, pady=12)
        self.protocol("WM_DELETE_WINDOW", self.close)
        self.refresh()
        self.update_size()
        self.after(200, self.pump)

    def areas(self):
        return terrain_sd.plan([k for k, v in self.regions.items() if v.get()])

    def update_size(self):
        a = self.areas()
        self.need = terrain_sd.estimate_bytes(a)
        self.size.configure(text=f"{terrain_sd.count_tiles(a):,} tiles to check, about {self.need / 1e9:.1f} GB on the card")

    def refresh(self):
        self.drives = removable_drives()
        self.drive["values"] = [d[3] for d in self.drives]
        if self.drives:
            self.drive.current(0)
        else:
            self.drive.set("No SD card found - insert one and click Refresh")

    def start(self):
        if self.builder:                         # Stop
            self.builder.cancel.set()
            self.go.configure(state="disabled")
            self.status.configure(text="Stopping ...")
            return
        if not self.drives or self.drive.current() < 0:
            return
        root, fs, free, _ = self.drives[self.drive.current()]
        if fs.upper() != "FAT32":
            messagebox.showwarning(APP_NAME, f"The card is formatted {fs}. The display reads FAT32 only.\n\n"
                                   "Format it as FAT32 in Windows (cards up to 32 GB), then try again.", parent=self)
            return
        have = os.path.exists(os.path.join(root, "TERRAIN"))
        if free < self.need * 1.05 and not have:
            messagebox.showwarning(APP_NAME, f"The card has {free / 1e9:.1f} GB free; about {self.need / 1e9:.1f} GB is needed.",
                                   parent=self)
            return
        self.builder = terrain_sd.Builder(root, self.areas(), progress=lambda *a: self.q.put(a),
                                          log=mfd_bridge.log, workers=24)
        self.go.configure(text="Stop")
        self.status.configure(text="Starting ...")
        self.t0 = time.time()
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        try:
            ok = self.builder.run()
            self.q.put(("done", ok))
        except Exception as e:
            self.q.put(("error", str(e)))

    def pump(self):
        try:
            while True:
                m = self.q.get_nowait()
                if m[0] == "done":
                    self.builder = None
                    self.go.configure(text="Start", state="normal")
                    self.status.configure(text="Done. Put the card into the display and restart it." if m[1]
                                          else "Stopped. Start again to continue where it left off.")
                elif m[0] == "error":
                    self.builder = None
                    self.go.configure(text="Start", state="normal")
                    self.status.configure(text=f"Failed: {m[1]}")
                else:
                    done, total, written = m
                    self.progress["value"] = 100.0 * done / max(1, total)
                    el = time.time() - self.t0
                    left = ""
                    if done > 50 and el > 20:
                        rate = (done - getattr(self, "done0", 0)) / el
                        if rate > 0:
                            left = f", about {int((total - done) / rate / 60) + 1} min left"
                    if not hasattr(self, "done0"):
                        self.done0 = done
                    self.status.configure(text=f"{done:,} of {total:,} tiles, {written / 1e9:.2f} GB written{left}")
        except queue.Empty:
            pass
        if self.winfo_exists():
            self.after(300, self.pump)

    def close(self):
        if self.builder:
            if not messagebox.askyesno(APP_NAME, "Stop preparing the card? You can continue later.", parent=self):
                return
            self.builder.cancel.set()
        self.destroy()


class App:
    def __init__(self, root, minimized):
        self.root = root
        self.settings = load_settings()
        self.settings["autostart"] = get_autostart()      # the installer may have set it
        self.log_q = queue.Queue()
        mfd_bridge.LOG_SINKS.append(self.log_q.put)

        root.title(f"{APP_NAME} {APP_VERSION} - AeroRelics")
        root.minsize(620, 640)
        try:
            root.iconbitmap(resource("mfd.ico"))
        except tk.TclError:
            pass
        self._build()
        self.log_line(f"{APP_NAME} {APP_VERSION} starting")

        cache = resource("data", navdb.CACHE_NAME)
        data_dir = os.path.join(APPDATA, "data")
        threading.Thread(target=self._load_db, args=(cache, data_dir), daemon=True).start()
        self.runner = None
        if minimized:
            root.iconify()
        self.root.after(200, self.tick)
        root.protocol("WM_DELETE_WINDOW", self.quit)

    # -------- layout

    def _build(self):
        r = self.root
        style = ttk.Style()
        style.configure("Big.TLabel", font=("Segoe UI", 12, "bold"))
        style.configure("Value.TLabel", font=("Segoe UI", 12))
        # AeroRelics banner (made by tools/make_branding.py; bundled by make_release).
        for p in (resource("banner.png"), os.path.join(os.path.dirname(BUNDLE), "release", "build", "branding", "banner.png")):
            if os.path.exists(p):
                try:
                    self.banner = tk.PhotoImage(file=p)
                except tk.TclError:
                    break
                tk.Label(r, image=self.banner, bg="white", pady=8).pack(fill="x")
                break
        main = ttk.Frame(r, padding=12)
        main.pack(fill="both", expand=True)

        status = ttk.LabelFrame(main, text="Status", padding=10)
        status.pack(fill="x")
        self.dots = {}
        self.values = {}
        for row, (key, label) in enumerate((("sim", "Simulator"), ("display", "Display"), ("pc", "This PC"))):
            c = tk.Canvas(status, width=16, height=16, highlightthickness=0)
            c.grid(row=row, column=0, padx=(0, 8), pady=3)
            self.dots[key] = (c, c.create_oval(2, 2, 14, 14, fill=GREY, outline=""))
            ttk.Label(status, text=label, style="Big.TLabel", width=10).grid(row=row, column=1, sticky="w")
            v = ttk.Label(status, text="...", style="Value.TLabel")
            v.grid(row=row, column=2, sticky="w")
            self.values[key] = v
        ttk.Button(status, text="Copy address", command=self.copy_ip).grid(row=2, column=3, padx=8)
        status.columnconfigure(2, weight=1)

        cfg = ttk.LabelFrame(main, text="Settings", padding=10)
        cfg.pack(fill="x", pady=10)
        ttk.Label(cfg, text="Simulator").grid(row=0, column=0, sticky="w", pady=3)
        self.sim = ttk.Combobox(cfg, state="readonly", width=34, values=[c[0] for c in SIM_CHOICES])
        self.sim.current(next((i for i, c in enumerate(SIM_CHOICES) if c[1] == self.settings["sim"]), 0))
        self.sim.grid(row=0, column=1, sticky="w")
        ttk.Label(cfg, text="X-Plane PC").grid(row=1, column=0, sticky="w", pady=3)
        self.xp_host = ttk.Entry(cfg, width=24)
        self.xp_host.insert(0, self.settings["xp_host"])
        self.xp_host.grid(row=1, column=1, sticky="w")
        ttk.Label(cfg, text="(127.0.0.1 = this PC)", foreground=GREY).grid(row=1, column=2, sticky="w")
        ttk.Label(cfg, text="Flight plan").grid(row=2, column=0, sticky="w", pady=3)
        self.plan = ttk.Entry(cfg, width=36)
        self.plan.insert(0, self.settings["plan"])
        self.plan.grid(row=2, column=1, sticky="w")
        ttk.Button(cfg, text="Browse...", command=self.pick_plan).grid(row=2, column=2, sticky="w")
        ttk.Label(cfg, text="Weather page").grid(row=3, column=0, sticky="w", pady=3)
        self.wx = ttk.Combobox(cfg, state="readonly", width=44, values=[c[0] for c in WX_CHOICES])
        self.wx.current(next((i for i, c in enumerate(WX_CHOICES) if c[1] == self.settings["wx"]), 0))
        self.wx.grid(row=3, column=1, columnspan=2, sticky="w")
        self.metar = tk.BooleanVar(value=self.settings["metar"])
        ttk.Checkbutton(cfg, text="METARs on the NEAREST page (aviationweather.gov, internet)",
                        variable=self.metar).grid(row=4, column=0, columnspan=3, sticky="w", pady=3)
        self.autostart = tk.BooleanVar(value=self.settings["autostart"])
        ttk.Checkbutton(cfg, text="Start with Windows (minimized)", variable=self.autostart).grid(
            row=5, column=0, columnspan=3, sticky="w", pady=3)
        ttk.Button(cfg, text="Apply", command=self.apply).grid(row=6, column=2, sticky="e", pady=(6, 0))

        btns = ttk.Frame(main)
        btns.pack(fill="x")
        ttk.Button(btns, text="Update display over USB...", command=lambda: FlashDialog(self)).pack(side="left")
        ttk.Button(btns, text="Terrain SD card...", command=lambda: TerrainDialog(self)).pack(side="left", padx=(8, 0))
        ttk.Button(btns, text="Quick Start guide", command=self.open_guide).pack(side="left", padx=8)
        ttk.Button(btns, text="Open log folder", command=lambda: os.startfile(APPDATA) if os.path.isdir(APPDATA) else None
                   ).pack(side="left")
        ttk.Button(btns, text="About", command=self.about).pack(side="left", padx=(8, 0))

        logf = ttk.LabelFrame(main, text="Log", padding=6)
        logf.pack(fill="both", expand=True, pady=(10, 0))
        self.log = tk.Text(logf, height=10, font=("Consolas", 9), state="disabled", wrap="none")
        sb = ttk.Scrollbar(logf, command=self.log.yview)
        self.log.configure(yscrollcommand=sb.set)
        self.log.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")

    # -------- actions

    def _load_db(self, cache, data_dir):
        try:
            db = navdb.NavDB(data_dir=data_dir, cache=cache if os.path.exists(cache) else None, log=mfd_bridge.log)
        except Exception as e:
            mfd_bridge.log(f"Airport database unavailable ({e}); NEAREST and map symbols are off")
            db = None
        self.root.after(0, self._db_ready, db)

    def _db_ready(self, db):
        self.runner = BridgeRunner(db)
        self.runner.start(self.settings)

    def pick_plan(self):
        p = filedialog.askopenfilename(title="Flight plan",
                                       filetypes=[("Flight plans", "*.pln *.fms"), ("All files", "*.*")])
        if p:
            self.plan.delete(0, "end")
            self.plan.insert(0, p)

    def apply(self):
        s = self.settings
        s["sim"] = SIM_CHOICES[self.sim.current()][1]
        s["xp_host"] = self.xp_host.get().strip() or "127.0.0.1"
        s["plan"] = self.plan.get().strip()
        s["wx"] = WX_CHOICES[self.wx.current()][1]
        s["metar"] = bool(self.metar.get())
        if s["autostart"] != bool(self.autostart.get()):
            s["autostart"] = bool(self.autostart.get())
            try:
                set_autostart(s["autostart"])
            except OSError as e:
                messagebox.showerror(APP_NAME, f"Could not change the startup setting: {e}")
        save_settings(s)
        if self.runner:
            self.log_line("Restarting with new settings")
            self.runner.start(s)

    def copy_ip(self):
        self.root.clipboard_clear()
        self.root.clipboard_append(mfd_bridge.primary_ip())

    def open_guide(self):
        for p in (resource("QUICKSTART.html"), os.path.join(os.path.dirname(BUNDLE), "QUICKSTART.html")):
            if os.path.exists(p):
                webbrowser.open(f"file:///{p}")
                return
        messagebox.showinfo(APP_NAME, "The Quick Start guide is not included in this copy.")

    def about(self):
        # The notices sit next to the app folder when installed ({app}), at the project root from source.
        notices = os.path.join(os.path.dirname(BUNDLE), "THIRD_PARTY_NOTICES.txt")
        text = (f"{APP_NAME} {APP_VERSION}\n"
                "Copyright (c) 2026 AeroRelics - David Eckert\n\n"
                "Free, open-source software under the MIT License, provided \"as is\" without warranty.\n"
                "Source and updates: github.com/thedaviator/mfd-flightsim\n\n"
                "For flight simulation only. Not certified avionics: never use it for real-world flight or "
                "navigation. Use at your own risk.\n\n"
                "Not affiliated with Microsoft, Laminar Research, Garmin, Avidyne or Waveshare.")
        if os.path.exists(notices):
            if messagebox.askyesno(APP_NAME, text + "\n\nOpen the license and third-party notices?", parent=self.root):
                os.startfile(notices)
        else:
            messagebox.showinfo(APP_NAME, text, parent=self.root)

    def quit(self):
        if self.runner:
            self.runner.stop()
        self.root.destroy()

    # -------- periodic refresh

    def log_line(self, line):
        self.log.configure(state="normal")
        self.log.insert("end", line + "\n")
        n = int(self.log.index("end-1c").split(".")[0])
        if n > 500:
            self.log.delete("1.0", f"{n - 500}.0")
        self.log.see("end")
        self.log.configure(state="disabled")
        try:
            os.makedirs(APPDATA, exist_ok=True)
            with open(os.path.join(APPDATA, "bridge.log"), "a", encoding="utf-8") as f:
                f.write(line + "\n")
        except OSError:
            pass

    def set_status(self, key, color, text):
        c, oval = self.dots[key]
        c.itemconfigure(oval, fill=color)
        self.values[key].configure(text=text)

    def tick(self):
        while not self.log_q.empty():
            self.log_line(self.log_q.get_nowait())
        b = self.runner.bridge if self.runner else None
        if self.runner is None:
            self.set_status("sim", GREY, "loading airport database ...")
        elif self.runner.error:
            self.set_status("sim", RED, self.runner.error)
        elif b:
            names = {"MSFS": "Microsoft Flight Simulator", "XPLANE": "X-Plane", "DEMO": "Demo flight"}
            if b.source:
                self.set_status("sim", GREEN, f"{names.get(b.source, b.source)} - sending data")
            else:
                self.set_status("sim", AMBER, "waiting for MSFS or X-Plane ..." if b.args.sim == "auto"
                                else f"waiting for {dict((v, k) for k, v in SIM_CHOICES)[b.args.sim]} ...")
            if b.displays:
                peer, fw = next(iter(b.displays.items()))
                text = f"connected ({peer})"
                if fw and fw != "?":
                    text += f" - firmware {fw.replace('mfd7 ', '')}"
                    if b.firmware and mfd_bridge.version_tuple(b.firmware.version) > mfd_bridge.version_tuple(fw):
                        text += f"\nUpdate {b.firmware.version.replace('mfd7 ', '')} ready: tap INSTALL on the display's Settings page"
                self.set_status("display", GREEN, text)
            else:
                self.set_status("display", AMBER, "not connected - turn the display on, check its WiFi settings")
        ip = mfd_bridge.primary_ip()
        self.set_status("pc", GREEN if ip else RED, f"{socket.gethostname()}   {ip or 'no network'}")
        self.root.after(500, self.tick)


def main():
    minimized = "--minimized" in sys.argv
    root = tk.Tk()
    App(root, minimized)
    root.mainloop()


if __name__ == "__main__":
    main()
