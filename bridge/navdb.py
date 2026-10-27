"""Navigation database for the bridge: airports, runways, frequencies and navaids.

Built from the public-domain OurAirports data set (https://ourairports.com/data/).
The CSV files are downloaded once into bridge/data/ and reused after that.

The bridge uses this to fill the NEAREST list (with longest runway and tower frequency)
and the VOR / NDB / airport symbols on the moving map, so the display itself needs no
database. Flight plans can be loaded from an MSFS .pln or X-Plane .fms file.
"""
import bisect
import csv
import gzip
import math
import pickle
import os
import re
import urllib.request
import xml.etree.ElementTree as ET

DATA_URL = "https://davidmegginson.github.io/ourairports-data/"
FILES = ("airports.csv", "runways.csv", "airport-frequencies.csv", "navaids.csv")
DATA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
CACHE_NAME = "navdb.pkl.gz"     # compact pre-built database (shipped with the .exe)

AIRPORT_TYPES = {"small_airport": 1, "medium_airport": 2, "large_airport": 3}
VOR_TYPES = {"VOR", "VOR-DME", "VORTAC", "TACAN", "DME"}
NDB_TYPES = {"NDB", "NDB-DME"}
# Frequency shown in the NEAREST list: the first type found in this order.
FREQ_PREFERENCE = ("TWR", "CTAF", "UNIC", "AFIS", "A/G", "MULTICOM", "RDO")

EARTH_NM = 3440.065


