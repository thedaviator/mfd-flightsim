"""METARs for the NEAREST page, from the Aviation Weather Center's data API
(aviationweather.gov, worldwide reports). A background thread fetches the reports of the
airports on the list and refreshes them every 10 minutes."""
import json
import threading
import time
import urllib.parse

from wx import http_get, short_error

URL = "https://aviationweather.gov/api/data/metar"
REFRESH = 600           # s a report (or "no report") is kept before asking again
BATCH = 40              # stations per request


def short_metar(raw, ident):
    """'METAR KJFK 081451Z 28012G18KT 10SM FEW090 21/12 A2987 RMK AO2 ...' ->
    '081451Z 28012G18KT 10SM FEW090 21/12 A2987' (the list shows the identifier)."""
    words = raw.split()
    if words and words[0] in ("METAR", "SPECI"):
        words = words[1:]
    if words and words[0] == ident:
        words = words[1:]
    if "RMK" in words:
        words = words[:words.index("RMK")]
    return " ".join(words)[:80]


class MetarService:
    def __init__(self, log):
        self.log = log
        self.reports = {}           # ident -> (text, flight category, fetched at) or (None, None, t)
        self.want = []
        self.error = None
        self.stop = threading.Event()
        self.wake = threading.Event()
        threading.Thread(target=self._run, daemon=True, name="metar").start()

    def request(self, idents):
        """The airports on the NEAREST list (fetched in the background)."""
        idents = [i for i in idents if 3 <= len(i) <= 4 and i.isalnum()]
        if idents != self.want:
            self.want = idents
            self.wake.set()

    def get(self, ident):
        """(METAR text, flight category) or None."""
        r = self.reports.get(ident)
        return (r[0], r[1]) if r and r[0] else None

    def _fetch(self, ids):
        q = urllib.parse.urlencode({"ids": ",".join(ids), "format": "json"})
        body = http_get(f"{URL}?{q}").strip()
        data = json.loads(body) if body else []          # empty reply (HTTP 204): no reports
        now = time.time()
        got = {}
        for m in data if isinstance(data, list) else []:
            ident = m.get("icaoId")
            if ident in ids and m.get("rawOb") and ident not in got:
                got[ident] = (short_metar(m["rawOb"], ident), m.get("fltCat") or "", now)
        for ident in ids:
            self.reports[ident] = got.get(ident, (None, None, now))

    def _run(self):
        while not self.stop.is_set():
            now = time.time()
            stale = [i for i in self.want if now - self.reports.get(i, (0, 0, 0))[2] > REFRESH]
            try:
                for k in range(0, len(stale), BATCH):
                    self._fetch(stale[k:k + BATCH])
                if self.error:
                    self.log("METARs available again")
                self.error = None
            except Exception as e:
                if self.error is None:
                    self.log(f"METARs unavailable: {e}")
                self.error = short_error(e)
                self.stop.wait(60)
            self.wake.wait(10)
            self.wake.clear()

    def close(self):
        self.stop.set()
        self.wake.set()
