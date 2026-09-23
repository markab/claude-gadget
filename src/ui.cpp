#include "ui.h"
#include "board.h"
#include "settings.h"
#include "display.h"
#include <lvgl.h>
#include <time.h>
#include <vector>

// 466x466 round AMOLED: true black background (pixels off), Claude-ish palette.
#define COL_BG        lv_color_hex(0x000000)
#define COL_TEXT      lv_color_hex(0xF0EEE6)
#define COL_MUTED     lv_color_hex(0x8A877F)
#define COL_TRACK     lv_color_hex(0x262422)
#define COL_CLAUDE    lv_color_hex(0xD97757)
#define COL_WEEK      lv_color_hex(0xC2B8A3)
#define COL_WARN      lv_color_hex(0xE8A33D)
#define COL_CRIT      lv_color_hex(0xE5484D)
#define COL_OK        lv_color_hex(0x5DB075)

static lv_obj_t *tv;

// Clock tile
static lv_obj_t *lblClockTime, *lblClockDate;

// Usage tile
static lv_obj_t *arcSession, *arcWeek;
static lv_obj_t *lblStatusBar, *lblSessionPct, *lblSessionReset, *lblWeekPct, *lblWeekReset, *lblFooter;

// Battery tile
static lv_obj_t *arcBatt, *lblBattPct, *lblBattState, *lblBattDetail;

// Info tile
static lv_obj_t *lblInfo;

// Overlays
static lv_obj_t *setupLayer, *setupTitle, *setupBody, *setupQr;
static lv_obj_t *toast;
static String lastQr;
// Wi-Fi tile
static lv_obj_t *wifiList;
static std::vector<String> wifiSsids;   // backs the delete buttons' user_data
static uint32_t wifiListVersion = UINT32_MAX;
static String wifiListCurrent;
static lv_obj_t *armedBtn = nullptr;     // first tap arms a delete, second confirms
static lv_timer_t *disarmTimer = nullptr;

// Settings tile
static lv_obj_t *sldBright, *lblBright, *sldDimLevel, *lblDimLevel, *sldDim, *lblDim;

// ---------------------------------------------------------------- helpers

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

static lv_obj_t *make_arc(lv_obj_t *parent, lv_coord_t size, lv_coord_t width, lv_color_t color) {
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_set_size(a, size, size);
    lv_obj_center(a);
    lv_arc_set_bg_angles(a, 135, 45);   // 270° gauge, opening at the bottom
    lv_arc_set_range(a, 0, 1000);
    lv_arc_set_value(a, 0);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, COL_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, color, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
    return a;
}

static lv_color_t level_color(float pct, lv_color_t normal) {
    if (pct >= 90) return COL_CRIT;
    if (pct >= 75) return COL_WARN;
    return normal;
}

static String fmt_duration(long secs) {
    if (secs <= 0) return "now";
    long m = (secs + 59) / 60;
    char b[24];
    if (m < 60) snprintf(b, sizeof(b), "%ldm", m);
    else if (m < 24 * 60) snprintf(b, sizeof(b), "%ldh %02ldm", m / 60, m % 60);
    else snprintf(b, sizeof(b), "%ldd %ldh", m / 1440, (m % 1440) / 60);
    return b;
}

static String fmt_local(time_t t, const char *fmt) {
    struct tm tm;
    localtime_r(&t, &tm);
    char b[32];
    strftime(b, sizeof(b), fmt, &tm);
    return b;
}

