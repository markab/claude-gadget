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
#include <esp_sleep.h>
#include <driver/gpio.h>
#include "board.h"
#include "settings.h"
#include "power.h"
#include "display.h"
#include "net.h"
#include "claude.h"
#include "ui.h"
#include "sound.h"

static const uint16_t LOW_BATT_OFF_MV = 3300;      // auto power-off threshold
static const uint32_t BOOT_HOLD_MS = 3000;

static bool dimmed = false;
static uint32_t lowBattSince = 0;

// PWR hold-to-power-off animation state
static const uint32_t PWR_LONG_MS = 2000;   // matches the PMU IRQ level time
static const uint32_t HOLD_SHOW_MS = 350;   // quick taps never show the overlay
static uint32_t keyDownAt = 0;
static bool holdShown = false;

static void screen_off() {
    display_sleep();
    setCpuFrequencyMhz(80);
    // On battery, Wi-Fi goes off until the screen comes back: no point polling
    // numbers nobody can see. On USB it stays up and keeps refreshing.
    // (Plugging/unplugging USB wakes the screen, so this is re-evaluated then.)
    if (!power_battery().vbus) {
        claude_set_low_power(true);
        net_suspend();
    }
}

static void screen_on() {
    setCpuFrequencyMhz(240);
    net_resume();    // rejoins in ~1-3 s; the poll task refreshes as soon as it's connected
    display_wake();
    display_set_brightness(settings.brightness);
    dimmed = false;
    claude_set_low_power(false);
}

// Light-sleep for up to 150 ms with the screen off and Wi-Fi down. The PMU
// latches PWR presses, so they're still seen on the next poll. A tap or BOOT
// press wakes immediately. Returns true if a tap woke us.
static bool nap() {
    esp_sleep_enable_timer_wakeup(150 * 1000);
    gpio_wakeup_enable((gpio_num_t)TP_INT, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable((gpio_num_t)BTN_BOOT, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    esp_light_sleep_start();
    bool tap = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO && digitalRead(TP_INT) == LOW;
    gpio_wakeup_disable((gpio_num_t)TP_INT);
    gpio_wakeup_disable((gpio_num_t)BTN_BOOT);
    gpio_set_intr_type((gpio_num_t)TP_INT, GPIO_INTR_NEGEDGE);  // restore the touch ISR's edge trigger
    return tap;
}

// Show Clawd + the time for 10 s when the hour changes (only if the screen is on).
static void handle_hourly() {
    static int lastHour = -1;
    time_t now = time(nullptr);
    if (now < 1700000000) return;  // clock not synced yet
    struct tm tm;
    localtime_r(&now, &tm);
    if (lastHour == -1) lastHour = tm.tm_hour;  // don't fire on boot
    if (tm.tm_hour == lastHour) return;
    lastHour = tm.tm_hour;
    if (tm.tm_min == 0 && display_is_awake()) {
        ui_show_hourly(now);
        sound_play(SND_HOURLY);
    }
}

// Low-quota alert: fires once per window when the quota left drops to the
// threshold; re-arms when that window resets.
static void check_quota_alert(const UsageSnapshot &u) {
    static time_t alertedFor[2] = {0, 0};
    if (u.state != FETCH_OK || settings.alertLeftPct == 0) return;
    const UsageWindow *w[2] = {&u.session, &u.weekly};
    const char *name[2] = {"Session", "Weekly"};
    for (int i = 0; i < 2; i++) {
        if (!w[i]->valid) continue;
        int left = max(0, 100 - (int)roundf(w[i]->pct));
        if (left > settings.alertLeftPct) {
            alertedFor[i] = 0;
            continue;
        }
        if (alertedFor[i] == w[i]->resetsAt && alertedFor[i] != 0) continue;
        alertedFor[i] = w[i]->resetsAt ? w[i]->resetsAt : 1;
        Serial.printf("[alert] %s quota %d%% left\n", name[i], left);
        if (!display_is_awake()) screen_on();
        char msg[48];
        snprintf(msg, sizeof(msg), LV_SYMBOL_WARNING " %s: %d%% left", name[i], left);
        ui_flash_message(msg);
        sound_play(SND_ALERT);
        break;  // one alert at a time
    }
}

static void shutdown_now() {
    if (!display_is_awake()) screen_on();
    sound_play(SND_SHUTDOWN);
    ui_play_shutdown();
    sound_wait(1500);
    power_off();
}

static void handle_power_events() {
    uint8_t ev = power_poll_events();
    bool abortedHold = false;
    if ((ev & PWR_EVT_KEY_DOWN) && !(ev & PWR_EVT_KEY_UP)) keyDownAt = millis();
    if (ev & PWR_EVT_KEY_UP) {
        abortedHold = holdShown;   // let go before 2 s: cancel, don't also toggle the screen
        if (holdShown) ui_hide_hold_to_off();
        holdShown = false;
        keyDownAt = 0;
    }
    if (keyDownAt && !holdShown && display_is_awake() && millis() - keyDownAt >= HOLD_SHOW_MS) {
        holdShown = true;
        ui_show_hold_to_off(millis() - keyDownAt, PWR_LONG_MS);
    }

    if (ev & PWR_EVT_LONG_PRESS) {
        shutdown_now();
    } else if ((ev & PWR_EVT_SHORT_PRESS) && !abortedHold) {
        if (display_is_awake()) screen_off();
        else screen_on();
    }
    // Plug/unplug wakes the screen; the battery icon shows the charge state.
    if ((ev & (PWR_EVT_VBUS_IN | PWR_EVT_VBUS_OUT)) && !display_is_awake()) screen_on();
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
                ui_mark_refreshing(claude_snapshot().seq);
                claude_refresh_now();
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
    sound_begin();
    display_init();
    display_set_brightness(settings.brightness);
    ui_init();
    sound_play(SND_STARTUP);
    ui_play_boot();
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
        if (display_take_wake_tap()) {
            screen_on();
            return;
        }
        // Light sleep kills USB serial, so only nap on battery.
        if (net_is_suspended() && !power_battery().vbus) {
            if (nap()) screen_on();
        } else {
            delay(20);
        }
        return;
    }

    handle_hourly();
    // Serial 'h' previews the hourly screen, 'a' the quota alert sound.
    if (Serial.available()) {
        int c = Serial.read();
        if (c == 'h') {
            ui_show_hourly(time(nullptr));
            sound_play(SND_HOURLY);
        } else if (c == 'a') {
            sound_play(SND_ALERT);
        }
    }

    UsageSnapshot u = claude_snapshot();
    if (now - lastUi >= 1000 || u.seq != lastSeq) {
        lastUi = now;
        if (u.seq != lastSeq) check_quota_alert(u);
        lastSeq = u.seq;
        BatteryInfo b = power_battery();
        ui_update(u, b, net_status());
        handle_idle(b);
    }

    lv_timer_handler();
    delay(5);
}
