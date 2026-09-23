#include "ota.h"
#include "certs.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>

static const char *SITE = "https://markab.github.io/claude-gadget/";
static const uint32_t CHECK_EVERY_MS = 6UL * 3600 * 1000;
static const uint32_t RETRY_MS = 10UL * 60 * 1000;

static SemaphoreHandle_t mux = nullptr;
static String latest;           // guarded by mux
static bool available = false;
static String lastError;

// "1.2.3" (optionally with a "-suffix") -> comparable number; -1 if not a version.
// Dev builds (git hashes) count as 0 so they are always offered the release.
static long parse_version(const String &v) {
    int a, b, c;
    if (sscanf(v.c_str(), "%d.%d.%d", &a, &b, &c) != 3) return -1;
    return (long)a * 1000000 + b * 1000 + c;
}

static String extract_version(const String &json) {
    int k = json.indexOf("\"version\"");
    if (k < 0) return "";
    int q1 = json.indexOf('"', json.indexOf(':', k) + 1);
    int q2 = json.indexOf('"', q1 + 1);
    return (q1 < 0 || q2 < 0) ? "" : json.substring(q1 + 1, q2);
}

static bool check_once() {
    WiFiClientSecure client;
    client.setCACert(PAGES_ROOT_CA);
    client.setTimeout(15);
    HTTPClient http;
    if (!http.begin(client, String(SITE) + "manifest.json")) return false;
    http.setTimeout(15000);
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[ota] manifest HTTP %d\n", code);
        http.end();
        return false;
    }
    String v = extract_version(http.getString());
    http.end();
    if (v.isEmpty()) return false;

    long remote = parse_version(v), local = parse_version(FW_VERSION);
    bool newer = remote >= 0 && remote > max(local, 0L);
    xSemaphoreTake(mux, portMAX_DELAY);
    latest = v;
    available = newer;
    xSemaphoreGive(mux);
    Serial.printf("[ota] running %s, latest %s%s\n", FW_VERSION, v.c_str(), newer ? " (update available)" : "");
    return true;
}

static void check_task(void *) {
    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(check_once() ? CHECK_EVERY_MS : RETRY_MS));
    }
}

void ota_begin() {
    mux = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(check_task, "ota", 8192, nullptr, 1, nullptr, 0);
}

bool ota_update_available() { return available; }

String ota_latest_version() {
    if (!mux) return "";   // the UI can ask before ota_begin() has run
    xSemaphoreTake(mux, portMAX_DELAY);
    String v = latest;
    xSemaphoreGive(mux);
    return v;
}

String ota_last_error() { return lastError; }

static void (*progressCb)(int) = nullptr;

bool ota_run(void (*progress)(int pct)) {
    if (WiFi.status() != WL_CONNECTED) {
        lastError = "No Wi-Fi";
        return false;
    }
    progressCb = progress;
    WiFiClientSecure client;
    client.setCACert(PAGES_ROOT_CA);
    client.setTimeout(30);

    httpUpdate.rebootOnUpdate(false);   // caller restarts after showing "done"
    httpUpdate.onProgress([](int cur, int total) {
        if (progressCb && total > 0) progressCb((int)((int64_t)cur * 100 / total));
    });
    Serial.println("[ota] downloading firmware");
    HTTPUpdateResult r = httpUpdate.update(client, String(SITE) + "firmware/firmware.bin");
    if (r == HTTP_UPDATE_OK) {
        Serial.println("[ota] update written");
        return true;
    }
    lastError = r == HTTP_UPDATE_NO_UPDATES ? String("No update found") : httpUpdate.getLastErrorString();
    Serial.printf("[ota] failed: %s\n", lastError.c_str());
    return false;
}
