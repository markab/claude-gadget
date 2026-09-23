#include "settings.h"
#include <Preferences.h>

Settings settings;

static const char *NS = "gadget";
static SemaphoreHandle_t tokenMux = nullptr;  // created in settings_load()

String settings_get_token() {
    xSemaphoreTake(tokenMux, portMAX_DELAY);
    String t = settings.token;
    xSemaphoreGive(tokenMux);
    return t;
}

void settings_set_token(const String &t) {
    xSemaphoreTake(tokenMux, portMAX_DELAY);
    settings.token = t;
    xSemaphoreGive(tokenMux);
}

void settings_load() {
    if (!tokenMux) tokenMux = xSemaphoreCreateMutex();
    Preferences p;
    p.begin(NS, true);
    settings.token      = p.getString("token", "");
    settings.tz         = p.getString("tz", "GMT0BST,M3.5.0/1,M10.5.0");
    settings.pollMins   = p.getUShort("poll", 5);
    settings.brightness = p.getUChar("bright", 180);
    settings.dimSecs    = p.getUShort("dim", 60);
    p.end();
    if (settings.pollMins < 1) settings.pollMins = 1;
    if (settings.brightness < 10) settings.brightness = 10;
}

void settings_save() {
    Preferences p;
    p.begin(NS, false);
    p.putString("token", settings.token);
    p.putString("tz", settings.tz);
    p.putUShort("poll", settings.pollMins);
    p.putUChar("bright", settings.brightness);
    p.putUShort("dim", settings.dimSecs);
    p.end();
}