static const char *battery_symbol(const BatteryInfo &b) {
    if (b.charging) return LV_SYMBOL_CHARGE;
    if (!b.present) return LV_SYMBOL_USB;
    if (b.percent >= 85) return LV_SYMBOL_BATTERY_FULL;
    if (b.percent >= 60) return LV_SYMBOL_BATTERY_3;
    if (b.percent >= 35) return LV_SYMBOL_BATTERY_2;
    if (b.percent >= 12) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

// Auto-dim slider steps, in seconds (0 = never).
static const uint16_t DIM_STEPS[] = {0, 15, 30, 60, 120, 300, 600, 1800};
static const int N_DIM_STEPS = sizeof(DIM_STEPS) / sizeof(DIM_STEPS[0]);

static String fmt_dim(uint16_t secs) {
    if (secs == 0) return "Never";
    if (secs < 60) return String(secs) + " s";
    return String(secs / 60) + " min";
}

// ---------------------------------------------------------------- build

static void build_usage(lv_obj_t *t) {
    arcSession = make_arc(t, 452, 20, COL_CLAUDE);
    arcWeek = make_arc(t, 396, 12, COL_WEEK);

    lv_obj_t *cap = make_label(t, &lv_font_montserrat_16, COL_MUTED);
    lv_label_set_text(cap, "SESSION  " LV_SYMBOL_BULLET "  5H");
    lv_obj_align(cap, LV_ALIGN_CENTER, 0, -124);

    lblSessionPct = make_label(t, &lv_font_montserrat_48, COL_TEXT);
    lv_obj_align(lblSessionPct, LV_ALIGN_CENTER, 0, -74);

    lblSessionReset = make_label(t, &lv_font_montserrat_20, COL_TEXT);
    lv_obj_align(lblSessionReset, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *line = lv_obj_create(t);
    lv_obj_set_size(line, 180, 2);
    lv_obj_set_style_bg_color(line, COL_TRACK, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_align(line, LV_ALIGN_CENTER, 0, 10);

    lblWeekPct = make_label(t, &lv_font_montserrat_28, COL_WEEK);
    lv_obj_align(lblWeekPct, LV_ALIGN_CENTER, 0, 42);

    lblWeekReset = make_label(t, &lv_font_montserrat_16, COL_MUTED);
    lv_obj_align(lblWeekReset, LV_ALIGN_CENTER, 0, 72);

    // Bottom sits in the gap of the rings: freshness line, then clock / Wi-Fi / battery.
    lblFooter = make_label(t, &lv_font_montserrat_16, COL_MUTED);
    lv_label_set_long_mode(lblFooter, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lblFooter, 260);
    lv_obj_align(lblFooter, LV_ALIGN_BOTTOM_MID, 0, -78);

    lblStatusBar = make_label(t, &lv_font_montserrat_20, COL_MUTED);
    lv_obj_align(lblStatusBar, LV_ALIGN_BOTTOM_MID, 0, -42);
}

static void build_battery(lv_obj_t *t) {
    arcBatt = make_arc(t, 452, 20, COL_OK);
    lv_arc_set_range(arcBatt, 0, 100);

    lv_obj_t *cap = make_label(t, &lv_font_montserrat_16, COL_MUTED);
    lv_label_set_text(cap, "BATTERY");
    lv_obj_align(cap, LV_ALIGN_CENTER, 0, -120);

    lblBattPct = make_label(t, &lv_font_montserrat_48, COL_TEXT);
    lv_obj_align(lblBattPct, LV_ALIGN_CENTER, 0, -72);

    lblBattState = make_label(t, &lv_font_montserrat_20, COL_TEXT);
    lv_obj_align(lblBattState, LV_ALIGN_CENTER, 0, -24);

    lblBattDetail = make_label(t, &lv_font_montserrat_20, COL_MUTED);
    lv_obj_set_style_text_line_space(lblBattDetail, 6, 0);
    lv_obj_align(lblBattDetail, LV_ALIGN_CENTER, 0, 60);

    lv_obj_t *hint = make_label(t, &lv_font_montserrat_14, COL_MUTED);
    lv_label_set_text(hint, "PWR: tap = screen  \xE2\x80\xA2  hold 2s = off");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -46);
}

static void build_info(lv_obj_t *t) {
    lv_obj_t *cap = make_label(t, &lv_font_montserrat_16, COL_MUTED);
    lv_label_set_text(cap, "DEVICE");
    lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 70);

    lblInfo = make_label(t, &lv_font_montserrat_20, COL_TEXT);
    lv_obj_set_style_text_line_space(lblInfo, 8, 0);
    lv_obj_align(lblInfo, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *hint = make_label(t, &lv_font_montserrat_14, COL_MUTED);
    lv_label_set_text(hint, "Hold BOOT 3s for Wi-Fi / token setup");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -56);
}

static int to_pct(int v) { return (v * 100 + 127) / 255; }

static void bright_cb(lv_event_t *e) {
    int v = lv_slider_get_value(sldBright);
    lv_label_set_text_fmt(lblBright, "Brightness  %d%%", to_pct(v));
    settings.brightness = v;
    display_set_brightness(v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) settings_save();  // only write flash on release
}

// Previews the dim level on the panel while dragging, then restores normal brightness.
static void dim_level_cb(lv_event_t *e) {
    int v = lv_slider_get_value(sldDimLevel);
    lv_label_set_text_fmt(lblDimLevel, "Dimmed  %d%%", to_pct(v));
    settings.dimBrightness = v;
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED) {
        display_set_brightness(settings.brightness);
        settings_save();
    } else if (lv_slider_is_dragged(sldDimLevel) || code == LV_EVENT_PRESSED) {
        display_set_brightness(v);
    }
}

