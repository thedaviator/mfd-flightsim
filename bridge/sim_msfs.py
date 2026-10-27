"""MSFS 2024 (and 2020) backend, using the Python-SimConnect package.

    pip install SimConnect

Python-SimConnect's own AircraftRequests asks for one SimVar per round trip, far too
slow for ~90 variables at 30 Hz. So this backend uses the package for the connection,
the DLL bindings and events, but registers a single data definition containing every
SimVar and has the simulator stream it once per visual frame.
"""
import ctypes
import math
import threading

try:
    from SimConnect import SimConnect
    from SimConnect.Enum import (SIMCONNECT_DATATYPE, SIMCONNECT_PERIOD,
                                 SIMCONNECT_SIMOBJECT_TYPE)
except ImportError:          # reported when the backend is first used
    SimConnect = None

from simstate import AP_KEYS, RADIOS, XPDR_MODES, valid_squawk

UNUSED = 0xFFFFFFFF
OBJECT_ID_USER = 0


def _r2f(v):            # rankine -> F
    return v - 459.67


def _c2f(v):
    return v * 1.8 + 32.0


def _neg(v):
    return -v


# (SimVar, unit, SimState field, conversion)
SIMVARS = [
    ("PLANE LATITUDE", "degrees", "lat", None),
    ("PLANE LONGITUDE", "degrees", "lon", None),
    ("INDICATED ALTITUDE", "feet", "alt", None),
    ("PLANE ALT ABOVE GROUND", "feet", "agl", None),
    ("PLANE PITCH DEGREES", "degrees", "pitch", _neg),      # MSFS: + is nose down
    ("PLANE BANK DEGREES", "degrees", "roll", _neg),        # MSFS: + is left wing down
    ("PLANE HEADING DEGREES MAGNETIC", "degrees", "hdg", None),
    ("PLANE HEADING DEGREES TRUE", "degrees", "hdg_true", None),
    ("GPS GROUND MAGNETIC TRACK", "degrees", "trk", None),
    ("AIRSPEED INDICATED", "knots", "ias", None),
    ("AIRSPEED TRUE", "knots", "tas", None),
    ("GROUND VELOCITY", "knots", "gs", None),
    ("VERTICAL SPEED", "feet per minute", "vsi", None),
    ("TURN COORDINATOR BALL", "position", "slip", None),
    ("TURN INDICATOR RATE", "degrees per second", "turn", None),
    ("KOHLSMAN SETTING HG:1", "inHg", "baro", None),
    ("AMBIENT TEMPERATURE", "celsius", "oat", None),
    ("AMBIENT WIND DIRECTION", "degrees", "wind_dir", None),
    ("AMBIENT WIND VELOCITY", "knots", "wind_spd", None),
    ("SIM ON GROUND", "bool", "on_ground", bool),
    ("BRAKE PARKING POSITION", "bool", "park_brake", bool),

    ("GENERAL ENG RPM:1", "rpm", "rpm", None),
    ("RECIP ENG MANIFOLD PRESSURE:1", "inHg", "mrp", None),
    ("ENG FUEL FLOW GPH:1", "gallons per hour", "ff", None),
    ("GENERAL ENG OIL TEMPERATURE:1", "rankine", "oil_t", _r2f),
    ("GENERAL ENG OIL PRESSURE:1", "psi", "oil_p", None),
    ("GENERAL ENG FUEL PRESSURE:1", "psi", "fuel_p", None),
    ("RECIP ENG CYLINDER HEAD TEMPERATURE:1", "celsius", "_cht", _c2f),
    ("GENERAL ENG EXHAUST GAS TEMPERATURE:1", "rankine", "_egt", _r2f),
    ("FUEL LEFT QUANTITY", "gallons", "fuel_l", None),
    ("FUEL RIGHT QUANTITY", "gallons", "fuel_r", None),
    ("FUEL LEFT CAPACITY", "gallons", "fuel_cap", None),
    ("ELECTRICAL MAIN BUS VOLTAGE", "volts", "volts", None),
    ("ELECTRICAL GENALT BUS AMPS:1", "amperes", "amps", None),

    ("COM ACTIVE FREQUENCY:1", "MHz", "com1_act", None),
    ("COM STANDBY FREQUENCY:1", "MHz", "com1_stby", None),
    ("COM ACTIVE FREQUENCY:2", "MHz", "com2_act", None),
    ("COM STANDBY FREQUENCY:2", "MHz", "com2_stby", None),
    ("NAV ACTIVE FREQUENCY:1", "MHz", "nav1_act", None),
    ("NAV STANDBY FREQUENCY:1", "MHz", "nav1_stby", None),
    ("NAV ACTIVE FREQUENCY:2", "MHz", "nav2_act", None),
    ("NAV STANDBY FREQUENCY:2", "MHz", "nav2_stby", None),
    ("TRANSPONDER CODE:1", "Bco16", "_xpdr", None),
    ("TRANSPONDER STATE:1", "enum", "_xpdr_state", None),

    ("AUTOPILOT MASTER", "bool", "ap_master", bool),
    ("AUTOPILOT FLIGHT DIRECTOR ACTIVE", "bool", "ap_fd", bool),
    ("AUTOPILOT YAW DAMPER", "bool", "ap_yd", bool),
    ("AUTOPILOT HEADING LOCK", "bool", "_hdg", bool),
    ("AUTOPILOT NAV1 LOCK", "bool", "_nav", bool),
    ("AUTOPILOT APPROACH HOLD", "bool", "_apr", bool),
    ("AUTOPILOT BACKCOURSE HOLD", "bool", "_bc", bool),
    ("AUTOPILOT GLIDESLOPE HOLD", "bool", "_gs", bool),
    ("AUTOPILOT GLIDESLOPE ARM", "bool", "_gs_arm", bool),
    ("AUTOPILOT ALTITUDE LOCK", "bool", "_alt", bool),
    ("AUTOPILOT ALTITUDE ARM", "bool", "_alt_arm", bool),
    ("AUTOPILOT VERTICAL HOLD", "bool", "_vs", bool),
    ("AUTOPILOT FLIGHT LEVEL CHANGE", "bool", "_flc", bool),
    ("AUTOPILOT HEADING LOCK DIR", "degrees", "ap_hdg", None),
    ("AUTOPILOT ALTITUDE LOCK VAR", "feet", "ap_alt", None),
    ("AUTOPILOT VERTICAL HOLD VAR", "feet per minute", "ap_vs", None),
    ("AUTOPILOT AIRSPEED HOLD VAR", "knots", "ap_ias", None),

    ("GPS IS ACTIVE WAY POINT", "bool", "_gps_active", bool),
    ("GPS WP NEXT LAT", "degrees", "_to_lat", None),
    ("GPS WP NEXT LON", "degrees", "_to_lon", None),
    ("GPS WP PREV LAT", "degrees", "_from_lat", None),
    ("GPS WP PREV LON", "degrees", "_from_lon", None),
]

