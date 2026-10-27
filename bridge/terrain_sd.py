#!/usr/bin/env python3
"""Prepare a microSD card with terrain elevation for the display's MAP page.

Downloads the free Terrarium elevation tiles (AWS Open Data "Terrain Tiles", sources
listed below), converts them to raw elevation and writes them to the card:

    TERRAIN/INFO.TXT                levels and the areas each one covers
    TERRAIN/<z>/<x>/<y>.BIN         256 x 256 int16 little-endian, metres, row 0 = north
    TERRAIN/<z>/OCEAN.TXT           tiles that are all water (not written; "x y" per line)

Levels: 5 and 7 for the whole world (about 4 km and 1 km per pixel), 8 and 10 for the
United States (about 500 m and 120 m). Web-Mercator tile numbering, as on web maps. Tiles that are all
water are left out; the display draws water where a covered tile is missing. Running it
again resumes: tiles already on the card are skipped.

    python terrain_sd.py E:                      # world + all US regions
    python terrain_sd.py E: --regions conus hawaii
    python terrain_sd.py out --area 40.0 -75.0 60 # small test set around a point (preview)

Data: Mapzen / AWS Terrain Tiles - SRTM, GMTED2010, ETOPO1, USGS 3DEP/NED and others;
see https://github.com/tilezen/joerd/blob/master/docs/attribution.md
"""
import argparse
import io
import math
import os
import threading
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed

import numpy as np

URL = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png"
WORLD_LEVELS = (5, 7)
DETAIL_LEVELS = (8, 10)
# lat0, lon0, lat1, lon1
US_REGIONS = {
    "conus": ("Continental US", (24.3, -125.0, 49.5, -66.8)),
    "alaska": ("Alaska", (51.0, -170.0, 71.5, -129.5)),
    "aleutians": ("Western Aleutians", (51.0, 172.0, 55.0, 180.0)),
    "hawaii": ("Hawaii", (18.8, -160.5, 22.4, -154.7)),
    "puerto_rico": ("Puerto Rico and USVI", (17.6, -67.4, 18.6, -64.5)),
}
TILE_BYTES = 256 * 256 * 2


def tile_xy(lat, lon, z):
    n = 2 ** z
    lat = max(-85.05, min(85.05, lat))
    x = int((lon + 180.0) / 360.0 * n)
    y = int((1.0 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2.0 * n)
    return min(n - 1, max(0, x)), min(n - 1, max(0, y))


def bbox_tiles(box, z):
    """(x0, y0, x1, y1) inclusive tile range of a lat/lon box."""
    lat0, lon0, lat1, lon1 = box
    x0, y1 = tile_xy(lat0, lon0, z)
    x1, y0 = tile_xy(lat1, lon1, z)
    return x0, y0, x1, y1


def plan(regions, area=None):
    """[(z, (x0, y0, x1, y1))] areas to fetch."""
    if area:
        lat, lon, nm = area
        dlat, dlon = nm / 60.0, nm / (60.0 * math.cos(math.radians(lat)))
        box = (lat - dlat, lon - dlon, lat + dlat, lon + dlon)
        return [(z, bbox_tiles(box, z)) for z in WORLD_LEVELS + DETAIL_LEVELS]
    out = [(z, (0, 0, 2 ** z - 1, 2 ** z - 1)) for z in WORLD_LEVELS]
    out += [(z, bbox_tiles(US_REGIONS[r][1], z)) for z in DETAIL_LEVELS for r in regions]
    return out


def count_tiles(areas):
    return sum((x1 - x0 + 1) * (y1 - y0 + 1) for _, (x0, y0, x1, y1) in areas)


def estimate_bytes(areas):
    """Rough size on the card: world levels are about 30 % land, the US detail about 80 %."""
    total = 0
    for z, (x0, y0, x1, y1) in areas:
        n = (x1 - x0 + 1) * (y1 - y0 + 1)
        total += n * TILE_BYTES * (0.3 if z in WORLD_LEVELS and n > 1000 else 0.8)
    return int(total)


def decode(png):
    from PIL import Image
    a = np.asarray(Image.open(io.BytesIO(png)).convert("RGB")).astype(np.float32)
    e = a[..., 0] * 256.0 + a[..., 1] + a[..., 2] / 256.0 - 32768.0
    return np.clip(np.round(e), -12000, 9000).astype("<i2")


def fetch(url, tries=4):
    for k in range(tries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "MFD-Bridge terrain"})
            with urllib.request.urlopen(req, timeout=30) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return None
            if k == tries - 1:
                raise
        except OSError:
            if k == tries - 1:
                raise
        time.sleep(1.5 * (k + 1))


