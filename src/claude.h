#pragma once
#include <Arduino.h>

struct UsageWindow {
    bool valid;
    float pct;          // 0-100
    time_t resetsAt;    // UTC epoch seconds, 0 if unknown
    char status[16];    // "allowed", "allowed_warning", "rejected"...
};

enum FetchState : uint8_t {
    FETCH_IDLE,         // nothing fetched yet
    FETCH_OK,
    FETCH_NO_TOKEN,
    FETCH_AUTH_ERROR,   // 401/403: token expired or revoked
    FETCH_NET_ERROR,
    FETCH_HTTP_ERROR,
};

struct UsageSnapshot {
    UsageWindow session;    // 5-hour window
    UsageWindow weekly;     // 7-day window
    UsageWindow overage;    // extra usage / spend limit (if the account has one)
    char overallStatus[16];
    FetchState state;
    int httpCode;
    time_t fetchedAt;       // UTC epoch of last successful fetch
    uint32_t seq;           // bumps on every attempt, so the UI knows to redraw
};

void claude_start();                  // start the background poll task
void claude_refresh_now();            // wake the task for an immediate poll
UsageSnapshot claude_snapshot();      // thread-safe copy of the latest data
void claude_set_low_power(bool low);  // poll less often while the screen is off
