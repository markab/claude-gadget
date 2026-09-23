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
void ui_capture_screens();                // serial 's': stream each page as RGB565 (demo data)
void ui_show_hourly(time_t now);
bool ui_take_update_request();             // true once after the Device page's Update pill is tapped
void ui_show_updating(const char *version);
void ui_set_update_progress(int pct);      // pumps LVGL; call from the OTA progress callback
void ui_show_update_result(bool ok, const char *msg);  // blocking; removes the overlay on failure          // Clawd + time for 10 s, tap to dismiss
void ui_mark_refreshing(uint32_t seq);     // footer says "Refreshing..." until the snapshot seq moves on
void ui_flash_message(const char *msg);   // brief toast at the bottom of the screen
