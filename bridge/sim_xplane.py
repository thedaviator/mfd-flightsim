"""X-Plane 12 backend over X-Plane's UDP interface (port 49000).

Datarefs are subscribed with RREF (X-Plane streams them at the requested rate), radio
and autopilot targets are written with DREF, and buttons are pressed with CMND. No
plugin is needed; X-Plane only has to accept incoming network connections.
"""
import math
import socket
import struct
import time

from navdb import offset
from simstate import AP_KEYS, RADIOS, XPDR_MODES, valid_squawk

KG_PER_GAL = 2.72          # avgas
MS_TO_KT = 1.943844
M_TO_FT = 3.28084


def _c2f(v):
    return v * 1.8 + 32.0


# (dataref, SimState field, conversion). Fields starting with "_" are post-processed.
DATAREFS = [
    ("sim/flightmodel/position/latitude", "lat", None),
    ("sim/flightmodel/position/longitude", "lon", None),
    ("sim/cockpit2/gauges/indicators/altitude_ft_pilot", "alt", None),
    ("sim/flightmodel/position/y_agl", "agl", lambda v: v * M_TO_FT),
    ("sim/flightmodel/position/theta", "pitch", None),
    ("sim/flightmodel/position/phi", "roll", None),
    ("sim/flightmodel/position/mag_psi", "hdg", None),
    ("sim/flightmodel/position/psi", "hdg_true", None),
    ("sim/cockpit2/gauges/indicators/ground_track_mag_pilot", "trk", None),
    ("sim/cockpit2/gauges/indicators/airspeed_kts_pilot", "ias", None),
    ("sim/flightmodel/position/true_airspeed", "tas", lambda v: v * MS_TO_KT),
    ("sim/flightmodel/position/groundspeed", "gs", lambda v: v * MS_TO_KT),
    ("sim/cockpit2/gauges/indicators/vvi_fpm_pilot", "vsi", None),
    ("sim/cockpit2/gauges/indicators/slip_deg", "slip", lambda v: max(-1.0, min(1.0, v / 10.0))),
    ("sim/cockpit2/gauges/indicators/turn_rate_heading_deg_pilot", "turn", None),
    ("sim/cockpit2/gauges/actuators/barometer_setting_in_hg_pilot", "baro", None),
    ("sim/cockpit2/temperature/outside_air_temp_degc", "oat", None),
    ("sim/cockpit2/gauges/indicators/wind_heading_deg_mag", "_wind_mag", None),
    ("sim/cockpit2/gauges/indicators/wind_speed_kts", "wind_spd", None),
    ("sim/flightmodel/failures/onground_any", "on_ground", bool),
    ("sim/cockpit2/controls/parking_brake_ratio", "park_brake", lambda v: v >= 0.5),

    ("sim/cockpit2/engine/indicators/engine_speed_rpm[0]", "rpm", None),
    ("sim/cockpit2/engine/indicators/MPR_in_hg[0]", "mrp", None),
    ("sim/cockpit2/engine/indicators/fuel_flow_kg_sec[0]", "ff", lambda v: v * 3600.0 / KG_PER_GAL),
    ("sim/cockpit2/engine/indicators/oil_temperature_deg_C[0]", "oil_t", _c2f),
    ("sim/cockpit2/engine/indicators/oil_pressure_psi[0]", "oil_p", None),
    ("sim/cockpit2/engine/indicators/fuel_pressure_psi[0]", "fuel_p", None),
    ("sim/cockpit2/engine/indicators/CHT_deg_C[0]", "_cht", _c2f),
    ("sim/cockpit2/engine/indicators/EGT_deg_C[0]", "_egt", _c2f),
    ("sim/cockpit2/fuel/fuel_quantity[0]", "fuel_l", lambda v: v / KG_PER_GAL),
    ("sim/cockpit2/fuel/fuel_quantity[1]", "fuel_r", lambda v: v / KG_PER_GAL),
    ("sim/aircraft/weight/acf_m_fuel_tot", "fuel_cap", lambda v: v / 2 / KG_PER_GAL),
    ("sim/cockpit2/electrical/bus_volts[0]", "volts", None),
    ("sim/cockpit2/electrical/generator_amps[0]", "amps", None),

    ("sim/cockpit2/radios/actuators/com1_frequency_hz_833", "com1_act", lambda v: v / 1000.0),
    ("sim/cockpit2/radios/actuators/com1_standby_frequency_hz_833", "com1_stby", lambda v: v / 1000.0),
    ("sim/cockpit2/radios/actuators/com2_frequency_hz_833", "com2_act", lambda v: v / 1000.0),
    ("sim/cockpit2/radios/actuators/com2_standby_frequency_hz_833", "com2_stby", lambda v: v / 1000.0),
    ("sim/cockpit2/radios/actuators/nav1_frequency_hz", "nav1_act", lambda v: v / 100.0),
    ("sim/cockpit2/radios/actuators/nav1_standby_frequency_hz", "nav1_stby", lambda v: v / 100.0),
    ("sim/cockpit2/radios/actuators/nav2_frequency_hz", "nav2_act", lambda v: v / 100.0),
    ("sim/cockpit2/radios/actuators/nav2_standby_frequency_hz", "nav2_stby", lambda v: v / 100.0),
    ("sim/cockpit2/radios/actuators/transponder_code", "_xpdr", None),
    ("sim/cockpit2/radios/actuators/transponder_mode", "_xpdr_mode", None),

    ("sim/cockpit2/autopilot/servos_on", "ap_master", bool),
    ("sim/cockpit2/autopilot/flight_director_mode", "ap_fd", bool),
    ("sim/cockpit2/switches/yaw_damper_on", "ap_yd", bool),
    ("sim/cockpit2/autopilot/heading_status", "_hdg", None),
    ("sim/cockpit2/autopilot/nav_status", "_nav", None),
    ("sim/cockpit2/autopilot/approach_status", "_apr", None),
    ("sim/cockpit2/autopilot/backcourse_status", "_bc", None),
    ("sim/cockpit2/autopilot/altitude_hold_status", "_alt", None),
    ("sim/cockpit2/autopilot/vvi_status", "_vs", None),
    ("sim/cockpit2/autopilot/speed_status", "_flc", None),
    ("sim/cockpit2/autopilot/glideslope_status", "_gs", None),
    ("sim/cockpit2/autopilot/heading_dial_deg_mag_pilot", "ap_hdg", None),
    ("sim/cockpit2/autopilot/altitude_dial_ft", "ap_alt", None),
    ("sim/cockpit2/autopilot/vvi_dial_fpm", "ap_vs", None),
    ("sim/cockpit2/autopilot/airspeed_dial_kts_mach", "ap_ias", None),

    ("sim/cockpit2/radios/indicators/gps_bearing_deg_mag", "_gps_brg", None),
    ("sim/cockpit2/radios/indicators/gps_dme_distance_nm", "_gps_dist", None),
]