def distance_bearing(lat1, lon1, lat2, lon2):
    """Great-circle distance (nm) and initial true bearing (deg) from point 1 to point 2."""
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dl = math.radians(lon2 - lon1)
    a = math.sin((p2 - p1) / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    dist = 2 * EARTH_NM * math.asin(min(1.0, math.sqrt(a)))
    y = math.sin(dl) * math.cos(p2)
    x = math.cos(p1) * math.sin(p2) - math.sin(p1) * math.cos(p2) * math.cos(dl)
    return dist, (math.degrees(math.atan2(y, x)) + 360.0) % 360.0


def offset(lat, lon, dist_nm, brg_deg):
    """The point dist_nm from (lat, lon) along true bearing brg_deg."""
    d = dist_nm / EARTH_NM
    b = math.radians(brg_deg)
    p1, l1 = math.radians(lat), math.radians(lon)
    p2 = math.asin(math.sin(p1) * math.cos(d) + math.cos(p1) * math.sin(d) * math.cos(b))
    l2 = l1 + math.atan2(math.sin(b) * math.sin(d) * math.cos(p1), math.cos(d) - math.sin(p1) * math.sin(p2))
    return math.degrees(p2), (math.degrees(l2) + 540.0) % 360.0 - 180.0


class Airport:
    __slots__ = ("ident", "name", "lat", "lon", "size", "elev", "rwy_ft", "freq", "ftype", "city", "codes")

    def __init__(self, ident, name, lat, lon, size, elev, city="", codes=()):
        self.ident, self.name, self.lat, self.lon, self.size, self.elev = ident, name, lat, lon, size, elev
        self.rwy_ft = 0
        self.freq = ""
        self.ftype = ""
        self.city = city            # municipality
        self.codes = codes          # other idents it is known by (IATA, FAA local, GPS code)


class Navaid:
    __slots__ = ("ident", "name", "kind", "lat", "lon", "freq")

    def __init__(self, ident, name, kind, lat, lon, freq):
        self.ident, self.name, self.kind, self.lat, self.lon, self.freq = ident, name, kind, lat, lon, freq


class GridIndex:
    """Objects bucketed by whole degree of latitude and longitude, for radius queries."""

    def __init__(self):
        self.cells = {}

    def add(self, obj):
        key = (math.floor(obj.lat), math.floor(obj.lon))
        self.cells.setdefault(key, []).append(obj)

    def query(self, lat, lon, radius_nm):
        """Yield (distance_nm, true_bearing, obj) for every object within radius_nm."""
        dlat = radius_nm / 60.0
        dlon = radius_nm / (60.0 * max(0.05, math.cos(math.radians(lat))))
        lat0, lat1 = math.floor(lat - dlat), math.floor(lat + dlat)
        lon0, lon1 = math.floor(lon - dlon), math.floor(lon + dlon)
        if lon1 - lon0 >= 360:
            lon0, lon1 = -180, 179
        for ci in range(lat0, lat1 + 1):
            for cj in range(lon0, lon1 + 1):
                cj_wrapped = (cj + 180) % 360 - 180
                for obj in self.cells.get((ci, cj_wrapped), ()):
                    d, b = distance_bearing(lat, lon, obj.lat, obj.lon)
                    if d <= radius_nm:
                        yield d, b, obj


def _download(data_dir, log):
    os.makedirs(data_dir, exist_ok=True)
    for name in FILES:
        path = os.path.join(data_dir, name)
        if os.path.exists(path) and os.path.getsize(path) > 0:
            continue
        log(f"Downloading {name} from OurAirports ...")
        tmp = path + ".part"
        urllib.request.urlretrieve(DATA_URL + name, tmp)
        os.replace(tmp, path)


def _f(v, default=0.0):
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


class NavDB:
    def __init__(self, data_dir=DATA_DIR, log=print, download=True, cache=None):
        self.airports = {}
        self.apt_index = GridIndex()
        self.nav_index = GridIndex()
        self.navaids_by_id = {}
        self._sorted_idents = None
        self._by_code = None
        cache = cache or os.path.join(data_dir, CACHE_NAME)
        if os.path.exists(cache):
            self._load_cache(cache)
        else:
            if download:
                _download(data_dir, log)
            self._load(data_dir)
        log(f"Nav database: {len(self.airports)} airports, {sum(len(v) for v in self.navaids_by_id.values())} navaids")

    def save_cache(self, path):
        apts = [(a.ident, a.name, a.lat, a.lon, a.size, a.elev, a.rwy_ft, a.freq, a.ftype, a.city, a.codes)
                for a in self.airports.values()]
        navs = [(n.ident, n.name, n.kind, n.lat, n.lon, n.freq) for v in self.navaids_by_id.values() for n in v]
        with gzip.open(path, "wb", compresslevel=9) as f:
            pickle.dump((apts, navs), f, protocol=pickle.HIGHEST_PROTOCOL)

    def _load_cache(self, path):
        with gzip.open(path, "rb") as f:
            apts, navs = pickle.load(f)
        for row in apts:
            # Caches from before 1.3 have no city / other codes.
            ident, name, lat, lon, size, elev, rwy, freq, ftype = row[:9]
            a = Airport(ident, name, lat, lon, size, elev, *row[9:11])
            a.rwy_ft, a.freq, a.ftype = rwy, freq, ftype
            self.airports[ident] = a
            self.apt_index.add(a)
        for ident, name, kind, lat, lon, freq in navs:
            n = Navaid(ident, name, kind, lat, lon, freq)
            self.nav_index.add(n)
            self.navaids_by_id.setdefault(ident, []).append(n)

    def _load(self, data_dir):
        def rows(name):
            with open(os.path.join(data_dir, name), newline="", encoding="utf-8") as f:
                yield from csv.DictReader(f)

        for r in rows("airports.csv"):
            size = AIRPORT_TYPES.get(r["type"])
            if not size:
                continue
            ident = r["icao_code"] or r["ident"]
            codes = tuple(sorted({c for c in (r["iata_code"], r["local_code"], r["gps_code"], r["ident"]) if c and c != ident}))
            apt = Airport(ident, r["name"], _f(r["latitude_deg"]), _f(r["longitude_deg"]), size, _f(r["elevation_ft"]),
                          r["municipality"], codes)
            self.airports[r["ident"]] = apt

        for r in rows("runways.csv"):
            apt = self.airports.get(r["airport_ident"])
            if apt and r["closed"] != "1":
                apt.rwy_ft = max(apt.rwy_ft, int(_f(r["length_ft"])))

        best = {}
        for r in rows("airport-frequencies.csv"):
            apt = self.airports.get(r["airport_ident"])
            if not apt:
                continue
            ftype = r["type"].upper()
            rank = FREQ_PREFERENCE.index(ftype) if ftype in FREQ_PREFERENCE else None
            if rank is None and ("TWR" in ftype or "TOWER" in r["description"].upper()):
                rank, ftype = 0, "TWR"
            if rank is None:
                continue
            if r["airport_ident"] not in best or rank < best[r["airport_ident"]]:
                best[r["airport_ident"]] = rank
                apt.freq = f"{_f(r['frequency_mhz']):.3f}"
                apt.ftype = ftype

        # Re-key by display ident and index everything.
        by_ident = {}
        for apt in self.airports.values():
            by_ident.setdefault(apt.ident, apt)
            self.apt_index.add(apt)
        self.airports = by_ident

        for r in rows("navaids.csv"):
            t = r["type"]
            kind = "V" if t in VOR_TYPES else "N" if t in NDB_TYPES else None
            if not kind or not r["latitude_deg"]:
                continue
            khz = _f(r["frequency_khz"])
            freq = f"{khz / 1000:.2f}" if kind == "V" else f"{khz:.0f}"
            nav = Navaid(r["ident"], r["name"], kind, _f(r["latitude_deg"]), _f(r["longitude_deg"]), freq)
            self.nav_index.add(nav)
            self.navaids_by_id.setdefault(nav.ident, []).append(nav)

    # ------------------------------------------------------------ queries

    def nearest_airports(self, lat, lon, count=25, radius_nm=100, min_rwy_ft=0):
        found = [(d, b, a) for d, b, a in self.apt_index.query(lat, lon, radius_nm) if a.rwy_ft >= min_rwy_ft]
        found.sort(key=lambda t: t[0])
        return found[:count]

    def map_features(self, lat, lon, radius_nm, limit=160):
        """Airports and navaids for the map, closest first. Small airports only when zoomed in."""
        feats = []
        for d, _, n in self.nav_index.query(lat, lon, radius_nm):
            feats.append((d, n.kind, n.ident, n.lat, n.lon, n.freq))
        small_ok = radius_nm <= 25
        for d, _, a in self.apt_index.query(lat, lon, radius_nm):
            if a.size >= 2 or (small_ok and a.rwy_ft >= 1500):
                feats.append((d, "A" if a.size >= 2 else "a", a.ident, a.lat, a.lon, a.freq))
        feats.sort(key=lambda t: t[0])
        return feats[:limit]

    def find_airports(self, q, near=None, limit=6):
        """Airports for an ident typed on the display, best first: the exact ident, then
        airports known by that code (IATA "ABE", FAA "N51") or whose ident starts with it.
        With the aircraft position `near`, those two kinds are ranked together, nearest
        first (so "KAB" offers KABE before Kariba, IATA KAB, when flying in Pennsylvania)."""
        q = q.strip().upper()
        if not q:
            return []
        if self._sorted_idents is None:
            self._sorted_idents = sorted(self.airports)
            self._by_code = {}
            for a in self.airports.values():
                for c in a.codes:
                    self._by_code.setdefault(c.upper(), []).append(a)
        exact = self.airports.get(q)
        by_code = sorted((a for a in self._by_code.get(q, ()) if a is not exact), key=lambda a: -a.size)
        ids = self._sorted_idents
        i = bisect.bisect_left(ids, q)
        prefix = []
        while i < len(ids) and ids[i].startswith(q):
            a = self.airports[ids[i]]
            if a is not exact and a not in by_code:
                prefix.append(a)
            i += 1
        rest = by_code + prefix
        if near:
            rest.sort(key=lambda a: (distance_bearing(near[0], near[1], a.lat, a.lon)[0], a.ident))
        return ([exact] if exact else []) + rest[:limit - (1 if exact else 0)]

    def find_point(self, ident, near=None):
        """Coordinates of an airport or navaid by ident (the one nearest `near` if several)."""
        if ident in self.airports:
            a = self.airports[ident]
            return a.lat, a.lon
        cands = self.navaids_by_id.get(ident)
        if not cands:
            return None
        if near:
            cands = sorted(cands, key=lambda n: distance_bearing(near[0], near[1], n.lat, n.lon)[0])
        return cands[0].lat, cands[0].lon


# ---------------------------------------------------------------- flight plan files

_DMS = re.compile(r"([NSEW])\s*(\d+)\D+(\d+)\D+([\d.]+)")


def _parse_msfs_pos(text):
    parts = _DMS.findall(text)
    if len(parts) < 2:
        return None
    vals = []
    for hemi, d, m, s in parts[:2]:
        v = float(d) + float(m) / 60 + float(s) / 3600
        vals.append(-v if hemi in "SW" else v)
    return vals[0], vals[1]


def load_flight_plan(path):
    """Read an MSFS .pln or X-Plane .fms file. Returns a list of (ident, lat, lon)."""
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    plan = []
    if path.lower().endswith(".pln") or text.lstrip().startswith("<?xml"):
        root = ET.fromstring(text)
        for wp in root.iter("ATCWaypoint"):
            pos = wp.findtext("WorldPosition")
            ll = _parse_msfs_pos(pos or "")
            if ll:
                plan.append((wp.get("id", "WPT")[:7], ll[0], ll[1]))
    else:
        for line in text.splitlines():
            parts = line.split()
            # v1100: <type> <ident> <via> <alt> <lat> <lon>; v3: <type> <ident> <alt> <lat> <lon>
            if len(parts) >= 5 and parts[0].isdigit() and parts[0] != "0" and len(parts[0]) <= 2:
                try:
                    lat, lon = float(parts[-2]), float(parts[-1])
                except ValueError:
                    continue
                if parts[0] in ("1", "2", "3", "11", "28") and abs(lat) <= 90:
                    plan.append((parts[1][:7], lat, lon))
    return plan


if __name__ == "__main__":
    # python navdb.py   -> download the CSVs if needed and write data/navdb.pkl.gz
    out = os.path.join(DATA_DIR, CACHE_NAME)
    if os.path.exists(out):
        os.remove(out)
    db = NavDB()
    db.save_cache(out)
    print(f"Wrote {out} ({os.path.getsize(out) // 1024} KB)")
