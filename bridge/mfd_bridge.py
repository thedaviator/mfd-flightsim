#!/usr/bin/env python3
"""Flight-sim telemetry bridge for the ESP32-S3 7-inch MFD.

Runs on the flight-sim PC. Reads MSFS 2024 (SimConnect) or X-Plane 12 (UDP datarefs)
at 30 Hz and streams compact JSON to the display over WebSocket (port 8080) and/or USB
serial (921,600 baud). Takes radio, transponder and autopilot commands back from the
display and applies them in the simulator.

    python mfd_bridge.py                     # auto-detect MSFS or X-Plane, WebSocket on :8080
    python mfd_bridge.py --sim demo          # built-in demo flight, no simulator needed
    python mfd_bridge.py --serial COM5       # also (or only) feed the display over USB
    python mfd_bridge.py --plan my.pln       # show this flight plan on the map

See ../README.md for the full packet format.
"""
import argparse
import asyncio
import json
import math
import os
import socket
import sys
import threading
import time

import navdb
from navdb import distance_bearing
from simstate import SimState
from sim_demo import DemoBackend
from sim_xplane import XPlaneBackend
from metar import MetarService
from wx import LiveWeather, SyntheticWeather

FAST_HZ = 30
FW_PORT = 8082                # HTTP: display firmware for updates over WiFi
SLOW_EVERY = FAST_HZ          # nearest / map / weather once a second
MID_EVERY = 3                 # engine / radios / autopilot at 10 Hz
BEACON_PORT = 8081
NRST_COUNT = 25
NRST_RADIUS = 150


LOG_SINKS = []           # extra log receivers (the GUI)


def log(msg):
    line = f"{time.strftime('%H:%M:%S')} {msg}"
    if sys.stdout:          # None in a windowed .exe
        try:
            print(line, flush=True)
        except Exception:
            pass
    for sink in LOG_SINKS:
        sink(line)


def r1(v):
    return round(v, 1)


def com(mhz):
    return f"{mhz:.3f}"


def nav(mhz):
    return f"{mhz:.2f}"


# ---------------------------------------------------------------- packet building

def apt_packet(db, q, near=None):
    """Answer to the display's apt_find: airports matching the typed ident, best first."""
    q = "".join(ch for ch in q.upper() if ch.isalnum())[:7]
    hits = db.find_airports(q, near) if db else []
    return {"apt": {"q": q, "list": [[a.ident, a.name[:40], a.city[:24], round(a.lat, 6), round(a.lon, 6), round(a.elev)]
                                     for a in hits]}}


