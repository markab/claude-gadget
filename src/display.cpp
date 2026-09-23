#include "display.h"
#include "board.h"
#include <Wire.h>
#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include <TouchDrvCSTXXX.hpp>

static Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
static Arduino_CO5300 *gfx = new Arduino_CO5300(bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, LCD_COL_OFFSET, 0, 0, 0);
static TouchDrvCST92xx touch;
static bool touchOk = false;
static volatile bool touchIrq = false;
static bool wasPressed = false;
static bool awake = true;
static uint8_t brightness = 180;
static uint32_t lastTouch = 0;

static lv_disp_draw_buf_t drawBuf;
static lv_disp_drv_t dispDrv;
static lv_indev_drv_t indevDrv;

// CO5300 needs even-aligned windows.
static void rounder_cb(lv_disp_drv_t *, lv_area_t *a) {
    if (a->x1 & 1) a->x1--;
    if (a->y1 & 1) a->y1--;
    if (!(a->x2 & 1)) a->x2++;
    if (!(a->y2 & 1)) a->y2++;
}

static void flush_cb(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *px) {
    uint32_t w = a->x2 - a->x1 + 1;
    uint32_t h = a->y2 - a->y1 + 1;
    gfx->draw16bitRGBBitmap(a->x1, a->y1, (uint16_t *)&px->full, w, h);
    lv_disp_flush_ready(d);
}

static void touch_read_cb(lv_indev_drv_t *, lv_indev_data_t *data) {
    data->state = LV_INDEV_STATE_REL;
    if (!touchOk || !awake) return;  // while asleep, main loop consumes the IRQ as a wake tap
    // Only hit the I2C bus after an interrupt or while a finger is down.
    if (!touchIrq && !wasPressed) return;
    touchIrq = false;

    int16_t x[5], y[5];
    uint8_t n = touch.getPoint(x, y, touch.getSupportTouchPoint());
    wasPressed = n > 0;
    if (wasPressed) {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = x[0];
        data->point.y = y[0];
        lastTouch = millis();
    }
}

bool display_init() {
    if (!gfx->begin()) {
        Serial.println("[disp] gfx begin failed");
        return false;
    }
    gfx->fillScreen(RGB565_BLACK);
    gfx->setBrightness(brightness);

    touch.setPins(TP_RST, TP_INT);
    touchOk = touch.begin(Wire, TP_ADDR, I2C_SDA, I2C_SCL);
    if (touchOk) {
        touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
        touch.setMirrorXY(true, true);
        attachInterrupt(TP_INT, [] { touchIrq = true; }, FALLING);
    } else {
        Serial.println("[disp] touch not found");
    }

    lv_init();

    // Draw buffers go in PSRAM: the QSPI bus copies through its own small DMA
    // buffer, and internal RAM is needed by Wi-Fi and TLS.
    const uint32_t bufPx = LCD_WIDTH * 80;
    auto *b1 = (lv_color_t *)heap_caps_malloc(bufPx * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    auto *b2 = (lv_color_t *)heap_caps_malloc(bufPx * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    lv_disp_draw_buf_init(&drawBuf, b1, b2, bufPx);

    lv_disp_drv_init(&dispDrv);
    dispDrv.hor_res = LCD_WIDTH;
    dispDrv.ver_res = LCD_HEIGHT;
    dispDrv.flush_cb = flush_cb;
    dispDrv.rounder_cb = rounder_cb;
    dispDrv.draw_buf = &drawBuf;
    lv_disp_drv_register(&dispDrv);

    lv_indev_drv_init(&indevDrv);
    indevDrv.type = LV_INDEV_TYPE_POINTER;
    indevDrv.read_cb = touch_read_cb;
    lv_indev_drv_register(&indevDrv);

    lastTouch = millis();
    return true;
}

void display_set_brightness(uint8_t level) {
    brightness = level;
    if (awake) gfx->setBrightness(level);
}

void display_sleep() {
    if (!awake) return;
    awake = false;
    gfx->setBrightness(0);
    gfx->displayOff();
    wasPressed = false;
    touchIrq = false;
}

bool display_take_wake_tap() {
    if (awake || !touchIrq) return false;
    touchIrq = false;
    return true;
}

void display_wake() {
    if (awake) return;
    gfx->displayOn();
    gfx->setBrightness(brightness);
    awake = true;
    lastTouch = millis();
    lv_obj_invalidate(lv_scr_act());
}

bool display_is_awake() { return awake; }
uint32_t display_last_touch_ms() { return lastTouch; }