FREQ_DREF = {"com1": ("sim/cockpit2/radios/actuators/com1_standby_frequency_hz_833", 1000),
             "com2": ("sim/cockpit2/radios/actuators/com2_standby_frequency_hz_833", 1000),
             "nav1": ("sim/cockpit2/radios/actuators/nav1_standby_frequency_hz", 100),
             "nav2": ("sim/cockpit2/radios/actuators/nav2_standby_frequency_hz", 100)}
SWAP_CMND = {r: f"sim/radios/{r}_standy_flip" for r in RADIOS}     # sic: X-Plane spells it "standy"
AP_CMND = {"AP": "sim/autopilot/servos_toggle", "FD": "sim/autopilot/fdir_toggle",
           "YD": "sim/systems/yaw_damper_toggle", "HDG": "sim/autopilot/heading",
           "NAV": "sim/autopilot/NAV", "APR": "sim/autopilot/approach", "BC": "sim/autopilot/back_course",
           "ALT": "sim/autopilot/altitude_hold", "VS": "sim/autopilot/vertical_speed",
           "FLC": "sim/autopilot/level_change"}
AP_DREF = {"hdg": "sim/cockpit2/autopilot/heading_dial_deg_mag_pilot",
           "alt": "sim/cockpit2/autopilot/altitude_dial_ft",
           "vs": "sim/cockpit2/autopilot/vvi_dial_fpm",
           "ias": "sim/cockpit2/autopilot/airspeed_dial_kts_mach"}
