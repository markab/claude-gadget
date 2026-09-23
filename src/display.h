#pragma once
#include <Arduino.h>

bool display_init();                  // display, touch, LVGL. Call after Wire.begin()
void display_set_brightness(uint8_t level);
void display_sleep();                 // panel off; a tap or PWR press wakes it
void display_wake();
bool display_is_awake();
bool display_take_wake_tap();         // true once per tap while the panel is asleep
uint32_t display_last_touch_ms();     // millis() of last touch