static void dim_cb(lv_event_t *e) {
    uint16_t secs = DIM_STEPS[lv_slider_get_value(sldDim)];
    lv_label_set_text_fmt(lblDim, "Auto-dim  %s", fmt_dim(secs).c_str());
    settings.dimSecs = secs;
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) settings_save();
}

static lv_obj_t *make_slider(lv_obj_t *parent, lv_coord_t y, lv_color_t color) {
    lv_obj_t *s = lv_slider_create(parent);
    lv_obj_set_size(s, 300, 14);
    lv_obj_align(s, LV_ALIGN_CENTER, 0, y);
    lv_obj_set_ext_click_area(s, 24);
    lv_obj_set_style_bg_color(s, COL_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, color, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, COL_TEXT, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 8, LV_PART_KNOB);
    return s;
}

static void build_settings(lv_obj_t *t) {
    lv_obj_t *cap = make_label(t, &lv_font_montserrat_16, COL_MUTED);
    lv_label_set_text(cap, "SETTINGS");
    lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 58);

    lblBright = make_label(t, &lv_font_montserrat_20, COL_TEXT);
    lv_obj_align(lblBright, LV_ALIGN_CENTER, 0, -118);
    sldBright = make_slider(t, -84, COL_CLAUDE);
    lv_slider_set_range(sldBright, 10, 255);
    lv_slider_set_value(sldBright, settings.brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(sldBright, bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sldBright, bright_cb, LV_EVENT_RELEASED, NULL);

    lblDimLevel = make_label(t, &lv_font_montserrat_20, COL_TEXT);
    lv_obj_align(lblDimLevel, LV_ALIGN_CENTER, 0, -34);
    sldDimLevel = make_slider(t, 0, COL_MUTED);
    lv_slider_set_range(sldDimLevel, 2, 255);
    lv_slider_set_value(sldDimLevel, settings.dimBrightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(sldDimLevel, dim_level_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sldDimLevel, dim_level_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(sldDimLevel, dim_level_cb, LV_EVENT_RELEASED, NULL);

    lblDim = make_label(t, &lv_font_montserrat_20, COL_TEXT);
    lv_obj_align(lblDim, LV_ALIGN_CENTER, 0, 50);
    sldDim = make_slider(t, 84, COL_WEEK);
    lv_slider_set_range(sldDim, 0, N_DIM_STEPS - 1);
    int idx = 0;
    for (int i = 0; i < N_DIM_STEPS; i++)
        if (DIM_STEPS[i] <= settings.dimSecs) idx = i;
    lv_slider_set_value(sldDim, idx, LV_ANIM_OFF);
    lv_obj_add_event_cb(sldDim, dim_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sldDim, dim_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *hint = make_label(t, &lv_font_montserrat_14, COL_MUTED);
    lv_label_set_text(hint, "Idle on USB: dims\nIdle on battery: screen off");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -56);

    // Populate the labels (dim-level first: its handler must not leave the panel dimmed).
    lv_event_send(sldDimLevel, LV_EVENT_VALUE_CHANGED, NULL);
    lv_event_send(sldBright, LV_EVENT_VALUE_CHANGED, NULL);
    lv_event_send(sldDim, LV_EVENT_VALUE_CHANGED, NULL);
}

static void set_trash_label(lv_obj_t *btn, bool armed) {
    lv_obj_t *l = lv_obj_get_child(btn, 0);
    lv_label_set_text(l, armed ? "Delete?" : LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(l, armed ? &lv_font_montserrat_16 : &lv_font_montserrat_28, 0);
    lv_obj_set_style_bg_color(btn, armed ? COL_CRIT : COL_TRACK, 0);
}

static void disarm() {
    if (armedBtn) set_trash_label(armedBtn, false);
    armedBtn = nullptr;
    if (disarmTimer) {
        lv_timer_del(disarmTimer);
        disarmTimer = nullptr;
    }
}

static void disarm_timer_cb(lv_timer_t *) {
    disarmTimer = nullptr;  // one-shot: LVGL deletes it after this returns
    if (armedBtn) set_trash_label(armedBtn, false);
    armedBtn = nullptr;
}

static void forget_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    if (armedBtn != btn) {
        disarm();
        armedBtn = btn;
        set_trash_label(btn, true);
        disarmTimer = lv_timer_create(disarm_timer_cb, 3000, NULL);
        lv_timer_set_repeat_count(disarmTimer, 1);
        return;
    }
    size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    String ssid = idx < wifiSsids.size() ? wifiSsids[idx] : String();
    disarm();
    if (ssid.isEmpty()) return;
    net_forget(ssid);
    ui_flash_message((String("Forgot ") + ssid).c_str());
}

static void rebuild_wifi_list(const NetStatus &n) {
    disarm();
    lv_obj_clean(wifiList);
    wifiSsids = net_saved_ssids();
    if (wifiSsids.empty()) {
        lv_obj_t *l = make_label(wifiList, &lv_font_montserrat_20, COL_MUTED);
        lv_label_set_text(l, "No saved networks");
        return;
    }
    for (size_t i = 0; i < wifiSsids.size(); i++) {
        bool current = n.mode == NET_CONNECTED && wifiSsids[i] == n.ssid;
        lv_obj_t *row = lv_obj_create(wifiList);
        lv_obj_set_size(row, lv_pct(100), 52);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *name = make_label(row, &lv_font_montserrat_20, current ? COL_CLAUDE : COL_TEXT);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, 200);
        lv_label_set_text(name, ((current ? LV_SYMBOL_WIFI " " : "") + wifiSsids[i]).c_str());
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 4, 0);

        lv_obj_t *btn = lv_btn_create(row);
        lv_obj_set_size(btn, 96, 42);
        lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_radius(btn, 21, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_t *bl = make_label(btn, &lv_font_montserrat_16, COL_TEXT);
        lv_obj_center(bl);
        set_trash_label(btn, false);
        lv_obj_add_event_cb(btn, forget_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
}

static void build_wifi(lv_obj_t *t) {
    lv_obj_t *cap = make_label(t, &lv_font_montserrat_16, COL_MUTED);
    lv_label_set_text(cap, "SAVED WI-FI");
    lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 58);

    wifiList = lv_obj_create(t);
    lv_obj_set_size(wifiList, 320, 290);
    lv_obj_align(wifiList, LV_ALIGN_CENTER, 0, 4);
    lv_obj_set_style_bg_opa(wifiList, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wifiList, 0, 0);
    lv_obj_set_style_pad_all(wifiList, 0, 0);
    lv_obj_set_style_pad_row(wifiList, 4, 0);
    lv_obj_set_flex_flow(wifiList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wifiList, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(wifiList, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(wifiList, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *hint = make_label(t, &lv_font_montserrat_14, COL_MUTED);
    lv_label_set_text(hint, "Tap " LV_SYMBOL_CLOSE " twice to forget\nHold BOOT 3s to add a network");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -52);
}

// ---------------------------------------------------------------- Clawd

// Clawd, as drawn in Claude Code's welcome banner. Each banner character is a
// half-block, so one character row = two square pixels here.
static const char *CLAWD[] = {
    ".#########.",
    ".#########.",
    "##.#####.##",   // eyes
    "###########",
    ".#########.",
    ".#########.",
    ".#.#...#.#.",   // legs
    ".#.#...#.#.",
};
static const int CLAWD_W = 11, CLAWD_H = 8;

static void anim_y_cb(void *obj, int32_t v) { lv_obj_set_style_translate_y((lv_obj_t *)obj, v, 0); }
static void anim_h_cb(void *obj, int32_t v) { lv_obj_set_height((lv_obj_t *)obj, v); }

// Builds an animated (hopping, blinking) Clawd with `px`-sized pixels. Caller aligns it.
static lv_obj_t *make_clawd(lv_obj_t *parent, int px) {
    lv_obj_t *sprite = lv_obj_create(parent);
    lv_obj_set_size(sprite, CLAWD_W * px, CLAWD_H * px);
    lv_obj_set_style_bg_opa(sprite, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sprite, 0, 0);
    lv_obj_set_style_pad_all(sprite, 0, 0);
    lv_obj_clear_flag(sprite, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    // One rectangle per horizontal run of pixels keeps the object count low.
    for (int y = 0; y < CLAWD_H; y++) {
        for (int x = 0; x < CLAWD_W;) {
            if (CLAWD[y][x] != '#') { x++; continue; }
            int x0 = x;
            while (x < CLAWD_W && CLAWD[y][x] == '#') x++;
            lv_obj_t *r = lv_obj_create(sprite);
            lv_obj_remove_style_all(r);
            lv_obj_set_style_bg_color(r, COL_CLAUDE, 0);
            lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
            lv_obj_set_pos(r, x0 * px, y * px);
            lv_obj_set_size(r, (x - x0) * px, px);
        }
    }

    // Blink: an orange lid over each eye closes briefly.
    for (int ex : {2, 8}) {
        lv_obj_t *lid = lv_obj_create(sprite);
        lv_obj_remove_style_all(lid);
        lv_obj_set_style_bg_color(lid, COL_CLAUDE, 0);
        lv_obj_set_style_bg_opa(lid, LV_OPA_COVER, 0);
        lv_obj_set_pos(lid, ex * px, 2 * px);
        lv_obj_set_size(lid, px, 0);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, lid);
        lv_anim_set_exec_cb(&a, anim_h_cb);
        lv_anim_set_values(&a, 0, px);
        lv_anim_set_time(&a, 90);
        lv_anim_set_playback_time(&a, 90);
        lv_anim_set_delay(&a, 1400);
        lv_anim_set_repeat_delay(&a, 1800);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }

    // Gentle hop.
    lv_anim_t hop;
    lv_anim_init(&hop);
    lv_anim_set_var(&hop, sprite);
    lv_anim_set_exec_cb(&hop, anim_y_cb);
    lv_anim_set_values(&hop, 0, -(px * 3) / 4);
    lv_anim_set_time(&hop, 260);
    lv_anim_set_playback_time(&hop, 260);
    lv_anim_set_repeat_delay(&hop, 500);
    lv_anim_set_path_cb(&hop, lv_anim_path_ease_out);
    lv_anim_set_repeat_count(&hop, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&hop);
    return sprite;
}

static void build_clock(lv_obj_t *t) {
    lv_obj_align(make_clawd(t, 18), LV_ALIGN_CENTER, 0, -72);

    lblClockTime = make_label(t, &lv_font_montserrat_48, COL_TEXT);
    lv_obj_align(lblClockTime, LV_ALIGN_CENTER, 0, 78);

    lblClockDate = make_label(t, &lv_font_montserrat_20, COL_MUTED);
    lv_obj_align(lblClockDate, LV_ALIGN_CENTER, 0, 126);
}

static void build_setup_layer() {
    setupLayer = lv_obj_create(lv_layer_top());
    lv_obj_set_size(setupLayer, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_style_bg_color(setupLayer, COL_BG, 0);
    lv_obj_set_style_bg_opa(setupLayer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(setupLayer, 0, 0);
    lv_obj_set_style_radius(setupLayer, 0, 0);
    lv_obj_clear_flag(setupLayer, LV_OBJ_FLAG_SCROLLABLE);

    setupTitle = make_label(setupLayer, &lv_font_montserrat_20, COL_CLAUDE);
    lv_obj_align(setupTitle, LV_ALIGN_TOP_MID, 0, 36);

    setupQr = lv_qrcode_create(setupLayer, 170, lv_color_hex(0x000000), lv_color_hex(0xFFFFFF));
    lv_obj_set_style_border_color(setupQr, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(setupQr, 8, 0);
    lv_obj_align(setupQr, LV_ALIGN_CENTER, 0, -40);

    setupBody = make_label(setupLayer, &lv_font_montserrat_16, COL_TEXT);
    lv_label_set_long_mode(setupBody, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(setupBody, 330);
    lv_obj_align(setupBody, LV_ALIGN_CENTER, 0, 118);

    lv_obj_add_flag(setupLayer, LV_OBJ_FLAG_HIDDEN);
}

void ui_init() {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_text_color(scr, COL_TEXT, 0);

    tv = lv_tileview_create(scr);
    lv_obj_set_style_bg_color(tv, COL_BG, 0);
    lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);
    // Clock sits left of Usage; the device starts on Usage.
    lv_obj_t *tClock = lv_tileview_add_tile(tv, 0, 0, LV_DIR_RIGHT);
    lv_obj_t *t0 = lv_tileview_add_tile(tv, 1, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    lv_obj_t *t1 = lv_tileview_add_tile(tv, 2, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    lv_obj_t *t2 = lv_tileview_add_tile(tv, 3, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    lv_obj_t *t3 = lv_tileview_add_tile(tv, 4, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    lv_obj_t *t4 = lv_tileview_add_tile(tv, 5, 0, LV_DIR_LEFT);
    build_clock(tClock);
    build_usage(t0);
    build_settings(t1);
    build_wifi(t2);
    build_battery(t3);
    build_info(t4);
    lv_obj_set_tile_id(tv, 1, 0, LV_ANIM_OFF);

    build_setup_layer();

    toast = make_label(lv_layer_top(), &lv_font_montserrat_20, COL_TEXT);
    lv_obj_set_style_bg_color(toast, lv_color_hex(0x2A2724), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(toast, 18, 0);
    lv_obj_set_style_pad_ver(toast, 10, 0);
    lv_obj_set_style_radius(toast, 22, 0);
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -112);
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------- update

static void set_qr(const String &data) {
    if (data == lastQr) return;
    lastQr = data;
    lv_qrcode_update(setupQr, data.c_str(), data.length());
}

static void update_setup(const UsageSnapshot &u, const NetStatus &n) {
    bool noToken = settings_get_token().isEmpty();
    if (n.mode == NET_AP_PORTAL) {
        lv_label_set_text(setupTitle, "SETUP  \xE2\x80\xA2  STEP 1");
        set_qr("WIFI:T:nopass;S:" + n.apName + ";;");
        lv_label_set_text_fmt(setupBody,
            "Scan or join Wi-Fi\n#D97757 %s#\nthen pick your network and\npaste your Claude token.",
            n.apName.c_str());
    } else if (n.mode == NET_CONNECTED && noToken && n.webPortal) {
        lv_label_set_text(setupTitle, "SETUP  \xE2\x80\xA2  STEP 2");
        set_qr("http://" + n.ip + "/param");
        lv_label_set_text_fmt(setupBody,
            "On your Mac run\n#D97757 claude setup-token#\nthen paste it at\nhttp://%s/param",
            n.ip.c_str());
    } else {
        lv_obj_add_flag(setupLayer, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_recolor(setupBody, true);
    lv_obj_clear_flag(setupLayer, LV_OBJ_FLAG_HIDDEN);
}

static void update_window_labels(const UsageWindow &w, lv_obj_t *arc, lv_color_t base,
                                 lv_obj_t *pctLbl, lv_obj_t *resetLbl, bool weekly, time_t now) {
    if (!w.valid) {
        lv_arc_set_value(arc, 0);
        lv_label_set_text(pctLbl, weekly ? "Week  --" : "--");
        lv_label_set_text(resetLbl, "");
        return;
    }
    lv_arc_set_value(arc, (int)(constrain(w.pct, 0.0f, 100.0f) * 10));
    lv_obj_set_style_arc_color(arc, level_color(w.pct, base), LV_PART_INDICATOR);

    if (weekly) lv_label_set_text_fmt(pctLbl, "Week  %d%%", (int)roundf(w.pct));
    else lv_label_set_text_fmt(pctLbl, "%d%%", (int)roundf(w.pct));

    if (w.resetsAt > 0) {
        String s = "resets in " + fmt_duration(w.resetsAt - now);
        if (weekly) s = "resets " + fmt_local(w.resetsAt, "%a %H:%M");
        lv_label_set_text(resetLbl, s.c_str());
    } else {
        lv_label_set_text(resetLbl, "");
    }
}

static void update_usage(const UsageSnapshot &u, const BatteryInfo &b, const NetStatus &n) {
    time_t now = time(nullptr);
    bool clockOk = now > 1700000000;

    // Status bar: clock \xE2\x80\xA2 wifi \xE2\x80\xA2 battery
    String bar;
    if (clockOk) bar += fmt_local(now, "%H:%M") + "   ";
    bar += n.mode == NET_CONNECTED ? LV_SYMBOL_WIFI "   " : "";
    bar += battery_symbol(b);
    if (b.percent >= 0) bar += " " + String(b.percent) + "%";
    lv_label_set_text(lblStatusBar, bar.c_str());

    update_window_labels(u.session, arcSession, COL_CLAUDE, lblSessionPct, lblSessionReset, false, now);
    update_window_labels(u.weekly, arcWeek, COL_WEEK, lblWeekPct, lblWeekReset, true, now);

    // Footer: freshness or what's wrong.
    String foot;
    if (n.mode == NET_CONNECTING) foot = "Connecting to Wi-Fi...";
    else if (n.mode == NET_AP_PORTAL) foot = "Setup hotspot on";
    else if (settings_get_token().isEmpty()) foot = "No Claude token";
    else if (u.state == FETCH_AUTH_ERROR) foot = "Token rejected\nhold BOOT to re-enter";
    else if (!clockOk) foot = "Syncing clock...";
    else if (u.state == FETCH_IDLE) foot = "Fetching usage...";
    else if (u.state == FETCH_NET_ERROR) foot = "Network error, retrying";
    else if (u.state == FETCH_HTTP_ERROR) foot = "API error " + String(u.httpCode) + ", retrying";
    else if (strcmp(u.overallStatus, "rejected") == 0) foot = "Limit reached";
    else if (u.fetchedAt) {
        long ago = now - u.fetchedAt;
        foot = ago < 60 ? String("Updated just now") : "Updated " + fmt_duration(ago) + " ago";
        if (u.overage.valid && u.overage.pct > 0) foot += "\nExtra usage " + String((int)roundf(u.overage.pct)) + "%";
    }
    lv_label_set_text(lblFooter, foot.c_str());
}

static void update_battery(const BatteryInfo &b) {
    if (!b.present) {
        lv_arc_set_value(arcBatt, 0);
        lv_label_set_text(lblBattPct, "--");
        lv_label_set_text(lblBattState, b.vbus ? "USB powered, no battery" : "No battery");
    } else {
        lv_arc_set_value(arcBatt, b.percent);
        lv_color_t c = b.charging ? COL_CLAUDE : b.percent <= 15 ? COL_CRIT : b.percent <= 30 ? COL_WARN : COL_OK;
        lv_obj_set_style_arc_color(arcBatt, c, LV_PART_INDICATOR);
        lv_label_set_text_fmt(lblBattPct, "%d%%", b.percent);
        String st = b.charging ? String(LV_SYMBOL_CHARGE " Charging \xE2\x80\xA2 ") + b.chargeState
                  : b.full && b.vbus ? String("Full \xE2\x80\xA2 on USB")
                  : b.vbus ? String("On USB \xE2\x80\xA2 ") + b.chargeState
                  : String("On battery");
        lv_label_set_text(lblBattState, st.c_str());
    }
    char d[160];
    snprintf(d, sizeof(d), "Battery   %.2f V\nUSB   %s\nSystem   %.2f V\nPMU   %.0f °C",
             b.battMv / 1000.0f, b.vbus ? String(b.vbusMv / 1000.0f, 2).c_str() : "--",
             b.sysMv / 1000.0f, b.pmuTempC);
    // String temporaries above live until the end of the full expression, so %s is safe.
    lv_label_set_text(lblBattDetail, d);
}

static void update_info(const NetStatus &n) {
    uint32_t up = millis() / 1000;
    String s;
    if (n.mode == NET_CONNECTED) {
        const char *q = n.rssi > -55 ? "excellent" : n.rssi > -67 ? "good" : n.rssi > -78 ? "fair" : "weak";
        s += LV_SYMBOL_WIFI " " + n.ssid + "\n";
        s += String(n.rssi) + " dBm (" + q + ")\n";
        s += n.ip + "\n";
    } else if (n.mode == NET_AP_PORTAL) {
        s += "Hotspot: " + n.apName + "\n";
    } else {
        s += "Wi-Fi connecting...\n";
    }
    s += "Saved Wi-Fi networks: " + String(n.saved) + "\n";
    s += "Refresh every " + String(settings.pollMins) + " min\n";
    s += "Up " + fmt_duration(up) + "\n";
    s += "Heap " + String(ESP.getFreeHeap() / 1024) + " KB \xE2\x80\xA2 PSRAM " + String(ESP.getFreePsram() / 1024) + " KB\n";
    s += "Firmware " FW_VERSION;
    lv_label_set_text(lblInfo, s.c_str());
}

void ui_update(const UsageSnapshot &u, const BatteryInfo &b, const NetStatus &n) {
    update_setup(u, n);
    update_usage(u, b, n);
    update_battery(b);
    update_info(n);

    time_t now = time(nullptr);
    if (now > 1700000000) {
        lv_label_set_text(lblClockTime, fmt_local(now, "%H:%M").c_str());
        lv_label_set_text(lblClockDate, fmt_local(now, "%A %e %B").c_str());
    } else {
        lv_label_set_text(lblClockTime, "--:--");
        lv_label_set_text(lblClockDate, "Waiting for time");
    }

    String current = n.mode == NET_CONNECTED ? n.ssid : String();
    if (net_saved_version() != wifiListVersion || current != wifiListCurrent) {
        wifiListVersion = net_saved_version();
        wifiListCurrent = current;
        rebuild_wifi_list(n);
    }
}

static void toast_hide_cb(lv_timer_t *t) {
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_del(t);
}

void ui_flash_message(const char *msg) {
    lv_label_set_text(toast, msg);
    lv_obj_clear_flag(toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(toast);
    lv_timer_create(toast_hide_cb, 1800, NULL);
}

void ui_show_power_off() {
    lv_obj_t *o = lv_obj_create(lv_layer_top());
    lv_obj_set_size(o, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_style_bg_color(o, COL_BG, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_t *l = make_label(o, &lv_font_montserrat_28, COL_CLAUDE);
    lv_label_set_text(l, LV_SYMBOL_POWER "\n\nPowering off");
    lv_obj_center(l);
    lv_refr_now(NULL);
}


// ---------------------------------------------------------------- hourly Clawd

static lv_obj_t *hourly = nullptr;
static lv_timer_t *hourlyTimer = nullptr;

static void hourly_close() {
    if (hourlyTimer) {
        lv_timer_del(hourlyTimer);
        hourlyTimer = nullptr;
    }
    if (hourly) {
        lv_obj_del_async(hourly);
        hourly = nullptr;
    }
}

static void hourly_timer_cb(lv_timer_t *) {
    hourlyTimer = nullptr;  // one-shot; LVGL deletes it after this returns
    if (hourly) {
        lv_obj_del_async(hourly);
        hourly = nullptr;
    }
}

static void hourly_tap_cb(lv_event_t *) { hourly_close(); }

void ui_show_hourly(time_t now) {
    hourly_close();

    hourly = lv_obj_create(lv_layer_top());
    lv_obj_set_size(hourly, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_style_bg_color(hourly, COL_BG, 0);
    lv_obj_set_style_bg_opa(hourly, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hourly, 0, 0);
    lv_obj_set_style_radius(hourly, 0, 0);
    lv_obj_clear_flag(hourly, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(hourly, hourly_tap_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_align(make_clawd(hourly, 24), LV_ALIGN_CENTER, 0, -60);

    lv_obj_t *clock = make_label(hourly, &lv_font_montserrat_48, COL_TEXT);
    lv_label_set_text(clock, fmt_local(now, "%H:%M").c_str());
    lv_obj_align(clock, LV_ALIGN_CENTER, 0, 100);

    lv_obj_t *date = make_label(hourly, &lv_font_montserrat_20, COL_MUTED);
    lv_label_set_text(date, fmt_local(now, "%A %e %B").c_str());
    lv_obj_align(date, LV_ALIGN_CENTER, 0, 148);

    lv_obj_move_foreground(hourly);
    hourlyTimer = lv_timer_create(hourly_timer_cb, 5000, NULL);
    lv_timer_set_repeat_count(hourlyTimer, 1);
}