# Step commands (what the cockpit knobs send), for aircraft that ignore writes to the dial
# datarefs: (up, down, done when within, first guess of one step). The bridge checks the
# value the sim reports and steps the rest of the way; see XPlaneBackend._chase.
AP_STEP = {"hdg": ("sim/autopilot/heading_up", "sim/autopilot/heading_down", 0.6, 1.0),
           "alt": ("sim/autopilot/altitude_up", "sim/autopilot/altitude_down", 60.0, 100.0),
           "vs": ("sim/autopilot/vertical_speed_up", "sim/autopilot/vertical_speed_down", 60.0, 100.0),
           "ias": ("sim/autopilot/airspeed_up", "sim/autopilot/airspeed_down", 0.6, 1.0)}
AP_FIELD = {"hdg": "ap_hdg", "alt": "ap_alt", "vs": "ap_vs", "ias": "ap_ias"}
CHASE_WAIT = 0.4        # s between a write or a burst of steps and the next check
CHASE_ROUNDS = 8
# transponder_mode in X-Plane 12: 0 off, 1 standby, 2 on (mode A), 3 alt (mode C), 4 test, 5 ground
XPDR_TO_MODE = {"STBY": 1, "ON": 2, "ALT": 3, "GND": 5}
MODE_TO_XPDR = {0: "STBY", 1: "STBY", 2: "ON", 3: "ALT", 4: "STBY", 5: "GND"}

CYL_SPREAD = (-8.0, 4.0, 10.0, -5.0)
RATE_HZ = 30


