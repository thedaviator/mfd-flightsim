#include "board.h"

#include <Arduino.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_vfs_fat.h>
#include <lvgl.h>
#include <sdmmc_cmd.h>

#include "config.h"

static esp_lcd_panel_handle_t s_panel = nullptr;
static uint8_t s_exio = 0;
static bool s_touch_ok = false;
static uint8_t s_gt_addr = 0x5D;

// ---------------------------------------------------------------- CH422G I/O expander
// The CH422G has no registers: the I2C address is the command. 0x24 sets the system
// parameters (bit 0 = push-pull outputs on IO0..7), 0x38 writes IO0..7.

static void exio_write(uint8_t v) {
    s_exio = v;
    Wire.beginTransmission(0x38);
    Wire.write(v);
    Wire.endTransmission();
}

static bool exio_begin() {
    Wire.beginTransmission(0x24);
    Wire.write(0x01);
    return Wire.endTransmission() == 0;
}

void board_backlight(bool on) {
    exio_write(on ? (s_exio | EXIO_BACKLIGHT) : (s_exio & ~EXIO_BACKLIGHT));
}

// ---------------------------------------------------------------- microSD (terrain)
// SPI on GPIO 11/12/13. The card's chip select is CH422G EXIO4, not a GPIO: it is held
// low (the card is the only device on this bus) and the SPI driver runs without one.

bool board_sd_begin(const char *mount, char *msg, size_t n) {
    exio_write(s_exio & ~EXIO_SD_CS);
    delay(10);
    spi_bus_config_t bus = {};
    bus.mosi_io_num = SD_PIN_MOSI;
    bus.miso_io_num = SD_PIN_MISO;
    bus.sclk_io_num = SD_PIN_CLK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 16384;
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        snprintf(msg, n, "SD: SPI bus failed (%s)", esp_err_to_name(err));
        return false;
    }
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = SDSPI_SLOT_NO_CS;            // the default is GPIO 13, our MISO
    slot.host_id = SPI2_HOST;
    esp_vfs_fat_sdmmc_mount_config_t cfg = {};
    cfg.format_if_mount_failed = false;
    cfg.max_files = 4;
    cfg.allocation_unit_size = 16 * 1024;
    sdmmc_card_t *card = nullptr;
    // 20 MHz first; some cards or traces only manage 4 MHz (the speed Waveshare's demo runs).
    static const int freqs[] = {20000, 4000};
    for (int f : freqs) {
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        host.slot = SPI2_HOST;
        host.max_freq_khz = f;
        err = esp_vfs_fat_sdspi_mount(mount, &host, &slot, &cfg, &card);
        Serial.printf("SD card at %d kHz: %s\n", f, esp_err_to_name(err));
        if (err == ESP_OK) break;
        if (err == ESP_FAIL) break;             // the card answered; it has no FAT file system
    }
    if (err != ESP_OK) {
        if (err == ESP_FAIL) snprintf(msg, n, "SD card not FAT32 (format it FAT32)");
        else snprintf(msg, n, "No SD card found (%s)", esp_err_to_name(err));
        spi_bus_free(SPI2_HOST);
        return false;
    }
    snprintf(msg, n, "SD card %.1f GB", (double)card->csd.capacity * card->csd.sector_size / 1e9);
    return true;
}

// ---------------------------------------------------------------- GT911 touch

static bool gt_read(uint16_t reg, uint8_t *buf, uint8_t n) {
    Wire.beginTransmission(s_gt_addr);
    Wire.write(reg >> 8);
    Wire.write(reg & 0xFF);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(s_gt_addr, n) != n) return false;
    for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
    return true;
}

static void gt_write(uint16_t reg, uint8_t v) {
    Wire.beginTransmission(s_gt_addr);
    Wire.write(reg >> 8);
    Wire.write(reg & 0xFF);
    Wire.write(v);
    Wire.endTransmission();
}

static bool touch_begin() {
    // Reset with INT held low so the GT911 takes address 0x5D.
    pinMode(TOUCH_PIN_INT, OUTPUT);
    digitalWrite(TOUCH_PIN_INT, LOW);
    exio_write(s_exio & ~EXIO_TP_RST);
    delay(20);
    exio_write(s_exio | EXIO_TP_RST);
    delay(10);
    pinMode(TOUCH_PIN_INT, INPUT);
    delay(60);
    for (uint8_t a : {(uint8_t)0x5D, (uint8_t)0x14}) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            s_gt_addr = a;
            uint8_t id[4] = {0};
            gt_read(0x8140, id, 4);
            Serial.printf("GT911 touch at 0x%02X, product id %.4s\n", a, (char *)id);
            return true;
        }
    }
    Serial.println("GT911 touch not found");
    return false;
}

