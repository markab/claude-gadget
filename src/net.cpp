#include "net.h"
#include "settings.h"
#include "claude.h"
#include "portal_theme.h"
#include <WiFi.h>
#include <WiFiManager.h>
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

// Last connection, for a fast rejoin (skips the scan) after net_resume().
static bool suspended = false;
static String lastSsid;
static uint8_t lastBssid[6];
static int32_t lastChannel = 0;

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

// Roaming: async scan, then join the strongest known network. Non-blocking so
// the UI keeps animating; driven from net_loop().
enum RoamState : uint8_t { ROAM_IDLE, ROAM_SCAN, ROAM_JOIN };
static RoamState roam = ROAM_IDLE;
static uint32_t roamStart = 0;
static bool bootRoam = false;   // the first attempt at power-on: open the hotspot if it fails

static void start_roam() {
    lastRoam = millis();
    if (known.empty() || roam != ROAM_IDLE) return;
    Serial.println("[net] scanning for known networks");
    if (WiFi.scanNetworks(true) == WIFI_SCAN_FAILED) return;
    roam = ROAM_SCAN;
    roamStart = millis();
}

static void roam_failed() {
    roam = ROAM_IDLE;
    if (bootRoam) {
        bootRoam = false;
        Serial.println("[net] no known network in range");
        portalForOutage = true;
        wm.startConfigPortal(apName.c_str());  // no timeout: nothing else to do until set up
        mode = NET_AP_PORTAL;
    }
}

static void roam_step() {
    if (roam == ROAM_SCAN) {
        int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING) return;
        if (n < 0) return roam_failed();
        int best = -1, bestRssi = -1000;
        const Cred *bestCred = nullptr;
        for (int i = 0; i < n; i++)
            for (auto &c : known)
                if (WiFi.SSID(i) == c.ssid && WiFi.RSSI(i) > bestRssi) {
                    best = i;
                    bestRssi = WiFi.RSSI(i);
                    bestCred = &c;
                }
        if (best < 0) {
            WiFi.scanDelete();
            return roam_failed();
        }
        uint8_t bssid[6];
        memcpy(bssid, WiFi.BSSID(best), 6);
        int32_t ch = WiFi.channel(best);
        WiFi.scanDelete();
        Serial.printf("[net] joining %s (%d dBm)\n", bestCred->ssid.c_str(), bestRssi);
        WiFi.begin(bestCred->ssid.c_str(), bestCred->psk.c_str(), ch, bssid);
        roam = ROAM_JOIN;
        roamStart = millis();
    } else if (roam == ROAM_JOIN) {
        if (WiFi.status() == WL_CONNECTED) {
            roam = ROAM_IDLE;
            bootRoam = false;
        } else if (millis() - roamStart > 12000) {
            roam_failed();
        }
    }
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
    wm.setCustomHeadElement(PORTAL_HEAD);  // Claude colours + Clawd (replaces WiFiManager's dark mode)
    wm.setShowInfoUpdate(false);           // no OTA upload from the portal
    std::vector<const char *> menu = {"wifi", "param", "info", "sep", "restart"};
    wm.setMenu(menu);

    load_known();
    if (known.empty()) {
        // First run (or upgraded from single-network firmware): let WiFiManager
        // use whatever the Wi-Fi stack has saved; it's added to the list on connect.
        mode = wm.autoConnect(apName.c_str()) ? NET_CONNECTED : NET_AP_PORTAL;
    } else {
        bootRoam = true;
        disconnectedSince = millis();
        start_roam();   // finishes in net_loop(); opens the hotspot if nothing is found
        mode = NET_CONNECTING;
    }
}

void net_loop() {
    if (suspended) return;
    wm.process();
    roam_step();

    bool connected = WiFi.status() == WL_CONNECTED;
    if (connected && !wasConnected) {
        Serial.printf("[net] connected to %s, IP %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
        mode = NET_CONNECTED;
        disconnectedSince = 0;
        portalForOutage = false;
        remember_current();
        lastSsid = WiFi.SSID();
        memcpy(lastBssid, WiFi.BSSID(), 6);
        lastChannel = WiFi.channel();
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
        if (!apInUse && roam == ROAM_IDLE && !known.empty() && now - disconnectedSince > 15000 &&
            now - lastRoam > every)
            start_roam();
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

bool net_suspend() {
    if (suspended) return true;
    if (wm.getConfigPortalActive()) return false;  // someone may be mid-setup
    if (wm.getWebPortalActive()) wm.stopWebPortal();
    Serial.println("[net] Wi-Fi off");
    suspended = true;
    if (roam == ROAM_SCAN) WiFi.scanDelete();
    roam = ROAM_IDLE;
    bootRoam = false;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    wasConnected = false;
    mode = NET_CONNECTING;
    return true;
}

void net_resume() {
    if (!suspended) return;
    suspended = false;
    Serial.println("[net] Wi-Fi on");
    WiFi.mode(WIFI_STA);
    disconnectedSince = lastRoam = millis();   // roaming kicks in if this rejoin fails
    portalForOutage = false;
    const Cred *c = nullptr;
    for (auto &k : known)
        if (k.ssid == lastSsid) c = &k;
    if (c && lastChannel > 0) WiFi.begin(c->ssid.c_str(), c->psk.c_str(), lastChannel, lastBssid);
    else WiFi.begin();  // whatever the Wi-Fi stack has saved
}

bool net_is_suspended() { return suspended; }

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
