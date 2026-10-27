#pragma once
// LVGL's objects, styles and label texts in PSRAM on the display: hundreds of small
// allocations (a list page has ~300 objects) would otherwise use up the internal RAM that
// WiFi and the drivers need. The PC preview uses plain malloc.
#include <stdlib.h>

#ifdef ARDUINO
#include <esp_heap_caps.h>
static inline void *lv_psram_alloc(size_t n) {
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}
static inline void *lv_psram_realloc(void *p, size_t n) {
    void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return q ? q : realloc(p, n);
}
static inline void lv_psram_free(void *p) { heap_caps_free(p); }
#else
#define lv_psram_alloc malloc
#define lv_psram_realloc realloc
#define lv_psram_free free
#endif
