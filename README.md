# Claude Gadget

A desk/pocket gadget that shows your Claude Pro/Max plan usage (5-hour session and weekly limits) on a
[Waveshare ESP32-S3-Touch-AMOLED-1.75C](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75C).
Standalone over Wi-Fi: no computer or phone app needs to stay running.

Inspired by [Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter), which uses the same usage source but relays it from a desktop daemon over BLE.

## Screens (swipe left/right)

1. **Usage**: outer ring is the 5-hour session, inner ring is the week. Shows the session reset countdown, the weekly reset day and time. Rings turn amber at 75% and red at 90%. The bottom shows when it last updated, then the clock, Wi-Fi and battery.
2. **Battery**: charge %, charging state (trickle/CC/CV/full), battery, USB and system voltages, and PMU temperature.
3. **Device**: Wi-Fi network, signal, IP, poll interval, uptime, free memory, firmware version.
4. **Settings**: brightness and auto-dim time sliders (15 s – 30 min, or never). Saved across reboots.

## Controls

| Input | Action |
|---|---|
| PWR tap | Screen on / off |
| PWR hold 2 s | Power off. Press PWR again to power on. The PMU forces a hard off at 6 s if the firmware hangs. |
| BOOT tap | Refresh now |
| BOOT hold 3 s | Open the Wi-Fi / token setup hotspot (closes after 5 idle minutes) |
| Screen tap | Wake the screen when it's off |

Power behaviour:
- **On battery**, the screen turns off after the auto-dim time (default 60 s) and usage polls slow to 3× the normal interval.
- **On USB**, the screen dims instead of turning off.
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
