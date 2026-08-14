# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Arduino firmware for the Microbots CodeCell C3 (ESP32-C3, onboard VCNL4040 light sensor) that acts as an external ambient light sensor for the Lunar macOS app. There is one sketch: `src/lunarsensor.ino`. `RESEARCH_AND_PLAN.md` documents the reverse-engineered Lunar protocol and the roadmap (OTA, captive portal, calibration are planned but unimplemented).

## Commands

```bash
pio run                    # compile
pio run --target upload    # compile + flash over USB-C
pio device monitor         # serial monitor, 115200 baud
```

There are no tests or linters. Verification is manual: watch the serial boot log, then from a Mac on the same network:

```bash
curl lunarsensor.local/sensor/ambient_light   # one-shot JSON reading
curl -N lunarsensor.local/events              # SSE stream, one event per 2 s
```

Building requires `src/secrets.h` (gitignored) — copy `src/secrets.h.template` and fill in 2.4 GHz Wi-Fi credentials.

## Protocol contract (do not break)

Lunar discovers the device via mDNS as `lunarsensor.local` on port 80 and expects exactly:

- `GET /sensor/ambient_light` → `{"id":"sensor-ambient_light","state":"42.0 lx","value":42.0}` — `value` is a float in lux, `state` ends in `" lx"`.
- `GET /events` → SSE stream emitting `event: state` + the same JSON as `data:` every 2 s.

Changing the hostname, port, paths, JSON field names, or event framing breaks Lunar pairing.

## Architecture notes

- The HTTP server is `ESPAsyncWebServer` (ESP32Async fork) with an `AsyncEventSource` for `/events`; multiple clients are supported concurrently. HTTP handlers run on the **async TCP task**, not in `loop()` — they must only format and send (no `delay()`, no I2C, no long work), or the watchdog resets the device. Sensor reads and the 2 s SSE emit both live in `loop()`. Note `AsyncEventSource` only claims requests carrying `Accept: text/event-stream` — a plain `curl /events` gets a 404; add `-H "Accept: text/event-stream"`.
- Sensor reads go through `myCodeCell.Run(SAMPLE_HZ)`, which gates timing internally and returns true when a sample interval elapsed — call it frequently, don't add your own timing around it. Lux conversion is `raw * 0.1f` (VCNL4040 at 80 ms integration time).
- Power management is deliberate and easy to regress: CPU pinned to 80 MHz, `WiFi.setSleep(true)`, and `delay()` yields in both `loop()` (50 ms) and the SSE loop (100 ms). The delays are what let Wi-Fi modem sleep engage — do not remove them or add busy-wait loops; battery runtime depends on it.

## Other agents

Devin CLI is also installed and active on this system and may work in this repo. Its local config lives in `.devin/` (gitignored) — don't modify or delete it, and be aware the working tree may change between sessions from its activity.

## Toolchain

`platformio.ini` pins the **pioarduino** platform release (Arduino core 3.2.0) rather than stock `espressif32`, because stock ships x86_64-only compilers and this project is developed on Apple Silicon without Rosetta. Keep platform bumps within pioarduino releases.
