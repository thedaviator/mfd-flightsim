#pragma once
// Waveshare ESP32-S3-Touch-LCD-7 hardware: RGB panel, GT911 touch, CH422G expander,
// and the LVGL display / input drivers on top of them.
#include <stddef.h>
#include <stdint.h>

bool board_init();                  // I2C, expander, panel, touch, LVGL. False if the panel failed.
void board_backlight(bool on);
bool board_touch_ok();
// Mount the microSD card (FAT32) at this path, e.g. "/sd". False without a card; msg says
// what was found either way.
bool board_sd_begin(const char *mount, char *msg, size_t n);
// Rendering statistics for the status overlay: frames per second over the last second
// and the average render+flush time of one refresh.
float board_fps();
float board_render_ms();
