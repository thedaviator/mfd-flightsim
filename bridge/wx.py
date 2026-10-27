"""Weather for the WEATHER page.

Neither MSFS nor X-Plane exposes weather-radar returns or lightning to add-ons, so the
bridge supplies them itself, in one of two ways:

    LiveWeather       real data from the internet: RainViewer's radar mosaic and NOAA
                      GOES Geostationary Lightning Mapper flashes (the Americas only).
                      It matches the sim's sky when the sim runs live weather.
    SyntheticWeather  storm cells generated around the aircraft that drift with the
                      sim's wind, for flying offline or with preset weather.

Both send the same packet: the radar as a north-up grid of precipitation levels
(0 none, 1 green, 2 yellow, 3 red, 4 magenta) centred on the aircraft, lightning as a
list of strikes, and a status line for each that the display shows under the mode name.
"""
import calendar
import io
import json
import math
import random
import re
import threading
import time
import urllib.error
import urllib.request

import numpy as np

from navdb import distance_bearing, offset

GRID_N = 96                     # grid cells per side
DBZ_LEVELS = (20, 30, 40, 50)   # dBZ where green, yellow, red and magenta start
LTNG_RADIUS = 200               # nm; the stormscope's largest range
LTNG_KEEP = 180                 # s of strikes kept (GLM data arrives 30-60 s late)
MAX_STRIKES = 150
USER_AGENT = "MFD-Bridge (flight-sim MFD weather)"

_LEVEL_CHARS = ".gyrm"


def encode_grid(levels):
    """Run-length text for a grid of levels 0-4, row by row from the north-west corner:
    a level character (".gyrm") followed by the run length when it is more than 1."""
    flat = levels.ravel()
    if flat.size == 0:
        return ""
    starts = np.concatenate(([0], np.flatnonzero(np.diff(flat)) + 1))
    lengths = np.diff(np.concatenate((starts, [flat.size])))
    return "".join(_LEVEL_CHARS[flat[s]] + (str(n) if n > 1 else "") for s, n in zip(starts.tolist(), lengths.tolist()))


def grid_offsets(half_nm, n, sub=1):
    """East and north offsets (nm) of the (sub-sampled) cell centres of an n x n grid
    covering +-half_nm, rows from north to south."""
    k = n * sub
    c = (np.arange(k) + 0.5) * (2.0 * half_nm / k) - half_nm
    return np.broadcast_to(c, (k, k)), np.broadcast_to(-c[:, None], (k, k))


def grid_packet(levels, lat, lon, half_nm):
    rle = encode_grid(levels)
    while len(rle) > 9000 and levels.shape[0] >= 32:
        # Very broken-up weather: halve the resolution so the packet stays well under the
        # display's 15 KB WebSocket limit (strongest return of each 2 x 2 block).
        n = levels.shape[0] // 2
        levels = levels[:2 * n, :2 * n].reshape(n, 2, n, 2).max(axis=(1, 3))
        rle = encode_grid(levels)
    return {"lat": round(lat, 5), "lon": round(lon, 5), "nm": round(half_nm, 2), "n": int(levels.shape[0]), "rle": rle}


# ---------------------------------------------------------------- synthetic

class Cell:
    __slots__ = ("lat", "lon", "r", "peak", "phase", "period")

    def __init__(self, lat, lon, r, peak):
        self.lat, self.lon, self.r, self.peak = lat, lon, r, peak
        self.phase = random.uniform(0, 2 * math.pi)
        self.period = random.uniform(600, 1500)

    def level(self, t):
        """Intensity 0..3 (green, yellow, red, magenta)."""
        return max(0.0, self.peak * (0.7 + 0.3 * math.sin(self.phase + 2 * math.pi * t / self.period)))


