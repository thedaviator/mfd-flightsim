#pragma once
// Board pin map, display timing, network settings and aircraft limits.

#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""            // empty: WiFi off, USB serial link only
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
#ifndef BRIDGE_HOST
#define BRIDGE_HOST ""          // empty: find the bridge by its UDP beacon
#endif
#define BRIDGE_PORT        8080
#define BEACON_PORT        8081
#define SERIAL_BAUD        921600
#define FW_VERSION         "mfd7 1.3.2"
#define SPLASH_MS          3000          // boot splash

// ---------------------------------------------------------------- display

#define SCREEN_W 800
#define SCREEN_H 480

// ST7262 RGB565 panel, 16-bit parallel. Data lines in esp_lcd order D0..D15 =
// B3..B7, G2..G7, R3..R7.
#define LCD_PIN_DE     5
#define LCD_PIN_VSYNC  3
#define LCD_PIN_HSYNC  46
#define LCD_PIN_PCLK   7
#define LCD_DATA_PINS  {14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40}
// Pixel clock and porches. The frame buffer is in PSRAM; the LCD DMA reads from bounce
// buffers in internal RAM that an interrupt refills from PSRAM (board.cpp). The wide
// horizontal porches give that interrupt slack on every line. ~32 Hz panel refresh.
#define LCD_PCLK_HZ    (14 * 1000 * 1000)
#define LCD_HSYNC_PULSE 4
#define LCD_HSYNC_BACK  40
#define LCD_HSYNC_FRONT 40
#define LCD_VSYNC_PULSE 4
#define LCD_VSYNC_BACK  8
#define LCD_VSYNC_FRONT 8
#define LCD_BOUNCE_LINES 20      // per bounce buffer (two are allocated; 480 must divide evenly)

// I2C bus shared by the GT911 touch controller and the CH422G I/O expander.
#define I2C_PIN_SDA    8
#define I2C_PIN_SCL    9
#define TOUCH_PIN_INT  4

// microSD over SPI (chip select: EXIO4 below)
#define SD_PIN_MOSI    11
#define SD_PIN_CLK     12
#define SD_PIN_MISO    13

// CH422G output bits (EXIO0..7)
#define EXIO_TP_RST    (1 << 1)
#define EXIO_BACKLIGHT (1 << 2)
#define EXIO_LCD_RST   (1 << 3)
#define EXIO_SD_CS     (1 << 4)
#define EXIO_USB_SEL   (1 << 5)

// LVGL draw buffer: this many lines, in internal RAM.
#define LVGL_BUF_LINES 40

// ---------------------------------------------------------------- aircraft (Cessna 172S)

// Airspeed tape colour bands, knots
#define V_S0   40   // white arc bottom (stall, flaps down)
#define V_FE   85   // white arc top (max flap extended)
#define V_S1   48   // green arc bottom
#define V_NO  129   // green arc top / yellow arc bottom
#define V_NE  163   // red line

// Engine gauge ranges: min, max, green from, green to, yellow-high to (0 = none), red line
#define G_RPM      0, 2800, 2100, 2700,    0, 2700
#define G_MAP     10,   35,   15,   25,    0,    0
#define G_FUELP    0,   10,  0.5,    8,    0,    8
#define G_OILT    50,  250,  100,  245,    0,  245
#define G_OILP     0,  120,   50,   90,  115,  115
#define G_CHT    100,  500,  200,  435,  460,  500
#define G_EGT   1000, 1650,    0,    0,    0,    0
#define G_VOLTS   22,   32, 24.5, 29.5,    0,   32
#define G_AMPS   -40,   60,    0,   60,    0,    0
#define FUEL_TANK_GAL 26.0f
