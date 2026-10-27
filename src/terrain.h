#pragma once
// Terrain elevation from the SD card (prepared by bridge/terrain_sd.py): Web-Mercator
// tiles of 256 x 256 int16 metres at a few zoom levels, a small cache of them in PSRAM,
// and a loader that reads tiles outside the UI task. Portable: the PC preview reads the
// same files from a folder.
#include <stddef.h>
#include <stdint.h>

// root: folder that holds TERRAIN/INFO.TXT ("/sd" on the display). False without data.
bool terrain_init(const char *root);
bool terrain_available();
// One line about the card and the data, for the MAP page and the status window
// ("NO SD CARD ...", "SD CARD HAS NO TERRAIN DATA", "TERRAIN: LEVELS 5 7 8 10").
const char *terrain_status();
void terrain_set_status(const char *s);
// Load requested tiles. Call from a background task (or the UI loop in the preview);
// returns true if it loaded something.
bool terrain_service();

// Sampling, for one map render: terrain_begin(), terrain_sample() per point, terrain_end().
// Points are Web-Mercator pixel coordinates at level z (tile * 256 + pixel in tile).
// Returns metres, TERRAIN_WATER, or TERRAIN_NONE (not covered, or not loaded yet).
#define TERRAIN_WATER (-32768)
#define TERRAIN_NONE  (-32767)
void terrain_begin();
// The level to draw at this scale (-1: no data). Tiles beyond its coverage come from
// coarser levels automatically.
int terrain_pick_level(float nm_per_px, double lat);
int16_t terrain_sample(int z, int32_t px, int32_t py);
// After a render: true if tiles it wanted are still loading (render again later).
bool terrain_end();
uint32_t terrain_generation();          // bumped whenever a tile arrives
