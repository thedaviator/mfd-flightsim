#pragma once
// Telemetry received from the bridge, and the commands sent back to it.
// Portable: compiled into the firmware and into the PC preview.
#include <stdint.h>
#include <stddef.h>

#define MAX_CYL      6
#define MAX_NRST     25
#define MAX_FPL      40
#define MAX_MAP_PTS  160
#define WX_GRID_MAX  96          // radar grid cells per side
#define MAX_STRIKES  150

struct Pfd {
    float pitch, roll;          // deg; roll + = right wing down
    float ias, tas, gs;         // kt
    float alt, agl;             // ft
    float hdg, trk;             // deg magnetic
    float vsi;                  // fpm
    float slip;                 // ball, -1..1
    float turn;                 // deg/s
    float baro;                 // inHg
    float oat;                  // deg C
    bool park_brake;            // parking brake set
    bool on_ground;
};

struct NavInfo {
    double lat, lon;
    float magvar;               // east +
    float wind_dir, wind_spd;   // from, true
    char to[8];
    bool has_to, has_dtk;
    float to_dist, to_brg, dtk, xtk;   // to_brg, dtk deg magnetic; xtk nm, + = right of course
    int ete;                    // s, -1 unknown
    float cdi_fs;               // CDI full scale, nm (2.0 from the bridge)
    bool from;                  // past the waypoint
    bool dct;                   // the waypoint is the display's own Direct-To (nav_dct)
};

// Airport lookup for the Direct-To window: the bridge's answer to cmd_apt_find(q).
#define MAX_APT_HITS 6
struct AptHit {
    char id[8];
    char name[42];
    char city[26];
    double lat, lon;
    int32_t elev;               // ft
};
struct AptFind {
    char q[8];                  // the query this answers
    uint8_t n;
    AptHit hit[MAX_APT_HITS];   // best first: exact ident, other codes (IATA / FAA), then nearest by prefix
    uint32_t seq;               // bumped with every answer
};

struct Eis {
    float rpm, mrp, ff, oil_t, oil_p, fuel_p;
    float cht[MAX_CYL], egt[MAX_CYL];
    uint8_t ncyl;
    float fuel_l, fuel_r, fuel_cap;
    float volts, amps;
};

struct Radios {
    char com1_act[8], com1_stby[8], com2_act[8], com2_stby[8];
    char nav1_act[8], nav1_stby[8], nav2_act[8], nav2_stby[8];
    char xpdr[5], xpdr_mode[5];
};

struct Autopilot {
    bool master, fd, yd;
    char lat[5], lat_arm[12], vert[5], vert_arm[12];
    float hdg, alt, vs, ias;
};

struct NrstApt {
    char icao[8];
    char name[42];
    float dist;
    int16_t brg;
    int32_t rwy, elev;
    char freq[8];
    char ftype[6];
    char metar[84];             // "081451Z 28012G18KT 10SM FEW090 21/12 A2987", "" none
    char cat[5];                // flight category: VFR MVFR IFR LIFR, "" unknown
};

struct FplWpt { char id[8]; float lat, lon; };
struct MapPt { char kind; char id[7]; float lat, lon; };   // kind: V vor, N ndb, A airport, a small airport
struct Strike { float lat, lon; int16_t age; };
// Radar returns: n x n cells, north up, row 0 the northernmost, centred on (lat, lon)
// and covering +-half_nm. Levels 0 none, 1 green, 2 yellow, 3 red, 4 magenta.
struct WxGrid {
    double lat, lon;
    float half_nm;
    uint8_t n;
    uint8_t lv[WX_GRID_MAX * WX_GRID_MAX];
};

struct Telemetry {
    char src[8];                // MSFS, XPLANE, DEMO, NONE ("" before any data)
    uint32_t rx_ms;             // millis() of the last packet with flight data
    uint32_t any_rx_ms;         // millis() of the last packet of any kind (bridge alive)
    uint32_t seq_fast, seq_mid, seq_slow;   // bumped when that part changes

    Pfd pfd;
    NavInfo nav;
    Eis eis;
    Radios rad;
    Autopilot ap;

    // Firmware the bridge offers for a WiFi update (fwu_ver[0] == 0: none)
    char fwu_ver[20];
    uint16_t fwu_port;
    char fwu_path[40];
    uint32_t fwu_size;

    AptFind apt;

    // Everything from here on is the slow part (once a second, seq_slow): the snapshot
    // copies it only when it changed. Keep the small, often-changing fields above.
    NrstApt nrst[MAX_NRST];
    uint8_t n_nrst;

    FplWpt fpl[MAX_FPL];
    uint8_t n_fpl, fpl_act;
    MapPt pts[MAX_MAP_PTS];
    uint16_t n_pts;

    bool wx_valid, has_grid;
    WxGrid grid;
    uint32_t grid_seq;          // bumped with every new grid
    Strike strikes[MAX_STRIKES];
    uint16_t n_strikes;
    uint32_t wx_rx_ms;
    // Data source / status lines for the page; ok false = show as a caution (yellow).
    char rdr_txt[24], ltg_txt[24];
    bool rdr_ok, ltg_ok;
};

// Compare "mfd7 1.2.3"-style versions: <0, 0, >0. Non-numeric text is ignored.
int version_cmp(const char *a, const char *b);

void telemetry_init();
// Parse one JSON packet from the bridge into the shared state. Thread-safe.
bool telemetry_ingest(const char *json, size_t len, uint32_t now_ms);
// Copy the shared state (thread-safe). The slow part (nearest, map, weather: ~40 KB) is
// copied only when it changed since the last snapshot into the same `out`.
void telemetry_snapshot(Telemetry &out);
// True when flight data arrived within the last 2 s.
bool telemetry_live(const Telemetry &t, uint32_t now_ms);

// ---------------------------------------------------------------- commands

// Where command JSON goes (set by the link layer; the preview logs it).
extern void (*g_cmd_sink)(const char *json);

void cmd_freq(const char *radio, float mhz);       // set standby: com1 com2 nav1 nav2
void cmd_swap(const char *radio);
void cmd_xpdr(const char *code);
void cmd_xpdr_mode(const char *mode);              // STBY GND ON ALT
void cmd_ap(const char *key);                      // AP FD YD HDG NAV APR BC ALT VS FLC
void cmd_ap_set(const char *var, float val);       // hdg alt vs ias
void cmd_map_range(float nm);
void cmd_wx_range(float nm);                       // weather radar range, for the grid size
void cmd_apt_find(const char *q);                  // airport lookup: answered with an "apt" packet
void cmd_hello();
