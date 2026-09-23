#include "net.h"
#include "settings.h"
#include "claude.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiMulti.h>
#include <Preferences.h>
#include <vector>

static WiFiManager wm;
static NetMode mode = NET_CONNECTING;
static String apName;
static bool wasConnected = false;
static char pollBuf[6];

// ---- Known networks: WiFiManager only remembers the last one, so we keep our
// own most-recently-used list and join whichever is strongest in range.
static const size_t MAX_KNOWN = 5;
struct Cred { String ssid, psk; };
static std::vector<Cred> known;
static uint32_t disconnectedSince = 0;
static uint32_t lastRoam = 0;
static bool portalForOutage = false;   // opened the hotspot for the current outage
static uint32_t knownVersion = 0;

static void load_known() {
    Preferences p;
    if (!p.begin("wifi", true)) return;
    uint8_t n = p.getUChar("n", 0);
    for (uint8_t i = 0; i < n && i < MAX_KNOWN; i++) {
        String k = String(i);
        Cred c{p.getString(("s" + k).c_str(), ""), p.getString(("p" + k).c_str(), "")};
        if (!c.ssid.isEmpty()) known.push_back(c);
    }
    p.end();
}

static void save_known() {
    Preferences p;
    p.begin("wifi", false);
    p.clear();
    p.putUChar("n", known.size());
    for (size_t i = 0; i < known.size(); i++) {
        String k = String(i);
        p.putString(("s" + k).c_str(), known[i].ssid);
        p.putString(("p" + k).c_str(), known[i].psk);
    }
    p.end();
}

// Move the current network to the front of the list (adding or updating it).
static void remember_current() {
    Cred c{WiFi.SSID(), WiFi.psk()};
    if (c.ssid.isEmpty()) return;
    if (!known.empty() && known[0].ssid == c.ssid && known[0].psk == c.psk) return;
    for (size_t i = 0; i < known.size(); i++)
        if (known[i].ssid == c.ssid) { known.erase(known.begin() + i); break; }
    known.insert(known.begin(), c);
    if (known.size() > MAX_KNOWN) known.resize(MAX_KNOWN);
    save_known();
    knownVersion++;
    Serial.printf("[net] remembered %s (%u saved)\n", c.ssid.c_str(), (unsigned)known.size());
}

// Scan and join the strongest known network. Blocks for a few seconds.
static bool try_known() {
    lastRoam = millis();
    if (known.empty()) return false;
    Serial.println("[net] looking for known networks");
    WiFiMulti multi;
    for (auto &c : known) multi.addAP(c.ssid.c_str(), c.psk.c_str());
    return multi.run(10000) == WL_CONNECTED;
}

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

    load_known();
    if (known.empty()) {
        // First run (or upgraded from single-network firmware): let WiFiManager
        // use whatever the Wi-Fi stack has saved; it's added to the list on connect.
        mode = wm.autoConnect(apName.c_str()) ? NET_CONNECTED : NET_AP_PORTAL;
    } else if (try_known()) {
        mode = NET_CONNECTED;
    } else {
        Serial.println("[net] no known network in range");
        wm.startConfigPortal(apName.c_str());
        mode = NET_AP_PORTAL;
    }
}

void net_loop() {
    wm.process();

    bool connected = WiFi.status() == WL_CONNECTED;
    if (connected && !wasConnected) {
        Serial.printf("[net] connected to %s, IP %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
        mode = NET_CONNECTED;
        disconnectedSince = 0;
        portalForOutage = false;
        remember_current();
        if (wm.getConfigPortalActive()) wm.stopConfigPortal();  // roamed onto a known network
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

    // Roaming: if the connection is gone (e.g. moved location), look for any
    // known network. Don't scan while someone is using the hotspot, as scanning
    // hops channels and drops them.
    if (!connected) {
        uint32_t now = millis();
        if (!disconnectedSince) disconnectedSince = now;
        bool portal = wm.getConfigPortalActive();
        bool apInUse = portal && WiFi.softAPgetStationNum() > 0;
        uint32_t every = portal ? 60000 : 30000;
        if (!apInUse && !known.empty() && now - disconnectedSince > 15000 && now - lastRoam > every)
            try_known();
        // Still nothing after 90 s: open the hotspot so a new network can be added.
        if (!portalForOutage && !portal && now - disconnectedSince > 90000) {
            portalForOutage = true;
            net_start_setup_portal();
        }
    }

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
    s.saved = known.size();
    return s;
}

std::vector<String> net_saved_ssids() {
    std::vector<String> v;
    for (auto &c : known) v.push_back(c.ssid);
    return v;
}

uint32_t net_saved_version() { return knownVersion; }

void net_forget(const String &ssid) {
    for (size_t i = 0; i < known.size(); i++) {
        if (known[i].ssid != ssid) continue;
        known.erase(known.begin() + i);
        save_known();
        knownVersion++;
        Serial.printf("[net] forgot %s (%u saved)\n", ssid.c_str(), (unsigned)known.size());
        break;
    }
    // The Wi-Fi stack keeps its own copy of the current network; erase that too
    // so it isn't rejoined on reboot. Roaming then looks for another saved network.
    if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == ssid) WiFi.disconnect(false, true);
}
