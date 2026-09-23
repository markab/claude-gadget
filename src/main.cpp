// Claude Gadget — shows your Claude Pro/Max plan usage on a
// Waveshare ESP32-S3-Touch-AMOLED-1.75C.
//
// Controls
//   PWR tap        screen on/off
//   PWR hold 2s    power off (press PWR again to power on; PMU forces off at 6s)
//   BOOT tap       refresh usage now
//   BOOT hold 3s   open the Wi-Fi / token setup hotspot
//   Screen tap     wake (when off); swipe for battery / device / settings pages

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <lvgl.h>
#include "board.h"
#include "settings.h"
#include "power.h"
#include "display.h"
#include "net.h"
#include "claude.h"
#include "ui.h"

static const uint16_t LOW_BATT_OFF_MV = 3300;      // auto power-off threshold
static const uint32_t BOOT_HOLD_MS = 3000;

static bool dimmed = false;
static uint32_t lowBattSince = 0;

static void screen_off() {
    display_sleep();
    claude_set_low_power(true);
    setCpuFrequencyMhz(80);
}

static void screen_on() {
    setCpuFrequencyMhz(240);
    display_wake();
    display_set_brightness(settings.brightness);
    dimmed = false;
    claude_set_low_power(false);
}

static void shutdown_now() {
    if (!display_is_awake()) screen_on();
    ui_show_power_off();
    delay(700);
    power_off();
}

static void handle_power_events() {
    uint8_t ev = power_poll_events();
    if (ev & PWR_EVT_LONG_PRESS) {
        shutdown_now();
    } else if (ev & PWR_EVT_SHORT_PRESS) {
        if (display_is_awake()) screen_off();
        else screen_on();
    }
    if (ev & (PWR_EVT_VBUS_IN | PWR_EVT_VBUS_OUT)) {
        if (!display_is_awake()) screen_on();
        ui_flash_message(ev & PWR_EVT_VBUS_IN ? LV_SYMBOL_CHARGE " Charging" : LV_SYMBOL_BATTERY_FULL " On battery");
    }
    if (ev & PWR_EVT_CHG_DONE && display_is_awake()) ui_flash_message(LV_SYMBOL_OK " Fully charged");
}

static void handle_boot_button() {
    static uint32_t downAt = 0;
    static bool holdFired = false;
    bool down = digitalRead(BTN_BOOT) == LOW;
    uint32_t now = millis();

    if (down && !downAt) {
        downAt = now;
        holdFired = false;
    } else if (down && !holdFired && now - downAt >= BOOT_HOLD_MS) {
        holdFired = true;
        if (!display_is_awake()) screen_on();
        net_start_setup_portal();
    } else if (!down && downAt) {
        if (!holdFired && now - downAt > 40) {
            if (!display_is_awake()) screen_on();
            else {
                claude_refresh_now();
                ui_flash_message(LV_SYMBOL_REFRESH " Refreshing");
            }
        }
        downAt = 0;
    }
}

static void handle_idle(const BatteryInfo &b) {
    if (!display_is_awake() || settings.dimSecs == 0) return;
    // Keep the screen on while the setup screens are up.
    if (net_status().mode == NET_AP_PORTAL) return;

    uint32_t idle = millis() - display_last_touch_ms();
    uint32_t limit = settings.dimSecs * 1000UL;
    if (b.vbus) {
        // On USB: dim instead of switching off (a desk gadget should stay readable).
        if (idle >= limit && !dimmed) {
            display_set_brightness(settings.dimBrightness);
            dimmed = true;
        } else if (idle < limit && dimmed) {
            display_set_brightness(settings.brightness);
            dimmed = false;
        }
    } else if (idle >= limit) {
        screen_off();
    }
}

static void handle_low_battery(const BatteryInfo &b) {
    if (!b.present || b.vbus || b.battMv == 0 || b.battMv >= LOW_BATT_OFF_MV) {
        lowBattSince = 0;
        return;
    }
    if (!lowBattSince) {
        lowBattSince = millis();
        ui_flash_message(LV_SYMBOL_BATTERY_EMPTY " Battery low");
    } else if (millis() - lowBattSince > 30000) {
        shutdown_now();
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[boot] Claude Gadget " FW_VERSION);

    pinMode(BTN_BOOT, INPUT_PULLUP);
    Wire.begin(I2C_SDA, I2C_SCL, 400000);

    settings_load();
    power_init();
    display_init();
    display_set_brightness(settings.brightness);
    ui_init();
    ui_update(UsageSnapshot{}, power_battery(), NetStatus{NET_CONNECTING});
    lv_timer_handler();

    claude_start();
    Serial.printf("[boot] free internal %u KB, PSRAM %u/%u KB\n", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
                  ESP.getFreePsram() / 1024, ESP.getPsramSize() / 1024);
    net_begin();   // blocks up to ~15 s while it tries saved Wi-Fi
}

void loop() {
    static uint32_t lastUi = 0;
    static uint32_t lastSeq = 0;

    static uint32_t lastBatt = 0;

    net_loop();
    handle_power_events();
    handle_boot_button();

    uint32_t now = millis();
    if (now - lastBatt >= 5000) {
        lastBatt = now;
        handle_low_battery(power_battery());  // also runs with the screen off
    }

    if (!display_is_awake()) {
        if (display_take_wake_tap()) screen_on();
        delay(20);
        return;
    }

    UsageSnapshot u = claude_snapshot();
    if (now - lastUi >= 1000 || u.seq != lastSeq) {
        lastUi = now;
        lastSeq = u.seq;
        BatteryInfo b = power_battery();
        ui_update(u, b, net_status());
        handle_idle(b);
    }

    lv_timer_handler();
    delay(5);
}
