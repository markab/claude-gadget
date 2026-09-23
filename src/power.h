#pragma once
#include <Arduino.h>

struct BatteryInfo {
    bool present;
    bool charging;
    bool vbus;          // USB power present
    bool full;          // charge done
    int percent;        // 0-100, -1 if unknown
    uint16_t battMv;
    uint16_t vbusMv;
    uint16_t sysMv;
    float pmuTempC;
    const char *chargeState;
};

enum PowerEvent : uint8_t {
    PWR_EVT_NONE = 0,
    PWR_EVT_SHORT_PRESS = 1 << 0,
    PWR_EVT_LONG_PRESS  = 1 << 1,
    PWR_EVT_VBUS_IN     = 1 << 2,
    PWR_EVT_VBUS_OUT    = 1 << 3,
    PWR_EVT_CHG_DONE    = 1 << 4,
};

bool power_init();              // call after Wire.begin()
uint8_t power_poll_events();    // returns PowerEvent bitmask
BatteryInfo power_battery();    // cached, refreshed every couple of seconds
void power_off();               // AXP2101 hard power-off; press PWR to turn back on
