/**
 * ==============================================================================
 * SmartFan IoT System - ESP32 Firmware (Corrected Relay Polarity & State Logic)
 * ==============================================================================
 * Hardware Pin Configuration:
 * - GPIO 4  : DHT11 Sensor DATA line
 * - GPIO 26 : 5V Relay IN (JQC3F-05VDC-C, Active-HIGH)
 *             HIGH = Relay Energized / Closed (Fan ON)
 *             LOW  = Relay De-energized / Open (Fan OFF)
 * - GPIO 25 : PWM Speed Output (Ready for future logic-level MOSFET)
 * - GPIO 2  : Built-in Status LED
 * 
 * Hardware Status:
 * - Relay-only mode: variable PWM disabled until MOSFET driver is installed.
 * - Single authority setFanState(bool enabled) controls relay.
 * ==============================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WebServer.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include <esp_arduino_version.h>

// ==========================================
// 1. Hardware Pin Definitions & Config
// ==========================================
#define DHTPIN         4       // GPIO 4 for DHT11 data line
#define DHTTYPE        DHT11   // Sensor type DHT11
#define RELAY_PIN      26      // GPIO 26 for 5V Relay module
#define PWM_PIN        25      // GPIO 25 for future MOSFET PWM speed control
#define LED_STATUS_PIN 2       // Built-in LED on GPIO 2

// Relay Module Polarity
// JQC3F-05VDC-C module is Active-HIGH:
// HIGH = Relay coil energized (Active / ON)
// LOW  = Relay coil de-energized (Deactivated / OFF)
#define RELAY_ACTIVE_HIGH true

#if RELAY_ACTIVE_HIGH
  #define RELAY_ON  HIGH
  #define RELAY_OFF LOW
#else
  #define RELAY_ON  LOW
  #define RELAY_OFF HIGH
#endif

// PWM Frequency & Resolution for Fan Motor (Ready for future MOSFET driver)
#define PWM_FREQ       25000   
#define PWM_RES        8       // 8-bit resolution (0 to 255)
#define MIN_PWM_DUTY   64      // ~25% minimum duty to avoid brushless motor stalling

#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  #define USE_CORE_V3 1
#else
  #define USE_CORE_V3 0
  const int PWM_CHANNEL = 0;
#endif

// ==========================================
// 2. Global Objects & State Variables
// ==========================================
DHT dht(DHTPIN, DHTTYPE);
WebServer server(80);
WiFiMulti wifiMulti;

// Environmental Telemetry
float currentTemperature = 0.0;
float currentHumidity = 0.0;
bool sensorValid = false;
unsigned long lastSensorReadTime = 0;
const unsigned long SENSOR_INTERVAL = 2000; // 2 seconds DHT11 read interval

// Fan Control & Confirmed Relay State
bool fanState = false;              // Logical fan state (false = OFF, true = ON)
bool relayConfirmed = false;        // Confirmed physical relay pin state
String fanMode = "auto";            // "manual" or "auto"
int fanSpeed = 0;                   // Kept ready for future PWM driver (0 or 100 on relay)

// Automatic Mode Temperature Thresholds
float tempThresholdOn = 30.0;       // Temperature >= 30.0 °C -> Fan ON
float tempThresholdOff = 28.0;      // Temperature <= 28.0 °C -> Fan OFF

// Diagnostics
unsigned long totalDhtReads = 0;
unsigned long failedDhtReads = 0;

// ==========================================
// 3. Low-Level Actuator Functions
// ==========================================
void applyPwmDuty(int speedPercent) {
  if (speedPercent <= 0) {
#if USE_CORE_V3
    ledcWrite(PWM_PIN, 0);
#else
    ledcWrite(PWM_CHANNEL, 0);
#endif
    return;
  }

  if (speedPercent > 100) speedPercent = 100;
  int duty = MIN_PWM_DUTY + ((255 - MIN_PWM_DUTY) * (speedPercent - 1)) / 99;

#if USE_CORE_V3
  ledcWrite(PWM_PIN, duty);
#else
  ledcWrite(PWM_CHANNEL, duty);
#endif
}

/**
 * Single function controlling the fan relay state.
 * When enabled is true, activates the relay (RELAY_ON = HIGH).
 * When enabled is false, deactivates the relay (RELAY_OFF = LOW).
 */
