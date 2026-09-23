#pragma once
#include "claude.h"
#include "power.h"
#include "net.h"

void ui_init();
void ui_update(const UsageSnapshot &u, const BatteryInfo &b, const NetStatus &n);
void ui_show_power_off();
void ui_show_hourly(time_t now);          // Clawd + time for 5 s, tap to dismiss
void ui_flash_message(const char *msg);   // brief toast at the bottom of the screen
