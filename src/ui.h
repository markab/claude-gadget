#pragma once
#include "claude.h"
#include "power.h"
#include "net.h"

void ui_init();
void ui_update(const UsageSnapshot &u, const BatteryInfo &b, const NetStatus &n);
void ui_show_hold_to_off(uint32_t heldMs, uint32_t totalMs);  // PWR held: Clawd + filling ring
void ui_hide_hold_to_off();
void ui_play_shutdown();                   // blocking ~1.2 s goodbye animation
void ui_play_boot();                       // ~1.3 s intro, then Clawd idles until usage loads
void ui_show_hourly(time_t now);          // Clawd + time for 5 s, tap to dismiss
void ui_flash_message(const char *msg);   // brief toast at the bottom of the screen
