/*
  Lunar Sensor - CodeCell C3 Firmware

  A standalone ambient light sensor for the Lunar macOS app
  (https://lunar.fyi) running on the Microbots CodeCell C3.

  Endpoints:
    GET /sensor/ambient_light  -> JSON lux reading (one-shot)
    GET /events                -> SSE stream of lux readings (every 2 s)

  Discovery:
    Advertised via mDNS as "lunarsensor.local"

  Hardware:
    CodeCell C3 (ESP32-C3) with onboard VCNL4040 light sensor

  Required libraries (Arduino Library Manager):
    - CodeCell  (by Microbots)
    - ArduinoJson  (by Benoit Blanchon, v6.x)
    - ESPAsyncWebServer + AsyncTCP  (ESP32Async forks)

  Board:
    ESP32C3 Dev Module  (Tools -> Board -> ESP32 Arduino)

  Important:
    - Copy secrets.h.template -> secrets.h and fill in your Wi-Fi credentials.
    - The ESP32-C3 only supports 2.4 GHz Wi-Fi.
*/

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <CodeCell.h>

#include "secrets.h"

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
static const char* SENSOR_HOSTNAME = "lunarsensor";
static const uint16_t HTTP_PORT    = 80;
static const uint8_t  SAMPLE_HZ    = 5;      // VCNL4040 sample rate
static const uint16_t SSE_MS       = 2000;   // SSE emit interval
static const uint16_t IDLE_MS      = 50;     // main-loop yield (lets Wi-Fi modem sleep)
static const uint16_t BATT_MS      = 5000;   // battery sample interval

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
CodeCell         myCodeCell;
AsyncWebServer   server(HTTP_PORT);
AsyncEventSource events("/events");
float            g_lux      = 400.0f;

// Battery snapshot, sampled in loop() (ADC + filter state are not safe to
// touch from the async TCP task). Handlers read these, never the CodeCell API.
uint8_t          g_battPct   = 0;
uint16_t         g_battMv    = 0;
uint8_t          g_battState = POWER_INIT;

// ---------------------------------------------------------------------------
// JSON helper
// ---------------------------------------------------------------------------
static String luxJson() {
  StaticJsonDocument<256> doc;
  doc["id"]    = "sensor-ambient_light";
  doc["state"] = String(g_lux, 1) + " lx";
  doc["value"] = g_lux;

  String out;
  serializeJson(doc, out);
  return out;
}

static const char* powerStateName(uint8_t state) {
  switch (state) {
    case POWER_BAT_RUN:  return "battery";
    case POWER_USB:      return "usb";
    case POWER_BAT_LOW:  return "battery_low";
    case POWER_BAT_FULL: return "battery_full";
    case POWER_BAT_CHRG: return "charging";
    default:             return "initializing";
  }
}

static String batteryJson() {
  // BatteryLevelRead() sentinels: 101 = charging, 102 = USB. The percent is
  // only meaningful on battery; power_state carries the context otherwise.
  float pct = (g_battPct > 100) ? 100.0f : (float)g_battPct;

  StaticJsonDocument<256> doc;
  doc["id"]          = "sensor-battery_level";
  doc["state"]       = String(pct, 0) + " %";
  doc["value"]       = pct;
  doc["voltage_mv"]  = g_battMv;
  doc["power_state"] = powerStateName(g_battState);

  String out;
  serializeJson(doc, out);
  return out;
}

// ---------------------------------------------------------------------------
// Sensor update
// ---------------------------------------------------------------------------
static void updateSensor() {
  if (myCodeCell.Run(SAMPLE_HZ)) {
    // VCNL4040 default integration time = 80 ms -> 1 count = 0.1 lux
    uint16_t raw = myCodeCell.Light_AmbientRead();
    g_lux = raw * 0.1f;
  }
}

