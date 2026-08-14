# Lunar Sensor - CodeCell C3 Firmware

Firmware that turns a [Microbots CodeCell C3](https://microbots.io/products/codecell) into an external ambient light sensor for the [Lunar](https://lunar.fyi) macOS app.

## Required Hardware

- CodeCell C3 (ESP32-C3, onboard VCNL4040 light sensor)
- USB-C data cable
- macOS computer with Lunar v4.7.0+
- 2.4 GHz Wi-Fi network

## Required Software

You can build and flash this project using either **PlatformIO** (recommended) or the **Arduino IDE**.

### Option A: PlatformIO (recommended)

- [PlatformIO Core](https://platformio.org/install) (CLI) or [PlatformIO IDE](https://platformio.org/install/ide) (VS Code extension)
- `platformio.ini` at the project root already configures everything

> **Note:** `platformio.ini` pins the [pioarduino](https://github.com/pioarduino/platform-espressif32) platform release instead of stock `espressif32`. This is deliberate: the stock platform ships x86_64-only compilers, which fail with `Bad CPU type in executable` on Apple Silicon Macs without Rosetta. pioarduino bundles the same Arduino core (3.2.0) with native arm64 toolchains — don't switch the `platform =` line back to `espressif32`.

### Option B: Arduino IDE

- [Arduino IDE](https://www.arduino.cc/en/software) 1.8.x or 2.x
- ESP32 Arduino Core (Boards Manager)
- Libraries (Library Manager):
  - **CodeCell** by Microbots
  - **ArduinoJson** by Benoit Blanchon (v6.x)

## Project Layout

```
lunar-sensor/
├── platformio.ini          # PlatformIO project config
├── src/
│   ├── lunarsensor.ino     # Main firmware sketch
│   ├── secrets.h.template  # Wi-Fi credentials template
│   └── secrets.h           # Your Wi-Fi credentials (gitignored; copy from template)
├── .gitignore
└── README.md
```

## Wiring / Assembly

No wiring required. The VCNL4040 light sensor is already on the CodeCell C3 board.

## Configuration

Copy `src/secrets.h.template` to `src/secrets.h` and fill in your Wi-Fi credentials:

```cpp
#define WIFI_SSID     "MyHomeNetwork"
#define WIFI_PASSWORD "MyPassword"
```

> **Note:** The ESP32-C3 only supports **2.4 GHz** Wi-Fi. Ensure your Mac and the CodeCell are on the same network.

## Building & Flashing (PlatformIO)

1. Install PlatformIO (see above).
2. Open a terminal in the project root (`lunar-sensor/`).
3. Build and flash in one step:
   ```bash
   pio run --target upload
   ```
4. Open the serial monitor to watch the boot log:
   ```bash
   pio device monitor
   ```

PlatformIO will automatically download the ESP32 platform, the `CodeCell` library, and `ArduinoJson` on first build.

## Building & Flashing (Arduino IDE)

### Installing Board Support

1. Open Arduino IDE → **Preferences**.
2. In **Additional Boards Manager URLs**, add:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Open **Tools → Board → Boards Manager...**, search **esp32**, and install **ESP32 by Espressif Systems**.

### Installing Libraries

1. **Sketch → Include Library → Manage Libraries...**
2. Search and install:
   - `CodeCell` (latest)
   - `ArduinoJson` (v6.x — e.g. 6.21.5)

### Upload

1. Connect the CodeCell C3 to your Mac via USB-C.
2. In Arduino IDE, select:
   - **Board:** `ESP32C3 Dev Module`
   - **Port:** The serial port that appears (e.g. `/dev/cu.usbserial-*` or `/dev/cu.usbmodem*`)
3. Open `src/lunarsensor.ino`.
4. Click **Upload**.

The CodeCell C3 does not need manual bootloader entry for Arduino IDE uploads — it enters boot mode automatically via the serial interface.

## Verification

After flashing, open the serial monitor (115200 baud) and watch for:
```
[LunarSensor] Booting...
[LunarSensor] VCNL4040 light sensor ready
[LunarSensor] IP address: 192.168.x.x
[LunarSensor] mDNS: lunarsensor.local
[LunarSensor] Ready — waiting for Lunar app
```

Test the endpoints from your Mac:
```bash
# One-shot lux reading
curl lunarsensor.local/sensor/ambient_light

# SSE stream (press Ctrl+C to stop)
curl -N lunarsensor.local/events
```

Expected output:
```json
{"id":"sensor-ambient_light","state":"42.0 lx","value":42.0}
```

## Pairing with Lunar

1. Open the **Lunar** app on your Mac.
2. Look for **Sensor Mode** in the mode dropdown (top-right).
3. If it does not appear automatically, Lunar may need a moment to discover `lunarsensor.local` via mDNS.
4. Click **Sensor Mode** to activate it.
5. Check the **Lunar menu** for the current lux value reported by the CodeCell.

If Lunar cannot find the sensor, you can force the hostname manually in macOS Terminal:
```bash
defaults write fyi.lunar.Lunar sensorHostname lunarsensor.local
defaults write fyi.lunar.Lunar sensorPort 80
```

## Calibration

The VCNL4040 datasheet specifies ±10 % accuracy for ambient light readings. If the reported lux values feel consistently high or low compared to your environment:

- Edit the conversion factor in `src/lunarsensor.ino`:
  ```cpp
  g_lux = raw * 0.1f;   // default: 1 count = 0.1 lux at 80 ms IT
  ```
- You may also experiment with `Light_WhiteRead()` instead of `Light_AmbientRead()` if your room is primarily lit by LED or daylight sources.

## Battery / Power

The firmware is tuned for running on a LiPo battery:

- CPU clocked at 80 MHz (half the ESP32-C3 default)
- Wi-Fi modem sleep enabled (`WiFi.setSleep(true)`) — the radio sleeps between router beacons
- The main loop and SSE loop idle in `delay()` between iterations, which is what allows modem sleep to engage

The trade-off is a small amount of added latency: one-shot HTTP requests may take up to ~50 ms longer, and some routers add a beacon-interval delay when the modem is sleeping. This is imperceptible in normal Lunar use. If you are permanently USB-powered and want minimum latency instead, set `WiFi.setSleep(false)` in `setup()`.

## Troubleshooting

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| Upload fails | Wrong port or board selected | Verify port in Tools → Port; ensure USB-C cable carries data |
| mDNS not resolving | Network does not relay mDNS | Try `curl <ip-address>/sensor/ambient_light` directly; check router settings |
| Lunar shows "Sensor Mode" but no lux | SSE connection not established | Verify `curl lunarsensor.local/events` works; check Serial Monitor for errors |
| Wi-Fi never connects | 5 GHz network or wrong password | Use 2.4 GHz SSID; double-check `secrets.h` |
| LED stays on / blinking | Normal CodeCell behaviour | `LED_SetBrightness(0)` in setup disables breathing; LED may still flash briefly on boot |

## Updating Over-the-Air (Optional)

After the initial USB flash, you can add ArduinoOTA support to reflash wirelessly. This is left as an optional enhancement — the sketch is kept minimal for reliability.