class SyntheticWeather:
    def __init__(self, seed=None, clock=time.monotonic):
        self.clock = clock
        self.rng = random.Random(seed)
        self.cells = []
        self.strikes = []          # (lat, lon, time)
        self.t0 = clock()
        self.last = self.t0
        self.origin = None
        self.radar_nm = 40.0       # half-width of the radar grid (the display's range)

    def close(self):
        pass

    def _seed_storms(self, lat, lon):
        self.origin = (lat, lon)
        for _ in range(5):
            # A storm system: one core and several satellite cells.
            clat, clon = offset(lat, lon, self.rng.uniform(12, 70), self.rng.uniform(0, 360))
            peak = self.rng.uniform(2.0, 3.9)
            self.cells.append(Cell(clat, clon, self.rng.uniform(2.5, 4.5), peak))
            for _ in range(self.rng.randint(4, 9)):
                slat, slon = offset(clat, clon, self.rng.uniform(2, 9), self.rng.uniform(0, 360))
                self.cells.append(Cell(slat, slon, self.rng.uniform(1.5, 4.0), peak * self.rng.uniform(0.3, 0.8)))
        # A broad area of light rain.
        rlat, rlon = offset(lat, lon, self.rng.uniform(20, 45), self.rng.uniform(0, 360))
        for _ in range(14):
            slat, slon = offset(rlat, rlon, self.rng.uniform(0, 14), self.rng.uniform(0, 360))
            self.cells.append(Cell(slat, slon, self.rng.uniform(3, 6), self.rng.uniform(0.6, 1.2)))

    def update(self, lat, lon, wind_from, wind_kt):
        now = self.clock()
        dt, self.last = now - self.last, now
        if self.origin is None:
            if lat == 0 and lon == 0:
                return
            self._seed_storms(lat, lon)
        # Re-seed if the aircraft has moved far from all weather (e.g. a new flight).
        if distance_bearing(lat, lon, *self.origin)[0] > 150:
            self.cells, self.strikes = [], []
            self._seed_storms(lat, lon)
        drift_nm = wind_kt * dt / 3600.0
        for c in self.cells:
            if drift_nm > 0:
                c.lat, c.lon = offset(c.lat, c.lon, drift_nm, (wind_from + 180) % 360)
        t = now - self.t0
        for c in self.cells:
            lvl = c.level(t)
            if lvl > 2.0 and self.rng.random() < (lvl - 2.0) * 0.35 * dt:
                slat, slon = offset(c.lat, c.lon, self.rng.uniform(0, c.r), self.rng.uniform(0, 360))
                self.strikes.append((slat, slon, now))
        self.strikes = [s for s in self.strikes if now - s[2] < 120]

    def _grid(self, lat, lon, half):
        """Cells as nested discs: each level's disc is smaller than the one below it."""
        east, north = grid_offsets(half, GRID_N)
        levels = np.zeros((GRID_N, GRID_N), np.uint8)
        t = self.clock() - self.t0
        coslat = math.cos(math.radians(lat))
        for c in self.cells:
            ce, cn = (c.lon - lon) * 60.0 * coslat, (c.lat - lat) * 60.0
            if abs(ce) > half + c.r or abs(cn) > half + c.r:
                continue
            lvl = min(c.level(t), 4.0) + 0.35          # level 0.65..1 still shows green
            if lvl < 1:
                continue
            d = np.hypot(east - ce, north - cn)
            for k in range(1, min(4, int(lvl)) + 1):
                levels[(d < c.r * (1.0 - (k - 1) * 0.28)) & (levels < k)] = k
        return levels

    def packet(self, lat, lon):
        now = self.clock()
        half = self.radar_nm
        strikes = [[round(s[0], 4), round(s[1], 4), int(now - s[2])]
                   for s in self.strikes if distance_bearing(lat, lon, s[0], s[1])[0] < LTNG_RADIUS]
        return {"grid": grid_packet(self._grid(lat, lon, half), lat, lon, half), "strikes": strikes[-MAX_STRIKES:],
                "rdr_txt": "SIMULATED", "rdr_ok": 0, "ltg_txt": "SIMULATED", "ltg_ok": 0}


# ---------------------------------------------------------------- live

