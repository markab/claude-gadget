#pragma once
#include <Arduino.h>

enum Sound : uint8_t {
    SND_STARTUP,
    SND_SHUTDOWN,
    SND_HOURLY,
    SND_ALERT,
    SND_PREVIEW,   // short blip when adjusting volume
};

bool sound_begin();              // I2S + ES8311 codec. Call after Wire.begin()
void sound_play(Sound s);        // queued, non-blocking; ignored when sound is off
void sound_wait(uint32_t maxMs); // block until the queue has drained (e.g. before power-off)