FREQ_SET = {"com1": "COM_STBY_RADIO_SET_HZ", "com2": "COM2_STBY_RADIO_SET_HZ",
            "nav1": "NAV1_STBY_SET_HZ", "nav2": "NAV2_STBY_SET_HZ"}
FREQ_SWAP = {"com1": "COM_STBY_RADIO_SWAP", "com2": "COM2_RADIO_SWAP",
             "nav1": "NAV1_RADIO_SWAP", "nav2": "NAV2_RADIO_SWAP"}
AP_EVENTS = {"AP": "AP_MASTER", "FD": "TOGGLE_FLIGHT_DIRECTOR", "YD": "YAW_DAMPER_TOGGLE",
             "HDG": "AP_PANEL_HEADING_HOLD", "NAV": "AP_NAV1_HOLD", "APR": "AP_APR_HOLD",
             "BC": "AP_BC_HOLD", "ALT": "AP_PANEL_ALTITUDE_HOLD", "VS": "AP_PANEL_VS_HOLD",
             "FLC": "FLIGHT_LEVEL_CHANGE"}
AP_SET_EVENTS = {"hdg": "HEADING_BUG_SET", "alt": "AP_ALT_VAR_SET_ENGLISH",
                 "vs": "AP_VS_VAR_SET_ENGLISH", "ias": "AP_SPD_VAR_SET"}