class XPlaneBackend:
    name = "XPLANE"

    def __init__(self, host="127.0.0.1", port=49000, log=print):
        self.log = log
        self.addr = (host, port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", 0))
        self.sock.setblocking(False)
        self.values = [0.0] * len(DATAREFS)
        self.last_rx = 0.0
        self.last_sub = 0.0
        self.announced = False
        self.chase = {}             # var -> autopilot setting being brought to its target
        self.chase_warned = set()

    @property
    def alive(self):
        return time.monotonic() - self.last_rx < 2.0

    def _subscribe(self, freq):
        for i, (ref, _, _) in enumerate(DATAREFS):
            self.sock.sendto(b"RREF\0" + struct.pack("<ii400s", freq, i, ref.encode()), self.addr)

    def _cmnd(self, command):
        self.sock.sendto(b"CMND\0" + command.encode() + b"\0", self.addr)

    def _dref(self, ref, value):
        self.sock.sendto(b"DREF\0" + struct.pack("<f500s", float(value), ref.encode() + b"\0"), self.addr)

    def poll(self, state, dt):
        now = time.monotonic()
        if not self.alive and now - self.last_sub > 3.0:
            try:
                self._subscribe(RATE_HZ)
            except OSError:
                pass
            self.last_sub = now
            if self.announced:
                self.log("X-Plane stopped sending data")
                self.announced = False
        while True:
            try:
                pkt, _ = self.sock.recvfrom(4096)
            except (BlockingIOError, ConnectionResetError, OSError):
                break
            if pkt[:4] != b"RREF" or len(pkt) < 5:
                continue
            for off in range(5, len(pkt) - 7, 8):
                idx, val = struct.unpack_from("<if", pkt, off)
                if 0 <= idx < len(self.values):
                    self.values[idx] = val
            self.last_rx = now
        if not self.alive:
            return False
        if not self.announced:
            self.log(f"Receiving X-Plane data from {self.addr[0]}:{self.addr[1]}")
            self.announced = True

        raw = {}
        for (_, field, conv), v in zip(DATAREFS, self.values):
            if math.isnan(v):
                v = 0.0
            v = conv(v) if conv else v
            if field.startswith("_"):
                raw[field] = v
            else:
                setattr(state, field, v)
        state.magvar = ((state.hdg_true - state.hdg + 180) % 360) - 180
        state.wind_dir = (raw["_wind_mag"] + state.magvar) % 360
        state.cht = [round(raw["_cht"] + d, 1) for d in CYL_SPREAD]
        state.egt = [round(raw["_egt"] + d * 2, 1) for d in CYL_SPREAD]
        code = int(round(raw["_xpdr"]))
        state.xpdr = f"{code:04d}"[-4:]
        state.xpdr_mode = MODE_TO_XPDR.get(int(raw["_xpdr_mode"]), "STBY")

        # *_status datarefs: 0 off, 1 armed, 2 active
        lat_modes = [("APR", raw["_apr"]), ("BC", raw["_bc"]), ("NAV", raw["_nav"]), ("HDG", raw["_hdg"])]
        state.ap_lat = next((m for m, s in lat_modes if s >= 2), "ROL")
        state.ap_lat_arm = " ".join(m for m, s in lat_modes if 0.5 < s < 1.5)
        vert_modes = [("GS", raw["_gs"]), ("ALT", raw["_alt"]), ("IAS", raw["_flc"]), ("VS", raw["_vs"])]
        state.ap_vert = next((m for m, s in vert_modes if s >= 2), "PIT")
        state.ap_vert_arm = " ".join(m for m, s in vert_modes if 0.5 < s < 1.5)

        if raw["_gps_dist"] > 0.05:
            brg_true = raw["_gps_brg"] + state.magvar
            state.to_lat, state.to_lon = offset(state.lat, state.lon, raw["_gps_dist"], brg_true)
        else:
            state.to_lat = state.to_lon = None
        state.from_lat = state.from_lon = None
        self._chase(state, now)
        return True

    def _chase(self, state, now):
        """Bring autopilot settings that did not follow the dataref write to their target
        with step commands. Re-checks after every burst, learning the aircraft's step size."""
        for var, c in list(self.chase.items()):
            if now < c["next"]:
                continue
            up, down, tol, _ = AP_STEP[var]
            cur = getattr(state, AP_FIELD[var])
            diff = c["target"] - cur
            if var == "hdg":
                diff = (diff + 180.0) % 360.0 - 180.0
            if abs(diff) <= tol:
                del self.chase[var]
                continue
            if c["rounds"] >= CHASE_ROUNDS:
                del self.chase[var]
                if var not in self.chase_warned:
                    self.chase_warned.add(var)
                    self.log(f"X-Plane: this aircraft does not take autopilot {var.upper()} settings from the bridge")
                continue
            if c["sent"] and c["last"] is not None:
                moved = cur - c["last"]
                if var == "hdg":
                    moved = (moved + 180.0) % 360.0 - 180.0
                if abs(moved) > 1e-3:
                    c["step"] = abs(moved) / c["sent"]
            n = int(min(60, max(1, round(abs(diff) / c["step"]))))
            for _ in range(n):
                self._cmnd(up if diff > 0 else down)
            c.update(sent=n, last=cur, rounds=c["rounds"] + 1, next=now + CHASE_WAIT)

    def apply_command(self, cmd):
        c = cmd.get("cmd")
        radio = cmd.get("radio")
        if c == "freq" and radio in RADIOS:
            ref, scale = FREQ_DREF[radio]
            self._dref(ref, round(float(cmd["mhz"]) * scale))
        elif c == "swap" and radio in RADIOS:
            self._cmnd(SWAP_CMND[radio])
        elif c == "xpdr" and valid_squawk(cmd.get("code")):
            self._dref("sim/cockpit2/radios/actuators/transponder_code", int(cmd["code"]))
        elif c == "xpdr_mode" and cmd.get("mode") in XPDR_MODES:
            self._dref("sim/cockpit2/radios/actuators/transponder_mode", XPDR_TO_MODE[cmd["mode"]])
        elif c == "ap" and cmd.get("key") in AP_KEYS:
            self._cmnd(AP_CMND[cmd["key"]])
        elif c == "ap_set" and cmd.get("var") in AP_DREF:
            val = float(cmd.get("val", 0))
            if cmd["var"] == "hdg":
                val %= 360
            self._dref(AP_DREF[cmd["var"]], val)
            # If the aircraft ignores the write, _chase steps it there instead.
            self.chase[cmd["var"]] = {"target": val, "next": time.monotonic() + CHASE_WAIT, "rounds": 0,
                                      "sent": 0, "last": None, "step": AP_STEP[cmd["var"]][3]}

    def close(self):
        try:
            self._subscribe(0)
            self.sock.close()
        except OSError:
            pass
