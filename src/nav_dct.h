#pragma once
// Direct-To navigation: the course from where the pilot pressed ACTIVATE to the chosen
// airport, and the guidance along it (DTK, DIS, ETE, XTK, TO/FROM), computed on the
// display from the aircraft's own position, track and ground speed.
// Portable: compiled into the firmware and into the PC preview.
#include <stdint.h>

#include "telemetry.h"

#define DCT_HZ 10                   // guidance update rate

struct DctTarget {
    char id[8];
    char name[42];
    double lat, lon;
};

struct DctNav {
    bool active;
    DctTarget wpt;
    double org_lat, org_lon;        // start of the course (aircraft position at ACTIVATE)
    float leg_nm;                   // origin to waypoint
    uint32_t gen;                   // bumped on every activate / cancel

    // Guidance, refreshed at DCT_HZ while there is a position.
    bool valid;
    float dis;                      // nm, great circle, aircraft to waypoint
    float brg;                      // deg magnetic, aircraft to waypoint
    float dtk;                      // deg magnetic, desired track at the abeam point
    float dtk_true;
    float trk;                      // deg magnetic, ground track (from the sim)
    float xtk;                      // nm, + = aircraft right of course
    int ete;                        // s, -1 below 30 kt
    bool from;                      // abeam point is past the waypoint
    float cdi_fs;                   // CDI full-scale deflection, nm (2.0 ENR, 1.0 TERM)
};

const DctNav &dct();
void dct_activate(const DctTarget &t, double lat, double lon, uint32_t now_ms);
void dct_cancel();
// Recompute the guidance (at most DCT_HZ times a second; cheap to call every tick).
void dct_update(const Telemetry &t, bool live, uint32_t now_ms);
// While Direct-To is active, replace the simulator's active waypoint in `n` with it, so
// every page (PFD waypoint and CDI, map data bar) shows the Direct-To guidance.
void dct_apply(NavInfo &n);

// Great-circle helpers (spherical earth, 3440.065 nm).
void gc_dist_brg(double lat1, double lon1, double lat2, double lon2, double &nm, double &brg_true);
// Point at fraction f (0..1) of the great circle from point 1 to point 2.
void gc_interp(double lat1, double lon1, double lat2, double lon2, double f, double &lat, double &lon);
// Cross-track (+ right) and along-track distances, nm, of point 3 from the course 1 -> 2.
void gc_cross_along(double lat1, double lon1, double lat2, double lon2, double lat3, double lon3, double &xtk,
                    double &atk);
// The point dist_nm from (lat, lon) along true bearing brg.
void gc_offset(double lat, double lon, double dist_nm, double brg_true, double &lat2, double &lon2);
