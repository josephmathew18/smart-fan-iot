/**
 * ==============================================================================
 * SmartFan IoT System - ESP32 Complete Production Firmware (Active-LOW Relay)
 * ==============================================================================
 * Hardware Pin Configuration:
 * - GPIO 4  : DHT11 Sensor DATA line
 * - GPIO 26 : 5V Relay IN (JQC3F-05VDC-C, Active-LOW)
 *             LOW  = Relay Energized / Contact Closed (Fan ON)
 *             HIGH = Relay De-energized / Contact Open (Fan OFF)
 * - GPIO 25 : PWM Speed Output (Ready for future logic-level MOSFET)
 * - GPIO 2  : Built-in Status LED (HIGH = ON, LOW = OFF)
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

// Relay Module Polarity (JQC3F-05VDC-C module is Active-LOW):
#define RELAY_ACTIVE_LOW true
#define RELAY_ON  LOW
#define RELAY_OFF HIGH

// PWM Frequency & Resolution for Fan Motor (Ready for future MOSFET driver)
#define PWM_FREQ       25000   
#define PWM_RES        8       // 8-bit resolution (0 to 255)
#define MIN_PWM_DUTY   64      // ~25% minimum duty

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
int fanSpeed = 0;

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

void setFanState(bool enabled) {
  fanState = enabled;
  
  if (enabled) {
    digitalWrite(RELAY_PIN, RELAY_ON);     // LOW turns relay ON
    digitalWrite(LED_STATUS_PIN, HIGH);
    applyPwmDuty(100);
    fanSpeed = 100;
  } else {
    digitalWrite(RELAY_PIN, RELAY_OFF);    // HIGH turns relay OFF
    digitalWrite(LED_STATUS_PIN, LOW);
    applyPwmDuty(0);
    fanSpeed = 0;
  }

  // Read back actual pin state
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

  bool physicalRelayActive = (digitalRead(RELAY_PIN) == RELAY_ON);

  doc["fanState"] = physicalRelayActive;
  doc["relayState"] = physicalRelayActive;
  doc["relayPhysicalLevel"] = (digitalRead(RELAY_PIN) == HIGH) ? "HIGH" : "LOW";
  doc["relayPolarity"] = "Active-LOW";
  doc["fanRotationMeasured"] = false;
  doc["fanMode"] = fanMode;
  doc["fanSpeed"] = fanSpeed;
  doc["pwmHardwareInstalled"] = false;
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

  if (!isnan(t) && !isnan(h) && t >= -10.0 && t <= 70.0 && h >= 5.0 && h <= 100.0) {
    currentTemperature = t;
    currentHumidity = h;
    sensorValid = true;

    // AUTOMATIC MODE: Temperature-Based Hysteresis
    if (fanMode == "auto") {
      if (currentTemperature >= tempThresholdOn) {
        if (!fanState) {
          setFanState(true);
        }
      } else if (currentTemperature <= tempThresholdOff) {
        if (fanState) {
          setFanState(false);
        }
      }
    }
  } else {
    failedDhtReads++;
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
    } else if (line.indexOf("{") != -1) {
      int s_idx = line.indexOf("{");
      int e_idx = line.lastIndexOf("}");
      if (e_idx > s_idx) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, line.substring(s_idx, e_idx + 1));
        if (!err) {
          if (doc["fanMode"].is<const char*>() || doc["fanMode"].is<String>()) {
            String m = doc["fanMode"].as<String>();
            m.toLowerCase();
            if (m == "auto" || m == "manual") {
              fanMode = m;
            }
          }

          if (doc["fanState"].is<bool>()) {
            bool requestedState = doc["fanState"].as<bool>();
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

          if (fanMode == "auto" && sensorValid) {
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
// 6. HTTP REST API Handlers
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
        if (!doc.containsKey("fanMode")) fanMode = "manual";
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
    if (currentTemperature >= tempThresholdOn) setFanState(true);
    else if (currentTemperature <= tempThresholdOff) setFanState(false);
  }
  readSensor();
  server.send(200, "application/json", buildStatusJson());
}

// ==========================================
// 7. Setup & Main Loop
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n=============================================");
  Serial.println("  SmartFan IoT System - Production Firmware  ");
  Serial.println("=============================================");

  // Initialize GPIO 26 to DEACTIVATED (HIGH for active-LOW relay) immediately
  digitalWrite(RELAY_PIN, RELAY_OFF);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF);

  pinMode(LED_STATUS_PIN, OUTPUT);
  digitalWrite(LED_STATUS_PIN, LOW);

  fanState = false;
  relayConfirmed = false;
  Serial.println("[RELAY] Initialized GPIO 26 to DEACTIVATED (HIGH, Relay OFF)");

  // PWM Standby
#if USE_CORE_V3
  ledcAttach(PWM_PIN, PWM_FREQ, PWM_RES);
  ledcWrite(PWM_PIN, 0);
#else
  ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RES);
  ledcAttachPin(PWM_PIN, PWM_CHANNEL);
  ledcWrite(PWM_CHANNEL, 0);
#endif

  // DHT11
  dht.begin();
  Serial.println("[DHT11] Initialized on GPIO 4");

  // Multi-WiFi with quick 3-second attempt
  wifiMulti.addAP("Amal Jyothi New", "amaljyothi");
  wifiMulti.addAP("Hitler rameshan", "12345678");
  wifiMulti.addAP("Joseph iPhone", "22334455jm");

  Serial.println("[WIFI] Checking connection...");
  int attempts = 0;
  while (wifiMulti.run() != WL_CONNECTED && attempts < 6) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Connected!");
    Serial.printf("[WIFI] Local IP: http://%s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WIFI] Offline mode (USB Serial Gateway active)");
  }

  server.on("/api/status", HTTP_OPTIONS, handleOptions);
  server.on("/api/status", HTTP_GET, handleGetStatus);
  server.on("/status", HTTP_GET, handleGetStatus);
  server.on("/api/control", HTTP_OPTIONS, handleOptions);
  server.on("/api/control", HTTP_POST, handlePostControl);
  server.on("/control", HTTP_POST, handlePostControl);
  server.begin();

  readSensor();
  Serial.println(buildStatusJson());
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    server.handleClient();
  }
  handleSerial();
  readSensor();

  // Periodic Serial JSON broadcast every 2 seconds
  static unsigned long lastSerialOutput = 0;
  unsigned long now = millis();
  if (now - lastSerialOutput >= 2000) {
    lastSerialOutput = now;
    Serial.println(buildStatusJson());
  }
}