static void touch_read_cb(lv_indev_drv_t *, lv_indev_data_t *data) {
    static int16_t lx = 0, ly = 0;
    static bool down = false;
    uint8_t status = 0;
    if (s_touch_ok && gt_read(0x814E, &status, 1) && (status & 0x80)) {
        uint8_t n = status & 0x0F;
        if (n > 0) {
            uint8_t p[7];
            if (gt_read(0x8150, p, 7)) {
                lx = constrain((int16_t)(p[0] | (p[1] << 8)), 0, SCREEN_W - 1);
                ly = constrain((int16_t)(p[2] | (p[3] << 8)), 0, SCREEN_H - 1);
                down = true;
            }
        } else {
            down = false;
        }
        gt_write(0x814E, 0);
    }
    data->point.x = lx;
    data->point.y = ly;
    data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

bool board_touch_ok() { return s_touch_ok; }

// ---------------------------------------------------------------- RGB panel

static bool panel_begin() {
    esp_lcd_rgb_panel_config_t cfg = {};
    cfg.clk_src = LCD_CLK_SRC_DEFAULT;
    cfg.timings.pclk_hz = LCD_PCLK_HZ;
    cfg.timings.h_res = SCREEN_W;
    cfg.timings.v_res = SCREEN_H;
    cfg.timings.hsync_pulse_width = LCD_HSYNC_PULSE;
    cfg.timings.hsync_back_porch = LCD_HSYNC_BACK;
    cfg.timings.hsync_front_porch = LCD_HSYNC_FRONT;
    cfg.timings.vsync_pulse_width = LCD_VSYNC_PULSE;
    cfg.timings.vsync_back_porch = LCD_VSYNC_BACK;
    cfg.timings.vsync_front_porch = LCD_VSYNC_FRONT;
    cfg.timings.flags.pclk_active_neg = 1;
    cfg.data_width = 16;
    cfg.bits_per_pixel = 16;
    cfg.num_fbs = 1;
    // Bounce buffers: the LCD DMA reads from two small buffers in internal RAM, which an
    // interrupt refills from the PSRAM frame buffer. This keeps the panel fed when WiFi
    // and drawing compete for PSRAM bandwidth.
    cfg.bounce_buffer_size_px = SCREEN_W * LCD_BOUNCE_LINES;
    cfg.dma_burst_size = 64;
    cfg.hsync_gpio_num = LCD_PIN_HSYNC;
    cfg.vsync_gpio_num = LCD_PIN_VSYNC;
    cfg.de_gpio_num = LCD_PIN_DE;
    cfg.pclk_gpio_num = LCD_PIN_PCLK;
    const int pins[16] = LCD_DATA_PINS;
    for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = pins[i];
    cfg.disp_gpio_num = -1;
    cfg.flags.fb_in_psram = 1;
    if (esp_lcd_new_rgb_panel(&cfg, &s_panel) != ESP_OK) return false;
    esp_lcd_panel_reset(s_panel);
    return esp_lcd_panel_init(s_panel) == ESP_OK;
}

// ---------------------------------------------------------------- LVGL glue

static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;
static lv_indev_drv_t s_indev_drv;
static uint32_t s_frames = 0, s_fps_t0 = 0, s_render_sum = 0, s_render_n = 0;
static float s_fps = 0, s_render_ms = 0;

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *px) {
    // Copies into the PSRAM frame buffer and writes the cache back for the LCD DMA.
    esp_lcd_panel_draw_bitmap(s_panel, a->x1, a->y1, a->x2 + 1, a->y2 + 1, px);
    lv_disp_flush_ready(drv);
}

static void monitor_cb(lv_disp_drv_t *, uint32_t time_ms, uint32_t) {
    s_frames++;
    s_render_sum += time_ms;
    s_render_n++;
    uint32_t now = millis();
    if (now - s_fps_t0 >= 1000) {
        s_fps = s_frames * 1000.0f / (now - s_fps_t0);
        s_render_ms = s_render_n ? (float)s_render_sum / s_render_n : 0;
        s_frames = s_render_sum = s_render_n = 0;
        s_fps_t0 = now;
    }
}

float board_fps() { return s_fps; }
float board_render_ms() { return s_render_ms; }

bool board_init() {
    Wire.begin(I2C_PIN_SDA, I2C_PIN_SCL, 400000);
    if (!exio_begin()) Serial.println("CH422G expander not responding");
    // Backlight off until the first frame is drawn; panel out of reset; touch in reset.
    exio_write(EXIO_LCD_RST | EXIO_USB_SEL | EXIO_SD_CS);
    delay(20);

    if (!panel_begin()) {
        Serial.println("RGB panel init failed");
        return false;
    }
    s_touch_ok = touch_begin();

    lv_init();
    size_t buf_px = SCREEN_W * LVGL_BUF_LINES;
    lv_color_t *buf = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        Serial.println("LVGL buffer: internal RAM short, using PSRAM");
        buf = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    }
    lv_disp_draw_buf_init(&s_draw_buf, buf, nullptr, buf_px);
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = SCREEN_W;
    s_disp_drv.ver_res = SCREEN_H;
    s_disp_drv.flush_cb = flush_cb;
    s_disp_drv.monitor_cb = monitor_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
    lv_disp_drv_register(&s_disp_drv);

    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type = LV_INDEV_TYPE_POINTER;
    s_indev_drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&s_indev_drv);
    return true;
}
