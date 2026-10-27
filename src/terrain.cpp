#include "terrain.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ARDUINO
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
static SemaphoreHandle_t s_mutex;
static void lock() { xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void unlock() { xSemaphoreGive(s_mutex); }
static void *big_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
#else
static void lock() {}
static void unlock() {}
static void *big_alloc(size_t n) { return malloc(n); }
#endif

#define TILE_PX     256
#define TILE_BYTES  (TILE_PX * TILE_PX * 2)
#define N_BUFS      24              // tiles held in PSRAM (128 KB each)
#define N_ENTRIES   96              // tiles known (loaded, water, queued)
#define MAX_LEVELS  6
#define MAX_AREAS   12

enum { E_FREE, E_QUEUED, E_LOADING, E_READY, E_WATER, E_ERROR };

struct Area { int32_t x0, y0, x1, y1; };
struct Level { int z; int n; Area a[MAX_AREAS]; };
struct Entry {
    volatile uint8_t state;
    int8_t buf;                     // index into s_bufs, -1 none
    uint8_t z;
    int32_t x, y;
    uint32_t used;                  // render number it was last used in
};

static char s_root[48];
static char s_status[64] = "NO SD CARD";
static Level s_levels[MAX_LEVELS];  // ascending z
static int s_n_levels;
static bool s_ok;
static int16_t *s_bufs[N_BUFS];
static bool s_buf_used[N_BUFS];
static Entry s_ent[N_ENTRIES];
static uint32_t s_render, s_gen;
static bool s_pending, s_full;     // this render: tiles still to come; no room for more
static int s_last[32];              // per level: entry of the previous sample there

// ---------------------------------------------------------------- setup

bool terrain_init(const char *root) {
#ifdef ARDUINO
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
#endif
    s_ok = false;
    s_n_levels = 0;
    snprintf(s_root, sizeof(s_root), "%s", root);
    char path[80];
    snprintf(path, sizeof(path), "%s/TERRAIN/INFO.TXT", root);
    FILE *f = fopen(path, "r");
    if (!f) {
        terrain_set_status("SD CARD HAS NO TERRAIN DATA (TERRAIN/INFO.TXT)");
        return false;
    }
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int z;
        Area a;
        if (sscanf(line, "level %d %ld %ld %ld %ld", &z, (long *)&a.x0, (long *)&a.y0, (long *)&a.x1, (long *)&a.y1) != 5)
            continue;
        int i = 0;
        while (i < s_n_levels && s_levels[i].z != z) i++;
        if (i == s_n_levels) {
            if (s_n_levels == MAX_LEVELS) continue;
            // keep ascending order
            int j = s_n_levels++;
            while (j > 0 && s_levels[j - 1].z > z) {
                s_levels[j] = s_levels[j - 1];
                j--;
            }
            s_levels[j].z = z;
            s_levels[j].n = 0;
            i = j;
        }
        if (s_levels[i].n < MAX_AREAS) s_levels[i].a[s_levels[i].n++] = a;
    }
    fclose(f);
    if (!s_n_levels) {
        terrain_set_status("TERRAIN/INFO.TXT LISTS NO LEVELS");
        return false;
    }
    for (int i = 0; i < N_BUFS; i++)
        if (!s_bufs[i]) s_bufs[i] = (int16_t *)big_alloc(TILE_BYTES);
    memset(s_ent, 0, sizeof(s_ent));
    memset(s_buf_used, 0, sizeof(s_buf_used));
    s_ok = s_bufs[N_BUFS - 1] != nullptr;
    if (!s_ok) {
        terrain_set_status("TERRAIN: NOT ENOUGH MEMORY");
        return false;
    }
    char t[64] = "TERRAIN: LEVELS";
    for (int i = 0; i < s_n_levels; i++) snprintf(t + strlen(t), sizeof(t) - strlen(t), " %d", s_levels[i].z);
    terrain_set_status(t);
    return true;
}

bool terrain_available() { return s_ok; }
const char *terrain_status() { return s_status; }
void terrain_set_status(const char *s) { snprintf(s_status, sizeof(s_status), "%s", s); }
uint32_t terrain_generation() { return s_gen; }

// ---------------------------------------------------------------- loader