def http_get(url, timeout=20):
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def json_get(url, timeout=20):
    return json.loads(http_get(url, timeout))


def short_error(e):
    """A few words for the display's status line."""
    if isinstance(e, urllib.error.HTTPError):
        return f"HTTP {e.code}"
    if isinstance(e, (urllib.error.URLError, OSError)):
        return "OFFLINE"
    return "ERROR"


# RainViewer's "Universal Blue" scheme (the only one its free API serves): rain colours
# as RGBA for -10 .. 95 dBZ in 1 dBZ steps (rainviewer.com/files/rainviewer_api_colors_table.csv).
_UB_RAIN = [
    0x63615914, 0x66635a19, 0x69665c1e, 0x6c685d24, 0x6f6b5f29, 0x726e612e, 0x75706234, 0x78736439,
    0x7c75653e, 0x7f786744, 0x827b6949, 0x857d6a4e, 0x88806c54, 0x8b826d59, 0x8e856f5e, 0x92887164,
    0x9e93756e, 0xaa9e7978, 0xb6a97e82, 0xc2b4828c, 0xcec08796, 0xd2c48ba0, 0xd6c88faa, 0xdacc93b4,
    0xded097be, 0x88ddeeff, 0x6cd1ebff, 0x51c5e8ff, 0x36bae5ff, 0x1baee2ff, 0x00a3e0ff, 0x009ad5ff,
    0x0091caff, 0x0088bfff, 0x007fb4ff, 0x0077aaff, 0x0070a3ff, 0x00699cff, 0x006295ff, 0x005b8eff,
    0x005588ff, 0x005180ff, 0x004e78ff, 0x004a70ff, 0x004768ff, 0xffee00ff, 0xffe000ff, 0xffd200ff,
    0xffc500ff, 0xffb700ff, 0xffaa00ff, 0xff9f00ff, 0xff9500ff, 0xff8b00ff, 0xff8100ff, 0xff4400ff,
    0xf23600ff, 0xe62800ff, 0xd91b00ff, 0xcd0d00ff, 0xc10000ff, 0xa80000ff, 0x8f0000ff, 0x760000ff,
    0x5d0000ff, 0xffaaffff, 0xff9fffff, 0xff95ffff, 0xff8bffff, 0xff81ffff, 0xff77ffff, 0xff6cffff,
    0xff62ffff, 0xff58ffff, 0xff4effff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0x00ff00ff, 0x00ff00ff, 0x00ff00ff,
    0x00ff00ff, 0x00ff00ff, 0x00ff00ff, 0x00ff00ff, 0x00ff00ff, 0x00ff00ff, 0x00ff00ff, 0x00ff00ff,
    0x00ff00ff, 0x00ff00ff, 0x00ff00ff, 0x00ff00ff, 0x00ff00ff, 0x00ff00ff, 0x00ff00ff, 0x00ff00ff,
    0x00ff00ff, 0x00ff00ff,
]


def _colour_levels():
    """Sorted RGBA keys and the precipitation level of each (repeated colours: lowest dBZ)."""
    lvl = {}
    for i, rgba in enumerate(_UB_RAIN):
        lvl.setdefault(rgba, int(np.searchsorted(DBZ_LEVELS, -10 + i, side="right")))
    keys = sorted(lvl)
    return np.array(keys, np.uint32), np.array([lvl[k] for k in keys], np.uint8)


