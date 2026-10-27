// Direct-To guidance. Spherical-earth great-circle math in double precision: at 10 Hz
// it costs well under a millisecond on the ESP32-S3, and keeps XTK exact to a few feet
// on legs of any length.
#include "nav_dct.h"

#include <math.h>
#include <string.h>

static const double R_NM = 3440.065;
static const double D2R = 0.017453292519943295, R2D = 57.29577951308232;

static DctNav s_dct;
static uint32_t s_last_ms, s_last_gen;

static double wrap360d(double a) {
    a = fmod(a, 360.0);
    return a < 0 ? a + 360.0 : a;
}

void gc_dist_brg(double lat1, double lon1, double lat2, double lon2, double &nm, double &brg_true) {
    double p1 = lat1 * D2R, p2 = lat2 * D2R, dl = (lon2 - lon1) * D2R;
    double sp = sin((p2 - p1) / 2), sl = sin(dl / 2);
    double a = sp * sp + cos(p1) * cos(p2) * sl * sl;
    nm = 2 * R_NM * asin(fmin(1.0, sqrt(a)));
    double y = sin(dl) * cos(p2), x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
    brg_true = wrap360d(atan2(y, x) * R2D);
}

void gc_interp(double lat1, double lon1, double lat2, double lon2, double f, double &lat, double &lon) {
    double p1 = lat1 * D2R, l1 = lon1 * D2R, p2 = lat2 * D2R, l2 = lon2 * D2R;
    double d, b;
    gc_dist_brg(lat1, lon1, lat2, lon2, d, b);
    double delta = d / R_NM;
    if (delta < 1e-9) {
        lat = lat1;
        lon = lon1;
        return;
    }
    double A = sin((1 - f) * delta) / sin(delta), B = sin(f * delta) / sin(delta);
    double x = A * cos(p1) * cos(l1) + B * cos(p2) * cos(l2);
    double y = A * cos(p1) * sin(l1) + B * cos(p2) * sin(l2);
    double z = A * sin(p1) + B * sin(p2);
    lat = atan2(z, sqrt(x * x + y * y)) * R2D;
    lon = atan2(y, x) * R2D;
}

void gc_cross_along(double lat1, double lon1, double lat2, double lon2, double lat3, double lon3, double &xtk,
                    double &atk) {
    double d13, b13, d12, b12;
    gc_dist_brg(lat1, lon1, lat3, lon3, d13, b13);
    gc_dist_brg(lat1, lon1, lat2, lon2, d12, b12);
    double dxt = asin(sin(d13 / R_NM) * sin((b13 - b12) * D2R));
    xtk = dxt * R_NM;
    double c = cos(d13 / R_NM) / cos(dxt);
    atk = acos(fmax(-1.0, fmin(1.0, c))) * R_NM;
    if (cos((b13 - b12) * D2R) < 0) atk = -atk;       // behind the start of the course
}

void gc_offset(double lat, double lon, double dist_nm, double brg_true, double &lat2, double &lon2) {
    double d = dist_nm / R_NM, b = brg_true * D2R, p1 = lat * D2R, l1 = lon * D2R;
    double p2 = asin(sin(p1) * cos(d) + cos(p1) * sin(d) * cos(b));
    double l2 = l1 + atan2(sin(b) * sin(d) * cos(p1), cos(d) - sin(p1) * sin(p2));
    lat2 = p2 * R2D;
    lon2 = fmod(l2 * R2D + 540.0, 360.0) - 180.0;
}

const DctNav &dct() { return s_dct; }

void dct_activate(const DctTarget &t, double lat, double lon, uint32_t now_ms) {
    s_dct.wpt = t;
    s_dct.wpt.id[sizeof(s_dct.wpt.id) - 1] = 0;
    s_dct.wpt.name[sizeof(s_dct.wpt.name) - 1] = 0;
    s_dct.org_lat = lat;
    s_dct.org_lon = lon;
    double d, b;
    gc_dist_brg(lat, lon, t.lat, t.lon, d, b);
    s_dct.leg_nm = (float)d;
    s_dct.active = true;
    s_dct.valid = false;
    s_dct.gen++;
    (void)now_ms;
}

void dct_cancel() {
    if (!s_dct.active) return;
    s_dct.active = false;
    s_dct.valid = false;
    s_dct.gen++;
}

void dct_update(const Telemetry &t, bool live, uint32_t now_ms) {
    DctNav &D = s_dct;
    if (!D.active) return;
    if (!live) {
        D.valid = false;
        return;
    }
    if (D.valid && s_last_gen == D.gen && now_ms - s_last_ms < 1000 / DCT_HZ) return;
    s_last_ms = now_ms;
    s_last_gen = D.gen;

    const double lat = t.nav.lat, lon = t.nav.lon;
    const double magvar = t.nav.magvar;          // east +: magnetic = true - magvar
    double dis, brg;
    gc_dist_brg(lat, lon, D.wpt.lat, D.wpt.lon, dis, brg);
    D.dis = (float)dis;
    D.brg = (float)wrap360d(brg - magvar);
    D.trk = (float)wrap360d(t.pfd.trk);

    double dtk_true;
    if (D.leg_nm < 0.1f) {
        // Activated on top of the waypoint: no course line, steer straight to it.
        dtk_true = brg;
        D.xtk = 0;
        D.from = false;
    } else {
        double xtk, atk, leg, b12, b21;
        gc_cross_along(D.org_lat, D.org_lon, D.wpt.lat, D.wpt.lon, lat, lon, xtk, atk);
        gc_dist_brg(D.org_lat, D.org_lon, D.wpt.lat, D.wpt.lon, leg, b12);
        D.xtk = (float)xtk;
        D.from = atk >= leg - 0.01;
        if (D.from) {
            // Past the waypoint: the course's final bearing, continued.
            double d2;
            gc_dist_brg(D.wpt.lat, D.wpt.lon, D.org_lat, D.org_lon, d2, b21);
            dtk_true = wrap360d(b21 + 180.0);
        } else {
            // A great circle's bearing changes along it: use the bearing at the abeam point.
            double plat, plon, d2;
            gc_offset(D.org_lat, D.org_lon, atk, b12, plat, plon);
            gc_dist_brg(plat, plon, D.wpt.lat, D.wpt.lon, d2, dtk_true);
        }
    }
    D.dtk_true = (float)dtk_true;
    D.dtk = (float)wrap360d(dtk_true - magvar);
    D.ete = t.pfd.gs > 30 ? (int)lround(dis / t.pfd.gs * 3600.0) : -1;
    // Garmin-style CDI scaling: ENR 2.0 nm, TERM 1.0 nm within 30 nm of the destination.
    D.cdi_fs = dis <= 30.0 ? 1.0f : 2.0f;
    D.valid = true;
}

void dct_apply(NavInfo &n) {
    const DctNav &D = s_dct;
    if (!D.active || !D.valid) return;
    n.has_to = n.has_dtk = true;
    strncpy(n.to, D.wpt.id, sizeof(n.to) - 1);
    n.to[sizeof(n.to) - 1] = 0;
    n.to_dist = D.dis;
    n.to_brg = D.brg;
    n.dtk = D.dtk;
    n.xtk = D.xtk;
    n.ete = D.ete;
    n.cdi_fs = D.cdi_fs;
    n.from = D.from;
    n.dct = true;
}
