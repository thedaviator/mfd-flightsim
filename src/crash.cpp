// Crash report: on a panic, the reason and backtrace go into RTC memory (kept across the
// restart). After the restart the display sends them to the bridge in its hello, and the
// bridge logs them; decode the addresses with xtensa-esp32s3-elf-addr2line and the
// firmware.elf of that version (tools/make_release.py keeps one in release/).
#include "crash.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <string.h>

#define CRASH_MAGIC 0x4D464437u     // "MFD7"
#define CRASH_BT 16

struct CrashInfo {
    uint32_t magic;
    char reason[40];
    uint32_t pc;
    uint32_t n;
    uint32_t bt[CRASH_BT];
    uint32_t note_magic;            // CRASH_MAGIC once crash_note() ran: the fields below are valid
    uint32_t heap_min_internal;     // lowest free internal heap seen before the crash
    uint32_t heap_internal;         // free internal heap at the last note
    char where[12];                 // page shown at the time
};
RTC_NOINIT_ATTR static CrashInfo s_crash;
static char s_report[300] = "";

static void panic_cb(arduino_panic_info_t *info, void *) {
    s_crash.magic = CRASH_MAGIC;
    strncpy(s_crash.reason, info->reason ? info->reason : "?", sizeof(s_crash.reason) - 1);
    s_crash.reason[sizeof(s_crash.reason) - 1] = 0;
    s_crash.pc = (uint32_t)info->pc;
    s_crash.n = info->backtrace_len < CRASH_BT ? info->backtrace_len : CRASH_BT;
    memcpy(s_crash.bt, info->backtrace, s_crash.n * sizeof(uint32_t));
}

static const char *reset_name(esp_reset_reason_t r) {
    switch (r) {
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout (power dip)";
    default: return nullptr;
    }
}

void crash_begin() {
    esp_reset_reason_t r = esp_reset_reason();
    const char *name = reset_name(r);
    if (name) {
        int k = snprintf(s_report, sizeof(s_report), "%s", name);
        if (s_crash.note_magic == CRASH_MAGIC) {
            s_crash.where[sizeof(s_crash.where) - 1] = 0;
            k += snprintf(s_report + k, sizeof(s_report) - k, " on %s, internal heap %lu KB free (lowest %lu KB)",
                          s_crash.where, (unsigned long)(s_crash.heap_internal / 1024),
                          (unsigned long)(s_crash.heap_min_internal / 1024));
        }
        if (s_crash.magic == CRASH_MAGIC) {
            k += snprintf(s_report + k, sizeof(s_report) - k, ": %s, pc 0x%08lx, bt", s_crash.reason, (unsigned long)s_crash.pc);
            for (uint32_t i = 0; i < s_crash.n && k < (int)sizeof(s_report) - 12; i++)
                k += snprintf(s_report + k, sizeof(s_report) - k, " 0x%08lx", (unsigned long)s_crash.bt[i]);
        }
        Serial.printf("Last reset: %s\n", s_report);
    }
    memset(&s_crash, 0, sizeof(s_crash));
    set_arduino_panic_handler(panic_cb, nullptr);
}

void crash_note(const char *where) {
    strncpy(s_crash.where, where, sizeof(s_crash.where) - 1);
    s_crash.heap_min_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    s_crash.heap_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_crash.note_magic = CRASH_MAGIC;
}

const char *crash_report() { return s_report; }