class RainViewer:
    """Radar mosaic tiles (wherever there are radars, about 0.6 nm per pixel), fetched in
    a background thread around the aircraft and turned into precipitation levels."""
    API = "https://api.rainviewer.com/public/weather-maps.json"
    Z, TS = 6, 512              # zoom 6 with 512 px tiles = the resolution of zoom 7, the API's limit
    WORLD = TS * 2 ** Z
    FRAME_EVERY = 120           # s between checks for a new frame (one comes every 10 min)
    MAX_TILES = 40

    def __init__(self, log):
        self.log = log
        self.keys, self.key_levels = _colour_levels()
        self.lock = threading.Lock()
        self.want = set()               # tiles (x, y) around the aircraft
        self.frame = None               # (host, path, unix time) of the newest frame
        self.shown = None               # the newest frame whose tiles are all loaded
        self.tiles = {}                 # (path, x, y) -> level array
        self.error = None
        self.stop = threading.Event()
        self.wake = threading.Event()
        threading.Thread(target=self._run, daemon=True, name="rainviewer").start()

    def _pixel(self, lat, lon):
        lat = np.clip(lat, -85.0, 85.0)
        x = (np.asarray(lon) + 180.0) / 360.0 * self.WORLD
        y = (1.0 - np.arcsinh(np.tan(np.radians(lat))) / math.pi) / 2.0 * self.WORLD
        return x, y

    def need(self, lat, lon, half_nm):
        """Ask for the tiles covering +-half_nm around (lat, lon)."""
        dlat = half_nm / 60.0
        dlon = half_nm / (60.0 * max(0.05, math.cos(math.radians(lat))))
        x0, y0 = self._pixel(lat + dlat, lon - dlon)
        x1, y1 = self._pixel(lat - dlat, lon + dlon)
        n = 2 ** self.Z
        want = {(x % n, y) for x in range(int(x0 // self.TS), int(x1 // self.TS) + 1)
                for y in range(max(0, int(y0 // self.TS)), min(n - 1, int(y1 // self.TS)) + 1)}
        with self.lock:
            changed = want != self.want
            self.want = want
        if changed:
            self.wake.set()

    def _decode(self, png):
        from PIL import Image
        im = np.asarray(Image.open(io.BytesIO(png)).convert("RGBA")).astype(np.uint32)
        rgba = (im[..., 0] << 24) | (im[..., 1] << 16) | (im[..., 2] << 8) | im[..., 3]
        idx = np.clip(np.searchsorted(self.keys, rgba), 0, len(self.keys) - 1)
        return np.where(self.keys[idx] == rgba, self.key_levels[idx], 0).astype(np.uint8)

    def _step(self, next_frame):
        if time.time() >= next_frame:
            info = json_get(self.API)
            past = info["radar"]["past"]
            if past:
                self.frame = (info["host"], past[-1]["path"], past[-1]["time"])
            next_frame = time.time() + self.FRAME_EVERY
        if not self.frame:
            return next_frame
        host, path, ftime = self.frame
        with self.lock:
            want = set(self.want)
        for x, y in sorted(want):
            if self.stop.is_set():
                return next_frame
            if (path, x, y) not in self.tiles:
                self.tiles[(path, x, y)] = self._decode(http_get(f"{host}{path}/{self.TS}/{self.Z}/{x}/{y}/2/0_0.png"))
        if want and all((path, x, y) in self.tiles for x, y in want):
            if not self.shown or self.shown[1] != path:
                self.log(f"Weather radar: RainViewer frame of {time.strftime('%H:%M', time.localtime(ftime))}")
            self.shown = self.frame
            # Keep only this frame's tiles, and only those near the aircraft once there are many.
            tiles = {k: v for k, v in self.tiles.items() if k[0] == path}
            if len(tiles) > self.MAX_TILES:
                tiles = {k: v for k, v in tiles.items() if (k[1], k[2]) in want}
            self.tiles = tiles
        return next_frame

    def _run(self):
        next_frame = 0.0
        while not self.stop.is_set():
            try:
                next_frame = self._step(next_frame)
                self.error = None
            except Exception as e:
                if self.error is None:
                    self.log(f"Weather radar unavailable: {e}")
                self.error = short_error(e)
                next_frame = min(next_frame, time.time() + 30)
                self.stop.wait(30)
            self.wake.wait(5)
            self.wake.clear()

    def grid(self, lat, lon, half_nm):
        """Levels on a GRID_N grid around (lat, lon), or None until the tiles are loaded.
        Each cell takes the strongest return of up to 4 x 4 samples inside it."""
        shown = self.shown
        if not shown:
            return None
        path = shown[1]
        coslat = max(0.05, math.cos(math.radians(lat)))
        px_nm = 360.0 / self.WORLD * 60.0 * coslat
        sub = int(min(4, max(1, math.ceil(2.0 * half_nm / GRID_N / px_nm))))
        east, north = grid_offsets(half_nm, GRID_N, sub)
        x, y = self._pixel(lat + north / 60.0, lon + east / (60.0 * coslat))
        tx, ty = (x // self.TS).astype(np.int64) % 2 ** self.Z, (y // self.TS).astype(np.int64)
        lx, ly = (x % self.TS).astype(np.int64), (y % self.TS).astype(np.int64)
        out = np.zeros(x.shape, np.uint8)
        for key, tile in list(self.tiles.items()):
            if key[0] == path:
                m = (tx == key[1]) & (ty == key[2])
                if m.any():
                    out[m] = tile[ly[m], lx[m]]
        return out.reshape(GRID_N, sub, GRID_N, sub).max(axis=(1, 3))

    def status(self):
        if self.shown:
            return f"RAINVIEWER {max(0, int((time.time() - self.shown[2]) / 60))} MIN", 1
        if self.error:
            return f"NO RADAR: {self.error}", 0
        return "RADAR LOADING", 0

    def close(self):
        self.stop.set()
        self.wake.set()


def _glm_start(key):
    """Unix time of a GLM file's first observation, from its name (..._sYYYYDDDHHMMSSs_...)."""
    m = re.search(r"_s(\d{4})(\d{3})(\d{2})(\d{2})(\d{2})", key)
    if not m:
        return 0
    y, doy, hh, mm, ss = (int(g) for g in m.groups())
    return calendar.timegm((y, 1, 1, hh, mm, ss, 0, 0, 0)) + (doy - 1) * 86400


class GoesLightning:
    """NOAA GOES-R Geostationary Lightning Mapper flashes from the public AWS buckets
    (one file per satellite every 20 s, about 250 KB). Covers the Americas and the
    neighbouring oceans; elsewhere the page says there is no coverage."""
    # (bucket, name, sub-satellite longitude)
    SATS = (("noaa-goes19", "GOES-19", -75.2), ("noaa-goes18", "GOES-18", -137.0))
    MAX_ANGLE = 54.0            # deg of arc from the sub-satellite point that GLM sees
    POLL = 20

    def __init__(self, log):
        self.log = log
        self.pos = None
        self.flashes = []               # (lat, lon, unix time)
        self.seen = set()
        self.sat = None
        self.error = None
        self.stop = threading.Event()
        threading.Thread(target=self._run, daemon=True, name="glm").start()

    def _pick(self, lat, lon):
        best = None
        for sat in self.SATS:
            ang = distance_bearing(0.0, sat[2], lat, lon)[0] / 60.0
            if ang <= self.MAX_ANGLE and (best is None or ang < best[0]):
                best = (ang, sat)
        return best[1] if best else None

    def _list(self, bucket, t):
        prefix = time.strftime("GLM-L2-LCFA/%Y/%j/%H/", time.gmtime(t))
        xml = http_get(f"https://{bucket}.s3.amazonaws.com/?list-type=2&prefix={prefix}").decode()
        return re.findall(r"<Key>([^<]+\.nc)</Key>", xml)

    @staticmethod
    def _read(data, lat, lon):
        """Flashes within reach of (lat, lon): [(lat, lon, unix time)]."""
        import h5py
        with h5py.File(io.BytesIO(data), "r") as f:
            flat = f["flash_lat"][:].astype(np.float64)
            flon = f["flash_lon"][:].astype(np.float64)
            if flat.size == 0:
                return []
            v = f["flash_time_offset_of_first_event"]
            raw = v[:].astype(np.int64)
            if v.attrs.get("_Unsigned", b"false") in (b"true", "true"):
                raw &= 0xFFFF
            dt = raw * float(np.ravel(v.attrs.get("scale_factor", 1.0))[0]) + float(np.ravel(v.attrs.get("add_offset", 0.0))[0])
            t0 = 946728000.0 + float(f["product_time"][()])      # seconds since 2000-01-01 12:00 UTC
            q = f["flash_quality_flag"][:]
        d = np.hypot((flon - lon) * 60.0 * math.cos(math.radians(lat)), (flat - lat) * 60.0)
        # Quality flags 1/3/5 only mark unusually long or large flashes (strong storms): keep them.
        keep = (q >= 0) & (d < LTNG_RADIUS + 150)
        return list(zip(flat[keep].tolist(), flon[keep].tolist(), (t0 + dt[keep]).tolist()))

    def _step(self):
        pos = self.pos
        if not pos:
            return
        sat = self._pick(*pos)
        if sat != self.sat:
            self.flashes, self.seen, self.sat = [], set(), sat
            if sat:
                self.log(f"Lightning: {sat[1]} GLM")
        if not sat:
            return
        now = time.time()
        keys = self._list(sat[0], now)
        if time.gmtime(now).tm_min < 4:
            keys = self._list(sat[0], now - 3600) + keys
        for key in keys:
            if key in self.seen or self.stop.is_set():
                continue
            if now - _glm_start(key) <= LTNG_KEEP:
                self.flashes = self.flashes + self._read(http_get(f"https://{sat[0]}.s3.amazonaws.com/{key}"), *pos)
            self.seen.add(key)
        self.flashes = [f for f in self.flashes if time.time() - f[2] <= LTNG_KEEP]
        if len(self.seen) > 1000:
            self.seen = set(sorted(self.seen)[-400:])

    def _run(self):
        while not self.stop.is_set():
            try:
                self._step()
                self.error = None
            except Exception as e:
                if self.error is None:
                    self.log(f"Lightning data unavailable: {e}")
                self.error = short_error(e)
            self.stop.wait(self.POLL if self.pos else 2)

    def strikes(self, lat, lon):
        """[[lat, lon, age s]] of the newest flashes within LTNG_RADIUS."""
        now = time.time()
        coslat = math.cos(math.radians(lat))
        out = []
        for fl in sorted(self.flashes, key=lambda f: -f[2]):
            if math.hypot((fl[1] - lon) * 60.0 * coslat, (fl[0] - lat) * 60.0) < LTNG_RADIUS:
                out.append([round(fl[0], 4), round(fl[1], 4), max(0, int(now - fl[2]))])
                if len(out) >= MAX_STRIKES:
                    break
        return out

    def status(self):
        if self.pos and not self._pick(*self.pos):
            return "NO GLM COVERAGE", 0
        if self.error:
            return f"NO LTNG: {self.error}", 0
        if self.sat:
            return f"{self.sat[1]} GLM", 1
        return "LTNG LOADING", 0

    def close(self):
        self.stop.set()


class LiveWeather:
    def __init__(self, log):
        self.radar = RainViewer(log)
        self.ltng = GoesLightning(log)
        self.radar_nm = 40.0

    def update(self, lat, lon, wind_from, wind_kt):
        if lat == 0 and lon == 0:
            return
        self.ltng.pos = (lat, lon)
        self.radar.need(lat, lon, self.radar_nm)

    def packet(self, lat, lon):
        rdr, rdr_ok = self.radar.status()
        ltg, ltg_ok = self.ltng.status()
        pkt = {"strikes": self.ltng.strikes(lat, lon), "rdr_txt": rdr, "rdr_ok": rdr_ok, "ltg_txt": ltg, "ltg_ok": ltg_ok}
        levels = self.radar.grid(lat, lon, self.radar_nm)
        if levels is not None:
            pkt["grid"] = grid_packet(levels, lat, lon, self.radar_nm)
        return pkt

    def close(self):
        self.radar.close()
        self.ltng.close()