// CodeCell's Light_Init() enables the VCNL4040 proximity channel with its IR
// emitter at 200 mA / 1/40 duty (~5 mA average) — unused here, so shut it
// down. ALS_CONF is a separate register; the ambient-light channel keeps
// running. Must not be called after setup(): I2C stays off-limits to the
// async TCP task, and LED brightness must stay 0 or LED_Breathing() would
// poll the now-dormant proximity register.
static void disableProximity() {
  Wire.beginTransmission(VCNL4040_ADDRESS);
  Wire.write(VCNL4040_PS_CONF1_REG);
  Wire.write(0x01);  // PS_CONF1: PS_SD=1 (proximity shutdown)
  Wire.write(0x00);  // PS_CONF2: defaults
  Wire.endTransmission();
}

static void updateBattery() {
  static unsigned long lastSample = 0;
  if (millis() - lastSample < BATT_MS && lastSample != 0) return;
  lastSample = millis();

  g_battMv    = myCodeCell.BatteryVoltageRead();
  g_battPct   = myCodeCell.BatteryLevelRead();
  uint8_t state = myCodeCell.PowerStateRead();

  if (state == POWER_BAT_LOW && g_battState != POWER_BAT_LOW) {
    Serial.printf("[LunarSensor] LOW BATTERY: %u mV\n", g_battMv);
  }
  g_battState = state;
}

// ---------------------------------------------------------------------------
// HTTP handlers
// ---------------------------------------------------------------------------
// Handlers run on the async TCP task: format and send only — no delay(),
// no I2C. Sensor reads stay in loop().
static void setupHttp() {
  server.on("/sensor/ambient_light", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "application/json", luxJson());
  });

  server.on("/sensor/battery_level", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "application/json", batteryJson());
  });

  events.onConnect([](AsyncEventSourceClient* client) {
    Serial.println("[LunarSensor] SSE client connected");
    client->send(luxJson().c_str(), "state", millis());
  });
  server.addHandler(&events);

  server.onNotFound([](AsyncWebServerRequest* req) {
    req->send(404, "text/plain", "Not Found");
  });

  server.begin();
}

// ---------------------------------------------------------------------------
// Wi-Fi reconnect helper
// ---------------------------------------------------------------------------
static void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.println("[LunarSensor] Wi-Fi lost, reconnecting...");
  WiFi.reconnect();

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(500);
    updateSensor();
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[LunarSensor] Wi-Fi restored, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("[LunarSensor] Wi-Fi reconnect failed, will retry");
  }
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  // On ESP32-C3 USB-CDC, wait up to 3s for a monitor to connect.
  // Times out automatically so the device still boots headlessly.
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) { delay(10); }

  Serial.println("\n[LunarSensor] Booting...");

  // 80 MHz is plenty for a 5 Hz sensor + tiny HTTP server and roughly
  // halves active CPU power vs the 160 MHz default.
  setCpuFrequencyMhz(80);

  // Initialise light sensor
  myCodeCell.Init(LIGHT);
  myCodeCell.LED_SetBrightness(0);          // silence breathing LED
  disableProximity();
  Serial.println("[LunarSensor] VCNL4040 light sensor ready (proximity off)");

  // Connect to Wi-Fi. Modem power-save (DTIM sleep) keeps the radio off
  // between beacons whenever the CPU idles in delay().
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.setSleep(true);
  Serial.print("[LunarSensor] Joining Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("[LunarSensor] IP address: ");
  Serial.println(WiFi.localIP());

  // mDNS advertisement
  if (MDNS.begin(SENSOR_HOSTNAME)) {
    Serial.print("[LunarSensor] mDNS: ");
    Serial.print(SENSOR_HOSTNAME);
    Serial.println(".local");
  } else {
    Serial.println("[LunarSensor] mDNS init failed");
  }

  // HTTP routes (async server; handlers run on the TCP task)
  setupHttp();

  Serial.println("[LunarSensor] Ready — waiting for Lunar app");
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
void loop() {
  static unsigned long lastSend = 0;

  updateSensor();
  updateBattery();

  if (millis() - lastSend >= SSE_MS) {
    // Emitting with no clients connected is a no-op.
    events.send(luxJson().c_str(), "state", millis());
    lastSend = millis();
  }

  maintainWiFi();
  delay(IDLE_MS);   // idle so modem sleep can engage; adds ≤50 ms request latency
}
