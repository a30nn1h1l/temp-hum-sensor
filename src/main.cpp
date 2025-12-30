#include "secrets.h"


#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <AHTxx.h>
#include <LittleFS.h>
#include <time.h>
#include <ESPmDNS.h>

// -------- AHT30 / I2C ----------
static const int I2C_SDA = 21;
static const int I2C_SCL = 22;
AHTxx aht(AHTXX_ADDRESS_X38, AHT2x_SENSOR);


// Optional: mDNS name (lets you open http://aht30.local/ on many networks)
static const char* MDNS_NAME = "aht30";

// -------- Logging ----------
static const char* LOG_PATH = "/aht30.csv";

// 1-minute sampling
static const uint32_t SAMPLE_MS = 60UL * 1000UL;

// Prune only every 6 hours (reduces flash rewrites)
static const uint32_t PRUNE_EVERY_MS = 6UL * 60UL * 60UL * 1000UL;

// Keep a rolling 7 days
static const time_t KEEP_SECONDS = 7 * 24 * 3600;

WebServer server(80);

static uint32_t lastSampleMs = 0;
static uint32_t lastPruneMs  = 0;

static time_t nowUtc() { return time(nullptr); }

static bool ensureTime(uint32_t timeoutMs = 8000) {
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    time_t t = nowUtc();
    if (t > 1700000000) return true; // sanity check (~2023+)
    delay(200);
  }
  return false;
}

static void appendReading(time_t ts, float tempC, float hum) {
  File f = LittleFS.open(LOG_PATH, FILE_APPEND);
  if (!f) return;
  // CSV: epoch,tempC,humidity
  f.printf("%ld,%.2f,%.2f\n", (long)ts, tempC, hum);
  f.close();
}

// Copy only records newer than cutoff into a temp file, then swap.
static void pruneOlderThan(time_t cutoffTs) {
  File in = LittleFS.open(LOG_PATH, FILE_READ);
  if (!in) return;

  File out = LittleFS.open("/tmp.csv", FILE_WRITE);
  if (!out) { in.close(); return; }

  while (in.available()) {
    String line = in.readStringUntil('\n');
    line.trim();
    if (line.length() < 5) continue;

    int c1 = line.indexOf(',');
    if (c1 <= 0) continue;

    time_t ts = (time_t) line.substring(0, c1).toInt();
    if (ts >= cutoffTs) out.println(line);
  }

  in.close();
  out.close();

  LittleFS.remove(LOG_PATH);
  LittleFS.rename("/tmp.csv", LOG_PATH);
}

static void maybePrune() {
  // Only prune at PRUNE_EVERY_MS intervals
  if (millis() - lastPruneMs < PRUNE_EVERY_MS) return;
  lastPruneMs = millis();

  time_t t = nowUtc();
  if (t <= 1700000000) return; // don't prune if time isn't valid

  time_t cutoff = t - KEEP_SECONDS;
  pruneOlderThan(cutoff);
}