class Packets:
    def __init__(self, args, db):
        self.args = args
        self.db = db
        self.wx = {"live": lambda: LiveWeather(log), "synthetic": SyntheticWeather}.get(args.wx, lambda: None)()
        self.metar = MetarService(log) if getattr(args, "metar", True) and db else None
        self.fw_offer = None
        self.plan = navdb.load_flight_plan(args.plan) if args.plan else None
        if self.plan:
            log(f"Flight plan {args.plan}: {' '.join(p[0] for p in self.plan)}")
        self.plan_act = 1
        self.map_range = 20.0

    def _active_leg(self, s, backend):
        """(plan, active index) for the map, and (from, to) points of the active leg."""
        plan = self.plan or getattr(backend, "plan", None)
        if plan and len(plan) >= 2:
            if s.to_lat is not None:
                # Follow the simulator's active waypoint if it is one of ours.
                dists = [distance_bearing(s.to_lat, s.to_lon, p[1], p[2])[0] for p in plan]
                i = min(range(len(plan)), key=dists.__getitem__)
                if dists[i] < 2.0 and i > 0:
                    self.plan_act = i
            elif self.plan_act < len(plan) - 1 and distance_bearing(s.lat, s.lon, *plan[self.plan_act][1:])[0] < 1.0:
                self.plan_act += 1
            act = max(1, min(self.plan_act, len(plan) - 1))
            return plan, act, plan[act - 1], plan[act]
        if s.to_lat is not None:
            to = (s.to_id or "WPT", s.to_lat, s.to_lon)
            fr = ("", s.from_lat, s.from_lon) if s.from_lat is not None else None
            return ([fr, to] if fr else [to]), (1 if fr else 0), fr, to
        return [], 0, None, None

    def fast(self, s, src):
        return {
            "src": src,
            "pfd": {"pitch": r1(s.pitch), "roll": r1(s.roll), "ias": r1(s.ias), "tas": round(s.tas),
                    "alt": round(s.alt), "hdg": r1(s.hdg), "trk": r1(s.trk), "vsi": round(s.vsi),
                    "gs": round(s.gs), "slip": round(s.slip, 2), "turn": round(s.turn, 2),
                    "baro": round(s.baro, 2), "oat": round(s.oat), "agl": round(s.agl),
                    "pbrk": int(bool(s.park_brake)), "gnd": int(bool(s.on_ground))},
        }

    def nav(self, s, backend, pkt):
        plan, act, fr, to = self._active_leg(s, backend)
        n = {"lat": round(s.lat, 6), "lon": round(s.lon, 6), "magvar": r1(s.magvar),
             "wind_dir": round(s.wind_dir), "wind_spd": round(s.wind_spd)}
        if to:
            dist, brg = distance_bearing(s.lat, s.lon, to[1], to[2])
            n["to"] = to[0]
            n["to_dist"] = r1(dist)
            n["to_brg"] = round((brg - s.magvar) % 360)
            if fr:
                leg_len, dtk = distance_bearing(fr[1], fr[2], to[1], to[2])
                d13, b13 = distance_bearing(fr[1], fr[2], s.lat, s.lon)
                xtk = math.asin(math.sin(d13 / navdb.EARTH_NM) * math.sin(math.radians(b13 - dtk))) * navdb.EARTH_NM
                n["dtk"] = round((dtk - s.magvar) % 360)
                n["xtk"] = round(xtk, 2)
            if s.gs > 30:
                n["ete"] = round(dist / s.gs * 3600)
        pkt["nav"] = n
        return plan, act

    def mid(self, s, pkt):
        pkt["eis"] = {"rpm": round(s.rpm), "mrp": r1(s.mrp), "ff": r1(s.ff), "oil_t": round(s.oil_t),
                      "oil_p": round(s.oil_p), "fuel_p": r1(s.fuel_p),
                      "cht": [round(v) for v in s.cht], "egt": [round(v) for v in s.egt],
                      "fuel_l": r1(s.fuel_l), "fuel_r": r1(s.fuel_r), "fuel_cap": r1(s.fuel_cap),
                      "volts": r1(s.volts), "amps": r1(s.amps)}
        pkt["rad"] = {"com1_act": com(s.com1_act), "com1_stby": com(s.com1_stby),
                      "com2_act": com(s.com2_act), "com2_stby": com(s.com2_stby),
                      "nav1_act": nav(s.nav1_act), "nav1_stby": nav(s.nav1_stby),
                      "nav2_act": nav(s.nav2_act), "nav2_stby": nav(s.nav2_stby),
                      "xpdr": s.xpdr, "xpdr_mode": s.xpdr_mode}
        pkt["ap"] = {"master": int(s.ap_master), "fd": int(s.ap_fd), "yd": int(s.ap_yd),
                     "lat": s.ap_lat, "lat_arm": s.ap_lat_arm, "vert": s.ap_vert, "vert_arm": s.ap_vert_arm,
                     "hdg": round(s.ap_hdg) % 360, "alt": round(s.ap_alt), "vs": round(s.ap_vs), "ias": round(s.ap_ias)}

    def close(self):
        for svc in (self.wx, self.metar):
            if svc:
                svc.close()

    def slow(self, s, plan, act, pkt):
        """Adds the once-a-second parts to pkt; returns further packets to send with it
        (weather goes on its own, as the display takes at most 15 KB per WebSocket message)."""
        if self.db and (s.lat or s.lon):
            nearest = self.db.nearest_airports(s.lat, s.lon, NRST_COUNT, NRST_RADIUS, self.args.min_rwy)
            pkt["nrst"] = []
            for d, b, a in nearest:
                row = {"icao": a.ident, "name": a.name[:40], "dist": r1(d), "brg": round((b - s.magvar) % 360),
                       "rwy": a.rwy_ft, "freq": a.freq, "ftype": a.ftype, "elev": round(a.elev)}
                m = self.metar.get(a.ident) if self.metar else None
                if m:
                    row["metar"], row["cat"] = m
                pkt["nrst"].append(row)
            if self.metar:
                self.metar.request([a.ident for _, _, a in nearest])
            pts = self.db.map_features(s.lat, s.lon, self.map_range * 1.1)
            map_pts = [[k, i, round(la, 5), round(lo, 5)] for _, k, i, la, lo, _ in pts]
        else:
            map_pts = []
        pkt["map"] = {"fpl": [[p[0], round(p[1], 5), round(p[2], 5)] for p in plan], "act": act, "pts": map_pts}
        if self.fw_offer:
            pkt["fwu"] = self.fw_offer
        if self.wx and (s.lat or s.lon):
            self.wx.update(s.lat, s.lon, s.wind_dir, s.wind_spd)
            return [{"wx": self.wx.packet(s.lat, s.lon)}]
        return []