void setFanState(bool enabled) {
  fanState = enabled;
  
  if (enabled) {
    digitalWrite(RELAY_PIN, RELAY_ON);
    digitalWrite(LED_STATUS_PIN, HIGH);
    applyPwmDuty(100);
    fanSpeed = 100;
  } else {
    digitalWrite(RELAY_PIN, RELAY_OFF);
    digitalWrite(LED_STATUS_PIN, LOW);
    applyPwmDuty(0);
    fanSpeed = 0;
  }

  // Read back and confirm actual physical pin state
  relayConfirmed = (digitalRead(RELAY_PIN) == RELAY_ON);

  Serial.printf("[ACTUATOR] setFanState(%s) -> GPIO%d=%s | Confirmed Relay=%s\n",
    enabled ? "true" : "false",
    RELAY_PIN,
    (digitalRead(RELAY_PIN) == HIGH) ? "HIGH" : "LOW",
    relayConfirmed ? "ACTIVE (ON)" : "DEACTIVATED (OFF)"
  );
}

// Builds canonical JSON status payload
String buildStatusJson() {
  JsonDocument doc;

  if (sensorValid) {
    doc["temperature"] = round(currentTemperature * 10.0) / 10.0;
    doc["humidity"] = round(currentHumidity * 10.0) / 10.0;
  } else {
    doc["temperature"] = nullptr;
    doc["humidity"] = nullptr;
  }

  // Read actual pin state for confirmation
  bool physicalRelayActive = (digitalRead(RELAY_PIN) == RELAY_ON);

  doc["fanState"] = physicalRelayActive;
  doc["relayState"] = physicalRelayActive;
  doc["relayPhysicalLevel"] = (digitalRead(RELAY_PIN) == HIGH) ? "HIGH" : "LOW";
  doc["relayPolarity"] = RELAY_ACTIVE_HIGH ? "Active-HIGH" : "Active-LOW";
  doc["fanRotationMeasured"] = false; // 2-wire fan without tachometer feedback
  doc["fanMode"] = fanMode;
  doc["fanSpeed"] = fanSpeed;
  doc["pwmHardwareInstalled"] = false; // MOSFET not yet installed
  doc["deviceConnected"] = true;
  doc["lastUpdated"] = millis() / 1000;
  
  doc["tempThresholdOn"] = tempThresholdOn;
  doc["tempThresholdOff"] = tempThresholdOff;
  doc["sensorValid"] = sensorValid;
  doc["wifiRssi"] = WiFi.RSSI();
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["uptime"] = millis() / 1000;
  doc["pwmPin"] = PWM_PIN;
  doc["relayPin"] = RELAY_PIN;
  doc["dhtPin"] = DHTPIN;

  String output;
  serializeJson(doc, output);
  return output;
}

// ==========================================
// 4. Sensor Reading & Automatic Cooling
// ==========================================
void readSensor() {
  unsigned long now = millis();
  if (now - lastSensorReadTime < SENSOR_INTERVAL && lastSensorReadTime != 0) {
    return;
  }
  lastSensorReadTime = now;
  totalDhtReads++;

  float t = dht.readTemperature();
  float h = dht.readHumidity();

  // Validate reading
  if (!isnan(t) && !isnan(h) && t >= -10.0 && t <= 70.0 && h >= 5.0 && h <= 100.0) {
    currentTemperature = t;
    currentHumidity = h;
    sensorValid = true;

    // -------------------------------------------------------------
    // AUTOMATIC MODE: Temperature-Based Hysteresis
    // CRITICAL: Manual mode MUST NEVER be overridden by auto-cooling!
    // -------------------------------------------------------------
    if (fanMode == "auto") {
      if (currentTemperature >= tempThresholdOn) {
        // Temperature >= 30.0 C -> Fan ON
        if (!fanState) {
          setFanState(true);
        }
      } 
      else if (currentTemperature <= tempThresholdOff) {
        // Temperature <= 28.0 C -> Fan OFF
        if (fanState) {
          setFanState(false);
        }
      }
      // Between 28.0 C and 30.0 C: Hysteresis deadband -> maintain current state
    }

  } else {
    failedDhtReads++;
  }

  // Periodic Serial JSON broadcast for USB communication
  static unsigned long lastSerialOutput = 0;
  if (now - lastSerialOutput >= 2500) {
    lastSerialOutput = now;
    Serial.println(buildStatusJson());
  }
}