# TRANSPONDER STATE: 0 off, 1 standby, 2 test, 3 on, 4 alt. MSFS has no ground mode,
# so GND is sent as ON and remembered so the display keeps showing GND.
XPDR_TO_STATE = {"STBY": 1, "GND": 3, "ON": 3, "ALT": 4}
STATE_TO_XPDR = {0: "STBY", 1: "STBY", 2: "STBY", 3: "ON", 4: "ALT"}

CYL_SPREAD = (-8.0, 4.0, 10.0, -5.0)


class _Connection(SimConnect if SimConnect else object):
    """SimConnect with a dispatch hook for our bulk data request."""

    def __init__(self, n_vars):
        self.bulk_req = None
        self.n_vars = n_vars
        self.latest = None
        self.lock = threading.Lock()
        super().__init__(auto_connect=True)

    def handle_simobject_event(self, obj):
        if self.bulk_req is not None and obj.dwRequestID == self.bulk_req:
            vals = ctypes.cast(obj.dwData, ctypes.POINTER(ctypes.c_double * self.n_vars)).contents
            with self.lock:
                self.latest = list(vals)
        else:
            super().handle_simobject_event(obj)

    def my_dispatch_proc(self, pData, cbData, pContext):
        # The base class only routes ..._BYTYPE messages; a periodic request answers
        # with SIMOBJECT_DATA (id 8), so route that too.
        from SimConnect.Enum import SIMCONNECT_RECV_ID, SIMCONNECT_RECV_SIMOBJECT_DATA
        if pData.contents.dwID == SIMCONNECT_RECV_ID.SIMCONNECT_RECV_ID_SIMOBJECT_DATA:
            self.handle_simobject_event(ctypes.cast(pData, ctypes.POINTER(SIMCONNECT_RECV_SIMOBJECT_DATA)).contents)
            return
        super().my_dispatch_proc(pData, cbData, pContext)


