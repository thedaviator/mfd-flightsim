"""Built-in demo flight: no simulator needed.

A Cessna-like aircraft flies a loop of Philadelphia-area airports. It follows the
autopilot modes and targets the display sets (heading bug, altitude, vertical speed,
NAV), and it applies radio and transponder changes, so every control on the display
can be tested end to end with nothing but this bridge running.
"""
import math
import random

from navdb import distance_bearing, offset
from simstate import AP_KEYS, RADIOS, XPDR_MODES, SimState, valid_squawk

# (ident, lat, lon). Coordinates are used if the nav database is not loaded.
DEMO_ROUTE = [
    ("KPNE", 40.0819, -75.0106),
    ("KDYL", 40.3330, -75.1223),
    ("KUKT", 40.4352, -75.3819),
    ("KLOM", 40.1375, -75.2651),
    ("KPHL", 39.8719, -75.2411),
]
MAGVAR = -11.5          # Philadelphia: 11.5 deg west
WIND_FROM, WIND_KT = 280.0, 14.0
CYL = 4


def _wrap180(a):
    return (a + 180.0) % 360.0 - 180.0


def _approach(cur, target, rate, dt):
    step = rate * dt
    return target if abs(target - cur) <= step else cur + math.copysign(step, target - cur)


class DemoBackend:
    name = "DEMO"

    def __init__(self, navdb=None, log=print):
        self.log = log
        self.route = []
        for ident, lat, lon in DEMO_ROUTE:
            pt = navdb.find_point(ident) if navdb else None
            self.route.append((ident, *(pt or (lat, lon))))
        self.plan = list(self.route)          # the bridge shows this as the flight plan
        self.leg = 1
        s = self.s = SimState()
        start = self.route[0]
        s.lat, s.lon = offset(start[1], start[2], 3.0, 20.0)
        s.hdg_true = distance_bearing(s.lat, s.lon, self.route[1][1], self.route[1][2])[1]
        s.alt, s.ias, s.tas = 2500.0, 105.0, 110.0
        s.com1_act, s.com1_stby = 121.500, 118.300
        s.com2_act, s.com2_stby = 126.900, 124.350
        s.nav1_act, s.nav1_stby = 112.80, 116.50
        s.nav2_act, s.nav2_stby = 113.20, 110.30
        s.xpdr, s.xpdr_mode = "1200", "ALT"
        s.ap_master, s.ap_fd = True, True
        s.ap_lat, s.ap_vert = "NAV", "VS"
        s.ap_vert_arm = "ALT"
        s.ap_hdg, s.ap_alt, s.ap_vs, s.ap_ias = 30.0, 4500.0, 500.0, 100.0
        s.fuel_l = s.fuel_r = 24.0
        s.fuel_cap = 26.0
        s.magvar = MAGVAR
        s.baro = 30.02
        self.t = 0.0
        self.cht_off = [random.uniform(-15, 15) for _ in range(CYL)]
        self.egt_off = [random.uniform(-30, 30) for _ in range(CYL)]

    # ------------------------------------------------------------ simulation

    def poll(self, state, dt):
        s = self.s
        self.t += dt
        dt = min(dt, 0.2)

        # Lateral guidance
        to = self.route[self.leg]
        dist, brg_true = distance_bearing(s.lat, s.lon, to[1], to[2])
        if dist < 1.0:
            self.leg = (self.leg + 1) % len(self.route)
            to = self.route[self.leg]
            dist, brg_true = distance_bearing(s.lat, s.lon, to[1], to[2])
        if s.ap_lat in ("NAV", "APR"):
            want_true = brg_true
        elif s.ap_lat == "HDG":
            want_true = s.ap_hdg + MAGVAR
        else:
            want_true = s.hdg_true
        # Fly the track: correct heading for wind drift.
        wca = math.degrees(math.asin(max(-1, min(1, WIND_KT * math.sin(math.radians(WIND_FROM - want_true)) / max(s.tas, 40)))))
        err = _wrap180(want_true + wca - s.hdg_true)
        bank_target = max(-20.0, min(20.0, err * 1.6))
        s.roll = _approach(s.roll, bank_target, 6.0, dt)
        turn = 1091.0 * math.tan(math.radians(s.roll)) / max(s.tas, 40)
        s.hdg_true = (s.hdg_true + turn * dt) % 360.0
        s.turn = turn

        # Vertical guidance
        alt_err = s.ap_alt - s.alt
        if s.ap_vert == "VS":
            vs_target = s.ap_vs
            if s.ap_vert_arm == "ALT" and abs(alt_err) < abs(s.vsi) / 10 + 20 and (alt_err > 0) == (s.ap_vs > 0):
                s.ap_vert, s.ap_vert_arm = "ALT", ""
        elif s.ap_vert == "FLC":
            vs_target = math.copysign(min(700, abs(alt_err) * 2), alt_err) if abs(alt_err) > 30 else 0
            if abs(alt_err) < 50:
                s.ap_vert = "ALT"
        elif s.ap_vert == "ALT":
            vs_target = max(-600, min(600, alt_err * 3))
        else:
            vs_target = 0
        s.vsi = _approach(s.vsi, vs_target, 300.0, dt) + random.gauss(0, 6)
        s.alt += s.vsi / 60.0 * dt

        # Speed and attitude
        climb = s.vsi > 200
        ias_target = 85.0 if climb else (s.ap_ias if s.ap_vert == "FLC" else 118.0) if s.vsi > -200 else 128.0
        s.ias = _approach(s.ias, ias_target, 1.5, dt)
        s.tas = s.ias * (1 + s.alt / 1000.0 * 0.02)
        gamma = math.degrees(math.asin(max(-1, min(1, s.vsi / (s.tas * 101.27)))))
        aoa = 2.0 + (110 - s.ias) * 0.08
        s.pitch = aoa + gamma + 0.6 * math.sin(self.t * 0.9) * 0.3 + random.gauss(0, 0.05)
        s.slip = 0.08 * math.sin(self.t * 0.37) + random.gauss(0, 0.01)

        # Ground vector from TAS + wind
        hx = s.tas * math.sin(math.radians(s.hdg_true))
        hy = s.tas * math.cos(math.radians(s.hdg_true))
        wx = -WIND_KT * math.sin(math.radians(WIND_FROM))
        wy = -WIND_KT * math.cos(math.radians(WIND_FROM))
        gx, gy = hx + wx, hy + wy
        s.gs = math.hypot(gx, gy)
        trk_true = math.degrees(math.atan2(gx, gy)) % 360.0
        s.lat, s.lon = offset(s.lat, s.lon, s.gs * dt / 3600.0, trk_true)
        s.hdg = (s.hdg_true - MAGVAR) % 360.0
        s.trk = (trk_true - MAGVAR) % 360.0
        s.agl = s.alt - 200
        s.oat = 15 - s.alt / 1000 * 2
        s.wind_dir, s.wind_spd = WIND_FROM, WIND_KT

        # Engine
        s.rpm = _approach(s.rpm or 2300, 2550 if climb else 2400, 60, dt) + random.gauss(0, 3)
        s.mrp = 29.4 - s.alt / 1000.0 - (0 if climb else 4.5) + random.gauss(0, 0.03)
        s.ff = (12.8 if climb else 9.4) + random.gauss(0, 0.05)
        base_cht = 395 if climb else 365
        base_egt = 1290 if climb else 1395
        s.cht = [round(base_cht + o + 4 * math.sin(self.t * 0.05 + i), 1) for i, o in enumerate(self.cht_off)]
        s.egt = [round(base_egt + o + 6 * math.sin(self.t * 0.07 + i), 1) for i, o in enumerate(self.egt_off)]
        s.oil_t = 188 + 6 * math.sin(self.t * 0.01)
        s.oil_p = 74 + random.gauss(0, 0.3)
        s.fuel_p = 5.6 + random.gauss(0, 0.05)
        burn = s.ff * dt / 3600.0 / 2
        s.fuel_l = max(0.0, s.fuel_l - burn)
        s.fuel_r = max(0.0, s.fuel_r - burn)
        s.volts = 28.2 + random.gauss(0, 0.02)
        s.amps = 11.5 + random.gauss(0, 0.2)

        # Active GPS leg
        fr = self.route[self.leg - 1]
        s.to_id, s.to_lat, s.to_lon = to[0], to[1], to[2]
        s.from_lat, s.from_lon = fr[1], fr[2]

        state.__dict__.update(s.__dict__)
        state.cht, state.egt = list(s.cht), list(s.egt)
        return True

    # ------------------------------------------------------------ commands

    def apply_command(self, cmd):
        s = self.s
        c = cmd.get("cmd")
        radio = cmd.get("radio")
        if c == "freq" and radio in RADIOS:
            setattr(s, f"{radio}_stby", float(cmd["mhz"]))
        elif c == "swap" and radio in RADIOS:
            a, b = f"{radio}_act", f"{radio}_stby"
            va, vb = getattr(s, a), getattr(s, b)
            setattr(s, a, vb)
            setattr(s, b, va)
        elif c == "xpdr" and valid_squawk(cmd.get("code")):
            s.xpdr = cmd["code"]
        elif c == "xpdr_mode" and cmd.get("mode") in XPDR_MODES:
            s.xpdr_mode = cmd["mode"]
        elif c == "ap" and cmd.get("key") in AP_KEYS:
            self._ap_key(cmd["key"])
        elif c == "ap_set":
            var, val = cmd.get("var"), float(cmd.get("val", 0))
            if var == "hdg":
                s.ap_hdg = val % 360
            elif var == "alt":
                s.ap_alt = max(0, val)
                if s.ap_vert == "ALT" and abs(s.ap_alt - s.alt) > 100:
                    pass     # a real G3X keeps holding; the pilot presses VS or FLC to leave
            elif var == "vs":
                s.ap_vs = max(-3000, min(3000, val))
            elif var == "ias":
                s.ap_ias = max(50, min(250, val))

    def _ap_key(self, key):
        s = self.s
        if key == "AP":
            s.ap_master = not s.ap_master
            if s.ap_master:
                s.ap_fd = True
        elif key == "FD":
            s.ap_fd = not s.ap_fd
            if not s.ap_fd:
                s.ap_master = False
        elif key == "YD":
            s.ap_yd = not s.ap_yd
        elif key in ("HDG", "NAV", "APR", "BC"):
            s.ap_lat = "ROL" if s.ap_lat == key else key
            s.ap_fd = True
        elif key == "ALT":
            if s.ap_vert == "ALT":
                s.ap_vert = "PIT"
            else:
                s.ap_vert, s.ap_vert_arm = "ALT", ""
                s.ap_alt = round(s.alt / 100) * 100 if abs(s.vsi) < 300 else s.ap_alt
            s.ap_fd = True
        elif key in ("VS", "FLC"):
            if s.ap_vert == key:
                s.ap_vert, s.ap_vert_arm = "PIT", ""
            else:
                s.ap_vert, s.ap_vert_arm = key, "ALT"
                if key == "VS" and (s.ap_alt - s.alt) * s.ap_vs <= 0:
                    s.ap_vs = math.copysign(500, s.ap_alt - s.alt)
            s.ap_fd = True

    def close(self):
        pass
