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

  Board:
    ESP32C3 Dev Module  (Tools -> Board -> ESP32 Arduino)

  Important:
    - Copy secrets.h.template -> secrets.h and fill in your Wi-Fi credentials.
    - The ESP32-C3 only supports 2.4 GHz Wi-Fi.
*/

#include <WiFi.h>
#include <WebServer.h>
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
static const uint16_t SSE_IDLE_MS  = 100;    // SSE-loop yield between client checks

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
CodeCell    myCodeCell;
WebServer   server(HTTP_PORT);
float       g_lux         = 400.0f;
bool        g_sseActive   = false;
uint16_t    g_lastProx    = 0;           // kept for future use

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

// ---------------------------------------------------------------------------
// Sensor update
// ---------------------------------------------------------------------------
static void updateSensor() {
  if (myCodeCell.Run(SAMPLE_HZ)) {
    // VCNL4040 default integration time = 80 ms -> 1 count = 0.1 lux
    uint16_t raw = myCodeCell.Light_AmbientRead();
    g_lux = raw * 0.1f;
    g_lastProx = myCodeCell.Light_ProximityRead();
  }
}

// ---------------------------------------------------------------------------
// HTTP handlers
// ---------------------------------------------------------------------------
static void handleAmbientLight() {
  server.send(200, "application/json", luxJson());
}

static void handleEvents() {
  g_sseActive = true;

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/event-stream", "");

  unsigned long lastSend = 0;

  while (server.client().connected()) {
    updateSensor();

    if (millis() - lastSend >= SSE_MS) {
      server.sendContent("event: state\ndata: " + luxJson() + "\n\n");
      lastSend = millis();
    }
    delay(SSE_IDLE_MS);   // yield to Wi-Fi stack; emit interval is 2 s, no need to spin
  }

  g_sseActive = false;
}

static void handleNotFound() {
  server.send(404, "text/plain", "Not Found");
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
  Serial.println("[LunarSensor] VCNL4040 light sensor ready");

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

  // HTTP routes
  server.on("/sensor/ambient_light", HTTP_GET, handleAmbientLight);
  server.on("/events",               HTTP_GET, handleEvents);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println("[LunarSensor] Ready — waiting for Lunar app");
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
void loop() {
  if (!g_sseActive) {
    updateSensor();
    server.handleClient();
    maintainWiFi();
    delay(IDLE_MS);   // idle so modem sleep can engage; adds ≤50 ms request latency
  }
}