class MsfsBackend:
    name = "MSFS"

    def __init__(self, log=print):
        if SimConnect is None:
            raise RuntimeError("Python-SimConnect is not installed: pip install SimConnect")
        self.log = log
        self.sm = _Connection(len(SIMVARS))
        if not self.sm.ok:
            raise ConnectionError("MSFS is not running")
        dll, h = self.sm.dll, self.sm.hSimConnect
        def_id = self.sm.new_def_id().value
        for name, unit, _, _ in SIMVARS:
            dll.AddToDataDefinition(h, def_id, name.encode(), unit.encode(),
                                    SIMCONNECT_DATATYPE.SIMCONNECT_DATATYPE_FLOAT64, 0.0, UNUSED)
        req_id = self.sm.new_request_id().value
        self.sm.bulk_req = req_id
        dll.RequestDataOnSimObject(h, req_id, def_id, OBJECT_ID_USER,
                                   SIMCONNECT_PERIOD.SIMCONNECT_PERIOD_VISUAL_FRAME, 0, 0, 0, 0)

        # A one-variable definition for writing the transponder state.
        self.xpdr_def = self.sm.new_def_id().value
        dll.AddToDataDefinition(h, self.xpdr_def, b"TRANSPONDER STATE:1", b"enum",
                                SIMCONNECT_DATATYPE.SIMCONNECT_DATATYPE_FLOAT64, 0.0, UNUSED)
        self.events = {}
        self.gnd_requested = False
        log("Connected to MSFS through SimConnect")

    @property
    def alive(self):
        return self.sm.quit == 0

    def _event(self, name):
        if name not in self.events:
            self.events[name] = self.sm.map_to_sim_event(name.encode())
        return self.events[name]

    def _send(self, name, value=0):
        evt = self._event(name)
        if evt is not None:
            self.sm.send_event(evt, ctypes.c_uint32(int(value) & 0xFFFFFFFF))

    # ------------------------------------------------------------ data

    def poll(self, state, dt):
        if not self.alive:
            raise ConnectionError("MSFS closed the connection")
        with self.sm.lock:
            vals = self.sm.latest
        if vals is None:
            return False
        raw = {}
        for (_, _, field, conv), v in zip(SIMVARS, vals):
            if math.isnan(v):
                v = 0.0
            v = conv(v) if conv else v
            if field.startswith("_"):
                raw[field] = v
            else:
                setattr(state, field, v)

        state.magvar = ((state.hdg_true - state.hdg + 180) % 360) - 180
        state.cht = [round(raw["_cht"] + d, 1) for d in CYL_SPREAD]
        state.egt = [round(raw["_egt"] + d * 2, 1) for d in CYL_SPREAD]
        state.xpdr = format(int(raw["_xpdr"]), "04x")
        mode = STATE_TO_XPDR.get(int(raw["_xpdr_state"]), "STBY")
        state.xpdr_mode = "GND" if mode == "ON" and self.gnd_requested else mode

        if raw["_apr"]:
            state.ap_lat = "APR"
        elif raw["_bc"]:
            state.ap_lat = "BC"
        elif raw["_nav"]:
            state.ap_lat = "NAV"
        elif raw["_hdg"]:
            state.ap_lat = "HDG"
        else:
            state.ap_lat = "ROL"
        state.ap_lat_arm = ""
        if raw["_gs"]:
            state.ap_vert = "GS"
        elif raw["_alt"]:
            state.ap_vert = "ALT"
        elif raw["_flc"]:
            state.ap_vert = "IAS"
        elif raw["_vs"]:
            state.ap_vert = "VS"
        else:
            state.ap_vert = "PIT"
        arm = []
        if raw["_alt_arm"] and state.ap_vert != "ALT":
            arm.append("ALT")
        if raw["_gs_arm"] and state.ap_vert != "GS":
            arm.append("GS")
        state.ap_vert_arm = " ".join(arm)

        if raw["_gps_active"] and (raw["_to_lat"] or raw["_to_lon"]):
            state.to_lat, state.to_lon = raw["_to_lat"], raw["_to_lon"]
            state.from_lat, state.from_lon = raw["_from_lat"], raw["_from_lon"]
        else:
            state.to_lat = state.to_lon = state.from_lat = state.from_lon = None
        return True

    # ------------------------------------------------------------ commands

    def apply_command(self, cmd):
        c = cmd.get("cmd")
        radio = cmd.get("radio")
        if c == "freq" and radio in RADIOS:
            self._send(FREQ_SET[radio], round(float(cmd["mhz"]) * 1_000_000))
        elif c == "swap" and radio in RADIOS:
            self._send(FREQ_SWAP[radio])
        elif c == "xpdr" and valid_squawk(cmd.get("code")):
            self._send("XPNDR_SET", int(cmd["code"], 16))     # BCD
        elif c == "xpdr_mode" and cmd.get("mode") in XPDR_MODES:
            self.gnd_requested = cmd["mode"] == "GND"
            val = (ctypes.c_double * 1)(XPDR_TO_STATE[cmd["mode"]])
            self.sm.dll.SetDataOnSimObject(self.sm.hSimConnect, self.xpdr_def, OBJECT_ID_USER, 0, 0,
                                           ctypes.sizeof(val), ctypes.cast(val, ctypes.c_void_p))
        elif c == "ap" and cmd.get("key") in AP_KEYS:
            self._send(AP_EVENTS[cmd["key"]])
        elif c == "ap_set" and cmd.get("var") in AP_SET_EVENTS:
            val = float(cmd.get("val", 0))
            if cmd["var"] == "hdg":
                val %= 360
            self._send(AP_SET_EVENTS[cmd["var"]], round(val))

    def close(self):
        try:
            self.sm.exit()
        except Exception:
            pass
