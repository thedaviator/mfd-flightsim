"""The simulator-independent aircraft state every backend fills in, plus the commands
the display can send back.

Units: degrees, knots, feet, feet per minute, inHg, psi, deg F for engine temperatures,
US gallons, MHz for radios. Headings and tracks are magnetic unless named *_true.
"""
from dataclasses import dataclass, field


@dataclass
class SimState:
    # Position and attitude
    lat: float = 0.0
    lon: float = 0.0
    alt: float = 0.0            # indicated altitude, ft
    agl: float = 0.0
    pitch: float = 0.0          # nose up positive
    roll: float = 0.0           # right wing down positive
    hdg: float = 0.0            # magnetic heading
    hdg_true: float = 0.0
    trk: float = 0.0            # magnetic ground track
    magvar: float = 0.0         # east positive (true = mag + magvar)
    ias: float = 0.0
    tas: float = 0.0
    gs: float = 0.0
    vsi: float = 0.0
    slip: float = 0.0           # inclinometer ball, -1 (full left) .. +1 (full right)
    turn: float = 0.0           # turn rate, deg/s, right positive
    baro: float = 29.92
    oat: float = 15.0           # deg C
    wind_dir: float = 0.0       # direction the wind blows from, true
    wind_spd: float = 0.0

    # Engine (engine 1 of the aircraft)
    rpm: float = 0.0
    mrp: float = 0.0            # manifold pressure, inHg
    ff: float = 0.0             # fuel flow, gph
    oil_t: float = 0.0          # deg F
    oil_p: float = 0.0
    fuel_p: float = 0.0
    cht: list = field(default_factory=lambda: [0.0] * 4)
    egt: list = field(default_factory=lambda: [0.0] * 4)
    fuel_l: float = 0.0
    fuel_r: float = 0.0
    fuel_cap: float = 0.0       # per tank
    volts: float = 0.0
    amps: float = 0.0

    # Radios
    com1_act: float = 0.0
    com1_stby: float = 0.0
    com2_act: float = 0.0
    com2_stby: float = 0.0
    nav1_act: float = 0.0
    nav1_stby: float = 0.0
    nav2_act: float = 0.0
    nav2_stby: float = 0.0
    xpdr: str = "1200"
    xpdr_mode: str = "STBY"     # STBY, GND, ON, ALT

    # Autopilot
    ap_master: bool = False
    ap_fd: bool = False
    ap_yd: bool = False
    ap_lat: str = "ROL"         # active lateral mode: ROL, HDG, NAV, APR, BC
    ap_lat_arm: str = ""
    ap_vert: str = "PIT"        # active vertical mode: PIT, ALT, VS, IAS, GS
    ap_vert_arm: str = ""
    ap_hdg: float = 0.0         # heading bug
    ap_alt: float = 0.0         # selected altitude
    ap_vs: float = 0.0          # selected vertical speed
    ap_ias: float = 0.0         # selected airspeed

    # GPS: active leg as reported by the simulator (to_lat None if none)
    to_id: str = ""
    to_lat: float = None
    to_lon: float = None
    from_lat: float = None
    from_lon: float = None

    on_ground: bool = False
    park_brake: bool = False    # parking brake set


# Commands the display sends ({"cmd": ...}). Each backend implements apply_command().
#   {"cmd":"freq","radio":"com1","mhz":118.3}        set standby frequency (com1 com2 nav1 nav2)
#   {"cmd":"swap","radio":"com1"}                    swap active and standby
#   {"cmd":"xpdr","code":"7000"}
#   {"cmd":"xpdr_mode","mode":"ALT"}                 STBY GND ON ALT
#   {"cmd":"ap","key":"HDG"}                         toggle AP FD YD HDG NAV APR BC ALT VS FLC
#   {"cmd":"ap_set","var":"hdg","val":270}           hdg alt vs ias
#   {"cmd":"map_range","nm":20}                      handled by the bridge, not the sim
RADIOS = ("com1", "com2", "nav1", "nav2")
AP_KEYS = ("AP", "FD", "YD", "HDG", "NAV", "APR", "BC", "ALT", "VS", "FLC")
XPDR_MODES = ("STBY", "GND", "ON", "ALT")


def valid_squawk(code):
    return isinstance(code, str) and len(code) == 4 and all(c in "01234567" for c in code)