# ---------------------------------------------------------------- transports

class SerialLink:
    """Newline-delimited JSON over a serial port. Lines from the display that start with
    '{' are commands; anything else is the firmware's log, printed with -v."""

    def __init__(self, port, baud, on_command, verbose):
        import serial
        self.ser = serial.Serial(port, baud, timeout=0.2)
        self.on_command = on_command
        self.verbose = verbose
        self.lock = threading.Lock()
        threading.Thread(target=self._reader, daemon=True).start()
        log(f"Serial link on {port} at {baud} baud")

    def _reader(self):
        buf = b""
        while True:
            try:
                buf += self.ser.read(self.ser.in_waiting or 1)
            except Exception as e:
                log(f"Serial read failed: {e}")
                return
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").strip()
                if text.startswith("{"):
                    self.on_command(text)
                elif text and self.verbose:
                    log(f"[display] {text}")

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def send(self, text):
        with self.lock:
            try:
                self.ser.write(text.encode() + b"\n")
            except Exception as e:
                log(f"Serial write failed: {e}")


def local_broadcasts():
    addrs = {"255.255.255.255"}
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if not ip.startswith("127."):
                addrs.add(ip.rsplit(".", 1)[0] + ".255")
    except OSError:
        pass
    return addrs


def version_tuple(v):
    """'mfd7 1.0.3' -> (7, 1, 0, 3); for comparing firmware versions."""
    import re
    return tuple(int(x) for x in re.findall(r"\d+", v or ""))


def load_firmware(fw_dir):
    """Firmware offered to displays: (version, path to the app image) or None.
    fw_dir holds parts.json ({"version": ..., "parts": [{"offset", "file"}]}) from make_release."""
    if not fw_dir:
        return None
    try:
        with open(os.path.join(fw_dir, "parts.json"), encoding="utf-8") as f:
            info = json.load(f)
    except (OSError, ValueError):
        return None
    if not isinstance(info, dict) or not info.get("version"):
        return None
    app = next((p for p in info["parts"] if p["offset"].lower() in ("0x10000", "65536")), None)
    path = os.path.join(fw_dir, app["file"]) if app else None
    if not path or not os.path.exists(path):
        return None
    return info["version"], path


