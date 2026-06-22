// board_display.cpp
//
// Best-effort bring-up for the Waveshare ESP32-S3-Touch-AMOLED-1.64 using
// Arduino_GFX (CO5300 QSPI) + a FocalTech (FT3168) capacitive touch over I2C.
//
// ┌─────────────────────────────────────────────────────────────────────────┐
// │  IMPORTANT — verify before trusting:                                      │
// │  • The QSPI pins and the Arduino_CO5300 constructor arguments below are    │
// │    placeholders from config.h. Copy the exact constructor Waveshare uses   │
// │    in their Arduino LVGL demo for THIS board.                              │
// │  • If colours look inverted/byte-swapped, flip LV_COLOR_16_SWAP in         │
// │    lv_conf.h, or switch draw16bitRGBBitmap <-> draw16bitBeRGBBitmap.       │
// │  • If touch is offset/mirrored, set TOUCH_SWAP_XY / TOUCH_INVERT_* in      │
// │    config.h.                                                               │
// │  The cleanest path: start from Waveshare's working LVGL demo, then drop in │
// │  scale.*, grinder.*, ui.* and call ui_create()/ui_update() from it.        │
// └─────────────────────────────────────────────────────────────────────────┘

#include "board_display.h"
#include "config.h"
#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <Arduino_GFX_Library.h>

// --- panel ------------------------------------------------------------------
static Arduino_DataBus* bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

// GFX Library for Arduino 1.6.x signature (no IPS arg):
//   Arduino_CO5300(bus, rst, rotation, w, h, col_off1, row_off1, col_off2, row_off2)
// Offsets 20,0,180,24 are the verified values for this panel
// (jaapp/smart-grind-by-weight) — they center the 280x456 active area in the
// CO5300's RAM. With 0,0,0,0 the image lands shifted.
static Arduino_GFX* gfx = new Arduino_CO5300(
    bus, LCD_RST, 0 /*rotation*/,
    LCD_WIDTH, LCD_HEIGHT, 20, 0, 180, 24);

// --- LVGL draw buffers (in PSRAM if available) ------------------------------
static lv_disp_draw_buf_t draw_buf;
static lv_color_t* buf1 = nullptr;
static const uint32_t BUF_LINES = 40;

static void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  // LV_COLOR_16_SWAP is 1, so LVGL hands us big-endian RGB565 -> use the Be draw.
  // (draw16bitRGBBitmap here is what turns the UI magenta.)
  gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t*)color_p, w, h);
  lv_disp_flush_ready(drv);
}

// CO5300 shows artifacts on partial-width writes; force every flush to span the
// full panel width, matching the reference driver.
static void rounder_cb(lv_disp_drv_t* /*drv*/, lv_area_t* area) {
  area->x1 = 0;
  area->x2 = LCD_WIDTH - 1;
}

// --- FocalTech FT3168 touch over I2C ---------------------------------------
static bool ft_read(uint16_t* x, uint16_t* y) {
  Wire.beginTransmission(TOUCH_I2C_ADDR);
  Wire.write(0x02);                       // touch-count register
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)TOUCH_I2C_ADDR, 5) != 5) return false;
  uint8_t touches = Wire.read() & 0x0F;
  uint8_t xh = Wire.read(), xl = Wire.read(), yh = Wire.read(), yl = Wire.read();
  if (touches == 0) return false;
  uint16_t rx = ((xh & 0x0F) << 8) | xl;
  uint16_t ry = ((yh & 0x0F) << 8) | yl;
#if TOUCH_SWAP_XY
  uint16_t t = rx; rx = ry; ry = t;
#endif
#if TOUCH_INVERT_X
  rx = LCD_WIDTH - 1 - rx;
#endif
#if TOUCH_INVERT_Y
  ry = LCD_HEIGHT - 1 - ry;
#endif
  *x = rx; *y = ry;
  return true;
}

static void touch_cb(lv_indev_drv_t*, lv_indev_data_t* data) {
  uint16_t x, y;
  if (ft_read(&x, &y)) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = x;
    data->point.y = y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

void board_display_init() {
  // Panel
  gfx->begin();
  gfx->fillScreen(0x0000);   // RGB565 black (avoids the BLACK macro, which this GFX version doesn't export)
#if (LCD_BL >= 0)
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);
#endif

  // Touch I2C
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
#if (TOUCH_RST >= 0)
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);  delay(10);
  digitalWrite(TOUCH_RST, HIGH); delay(50);
#endif

  // LVGL core
  lv_init();
  uint32_t px = LCD_WIDTH * BUF_LINES;
  // Single buffer in internal DMA-capable RAM (PSRAM can't back a DMA buffer).
  buf1 = (lv_color_t*)heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!buf1) buf1 = (lv_color_t*)malloc(px * sizeof(lv_color_t));  // last-resort fallback
  lv_disp_draw_buf_init(&draw_buf, buf1, nullptr, px);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = LCD_WIDTH;
  disp_drv.ver_res = LCD_HEIGHT;
  disp_drv.flush_cb = flush_cb;
  disp_drv.rounder_cb = rounder_cb;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touch_cb;
  lv_indev_drv_register(&indev_drv);
}