bool terrain_service() {
    if (!s_ok) return false;
    lock();
    int e = -1;
    for (int i = 0; i < N_ENTRIES; i++)
        if (s_ent[i].state == E_QUEUED) {
            e = i;
            break;
        }
    if (e < 0) {
        unlock();
        return false;
    }
    Entry &en = s_ent[e];
    en.state = E_LOADING;
    int z = en.z, x = en.x, y = en.y;
    int16_t *dst = s_bufs[en.buf];
    unlock();

    char path[96];
    snprintf(path, sizeof(path), "%s/TERRAIN/%d/%ld/%ld.BIN", s_root, z, (long)x, (long)y);
    FILE *f = fopen(path, "rb");
    int st = E_WATER;
    if (f) {
        // In chunks through internal RAM: the SD driver is much slower reading straight
        // into PSRAM (it falls back to one sector at a time).
        static uint8_t *chunk;
        const size_t CH = 16384;
#ifdef ARDUINO
        if (!chunk) chunk = (uint8_t *)heap_caps_malloc(CH, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
#else
        if (!chunk) chunk = (uint8_t *)malloc(CH);
#endif
        size_t got = 0;
        while (chunk && got < TILE_BYTES) {
            size_t n = fread(chunk, 1, CH, f);
            if (!n) break;
            memcpy((uint8_t *)dst + got, chunk, n);
            got += n;
        }
        fclose(f);
        st = got == TILE_BYTES ? E_READY : E_ERROR;
    }
    lock();
    if (st != E_READY) {
        s_buf_used[en.buf] = false;
        en.buf = -1;
    }
    en.state = st;
    s_gen++;
    unlock();
    return true;
}

// ---------------------------------------------------------------- sampling (UI task)

void terrain_begin() {
    s_render++;
    s_pending = s_full = false;
    for (int i = 0; i < 32; i++) s_last[i] = -1;
}

bool terrain_end() { return s_pending; }

int terrain_pick_level(float nm_per_px, double lat) {
    if (!s_ok) return -1;
    float c = (float)cos(lat * 0.017453292519943295);
    // The coarsest level whose pixels are at most 3 screen pixels wide: finer would need
    // more tiles in view than the cache holds.
    for (int i = 0; i < s_n_levels; i++) {
        float px_nm = 84.375f / (float)(1 << s_levels[i].z) * c;
        if (px_nm <= nm_per_px * 3.0f) return s_levels[i].z;
    }
    return s_levels[s_n_levels - 1].z;
}

static const Level *level(int z) {
    for (int i = 0; i < s_n_levels; i++)
        if (s_levels[i].z == z) return &s_levels[i];
    return nullptr;
}

static bool covered(const Level *L, int32_t x, int32_t y) {
    for (int i = 0; i < L->n; i++)
        if (x >= L->a[i].x0 && x <= L->a[i].x1 && y >= L->a[i].y0 && y <= L->a[i].y1) return true;
    return false;
}

static int coarser(int z) {
    for (int i = s_n_levels - 1; i >= 0; i--)
        if (s_levels[i].z < z) return s_levels[i].z;
    return -1;
}

// A buffer for a new tile: a free one, or the least recently used loaded tile not used
// in this render.
static int grab_buffer() {
    for (int b = 0; b < N_BUFS; b++)
        if (!s_buf_used[b]) return b;
    int best = -1;
    for (int i = 0; i < N_ENTRIES; i++) {
        Entry &e = s_ent[i];
        if (e.state == E_READY && e.used != s_render && (best < 0 || e.used < s_ent[best].used)) best = i;
    }
    if (best < 0) return -1;
    int b = s_ent[best].buf;
    s_ent[best].state = E_FREE;
    s_ent[best].buf = -1;
    return b;
}

static int find_or_request(int z, int32_t x, int32_t y) {
    for (int i = 0; i < N_ENTRIES; i++) {
        Entry &e = s_ent[i];
        if (e.state != E_FREE && e.z == z && e.x == x && e.y == y) return i;
    }
    if (s_full) return -1;
    lock();
    int slot = -1;
    for (int i = 0; i < N_ENTRIES && slot < 0; i++)
        if (s_ent[i].state == E_FREE) slot = i;
    if (slot < 0) {                 // forget the oldest water / failed tile
        for (int i = 0; i < N_ENTRIES; i++) {
            Entry &e = s_ent[i];
            if ((e.state == E_WATER || e.state == E_ERROR) && e.used != s_render &&
                (slot < 0 || e.used < s_ent[slot].used))
                slot = i;
        }
    }
    int b = slot >= 0 ? grab_buffer() : -1;
    if (b < 0) {
        unlock();
        s_full = true;
        return -1;
    }
    s_buf_used[b] = true;
    Entry &e = s_ent[slot];
    e.z = (uint8_t)z;
    e.x = x;
    e.y = y;
    e.buf = (int8_t)b;
    e.used = s_render;
    e.state = E_QUEUED;
    unlock();
    return slot;
}

int16_t terrain_sample(int z, int32_t px, int32_t py) {
    for (;;) {
        const Level *L = level(z);
        if (!L) return TERRAIN_NONE;
        int32_t n = 1 << z;
        int32_t tx = px >> 8, ty = py >> 8;
        if (ty < 0 || ty >= n) return TERRAIN_NONE;
        tx = ((tx % n) + n) % n;
        int e = -1;
        if (covered(L, tx, ty)) {
            int l = s_last[z];
            if (l >= 0 && s_ent[l].z == z && s_ent[l].x == tx && s_ent[l].y == ty && s_ent[l].state != E_FREE)
                e = l;
            else
                e = find_or_request(z, tx, ty);
            if (e >= 0) {
                Entry &en = s_ent[e];
                en.used = s_render;
                s_last[z] = e;
                uint8_t st = en.state;
                if (st == E_READY) return s_bufs[en.buf][(py & 255) * TILE_PX + (px & 255)];
                if (st == E_WATER) return TERRAIN_WATER;
                if (st != E_ERROR) s_pending = true;        // queued / loading
            } else {
                s_pending = true;                           // cache full this render
            }
        }
        // Not covered at this level, or not loaded yet: use the next coarser level.
        int z2 = coarser(z);
        if (z2 < 0) return TERRAIN_NONE;
        px >>= (z - z2);
        py >>= (z - z2);
        z = z2;
    }
}
