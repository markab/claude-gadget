#pragma once
#include <Arduino.h>

// Persistent settings in NVS. Wi-Fi credentials are stored by the Wi-Fi stack itself.
struct Settings {
    String token;        // Claude OAuth token from `claude setup-token`
    String tz;           // POSIX TZ string, e.g. "GMT0BST,M3.5.0/1,M10.5.0"
    uint16_t pollMins;   // minutes between usage polls
    uint8_t brightness;  // 10..255, normal screen brightness
    uint8_t dimBrightness; // 2..255, brightness when idle on USB
    bool soundOn;
    uint8_t volume;         // 0-100, 5% steps
    uint8_t alertLeftPct;   // alert when this much quota is left (0 = off)
    uint16_t dimSecs;    // screen off after this many idle seconds on battery (0 = never)
};

extern Settings settings;

void settings_load();
// The token is read from the poll task, so access it through these.
String settings_get_token();
void settings_set_token(const String &t);
void settings_save();
