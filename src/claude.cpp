#include "claude.h"
#include "certs.h"
#include "settings.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// Subscription usage isn't exposed by a public API that a `claude setup-token`
// token can read (/api/oauth/usage needs the user:profile scope). Instead we
// make the smallest possible Messages call — Haiku, max_tokens=1 — and read
// the unified rate-limit headers that come back on every response. Same trick
// as github.com/HermannBjorgvin/Clawdmeter. Each poll costs a negligible
// sliver of your plan's quota.

static const char *API_URL = "https://api.anthropic.com/v1/messages";
static const char *MODEL = "claude-haiku-4-5-20251001";
static const char *USER_AGENT = "claude-gadget/" FW_VERSION;

static const char *HDRS[] = {
    "anthropic-ratelimit-unified-status",
    "anthropic-ratelimit-unified-5h-utilization",
    "anthropic-ratelimit-unified-5h-reset",
    "anthropic-ratelimit-unified-5h-status",
    "anthropic-ratelimit-unified-7d-utilization",
    "anthropic-ratelimit-unified-7d-reset",
    "anthropic-ratelimit-unified-7d-status",
    "anthropic-ratelimit-unified-overage-utilization",
    "anthropic-ratelimit-unified-overage-reset",
    "anthropic-ratelimit-unified-overage-status",
};
static const size_t N_HDRS = sizeof(HDRS) / sizeof(HDRS[0]);

static SemaphoreHandle_t mux;
static UsageSnapshot snap = {};
static TaskHandle_t task = nullptr;
static volatile bool lowPower = false;

static UsageSnapshot locked_copy() {
    xSemaphoreTake(mux, portMAX_DELAY);
    UsageSnapshot s = snap;
    xSemaphoreGive(mux);
    return s;
}

static void publish(const UsageSnapshot &s) {
    xSemaphoreTake(mux, portMAX_DELAY);
    snap = s;
    snap.seq++;
    xSemaphoreGive(mux);
}

static bool fill_window(HTTPClient &http, const char *prefix, UsageWindow &w) {
    char name[64];
    snprintf(name, sizeof(name), "anthropic-ratelimit-unified-%s-utilization", prefix);
    String util = http.header(name);
    if (util.isEmpty()) {
        w.valid = false;
        return false;
    }
    w.valid = true;
    w.pct = util.toFloat() * 100.0f;  // header is a 0-1 fraction
    snprintf(name, sizeof(name), "anthropic-ratelimit-unified-%s-reset", prefix);
    w.resetsAt = (time_t)http.header(name).toInt();
    snprintf(name, sizeof(name), "anthropic-ratelimit-unified-%s-status", prefix);
    strlcpy(w.status, http.header(name).c_str(), sizeof(w.status));
    return true;
}

static FetchState fetch_once(UsageSnapshot &s) {
    String token = settings_get_token();
    if (token.isEmpty()) return FETCH_NO_TOKEN;
    if (WiFi.status() != WL_CONNECTED) return FETCH_NET_ERROR;

    WiFiClientSecure client;
    client.setCACert(CLAUDE_ROOT_CA);
    client.setTimeout(15);

    HTTPClient http;
    if (!http.begin(client, API_URL)) return FETCH_NET_ERROR;
    http.setTimeout(15000);
    http.setUserAgent(USER_AGENT);
    http.addHeader("Authorization", "Bearer " + token);
    http.addHeader("anthropic-version", "2023-06-01");
    http.addHeader("anthropic-beta", "oauth-2025-04-20");
    http.addHeader("Content-Type", "application/json");
    http.collectHeaders(HDRS, N_HDRS);

    String body = String("{\"model\":\"") + MODEL +
                  "\",\"max_tokens\":1,\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
    int code = http.POST(body);
    s.httpCode = code;

    if (code <= 0) {
        Serial.printf("[claude] request failed: %s\n", http.errorToString(code).c_str());
        http.end();
        return FETCH_NET_ERROR;
    }
    if (code == 401 || code == 403) {
        Serial.printf("[claude] auth error %d: %s\n", code, http.getString().substring(0, 200).c_str());
        http.end();
        return FETCH_AUTH_ERROR;
    }

    // Rate-limit headers also arrive on 429s (i.e. when you've hit a limit),
    // so parse them before judging the status code.
    bool any = fill_window(http, "5h", s.session);
    any |= fill_window(http, "7d", s.weekly);
    any |= fill_window(http, "overage", s.overage);
    strlcpy(s.overallStatus, http.header(HDRS[0]).c_str(), sizeof(s.overallStatus));

    if (!any) {
        Serial.printf("[claude] HTTP %d with no usage headers: %s\n", code, http.getString().substring(0, 200).c_str());
        http.end();
        return FETCH_HTTP_ERROR;
    }
    http.end();
    s.fetchedAt = time(nullptr);
    Serial.printf("[claude] 5h %.0f%%  7d %.0f%%  (%s)\n", s.session.pct, s.weekly.pct, s.overallStatus);
    return FETCH_OK;
}

static bool clock_valid() { return time(nullptr) > 1700000000; }

static void poll_task(void *) {
    uint32_t failures = 0;
    for (;;) {
        uint32_t waitMs;
        if (WiFi.status() != WL_CONNECTED || !clock_valid()) {
            waitMs = 2000;  // wait for Wi-Fi + NTP before the first attempt
        } else {
            UsageSnapshot s = locked_copy();
            FetchState st = fetch_once(s);
            s.state = st;
            publish(s);

            uint32_t interval = settings.pollMins * 60000UL;
            if (lowPower) interval *= 3;
            if (st == FETCH_OK || st == FETCH_NO_TOKEN || st == FETCH_AUTH_ERROR) {
                failures = 0;
                waitMs = interval;
            } else {
                failures++;
                waitMs = min<uint32_t>(15000UL << min<uint32_t>(failures, 5), interval);
            }
        }
        // claude_refresh_now() cuts the wait short.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(waitMs));
    }
}

void claude_start() {
    mux = xSemaphoreCreateMutex();
    snap.state = FETCH_IDLE;
    // TLS handshakes need a generous stack. Core 0 keeps the UI on core 1 smooth.
    xTaskCreatePinnedToCore(poll_task, "claude", 12288, nullptr, 1, &task, 0);
}

void claude_refresh_now() {
    if (task) xTaskNotifyGive(task);
}

UsageSnapshot claude_snapshot() { return locked_copy(); }

void claude_set_low_power(bool low) { lowPower = low; }
