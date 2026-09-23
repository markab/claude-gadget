#include "net.h"
#include "settings.h"
#include "claude.h"
#include <WiFi.h>
#include <WiFiManager.h>

static WiFiManager wm;
static NetMode mode = NET_CONNECTING;
static String apName;
static bool wasConnected = false;
static char pollBuf[6];

// The token field is never pre-filled: anyone who joins the setup hotspot
// could otherwise read it. Leave it blank to keep the saved token.
static WiFiManagerParameter pToken("token", "Claude token (from <code>claude setup-token</code>, blank = keep current)",
                                   "", 255, "type='password' autocomplete='off'");
static WiFiManagerParameter pTz("tz", "Timezone (POSIX TZ string)", "", 63);
static WiFiManagerParameter pPoll("poll", "Refresh every N minutes", "", 4, "type='number' min='1' max='120'");

static void apply_time() {
    configTzTime(settings.tz.c_str(), "pool.ntp.org", "time.google.com");
}

static void on_save_params() {
    String tok = pToken.getValue();
    tok.trim();
    if (!tok.isEmpty()) settings_set_token(tok);

    String tz = pTz.getValue();
    tz.trim();
    if (!tz.isEmpty()) settings.tz = tz;

    int poll = atoi(pPoll.getValue());
    if (poll >= 1 && poll <= 120) settings.pollMins = poll;

    settings_save();
    apply_time();
    Serial.println("[net] settings saved");
    claude_refresh_now();
}

static void log_wifi_event(arduino_event_id_t ev, arduino_event_info_t info) {
    switch (ev) {
        case ARDUINO_EVENT_WIFI_AP_START: Serial.println("[wifi] AP started"); break;
        case ARDUINO_EVENT_WIFI_AP_STOP: Serial.println("[wifi] AP stopped"); break;
        case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
            Serial.printf("[wifi] client joined AP (aid %d)\n", info.wifi_ap_staconnected.aid);
            break;
        case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
            Serial.printf("[wifi] client left AP, reason %d\n", info.wifi_ap_stadisconnected.reason);
            break;
        case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED: Serial.println("[wifi] client got DHCP lease"); break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            Serial.printf("[wifi] STA disconnected, reason %d\n", info.wifi_sta_disconnected.reason);
            break;
        default: break;
    }
}

void net_begin() {
    WiFi.onEvent(log_wifi_event);
    uint32_t id = (uint32_t)(ESP.getEfuseMac() >> 24);
    char suffix[5];
    snprintf(suffix, sizeof(suffix), "%04X", (unsigned)(id & 0xFFFF));
    apName = String("Claude-Gadget-") + suffix;

    WiFi.mode(WIFI_STA);
    WiFi.setHostname("claude-gadget");

    pTz.setValue(settings.tz.c_str(), 63);
    snprintf(pollBuf, sizeof(pollBuf), "%u", settings.pollMins);
    pPoll.setValue(pollBuf, 4);

    wm.addParameter(&pToken);
    wm.addParameter(&pTz);
    wm.addParameter(&pPoll);
    wm.setSaveParamsCallback(on_save_params);
    wm.setBreakAfterConfig(true);          // save params even if the Wi-Fi join fails
    wm.setConfigPortalBlocking(false);
    wm.setConnectTimeout(20);
    wm.setTitle("Claude Gadget");
    wm.setDarkMode(true);
    wm.setShowInfoUpdate(false);           // no OTA upload from the portal
    std::vector<const char *> menu = {"wifi", "param", "info", "sep", "restart"};
    wm.setMenu(menu);

    if (wm.autoConnect(apName.c_str())) {
        mode = NET_CONNECTED;
    } else {
        mode = NET_AP_PORTAL;  // autoConnect left the setup hotspot running
    }
}

void net_loop() {
    wm.process();


    bool connected = WiFi.status() == WL_CONNECTED;
    if (connected && !wasConnected) {
        Serial.printf("[net] connected to %s, IP %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
        mode = NET_CONNECTED;
        apply_time();
        // No token yet: keep a settings page up on the LAN so it can be entered.
        if (settings_get_token().isEmpty() && !wm.getWebPortalActive() && !wm.getConfigPortalActive())
            wm.startWebPortal();
        claude_refresh_now();
    } else if (!connected && wasConnected) {
        Serial.println("[net] disconnected");
        if (!wm.getConfigPortalActive()) mode = NET_CONNECTING;  // the Wi-Fi stack auto-reconnects
    }
    wasConnected = connected;

    if (wm.getConfigPortalActive()) mode = NET_AP_PORTAL;
    else if (mode == NET_AP_PORTAL) mode = connected ? NET_CONNECTED : NET_CONNECTING;

    // Close the LAN settings page once a token is saved.
    if (wm.getWebPortalActive() && !wm.getConfigPortalActive() && !settings_get_token().isEmpty())
        wm.stopWebPortal();
}

void net_start_setup_portal() {
    if (wm.getConfigPortalActive()) return;
    if (wm.getWebPortalActive()) wm.stopWebPortal();
    Serial.println("[net] starting setup hotspot");
    wm.setConfigPortalTimeout(300);  // close it again after 5 idle minutes
    wm.startConfigPortal(apName.c_str());
    mode = NET_AP_PORTAL;
}

NetStatus net_status() {
    NetStatus s;
    s.mode = mode;
    s.webPortal = wm.getWebPortalActive() && !wm.getConfigPortalActive();
    s.apName = apName;
    bool connected = WiFi.status() == WL_CONNECTED;
    s.ssid = connected ? WiFi.SSID() : String();
    s.ip = connected ? WiFi.localIP().toString() : String();
    s.rssi = connected ? WiFi.RSSI() : 0;
    return s;
}
