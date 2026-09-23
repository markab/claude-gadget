#include "sound.h"
#include "settings.h"
#include <ESP_I2S.h>
extern "C" {
#include "es8311/es8311.h"
}

// ES8311 codec on I2S, speaker amp enabled by PA. Tones are synthesised on the
// fly (sine + a touch of 2nd harmonic, fast attack, exponential decay) so there
// are no audio files. Playback runs in its own task.

#define I2S_MCLK 16
#define I2S_BCLK 9
#define I2S_WS   45
#define I2S_DOUT 8
#define PA_EN    46

static const uint32_t RATE = 16000;
static I2SClass i2s;
static QueueHandle_t queue;
static volatile bool busy = false;
static bool ok = false;

struct Note {
    uint16_t freq;   // Hz, 0 = rest
    uint16_t ms;     // length incl. decay
    uint16_t gapMs;  // silence after
};

// Note frequencies (Hz)
enum : uint16_t { N_C5 = 523, N_E5 = 659, N_G5 = 784, N_A5 = 880, N_C6 = 1047, N_E6 = 1319 };

static const Note STARTUP[]  = {{N_C5, 140, 10}, {N_E5, 140, 10}, {N_G5, 140, 10}, {N_C6, 420, 0}};
static const Note SHUTDOWN[] = {{N_C6, 140, 10}, {N_G5, 140, 10}, {N_E5, 140, 10}, {N_C5, 460, 0}};
static const Note HOURLY[]   = {{N_E6, 650, 60}, {N_C6, 900, 0}};
static const Note ALERT[]    = {{N_A5, 110, 70}, {N_A5, 110, 70}, {N_A5, 110, 0}};
static const Note PREVIEW[]  = {{N_E6, 120, 0}};

static void write_silence(uint32_t ms) {
    static int16_t zeros[256 * 2] = {};
    uint32_t frames = RATE * ms / 1000;
    while (frames) {
        uint32_t n = min<uint32_t>(frames, 256);
        i2s.write((uint8_t *)zeros, n * 4);
        frames -= n;
    }
}

static void play_note(const Note &n, float amp) {
    static int16_t buf[256 * 2];
    uint32_t total = RATE * n.ms / 1000;
    uint32_t attack = RATE * 6 / 1000;
    float w = 2.0f * PI * n.freq / RATE;
    float decay = expf(-5.0f / total);   // ~ -43 dB by the end of the note
    float env = 1.0f;
    uint32_t i = 0;
    while (i < total) {
        uint32_t chunk = min<uint32_t>(total - i, 256);
        for (uint32_t k = 0; k < chunk; k++, i++) {
            float a = i < attack ? (float)i / attack : env;
            if (i >= attack) env *= decay;
            float s = n.freq ? (sinf(w * i) + 0.3f * sinf(2 * w * i)) / 1.3f : 0;
            int16_t v = (int16_t)(s * a * amp * 32767);
            buf[2 * k] = buf[2 * k + 1] = v;
        }
        i2s.write((uint8_t *)buf, chunk * 4);
    }
    if (n.gapMs) write_silence(n.gapMs);
}

static void play(Sound s) {
    const Note *notes;
    size_t count;
    switch (s) {
        case SND_STARTUP:  notes = STARTUP;  count = sizeof(STARTUP) / sizeof(Note); break;
        case SND_SHUTDOWN: notes = SHUTDOWN; count = sizeof(SHUTDOWN) / sizeof(Note); break;
        case SND_HOURLY:   notes = HOURLY;   count = sizeof(HOURLY) / sizeof(Note); break;
        case SND_ALERT:    notes = ALERT;    count = sizeof(ALERT) / sizeof(Note); break;
        default:           notes = PREVIEW;  count = sizeof(PREVIEW) / sizeof(Note); break;
    }
    // Perceptual volume: square the 0-100% setting. Peak at 100% is ~70% FS to avoid clipping.
    float v = settings.volume / 100.0f;
    float amp = v * v * 0.7f;

    digitalWrite(PA_EN, HIGH);
    write_silence(30);   // let the amp settle (avoids a pop)
    for (size_t i = 0; i < count; i++) play_note(notes[i], amp);
    write_silence(60);   // flush the DMA buffers before muting
    digitalWrite(PA_EN, LOW);
}

static void sound_task(void *) {
    Sound s;
    for (;;) {
        if (xQueueReceive(queue, &s, portMAX_DELAY) == pdTRUE) {
            busy = true;
            play(s);
            busy = uxQueueMessagesWaiting(queue) > 0;
        }
    }
}

bool sound_begin() {
    pinMode(PA_EN, OUTPUT);
    digitalWrite(PA_EN, LOW);

    i2s.setPins(I2S_BCLK, I2S_WS, I2S_DOUT, -1, I2S_MCLK);
    if (!i2s.begin(I2S_MODE_STD, RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
        Serial.println("[sound] I2S init failed");
        return false;
    }

    es8311_handle_t es = es8311_create(0, ES8311_ADDRRES_0);  // I2C port 0 = Wire
    const es8311_clock_config_t clk = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = (int)RATE * 256,
        .sample_frequency = (int)RATE,
    };
    if (!es || es8311_init(es, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) {
        Serial.println("[sound] ES8311 not found");
        return false;
    }
    es8311_sample_frequency_config(es, clk.mclk_frequency, clk.sample_frequency);
    es8311_voice_volume_set(es, 75, NULL);  // 0 dB; volume is applied in software

    queue = xQueueCreate(4, sizeof(Sound));
    xTaskCreatePinnedToCore(sound_task, "sound", 4096, nullptr, 2, nullptr, 0);
    ok = true;
    return true;
}

void sound_play(Sound s) {
    if (!ok || !settings.soundOn || settings.volume == 0) return;
    busy = true;
    xQueueSend(queue, &s, 0);
}

void sound_wait(uint32_t maxMs) {
    uint32_t start = millis();
    while ((busy || (ok && uxQueueMessagesWaiting(queue))) && millis() - start < maxMs) delay(10);
}