class FirmwareServer:
    """Serves the display firmware over HTTP for updates over WiFi. The display downloads
    it with ESP32 HTTPUpdate, which checks the x-MD5 header before switching to it."""

    def __init__(self, version, path, port=FW_PORT):
        import hashlib
        import http.server
        self.version, self.path, self.port = version, path, port
        with open(path, "rb") as f:
            data = f.read()
        self.size = len(data)
        md5 = hashlib.md5(data).hexdigest()

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(h):
                if h.path.split("?")[0] != "/firmware.bin":
                    h.send_error(404)
                    return
                log(f"Display {h.client_address[0]} is downloading firmware {version}")
                # Headers and the start of the image in one write: ESP32 HTTPUpdate (in
                # firmware up to 1.1.7) checks the first byte the moment the headers are read,
                # without waiting, and fails ("Verify Bin Header Failed") if it is not there yet.
                head = ("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                        f"Content-Length: {len(data)}\r\nx-MD5: {md5}\r\nConnection: close\r\n\r\n").encode()
                h.close_connection = True
                try:
                    h.wfile.write(head + data[:8192])
                    h.wfile.write(data[8192:])
                except OSError as e:
                    log(f"Firmware download interrupted: {e}")

            def log_message(h, *a):
                pass

        self.httpd = http.server.ThreadingHTTPServer(("0.0.0.0", port), Handler)
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        log(f"Offering display firmware {version} on port {port}")

    def offer(self):
        return {"ver": self.version, "port": self.port, "path": "/firmware.bin", "size": self.size}

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def primary_ip():
    """The address other devices on the LAN reach this PC on (the default-route interface)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))      # no packet is sent; this only picks the interface
        return s.getsockname()[0]
    except OSError:
        return ""
    finally:
        s.close()


def local_ips():
    try:
        return sorted({i[4][0] for i in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)
                       if not i[4][0].startswith("127.")})
    except OSError:
        return []


# ---------------------------------------------------------------- main loop

class Bridge:
    def __init__(self, args, db=None):
        self.args = args
        self.state = SimState()
        self.db = db if db is not None else (None if args.no_navdb else navdb.NavDB(log=log))
        self.packets = Packets(args, self.db)
        self.backend = None
        self.xplane = None
        self.msfs = None
        self.msfs_next_try = 0.0
        self.msfs_connecting = False
        self.ws_clients = set()
        self.serial = None
        self.send_slow_now = True
        self.commands = asyncio.Queue()
        self.stats_t = time.monotonic()
        self.stats_bytes = 0
        # Status for the GUI
        self.source = None              # MSFS / XPLANE / DEMO / None
        self.displays = {}              # peer address -> firmware string
        self.stop_requested = False
        self.firmware = None            # FirmwareServer when firmware is bundled

        if args.sim == "demo":
            self.backend = DemoBackend(self.db, log)
        if args.sim in ("auto", "xplane"):
            self.xplane = XPlaneBackend(args.xp_host, args.xp_port, log)

    # -------- simulator selection

    async def _try_msfs(self):
        self.msfs_connecting = True
        try:
            from sim_msfs import MsfsBackend
            self.msfs = await asyncio.get_running_loop().run_in_executor(None, MsfsBackend, log)
        except ImportError as e:
            log(f"MSFS backend unavailable: {e}")
            self.msfs_next_try = float("inf")
        except Exception as e:
            if self.args.verbose or self.args.sim == "msfs":
                log(f"MSFS: {e}")
            self.msfs = None
        finally:
            self.msfs_connecting = False
            self.msfs_next_try = max(self.msfs_next_try, time.monotonic() + 5.0)

    def _poll_sim(self, dt):
        """Poll the active simulator; returns the source name if live data arrived."""
        if self.args.sim == "demo":
            self.backend.poll(self.state, dt)
            return "DEMO"
        if self.msfs is not None:
            try:
                if self.msfs.poll(self.state, dt):
                    self.backend = self.msfs
                    return "MSFS"
            except ConnectionError as e:
                log(f"MSFS: {e}")
                self.msfs.close()
                self.msfs = None
        elif self.args.sim in ("auto", "msfs") and not self.msfs_connecting and time.monotonic() >= self.msfs_next_try:
            asyncio.ensure_future(self._try_msfs())
        if self.xplane is not None and self.xplane.poll(self.state, dt):
            self.backend = self.xplane
            return "XPLANE"
        self.backend = None
        return None

    # -------- commands from the display

    def on_command_text(self, text, loop=None):
        try:
            cmd = json.loads(text)
        except ValueError:
            return
        if not isinstance(cmd, dict) or "cmd" not in cmd:
            return
        if loop:
            loop.call_soon_threadsafe(self.commands.put_nowait, cmd)
        else:
            self.commands.put_nowait(cmd)

    def _apply(self, cmd):
        if self.args.verbose:
            log(f"command {cmd}")
        if cmd["cmd"] == "map_range":
            try:
                self.packets.map_range = max(1.0, min(200.0, float(cmd.get("nm", 20))))
            except (TypeError, ValueError):
                pass
            self.send_slow_now = True
        elif cmd["cmd"] == "wx_range":
            if self.packets.wx:
                try:
                    # The grid covers the radar's range ahead and to the sides, plus a margin.
                    self.packets.wx.radar_nm = max(5.0, min(200.0, float(cmd.get("nm", 40)))) * 1.05
                except (TypeError, ValueError):
                    pass
            self.send_slow_now = True
        elif cmd["cmd"] == "apt_find":
            # Direct-To window: airports matching the ident typed so far. Answered at once,
            # with or without a simulator.
            s = self.state
            self._broadcast(apt_packet(self.db, str(cmd.get("q", "")), (s.lat, s.lon) if (s.lat or s.lon) else None))
        elif cmd["cmd"] == "hello":
            log(f"Display says hello: {cmd.get('fw', '?')}")
            if cmd.get("crash"):
                log(f"Display restarted after a crash: {cmd['crash']}")
            if cmd.get("_peer"):
                self.displays[cmd["_peer"]] = cmd.get("fw", "?")
            self.send_slow_now = True
        elif self.backend is not None:
            try:
                self.backend.apply_command(cmd)
            except Exception as e:
                log(f"command {cmd} failed: {e}")

    # -------- output

    def _broadcast(self, pkt):
        text = json.dumps(pkt, separators=(",", ":"))
        self.stats_bytes += len(text)
        if self.ws_clients:
            from websockets.asyncio.server import broadcast
            broadcast(self.ws_clients, text)
        if self.serial:
            self.serial.send(text)

    async def ws_handler(self, ws):
        peer = ws.remote_address[0] if ws.remote_address else "?"
        log(f"Display connected from {peer}")
        self.ws_clients.add(ws)
        self.displays[peer] = "?"
        self.send_slow_now = True
        try:
            async for msg in ws:
                if isinstance(msg, str):
                    if '"hello"' in msg:
                        msg = msg.rstrip("}") + f',"_peer":"{peer}"}}'
                    self.on_command_text(msg)
        except Exception:
            pass
        finally:
            self.ws_clients.discard(ws)
            self.displays.pop(peer, None)
            log(f"Display {peer} disconnected")

    async def beacon(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        msg = f"MFDBRIDGE {self.args.port} {socket.gethostname()}".encode()
        while not self.stop_requested:
            for addr in local_broadcasts():
                try:
                    sock.sendto(msg, (addr, BEACON_PORT))
                except OSError:
                    pass
            await asyncio.sleep(2.0)

    async def run(self):
        loop = asyncio.get_running_loop()
        if self.args.serial:
            self.serial = SerialLink(self.args.serial, self.args.baud,
                                     lambda t: self.on_command_text(t, loop), self.args.verbose)
        server = None
        if not self.args.no_ws:
            from websockets.asyncio.server import serve
            server = await serve(self.ws_handler, "0.0.0.0", self.args.port, ping_interval=10, max_size=2 ** 16)
            log(f"WebSocket server on port {self.args.port}. This PC: {primary_ip() or '?'} ({socket.gethostname()})")
            asyncio.ensure_future(self.beacon())
        log(f"Simulator: {self.args.sim}" + (" (waiting for MSFS or X-Plane)" if self.args.sim == "auto" else ""))
        fw = load_firmware(self.args.firmware_dir)
        if fw and not self.args.no_ws:
            try:
                self.firmware = FirmwareServer(*fw)
                self.packets.fw_offer = self.firmware.offer()
            except OSError as e:
                log(f"Firmware updates over WiFi unavailable (port {FW_PORT}: {e})")

        period = 1.0 / FAST_HZ
        tick = 0
        last = time.monotonic()
        next_t = last
        prev_src = None
        while not self.stop_requested:
            next_t += period
            now = time.monotonic()
            dt, last = now - last, now
            while not self.commands.empty():
                self._apply(self.commands.get_nowait())

            src = self._poll_sim(dt)
            if src != prev_src:
                log(f"Source: {src or 'no simulator data'}")
                prev_src = src
                self.source = src

            if src:
                s = self.state
                pkt = self.packets.fast(s, src)
                plan, act = self.packets.nav(s, self.backend, pkt)
                if tick % MID_EVERY == 0:
                    self.packets.mid(s, pkt)
                extra = []
                if tick % SLOW_EVERY == 0 or self.send_slow_now:
                    extra = self.packets.slow(s, plan, act, pkt)
                    self.send_slow_now = False
                self._broadcast(pkt)
                for p in extra:
                    self._broadcast(p)
            elif tick % FAST_HZ == 0:
                # Heartbeat without a simulator; still carries the firmware offer.
                hb = {"src": "NONE"}
                if self.packets.fw_offer:
                    hb["fwu"] = self.packets.fw_offer
                self._broadcast(hb)

            if self.args.verbose and now - self.stats_t >= 10:
                log(f"{len(self.ws_clients)} WebSocket client(s), {self.stats_bytes / (now - self.stats_t) / 1024:.1f} KB/s")
                self.stats_t, self.stats_bytes = now, 0
            tick += 1
            await asyncio.sleep(max(0.0, next_t - time.monotonic()))
            if time.monotonic() - next_t > 0.5:      # fell far behind (PC asleep): resync
                next_t = time.monotonic()
        # Stopped (the GUI restarts the bridge with new settings).
        if server:
            server.close()
            await server.wait_closed()
        for b in (self.msfs, self.xplane):
            if b:
                b.close()
        if self.serial:
            self.serial.close()
        if self.firmware:
            self.firmware.close()
        self.packets.close()


# release/firmware next to the bridge folder, when running from the source tree
DEFAULT_FW_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "release", "firmware")


def make_parser():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--sim", choices=("auto", "msfs", "xplane", "demo"), default="auto")
    ap.add_argument("--port", type=int, default=8080, help="WebSocket port (default 8080)")
    ap.add_argument("--no-ws", action="store_true", help="disable the WebSocket server")
    ap.add_argument("--serial", metavar="COMx", help="also send over this serial port")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--xp-host", default="127.0.0.1", help="X-Plane PC address (default this PC)")
    ap.add_argument("--xp-port", type=int, default=49000)
    ap.add_argument("--plan", metavar="FILE", help="flight plan for the map (.pln or .fms)")
    ap.add_argument("--wx", choices=("live", "synthetic", "off"), default="live",
                    help="weather page: live (RainViewer radar, GOES lightning; needs internet), synthetic "
                         "(generated storms) or off; the sims expose no radar or lightning (default live)")
    ap.add_argument("--no-metar", dest="metar", action="store_false",
                    help="no METARs on the NEAREST page (they come from aviationweather.gov)")
    ap.add_argument("--min-rwy", type=int, default=2500, help="NEAREST: minimum runway length, ft")
    ap.add_argument("--no-navdb", action="store_true", help="skip the airport database (no NEAREST, no map symbols)")
    ap.add_argument("--firmware-dir", default=DEFAULT_FW_DIR,
                    help="folder with parts.json; offered to displays for updates over WiFi")
    ap.add_argument("-v", "--verbose", action="store_true")
    return ap


def main():
    ap = make_parser()
    args = ap.parse_args()
    if args.no_ws and not args.serial:
        ap.error("--no-ws needs --serial")
    try:
        asyncio.run(Bridge(args).run())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    if sys.platform == "win32":
        # Clicking in a Command Prompt window enters QuickEdit mode, which freezes output
        # (and with it this loop) until a key is pressed. Turn QuickEdit off.
        try:
            import ctypes
            k32 = ctypes.windll.kernel32
            h = k32.GetStdHandle(-10)
            mode = ctypes.c_uint32()
            if k32.GetConsoleMode(h, ctypes.byref(mode)):
                k32.SetConsoleMode(h, (mode.value & ~0x0040) | 0x0080)
        except Exception:
            pass
    main()
