#pragma once
#include <Arduino.h>

enum NetMode : uint8_t {
    NET_CONNECTING,   // trying saved credentials
    NET_AP_PORTAL,    // setup hotspot is up, waiting for the user
    NET_CONNECTED,
};

struct NetStatus {
    NetMode mode;
    bool webPortal;     // settings page reachable on the LAN at http://<ip>/
    String apName;
    String ssid;
    String ip;
    int rssi;
    int saved;          // number of remembered networks
};

void net_begin();
void net_loop();
void net_start_setup_portal();   // open the setup hotspot (BOOT long-press)
NetStatus net_status();