// ==========================================
// 5. Serial Command Handling
// ==========================================
void handleSerial() {
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) return;

    if (line.equalsIgnoreCase("STATUS") || line.equalsIgnoreCase("GET_STATUS")) {
      readSensor();
      Serial.println(buildStatusJson());
    } else if (line.startsWith("{")) {
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, line);
      if (!err) {
        // 1. Process mode switch if provided
        if (doc["fanMode"].is<const char*>() || doc["fanMode"].is<String>()) {
          String m = doc["fanMode"].as<String>();
          m.toLowerCase();
          if (m == "auto" || m == "manual") {
            fanMode = m;
          }
        }

        // 2. Process fanState
        if (doc["fanState"].is<bool>()) {
          bool requestedState = doc["fanState"].as<bool>();
          // If a manual state command is sent, switch to manual mode
          if (!doc.containsKey("fanMode")) {
            fanMode = "manual";
          }
          setFanState(requestedState);
        }

        if (doc["tempThresholdOn"].is<float>()) {
          tempThresholdOn = doc["tempThresholdOn"].as<float>();
        }
        if (doc["tempThresholdOff"].is<float>()) {
          tempThresholdOff = doc["tempThresholdOff"].as<float>();
        }

        // If in auto mode, evaluate temperature immediately
        if (fanMode == "auto") {
          if (sensorValid) {
            if (currentTemperature >= tempThresholdOn) setFanState(true);
            else if (currentTemperature <= tempThresholdOff) setFanState(false);
          }
        }
      }
      readSensor();
      Serial.println(buildStatusJson());
    }
  }
}

// ==========================================
// 6. HTTP CORS & Helpers
// ==========================================
void sendCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With");
}

void handleOptions() {
  sendCorsHeaders();
  server.send(204);
}

// ==========================================
// 7. REST API Handlers
// ==========================================

// GET /api/status (and alias GET /status)
void handleGetStatus() {
  sendCorsHeaders();
  readSensor();
  server.send(200, "application/json", buildStatusJson());
}

// POST /api/control (and alias POST /control)
void handlePostControl() {
  sendCorsHeaders();

  String body = "";
  if (server.hasArg("plain")) {
    body = server.arg("plain");
  }

  if (body.length() > 0) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (!err) {
      if (doc["fanMode"].is<const char*>() || doc["fanMode"].is<String>()) {
        String newMode = doc["fanMode"].as<String>();
        newMode.toLowerCase();
        if (newMode == "auto" || newMode == "manual") {
          fanMode = newMode;
        }
      }

      if (doc["fanState"].is<bool>()) {
        bool requestedState = doc["fanState"].as<bool>();
        // Ensure manual command sets manual mode
        if (!doc.containsKey("fanMode")) {
          fanMode = "manual";
        }
        setFanState(requestedState);
      }

      if (doc["tempThresholdOn"].is<float>()) {
        float val = doc["tempThresholdOn"].as<float>();
        if (val > 15.0f && val < 50.0f) tempThresholdOn = val;
      }

      if (doc["tempThresholdOff"].is<float>()) {
        float val = doc["tempThresholdOff"].as<float>();
        if (val > 10.0f && val < tempThresholdOn) tempThresholdOff = val;
      }
    }
  }

  // Also support URL query parameters
  if (server.hasArg("fanMode")) {
    String m = server.arg("fanMode");
    m.toLowerCase();
    if (m == "auto" || m == "manual") fanMode = m;
  }
  if (server.hasArg("fanState") || server.hasArg("state")) {
    String s = server.hasArg("fanState") ? server.arg("fanState") : server.arg("state");
    s.toLowerCase();
    if (s == "on" || s == "true" || s == "1") {
      if (!server.hasArg("fanMode")) fanMode = "manual";
      setFanState(true);
    } else if (s == "off" || s == "false" || s == "0") {
      if (!server.hasArg("fanMode")) fanMode = "manual";
      setFanState(false);
    }
  }

  // If switched to auto mode, evaluate temperature thresholds
  if (fanMode == "auto" && sensorValid) {
    if (currentTemperature >= tempThresholdOn) setFanState(true);
    else if (currentTemperature <= tempThresholdOff) setFanState(false);
  }

  readSensor();
  server.send(200, "application/json", buildStatusJson());
}

