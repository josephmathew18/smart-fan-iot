/**
 * ==============================================================================
 * SmartFan IoT System - ESP32 Complete PlatformIO Firmware
 * ==============================================================================
 * Hardware Pin Configuration:
 * - GPIO 4  : DHT11 Sensor DATA line
 * - GPIO 26 : 5V Relay IN (Active-LOW Relay: LOW = Fan ON, HIGH = Fan OFF)
 * - GPIO 2  : Built-in Status LED (HIGH = ON, LOW = OFF)
 * 
 * Communication Channels:
 * 1. USB Serial Gateway (COM9 @ 115200 baud):
 *    - Real-time bidirectional JSON + token command protocol
 *    - Instant hardware acknowledgement for dashboard Turn ON / Turn OFF
 * 2. Local Wi-Fi HTTP Gateway (Port 80):
 *    - GET  /api/status  -> Returns real-time JSON telemetry
 *    - POST /api/control -> Receives manual ON/OFF or mode switches
 * 3. Render Cloud Telemetry Push (HTTPS):
 *    - Pushes live readings to https://smart-fan-iot.onrender.com/api/telemetry
 * ==============================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <DHT.h>
#include <ArduinoJson.h>

// ==============================================================================
// 1. Hardware Pin Definitions & Relay Polarity
// ==============================================================================
#define DHTPIN         4       // GPIO 4 for DHT11 data line
#define DHTTYPE        DHT11   // Sensor type DHT11
#define RELAY_PIN      26      // GPIO 26 for 5V Relay module
#define LED_STATUS_PIN 2       // Built-in LED on GPIO 2

// Active-LOW Relay Polarity:
// LOW  = Relay Energized / Contact Closed -> 12V Fan ON
// HIGH = Relay De-energized / Contact Open -> 12V Fan OFF
#define RELAY_ON       LOW
#define RELAY_OFF      HIGH

// Automatic Mode Temperature Thresholds
const float TEMP_THRESHOLD_ON  = 30.0; // >= 30.0 °C -> Fan ON
const float TEMP_THRESHOLD_OFF = 28.0; // <= 28.0 °C -> Fan OFF

// Intervals
const unsigned long SENSOR_INTERVAL    = 2000; // 2 seconds
const unsigned long CLOUD_INTERVAL     = 5000; // 5 seconds push to Render
const unsigned long SERIAL_INTERVAL    = 2000; // 2 seconds telemetry broadcast

// Render Cloud Backend Endpoint
const char* CLOUD_SERVER_URL = "https://smart-fan-iot.onrender.com/api/telemetry";

// ==============================================================================
// 2. Global Objects & System State
// ==============================================================================
DHT dht(DHTPIN, DHTTYPE);
WebServer server(80);
WiFiMulti wifiMulti;

// Environmental Telemetry
float currentTemperature = NAN;
float currentHumidity    = NAN;
bool sensorValid         = false;
unsigned long lastSensorReadTime = 0;
unsigned long lastCloudPushTime  = 0;
unsigned long lastSerialBroadcast = 0;

// Fan Actuator State
bool fanState       = false;          // false = OFF, true = ON
String fanMode      = "auto";         // "manual" or "auto"
String lastCloudMsg = "Initialized";

// ==============================================================================
// 3. Actuator Control
// ==============================================================================
void setFanState(bool enabled) {
  fanState = enabled;
  digitalWrite(RELAY_PIN, enabled ? RELAY_ON : RELAY_OFF);
  digitalWrite(LED_STATUS_PIN, enabled ? HIGH : LOW);
}

// Builds canonical JSON status payload
String buildStatusJson(bool isAck = false, const char* ackCmd = "") {
  JsonDocument doc;

  if (isAck) {
    doc["ack"] = true;
    doc["status"] = "ok";
    if (strlen(ackCmd) > 0) {
      doc["command"] = ackCmd;
    }
  }

  if (sensorValid && !isnan(currentTemperature)) {
    doc["temperature"] = round(currentTemperature * 10.0) / 10.0;
    doc["humidity"]    = round(currentHumidity * 10.0) / 10.0;
  } else {
    doc["temperature"] = nullptr;
    doc["humidity"]    = nullptr;
  }

  bool physicalRelayActive = (digitalRead(RELAY_PIN) == RELAY_ON);

  doc["fanState"]            = physicalRelayActive;
  doc["relayState"]          = physicalRelayActive;
  doc["relayPhysicalLevel"]  = (digitalRead(RELAY_PIN) == HIGH) ? "HIGH" : "LOW";
  doc["relayPolarity"]       = "Active-LOW";
  doc["fanRotationMeasured"] = false;
  doc["fanMode"]             = fanMode;
  doc["fanSpeed"]            = physicalRelayActive ? 100 : 0;
  doc["pwmHardwareInstalled"]= false;
  doc["deviceConnected"]     = true;
  doc["lastUpdated"]         = millis() / 1000;
  
  doc["tempThresholdOn"]     = TEMP_THRESHOLD_ON;
  doc["tempThresholdOff"]    = TEMP_THRESHOLD_OFF;
  doc["sensorValid"]         = sensorValid;
  doc["wifiConnected"]       = (WiFi.status() == WL_CONNECTED);
  doc["wifiSsid"]            = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : "Disconnected";
  doc["wifiIp"]              = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "0.0.0.0";
  doc["wifiRssi"]            = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  doc["uptime"]              = millis() / 1000;
  doc["freeHeap"]            = ESP.getFreeHeap();
  doc["cloudSyncStatus"]     = lastCloudMsg;

  String output;
  serializeJson(doc, output);
  return output;
}

// Emits an immediate hardware acknowledgement over Serial
void emitHardwareAck(const char* cmd) {
  String jsonOutput = buildStatusJson(true, cmd);
  Serial.println(jsonOutput);

  // Human-readable confirmation log
  Serial.printf("[ACK] %s | Fan %s | GPIO26=%s | Mode=%s\n",
    cmd,
    fanState ? "ON (12V Connected)" : "OFF (12V Disconnected)",
    (digitalRead(RELAY_PIN) == HIGH) ? "HIGH" : "LOW",
    fanMode.c_str()
  );
}

// ==============================================================================
// 4. Sensor Reading & Automatic Cooling (With Hysteresis)
// ==============================================================================
void readSensor() {
  unsigned long now = millis();
  if (now - lastSensorReadTime < SENSOR_INTERVAL && lastSensorReadTime != 0) {
    return;
  }
  lastSensorReadTime = now;

  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (!isnan(t) && !isnan(h) && t >= -10.0 && t <= 70.0 && h >= 5.0 && h <= 100.0) {
    currentTemperature = t;
    currentHumidity    = h;
    sensorValid        = true;

    // AUTOMATIC MODE: Temperature-Based Hysteresis
    // - Temp >= 30.0 °C -> Turn Fan ON (GPIO26 LOW)
    // - Temp <= 28.0 °C -> Turn Fan OFF (GPIO26 HIGH)
    // - 28.0 °C < Temp < 30.0 °C -> Maintain previous state
    //
    // MANUAL MODE:
    // - Automatic temperature control NEVER overrides manual commands.
    if (fanMode == "auto") {
      if (currentTemperature >= TEMP_THRESHOLD_ON) {
        if (!fanState) {
          setFanState(true);
          Serial.println("[AUTO] Temp >= 30.0°C -> Fan turned ON (GPIO26 LOW)");
        }
      } else if (currentTemperature <= TEMP_THRESHOLD_OFF) {
        if (fanState) {
          setFanState(false);
          Serial.println("[AUTO] Temp <= 28.0°C -> Fan turned OFF (GPIO26 HIGH)");
        }
      }
    }
  } else {
    sensorValid = false;
  }
}

// ==============================================================================
// 5. Serial Command Handling (USB COM9)
// ==============================================================================
void handleSerial() {
  while (Serial.available() > 0) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    // 1. FAN OFF (Manual Mode)
    if (
      line.equalsIgnoreCase("FAN_OFF") ||
      line == "0" ||
      line.indexOf("FAN_OFF") != -1 ||
      line.indexOf("\"fanState\":false") != -1 ||
      line.indexOf("\"fanState\": false") != -1
    ) {
      fanMode = "manual";
      setFanState(false); // GPIO 26 -> HIGH (Relay deactivates, 12V fan stops)
      emitHardwareAck("FAN_OFF");
      continue;
    }

    // 2. FAN ON (Manual Mode)
    else if (
      line.equalsIgnoreCase("FAN_ON") ||
      line == "1" ||
      line.indexOf("FAN_ON") != -1 ||
      line.indexOf("\"fanState\":true") != -1 ||
      line.indexOf("\"fanState\": true") != -1
    ) {
      fanMode = "manual";
      setFanState(true);  // GPIO 26 -> LOW (Relay activates, 12V fan starts)
      emitHardwareAck("FAN_ON");
      continue;
    }

    // 3. AUTOMATIC MODE
    else if (
      line.equalsIgnoreCase("MODE_AUTO") ||
      line.indexOf("MODE_AUTO") != -1 ||
      line.indexOf("\"fanMode\":\"auto\"") != -1 ||
      line.indexOf("\"fanMode\": \"auto\"") != -1
    ) {
      fanMode = "auto";
      if (sensorValid) {
        if (currentTemperature >= TEMP_THRESHOLD_ON) setFanState(true);
        else if (currentTemperature <= TEMP_THRESHOLD_OFF) setFanState(false);
      }
      emitHardwareAck("MODE_AUTO");
      continue;
    }

    // 4. MANUAL MODE
    else if (
      line.equalsIgnoreCase("MODE_MANUAL") ||
      line.indexOf("MODE_MANUAL") != -1 ||
      line.indexOf("\"fanMode\":\"manual\"") != -1 ||
      line.indexOf("\"fanMode\": \"manual\"") != -1
    ) {
      fanMode = "manual";
      emitHardwareAck("MODE_MANUAL");
      continue;
    }

    // 5. STATUS / HEARTBEAT
    else if (line.equalsIgnoreCase("STATUS") || line.equalsIgnoreCase("GET_STATUS")) {
      emitHardwareAck("STATUS");
      continue;
    }

    // 6. Generic JSON Packet Parsing
    else if (line.indexOf("{") != -1) {
      int s_idx = line.indexOf("{");
      int e_idx = line.lastIndexOf("}");
      if (e_idx > s_idx) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, line.substring(s_idx, e_idx + 1));
        if (!err) {
          if (doc["fanMode"].is<const char*>() || doc["fanMode"].is<String>()) {
            String m = doc["fanMode"].as<String>();
            m.toLowerCase();
            if (m == "auto" || m == "manual") fanMode = m;
          }

          if (doc["fanState"].is<bool>()) {
            bool reqState = doc["fanState"].as<bool>();
            if (!doc["fanMode"].is<const char*>() && !doc["fanMode"].is<String>()) fanMode = "manual";
            setFanState(reqState);
          }

          if (fanMode == "auto" && sensorValid) {
            if (currentTemperature >= TEMP_THRESHOLD_ON) setFanState(true);
            else if (currentTemperature <= TEMP_THRESHOLD_OFF) setFanState(false);
          }

          emitHardwareAck("JSON_CONTROL");
          continue;
        }
      }
    }

    Serial.print("[WARN] Unknown command: ");
    Serial.println(line);
  }
}

// ==============================================================================
// 6. Local HTTP Web Server Endpoints
// ==============================================================================
void sendCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With");
}

void handleOptions() {
  sendCorsHeaders();
  server.send(204);
}

void handleGetStatus() {
  sendCorsHeaders();
  readSensor();
  server.send(200, "application/json", buildStatusJson());
}

void handlePostControl() {
  sendCorsHeaders();
  String body = server.hasArg("plain") ? server.arg("plain") : "";
  if (body.length() > 0) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (!err) {
      if (doc["fanMode"].is<const char*>() || doc["fanMode"].is<String>()) {
        String newMode = doc["fanMode"].as<String>();
        newMode.toLowerCase();
        if (newMode == "auto" || newMode == "manual") fanMode = newMode;
      }
      if (doc["fanState"].is<bool>()) {
        if (!doc["fanMode"].is<const char*>() && !doc["fanMode"].is<String>()) fanMode = "manual";
        setFanState(doc["fanState"].as<bool>());
      }
    }
  }

  if (server.hasArg("fanMode")) {
    String m = server.arg("fanMode");
    m.toLowerCase();
    if (m == "auto" || m == "manual") fanMode = m;
  }

  if (server.hasArg("fanState") || server.hasArg("state")) {
    String s = server.hasArg("fanState") ? server.arg("fanState") : server.arg("state");
    s.toLowerCase();
    fanMode = "manual";
    setFanState(s == "on" || s == "true" || s == "1");
  }

  if (fanMode == "auto" && sensorValid) {
    if (currentTemperature >= TEMP_THRESHOLD_ON) setFanState(true);
    else if (currentTemperature <= TEMP_THRESHOLD_OFF) setFanState(false);
  }

  readSensor();
  server.send(200, "application/json", buildStatusJson(true, "HTTP_CONTROL"));
}

// ==============================================================================
// 7. Render Cloud HTTPS Integration
// ==============================================================================
void pushTelemetryToCloud() {
  if (WiFi.status() != WL_CONNECTED) return;

  unsigned long now = millis();
  if (now - lastCloudPushTime < CLOUD_INTERVAL && lastCloudPushTime != 0) return;
  lastCloudPushTime = now;

  WiFiClientSecure client;
  client.setInsecure(); // Accept Render's TLS certificate

  HTTPClient https;
  if (https.begin(client, CLOUD_SERVER_URL)) {
    https.addHeader("Content-Type", "application/json");
    https.setTimeout(3000);

    JsonDocument doc;
    if (sensorValid) {
      doc["temperature"] = round(currentTemperature * 10.0) / 10.0;
      doc["humidity"]    = round(currentHumidity * 10.0) / 10.0;
    }
    doc["fanState"]           = fanState;
    doc["relayState"]         = fanState;
    doc["relayPhysicalLevel"] = (digitalRead(RELAY_PIN) == HIGH) ? "HIGH" : "LOW";
    doc["relayPolarity"]      = "Active-LOW";
    doc["fanMode"]            = fanMode;
    doc["fanSpeed"]           = fanState ? 100 : 0;
    doc["wifiRssi"]           = WiFi.RSSI();

    String requestBody;
    serializeJson(doc, requestBody);

    int httpCode = https.POST(requestBody);
    if (httpCode > 0) {
      lastCloudMsg = "Cloud Sync OK (" + String(httpCode) + ")";
      if (httpCode == HTTP_CODE_OK) {
        String response = https.getString();
        JsonDocument resDoc;
        if (!deserializeJson(resDoc, response)) {
          // Cloud commanded state sync
          if (resDoc["fanMode"].is<const char*>() || resDoc["fanMode"].is<String>()) {
            String cm = resDoc["fanMode"].as<String>();
            cm.toLowerCase();
            if (cm == "auto" || cm == "manual") fanMode = cm;
          }
          if (resDoc["fanState"].is<bool>() && fanMode == "manual") {
            setFanState(resDoc["fanState"].as<bool>());
          }
        }
      }
    } else {
      lastCloudMsg = "Cloud Error: " + https.errorToString(httpCode);
    }
    https.end();
  }
}

// ==============================================================================
// 8. Setup & Main Loop
// ==============================================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  // Active-LOW relay hardware safety: Set pin HIGH before OUTPUT to prevent boot click
  digitalWrite(RELAY_PIN, RELAY_OFF);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF);

  pinMode(LED_STATUS_PIN, OUTPUT);
  digitalWrite(LED_STATUS_PIN, LOW);

  fanState = false;

  Serial.println();
  Serial.println("==================================================");
  Serial.println("  SmartFan IoT System - ESP32 PlatformIO Ready    ");
  Serial.println("==================================================");
  Serial.println("Relay Output Pin : GPIO 26 (Active-LOW: HIGH=OFF, LOW=ON)");
  Serial.println("DHT11 Data Pin   : GPIO 4");
  Serial.println("Initial Fan State: OFF (GPIO 26 = HIGH)");
  Serial.println("Initial Mode     : AUTOMATIC");
  Serial.println("==================================================");

  // Initialize DHT11 sensor
  dht.begin();

  // Multi-WiFi credentials
  wifiMulti.addAP("iQOO Neo7", "1234567890@07");
  wifiMulti.addAP("Amal Jyothi New", "amaljyothi");
  wifiMulti.addAP("Hitler rameshan", "12345678");
  wifiMulti.addAP("Joseph iPhone", "22334455jm");

  Serial.print("[WIFI] Connecting to Wi-Fi");
  int attempts = 0;
  while (wifiMulti.run() != WL_CONNECTED && attempts < 8) {
    delay(400);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Connected!");
    Serial.printf("[WIFI] Local IP: http://%s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[WIFI] Gateway: %s | RSSI: %d dBm\n", WiFi.gatewayIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.println("\n[WIFI] Wi-Fi not connected (USB Serial Gateway active)");
  }

  // WebServer routes
  server.on("/api/status", HTTP_OPTIONS, handleOptions);
  server.on("/api/status", HTTP_GET, handleGetStatus);
  server.on("/status", HTTP_GET, handleGetStatus);
  server.on("/api/control", HTTP_OPTIONS, handleOptions);
  server.on("/api/control", HTTP_POST, handlePostControl);
  server.on("/control", HTTP_POST, handlePostControl);
  server.begin();
  Serial.println("[HTTP] Local REST WebServer started on port 80");

  readSensor();
  emitHardwareAck("BOOT");
}

void loop() {
  // 1. Process Serial commands (USB COM9)
  handleSerial();

  // 2. Serve Local HTTP WebServer (if Wi-Fi active)
  if (WiFi.status() == WL_CONNECTED) {
    server.handleClient();
    pushTelemetryToCloud();
  }

  // 3. Read sensor & evaluate automatic cooling
  readSensor();

  // 4. Periodic Serial JSON broadcast every 2 seconds
  unsigned long now = millis();
  if (now - lastSerialBroadcast >= SERIAL_INTERVAL) {
    lastSerialBroadcast = now;
    Serial.println(buildStatusJson());
  }

  delay(10);
}