// -------- HTTP handlers --------
static void handleLatest() {
  File f = LittleFS.open(LOG_PATH, FILE_READ);
  if (!f) {
    server.send(404, "application/json", "{\"error\":\"no_data\"}");
    return;
  }

  String lastLine;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 5) lastLine = line;
  }
  f.close();

  if (lastLine.length() < 5) {
    server.send(404, "application/json", "{\"error\":\"no_data\"}");
    return;
  }

  int c1 = lastLine.indexOf(',');
  int c2 = lastLine.indexOf(',', c1 + 1);
  if (c1 <= 0 || c2 <= c1) {
    server.send(500, "application/json", "{\"error\":\"bad_log_format\"}");
    return;
  }

  time_t ts = (time_t) lastLine.substring(0, c1).toInt();
  float t = lastLine.substring(c1 + 1, c2).toFloat();
  float h = lastLine.substring(c2 + 1).toFloat();

  StaticJsonDocument<128> doc;
  doc["ts"] = (long)ts;
  doc["tempC"] = t;
  doc["humidity"] = h;

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void sendHistorySince(time_t sinceTs) {
  File f = LittleFS.open(LOG_PATH, FILE_READ);
  if (!f) {
    server.send(200, "application/json", "[]");
    return;
  }

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("[");

  bool first = true;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() < 5) continue;

    int c1 = line.indexOf(',');
    int c2 = line.indexOf(',', c1 + 1);
    if (c1 <= 0 || c2 <= c1) continue;

    time_t ts = (time_t) line.substring(0, c1).toInt();
    if (ts < sinceTs) continue;

    float t = line.substring(c1 + 1, c2).toFloat();
    float h = line.substring(c2 + 1).toFloat();

    StaticJsonDocument<128> doc;
    doc["ts"] = (long)ts;
    doc["tempC"] = t;
    doc["humidity"] = h;

    String obj;
    serializeJson(doc, obj);

    if (!first) server.sendContent(",");
    first = false;
    server.sendContent(obj);
  }

  f.close();
  server.sendContent("]");
}

static void handleHistory() {
  time_t sinceTs = 0;

  if (server.hasArg("since")) {
    sinceTs = (time_t) server.arg("since").toInt();
  } else if (server.hasArg("days")) {
    int days = server.arg("days").toInt();
    if (days <= 0) days = 7;
    time_t nowT = nowUtc();
    sinceTs = (nowT > 1700000000) ? (nowT - (time_t)days * 24 * 3600) : 0;
  } else {
    time_t nowT = nowUtc();
    sinceTs = (nowT > 1700000000) ? (nowT - KEEP_SECONDS) : 0;
  }

  sendHistorySince(sinceTs);
}

// -------- setup/loop --------
void setup() {
  Serial.begin(115200);
  delay(200);

  Wire.begin(I2C_SDA, I2C_SCL);

  if (aht.begin() != true) {
    Serial.println("AHT init failed (check wiring).");
  }

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed.");
  }

  if (!LittleFS.exists(LOG_PATH)) {
    File f = LittleFS.open(LOG_PATH, FILE_WRITE);
    if (f) f.close();
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  // mDNS (optional)
  if (MDNS.begin(MDNS_NAME)) {
    Serial.printf("mDNS: http://%s.local/\n", MDNS_NAME);
    MDNS.addService("http", "tcp", 80);
  } else {
    Serial.println("mDNS failed to start");
  }

  // NTP time
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  if (ensureTime()) Serial.println("Time synced.");
  else Serial.println("WARNING: NTP time not synced yet.");

  // Startup prune (once)
  time_t t = nowUtc();
  if (t > 1700000000) pruneOlderThan(t - KEEP_SECONDS);

  server.on("/api/latest", HTTP_GET, handleLatest);
  server.on("/api/history", HTTP_GET, handleHistory);
  server.begin();

  lastSampleMs = millis();
  lastPruneMs  = millis();

  Serial.println("HTTP server started.");
}

void loop() {
  server.handleClient();


  // sample every minute
  if (millis() - lastSampleMs >= SAMPLE_MS) {
    lastSampleMs += SAMPLE_MS; // keeps cadence stable

    float t = aht.readTemperature();
    delay(50);
    float h = aht.readHumidity();

    // basic sanity checks (optional)
    if (isnan(t) || isnan(h) || t < -40 || t > 125 || h < 0 || h > 100) {
      Serial.println("Sensor read invalid; skipped.");
    } else {
      time_t ts = nowUtc();
      if (ts > 1700000000) {
        appendReading(ts, t, h);
        Serial.printf("Logged: ts=%ld T=%.2fC H=%.2f%%\n", (long)ts, t, h);
      } else {
        Serial.println("Time not valid; skipped logging.");
      }
    }

    // prune occasionally (every 6 hours by default)
    maybePrune();
  }
}