// GET/POST /api/fan (convenience endpoint for quick ON/OFF toggling)
void handleFanToggle() {
  sendCorsHeaders();

  String s = "";
  if (server.hasArg("state")) s = server.arg("state");
  else if (server.hasArg("fanState")) s = server.arg("fanState");
  s.toLowerCase();

  fanMode = "manual"; // Manual command overrides auto mode
  if (s == "on" || s == "true" || s == "1") {
    setFanState(true);
  } else if (s == "off" || s == "false" || s == "0") {
    setFanState(false);
  }

  server.send(200, "application/json", buildStatusJson());
}

// 404 handler
void handleNotFound() {
  sendCorsHeaders();
  server.send(404, "application/json", "{\"error\":\"Endpoint not found\"}");
}

// ==========================================
// 8. Setup & Main Loop
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n=============================================");
  Serial.println("  SmartFan IoT System - Corrected Relay Logic");
  Serial.println("=============================================");

  // -------------------------------------------------------------------------
  // CRITICAL REQUIREMENT 4: Initialize GPIO26 immediately to DEACTIVATED state
  // Set output level before pinMode to avoid startup glitches
  // -------------------------------------------------------------------------
  digitalWrite(RELAY_PIN, RELAY_OFF);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF);

  pinMode(LED_STATUS_PIN, OUTPUT);
  digitalWrite(LED_STATUS_PIN, LOW);

  fanState = false;
  relayConfirmed = false;
  Serial.printf("[RELAY] Initialized GPIO %d to DEACTIVATED (%s)\n",
    RELAY_PIN,
    (RELAY_OFF == LOW) ? "LOW" : "HIGH"
  );

  // Initialize PWM for speed control on GPIO 25 (Ready for future MOSFET)
#if USE_CORE_V3
  ledcAttach(PWM_PIN, PWM_FREQ, PWM_RES);
  ledcWrite(PWM_PIN, 0);
#else
  ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RES);
  ledcAttachPin(PWM_PIN, PWM_CHANNEL);
  ledcWrite(PWM_CHANNEL, 0);
#endif
  Serial.printf("[PWM] Initialized on GPIO %d at %d Hz (Standby for MOSFET driver)\n", PWM_PIN, PWM_FREQ);

  // Initialize DHT11 sensor
  dht.begin();
  Serial.printf("[DHT11] Initialized on GPIO %d\n", DHTPIN);

  // Multi-WiFi Support
  wifiMulti.addAP("Amal Jyothi New", "amaljyothi");
  wifiMulti.addAP("Hitler rameshan", "12345678");
  wifiMulti.addAP("Joseph iPhone", "22334455jm");

  Serial.println("[WIFI] Connecting to available Wi-Fi network...");
  int attempts = 0;
  while (wifiMulti.run() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Connected successfully!");
    Serial.printf("[WIFI] SSID     : %s\n", WiFi.SSID().c_str());
    Serial.printf("[WIFI] Local IP : http://%s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[WIFI] RSSI     : %d dBm\n", WiFi.RSSI());
  } else {
    Serial.println("\n[WIFI WARN] Failed to connect. Running offline sensor loop.");
  }

  // Register WebServer Endpoints
  server.on("/api/status", HTTP_OPTIONS, handleOptions);
  server.on("/api/status", HTTP_GET, handleGetStatus);
  server.on("/status", HTTP_OPTIONS, handleOptions);
  server.on("/status", HTTP_GET, handleGetStatus);

  server.on("/api/control", HTTP_OPTIONS, handleOptions);
  server.on("/api/control", HTTP_POST, handlePostControl);
  server.on("/control", HTTP_OPTIONS, handleOptions);
  server.on("/control", HTTP_POST, handlePostControl);

  server.on("/api/fan", HTTP_OPTIONS, handleOptions);
  server.on("/api/fan", HTTP_GET, handleFanToggle);
  server.on("/api/fan", HTTP_POST, handleFanToggle);
  server.on("/fan", HTTP_OPTIONS, handleOptions);
  server.on("/fan", HTTP_GET, handleFanToggle);
  server.on("/fan", HTTP_POST, handleFanToggle);

  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("[HTTP] REST API Server listening on port 80");
  Serial.printf("[AUTO] Auto-Cooling configured: ON >= %.1f C | OFF <= %.1f C\n", tempThresholdOn, tempThresholdOff);

  readSensor();
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    server.handleClient();
  }
  handleSerial();
  readSensor();
}
