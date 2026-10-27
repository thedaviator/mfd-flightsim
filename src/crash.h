#pragma once
// Crash report kept across a restart (firmware only).

// At boot: read the previous crash (if the last reset was one) and arm the panic handler.
void crash_begin();
// Keep note of what the UI is doing (page key) and of the heap, for the next report.
void crash_note(const char *where);
// "panic: LoadProhibited on NRST, pc 0x..., bt 0x... ..." or "" if the last reset was normal.
const char *crash_report();
