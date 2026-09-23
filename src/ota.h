#pragma once
#include <Arduino.h>

// Over-the-air updates from the GitHub Pages install site: manifest.json gives
// the latest version, firmware/firmware.bin is the app image.

void ota_begin();                          // start the background check task
bool ota_update_available();
String ota_latest_version();

// Blocking download + flash into the spare app partition. `progress` gets 0-100.
// Returns true on success (caller restarts); false with ota_last_error() set.
bool ota_run(void (*progress)(int pct));
String ota_last_error();
