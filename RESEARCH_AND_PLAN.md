# Lunar External Light Sensor with CodeCell C3

## Research Findings

### 1. Lunar Sensor Protocol

Lunar (v4.7.0+) discovers and communicates with external ambient light sensors over the local network using a lightweight HTTP + Server-Sent Events (SSE) protocol.

#### Required Endpoints

| Method | Path | Response Format |
|--------|------|-----------------|
| `GET` | `/sensor/ambient_light` | JSON: `{"id":"sensor-ambient_light", "state":"400.0 lx", "value":400.0}` |
| `GET` | `/events` | SSE stream: `event: state` + `data: {"id":"sensor-ambient_light", "state":"400.0 lx", "value":400.0}` |

- **`value`** must be a **float** representing lux.
- **`state`** is a formatted string ending in `" lx"`.
- The SSE stream should emit a new event every **2 seconds** while the client is connected.
- One-shot readings are served from `/sensor/ambient_light`.

#### Discovery

- Lunar auto-discovers sensors via **mDNS/Bonjour** using the hostname **`lunarsensor.local`**.
- If the sensor is on a non-default hostname or port, configure Lunar from macOS:
  ```bash
  defaults write fyi.lunar.Lunar sensorHostname <hostname>
  defaults write fyi.lunar.Lunar sensorPort <port>
  ```
- Default port is **80**.

#### Reference Implementation

The official Python server (`alin23/lunarsensor`) runs on FastAPI and uses `sse_starlette` for the EventSource response. The core logic is:

```python
async def make_lux_response():
    global last_lux
    try:
        lux = await read_lux()
    except Exception as exc:
        log.exception(exc)
    else:
        if lux is not None and lux != last_lux:
            last_lux = lux
    return {"id": "sensor-ambient_light", "state": f"{last_lux} lx", "value": last_lux}
```

Source: <https://github.com/alin23/lunarsensor>

---

### 2. CodeCell C3 Hardware

| Spec | Value |
|------|-------|
| MCU | ESP32-C3 (32-bit RISC-V, single-core, 160 MHz) |
| Flash | 4 MB |
| SRAM | 400 KB |
| Wireless | Wi-Fi 4 (2.4 GHz only) + BLE 5 |
| USB | USB-C (Serial UART + LiPo charging) |
| Dimensions | 18.5 x 18.5 x 9.4 mm |
| Weight | 3.4 g |

#### Onboard Sensors

| Sensor | Type | I2C Address | Purpose |
|--------|------|-------------|---------|
| **VCNL4040** | Ambient Light + Proximity + White Light | `0x60` | Light sensing |
| **BNO085** | 9-axis IMU (optional on C3) | `0x4A` | Motion sensing |

The VCNL4040 is the key sensor for this project. It provides:
- **Ambient Light** (`Light_AmbientRead()`) — measures total brightness from all sources
- **White Light** (`Light_WhiteRead()`) — focuses on daylight/LED sources
- **Proximity** (`Light_ProximityRead()`) — IR-based proximity up to 20 cm

#### VCNL4040 Lux Conversion

The CodeCell library configures the VCNL4040 in `Light_Init()` with:
- Continuous conversion mode
- High dynamic range
- **Integration time: 80 ms** (ALS_CONF = `0x000`)

Per the VCNL4040 datasheet and community libraries, at 80 ms integration time:
- **Resolution: 0.1 lux per count**
- **Max range: ~6553 lux** (16-bit output)

Therefore:
```cpp
float lux = myCodeCell.Light_AmbientRead() * 0.1f;
```

The `PrintSensors()` method in the CodeCell library labels these as "lx", confirming the library treats raw counts as already-scaled lux values at the default integration time.

#### CodeCell Library API

```cpp
#include <CodeCell.h>

CodeCell myCodeCell;

void setup() {
    myCodeCell.Init(LIGHT);  // Enable light/proximity sensor
}

void loop() {
    if (myCodeCell.Run(10)) {  // Run at 10 Hz
        uint16_t ambient_raw = myCodeCell.Light_AmbientRead();   // uint16_t counts
        uint16_t white_raw   = myCodeCell.Light_WhiteRead();     // uint16_t counts
        uint16_t prox_raw    = myCodeCell.Light_ProximityRead();   // uint16_t counts
    }
}
```

- `Run(frequency)` manages timing internally using a hardware timer. Returns `true` when the sampling interval has elapsed.
- `Light_*Read()` functions return the most recently cached value from the last `Light_Read()` call (triggered inside `Run()`).

---

### 3. Arduino ESP32-C3 HTTP Server + mDNS Stack

For the CodeCell C3 (ESP32-C3), the standard Arduino-compatible libraries are:

| Library | Purpose | Notes |
|---------|---------|-------|
| `WiFi.h` | Wi-Fi connection | Standard ESP32 Arduino core |
| `ESPmDNS.h` | mDNS advertisement | Advertise `lunarsensor.local` |
| `WebServer.h` | Synchronous HTTP server | Built into ESP32 Arduino core; simpler than async |
| `ArduinoJson` | JSON serialization | Lightweight and widely used |