class Builder:
    """Writes the tiles; progress(done, total, written_bytes) is called from worker threads."""

    def __init__(self, root, areas, progress=None, log=print, workers=16):
        self.root = os.path.join(root, "TERRAIN")
        self.areas = areas
        self.progress = progress or (lambda *a: None)
        self.log = log
        self.workers = workers
        self.cancel = threading.Event()
        self.lock = threading.Lock()
        self.done = self.written = 0
        self.ocean = {}

    def _ocean_path(self, z):
        return os.path.join(self.root, str(z), "OCEAN.TXT")

    def _load_ocean(self, z):
        s = set()
        try:
            with open(self._ocean_path(z)) as f:
                for line in f:
                    p = line.split()
                    if len(p) == 2:
                        s.add((int(p[0]), int(p[1])))
        except OSError:
            pass
        return s

    def _tile_path(self, z, x, y):
        return os.path.join(self.root, str(z), str(x), f"{y}.BIN")

    def _one(self, z, x, y):
        if self.cancel.is_set():
            return
        png = fetch(URL.format(z=z, x=x, y=y))
        elev = decode(png) if png else None
        if elev is None or elev.max() <= 0:
            with self.lock:             # one shared list file per level
                self.ocean[z].add((x, y))
                with open(self._ocean_path(z), "a") as f:
                    f.write(f"{x} {y}\n")
            return
        # Tiles are separate files: written in parallel, outside the lock.
        path = self._tile_path(z, x, y)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = path + ".TMP"
        with open(tmp, "wb") as f:
            f.write(elev.tobytes())
        os.replace(tmp, path)
        with self.lock:
            self.written += TILE_BYTES

    def _write_info(self, complete):
        os.makedirs(self.root, exist_ok=True)
        with open(os.path.join(self.root, "INFO.TXT"), "w", newline="\n") as f:
            f.write("MFD7 TERRAIN 1\n")
            f.write("# level z x0 y0 x1 y1 : area covered at that level (tile numbers, inclusive)\n")
            for z, (x0, y0, x1, y1) in self.areas:
                f.write(f"level {z} {x0} {y0} {x1} {y1}\n")
            f.write(f"complete {1 if complete else 0}\n")
            f.write("# Elevation: Mapzen/AWS Terrain Tiles (SRTM, GMTED2010, ETOPO1, USGS 3DEP and others)\n")

    def run(self):
        os.makedirs(self.root, exist_ok=True)
        self._write_info(False)
        todo = []
        total = count_tiles(self.areas)
        for z, (x0, y0, x1, y1) in self.areas:
            os.makedirs(os.path.join(self.root, str(z)), exist_ok=True)
            self.ocean.setdefault(z, self._load_ocean(z))
            for x in range(x0, x1 + 1):
                d = os.path.join(self.root, str(z), str(x))
                have = set(os.listdir(d)) if os.path.isdir(d) else set()
                for y in range(y0, y1 + 1):
                    if f"{y}.BIN" in have or (x, y) in self.ocean[z]:
                        self.done += 1
                    else:
                        todo.append((z, x, y))
        if self.done:
            self.log(f"Resuming: {self.done} of {total} tiles already on the card")
        self.progress(self.done, total, self.written)
        # Coarse levels first, so a partly built card is already usable everywhere.
        todo.sort()
        errors = 0
        with ThreadPoolExecutor(self.workers) as pool:
            futs = [pool.submit(self._one, *t) for t in todo]
            for fut in as_completed(futs):
                if self.cancel.is_set():
                    for f in futs:
                        f.cancel()
                    break
                try:
                    fut.result()
                except Exception as e:
                    errors += 1
                    if errors <= 5:
                        self.log(f"Tile failed: {e}")
                    continue
                with self.lock:
                    self.done += 1
                    done = self.done
                self.progress(done, total, self.written)
        complete = not self.cancel.is_set() and errors == 0
        self._write_info(complete)
        if errors:
            self.log(f"{errors} tiles failed; run it again to fetch them")
        return complete


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("dest", help="SD card drive (E:) or a folder")
    ap.add_argument("--regions", nargs="*", default=list(US_REGIONS), choices=list(US_REGIONS),
                    help="US detail regions (default: all)")
    ap.add_argument("--area", nargs=3, type=float, metavar=("LAT", "LON", "NM"),
                    help="only this area, all levels (for testing)")
    args = ap.parse_args()
    dest = args.dest + ("\\" if len(args.dest) == 2 and args.dest[1] == ":" else "")
    areas = plan(args.regions, args.area)
    print(f"{count_tiles(areas)} tiles, about {estimate_bytes(areas) / 1e9:.1f} GB on the card")
    t0 = time.time()

    def progress(done, total, written):
        if done % 200 == 0 or done == total:
            print(f"\r{done}/{total} tiles  {written / 1e6:.0f} MB written  {time.time() - t0:.0f} s", end="", flush=True)

    ok = Builder(dest, areas, progress).run()
    print("\ndone" if ok else "\nincomplete - run again to finish")


if __name__ == "__main__":
    main()
