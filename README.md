# Claude Gadget

A desk/pocket gadget that shows your Claude Pro/Max plan usage (5-hour session and weekly limits) on a
[Waveshare ESP32-S3-Touch-AMOLED-1.75C](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75C).
Standalone over Wi-Fi: no computer or phone app needs to stay running.

Inspired by [Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter), which uses the same usage source but relays it from a desktop daemon over BLE.

## Screens (swipe left/right)

It starts on Usage. Swipe right from Usage for the clock; swipe left for the rest.

0. **Clock**: Clawd, the time with seconds, and the date. Clawd hops once per second, in time with the seconds, and blinks.
1. **Usage**: outer ring is the 5-hour session, inner ring is the week. Shows the session reset countdown, the weekly reset day and time. Rings turn amber at 75% and red at 90%. The bottom shows when it last updated, then the clock, Wi-Fi and battery.
2. **Settings**: sliders for normal brightness, dimmed brightness (previewed on the panel while you drag) and auto-dim time (15 s – 30 min, or never). Saved across reboots.
3. **Wi-Fi**: outer ring shows signal strength of the current network. Saved networks are listed, with the connected one highlighted. Tap its ✕ twice to forget one.
4. **Battery**: charge %, charging state (trickle/CC/CV/full), battery, USB and system voltages, and PMU temperature.
5. **Device**: Wi-Fi network, signal, IP, poll interval, uptime, free memory, firmware version.

On the hour, Clawd hops onto the screen with the time and date for 10 s (tap to dismiss). This only happens if the screen is on. To preview it, send `h` over the serial monitor.

At power-on Clawd wakes up and stays on screen until your usage has loaded (at least 4 s; tap to skip). Holding PWR shows Clawd with a ring that fills over 2 s; letting go early cancels. At power-off he closes his eyes and sinks away.

## Controls

| Input | Action |
|---|---|
| PWR tap | Screen on / off |
| PWR hold 2 s | Power off. Press PWR again to power on. The PMU forces a hard off at 6 s if the firmware hangs. |
| BOOT tap | Refresh now |
| BOOT hold 3 s | Open the Wi-Fi / token setup hotspot (closes after 5 idle minutes) |
| Screen tap | Wake the screen when it's off |

Power behaviour:
- **Screen off on battery** (PWR tap, or idle): Wi-Fi switches off completely and polling stops, and the chip light-sleeps, waking every 150 ms to check the buttons. When the screen comes back it shows the last numbers, rejoins Wi-Fi (about 1–3 s) and refreshes.
- **On battery**, the screen turns off after the auto-dim time (default 60 s).
- **On USB**, the screen drops to the dimmed brightness instead of turning off. If you turn it off with PWR, Wi-Fi stays connected and keeps refreshing.
- The device powers itself off after 30 s below 3.3 V.

## Build & flash

```bash
pio run -t upload && pio device monitor
```

## First-time setup

1. On your Mac, run `claude setup-token`. It opens a browser to log in and prints a long-lived token (`sk-ant-oat01-…`).
2. Power on the gadget. It shows a QR code for the **Claude-Gadget-XXXX** hotspot. Join it; the setup page opens (otherwise go to `192.168.4.1`).
3. Choose your Wi-Fi network, paste the token, and check the timezone. The default is UK: `GMT0BST,M3.5.0/1,M10.5.0`.
4. Save. The gadget joins your Wi-Fi and shows usage within a few seconds.

The gadget remembers the last 5 Wi-Fi networks it joined. When it boots, or loses its connection (e.g. you move location), it joins the strongest one in range. If none are in range for 90 s, the setup hotspot opens so you can add another.

To add a network while connected, hold BOOT for 3 s and use the hotspot. Leave the token field blank to keep the saved token; it's stored separately from the Wi-Fi credentials.

If Wi-Fi is already configured but no token is saved, the screen shows a QR code for `http://<device-ip>/param` so you can enter it over your LAN.

For a phone hotspot (iPhone), turn on **Maximize Compatibility**, because the ESP32-S3 only supports 2.4 GHz.

## How usage is read (and caveats)

There's no official public API for Pro/Max plan usage. Every Messages API response carries `anthropic-ratelimit-unified-5h-*` / `-7d-*` headers, so each poll sends the smallest possible request (Haiku, `max_tokens: 1`) and reads those headers. This means:

- Each poll uses a tiny amount of your plan quota. The default poll interval is 5 minutes.
- The approach is unofficial and could break if Anthropic changes those headers.
- The token is stored unencrypted in the ESP32's NVS flash. Anyone with physical access and a USB cable could extract it. To revoke it, go to claude.ai → Settings → Claude Code.

TLS is verified against embedded Google Trust Services and GlobalSign roots ([src/certs.h](src/certs.h)). If `api.anthropic.com` ever moves CA, regenerate that file.

## Layout

| File | Purpose |
|---|---|
| `src/main.cpp` | Setup, main loop, buttons, idle/low-battery handling |
| `src/claude.cpp` | Background poll task (core 0), header parsing |
| `src/net.cpp` | WiFiManager captive portal + settings page, NTP |
| `src/power.cpp` | AXP2101: PWR key IRQs, charging, voltages, shutdown |
| `src/display.cpp` | CO5300 QSPI panel, CST9217 touch, LVGL 8 glue |
| `src/ui.cpp` | LVGL screens |
| `src/settings.cpp` | NVS-backed settings |