Important constraint: **ESP32-C3 only supports 2.4 GHz Wi-Fi**. Do not attempt to connect to a 5 GHz network.

---

## Implementation Plan

### Phase 1: Proof-of-Concept Firmware

Create a standalone Arduino sketch (`lunarsensor.ino`) that:

1. **Connects to Wi-Fi**
   - Hardcode SSID/password for initial testing, or
   - Use WiFiManager (captive portal) for easier configuration

2. **Initializes the CodeCell light sensor**
   - `myCodeCell.Init(LIGHT)`
   - Disable LED breathing animation to reduce power/noise: `myCodeCell.LED_SetBrightness(0)`

3. **Reads ambient light continuously**
   - Use `myCodeCell.Run(5)` (5 Hz = every 200 ms)
   - Convert raw counts to lux: `lux = Light_AmbientRead() * 0.1f`
   - Store in a global variable protected by simple atomic access (single-core MCU)

4. **Runs an HTTP server**
   - `GET /sensor/ambient_light` → returns JSON lux reading
   - `GET /events` → SSE stream emitting lux every 2 seconds
   - Use `ArduinoJson` to build the JSON payload

5. **Advertises via mDNS**
   - `MDNS.begin("lunarsensor")` → resolves as `lunarsensor.local`
   - Add service advertisement if needed for discovery robustness

6. **Provides Serial debug output**
   - `Serial.begin(115200)`
   - Print IP address, connection status, and current lux values

### Phase 2: Hardening & Calibration

1. **Sensor calibration**
   - Compare CodeCell lux readings against a known lux meter or the TSL2591-based Lunar sensor.
   - The VCNL4040 has ±10% accuracy per datasheet. If readings are consistently offset, apply a scalar correction factor.
   - Consider using **White Light** (`Light_WhiteRead()`) instead of Ambient if the spectral response better matches typical monitor-room lighting (LED/office lights).

2. **Connection resilience**
   - Auto-reconnect Wi-Fi if the connection drops.
   - Restart mDNS if needed.
   - Consider watchdog timer to reset the device if it hangs.

3. **Power management**
   - The CodeCell supports LiPo battery operation with charging.
   - If running on battery, consider using `SleepTimer()` to deep-sleep between readings and wake every 2 seconds to send an SSE update. Note: SSE requires a persistent TCP connection, so deep sleep is only viable if Lunar falls back to polling `/sensor/ambient_light`.
   - Lunar supports **Auto Mode** which falls back to other modes if the sensor is unavailable, so intermittent sensor availability is acceptable.

4. **OTA updates**
   - The Lunar firmware installer supports updating over-the-air using `lunarsensor.local`.
   - For our custom firmware, we can add ArduinoOTA or ESP32 HTTPUpdate support so the device can be reflashed without USB after initial programming.

### Phase 3: Optional Integrations

1. **WiFiManager captive portal**
   - On first boot (or if Wi-Fi credentials are not saved), start an AP named `LunarSensor-Setup`.
   - Serve a simple web page to configure SSID, password, and optional hostname.
   - Store config in ESP32 NVS (non-volatile storage).

2. **Configuration endpoint**
   - `POST /config` to set lux scaling factor, sensor mode (ambient vs white), polling rate.
   - Persist settings to NVS.

3. **Home Assistant compatibility**
   - The official `lunarsensor` Python server doubles as a Home Assistant add-on.
   - Our ESP32 firmware could also expose a simple REST API or MQTT topic for HA, but this is out of scope for the primary Lunar integration.

---

## File Structure (Proposed)

```
/Users/mccoole/work/lunar-sensor/
├── RESEARCH_AND_PLAN.md      # This document
├── firmware/
│   ├── lunarsensor.ino       # Main Arduino sketch
│   ├── secrets.h               # Wi-Fi credentials (gitignored)
│   └── README.md             # Build/flash instructions
└── extras/
    └── calibration_notes.md
```

---

## Open Questions / Risks

| Risk | Mitigation |
|------|------------|
| VCNL4040 lux accuracy vs TSL2591 | Calibrate against known reference; apply correction factor |
| mDNS reliability on ESP32-C3 | Test on target network; fallback to static IP or IP-based config |
| SSE client disconnects | Ensure `events` endpoint handles client drops gracefully (use `client.connected()` checks) |
| Wi-Fi only 2.4 GHz | Ensure the target network has a 2.4 GHz band |
| CodeCell library conflicts with WiFi/WebServer | The library uses `Wire` on GPIO 8/9 and a hardware timer; no known conflicts with standard Arduino HTTP server |

---

## Next Steps

1. **Write the PoC Arduino sketch** (`lunarsensor.ino`) using `WebServer` + `ESPmDNS` + `CodeCell.h`.
2. **Flash to CodeCell C3** via Arduino IDE or PlatformIO.
3. **Test endpoints** with `curl`:
   ```bash
   curl lunarsensor.local/sensor/ambient_light
   curl -N lunarsensor.local/events
   ```
4. **Pair with Lunar app** — verify Sensor Mode appears and lux values track correctly.
5. **Calibrate** against a reference light meter or the official TSL2591 sensor.
