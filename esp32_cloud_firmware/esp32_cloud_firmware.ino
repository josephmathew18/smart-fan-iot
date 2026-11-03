/**
 * ==============================================================================
 * SmartFan IoT System - ESP32 Cloud Firmware (Render / Cloud Connected)
 * ==============================================================================
 * Connects your ESP32 to your Render-hosted Cloud Backend over Wi-Fi (HTTPS).
 * 
 * Hardware Pin Configuration:
 * - GPIO 4  : DHT11 Sensor DATA line
 * - GPIO 26 : 5V Relay Module IN (Active-LOW: LOW = Fan ON, HIGH = Fan OFF)
 * 
 * Flow:
 * 1. ESP32 connects to your Wi-Fi network (Home Wi-Fi or Mobile Hotspot).
 * 2. Every 2.5 seconds, it reads Temperature & Humidity from DHT11.
 * 3. Sends POST request with sensor readings to:
 *    https://<YOUR-RENDER-APP-NAME>.onrender.com/api/telemetry
 * 4. Receives back the commanded fan state from the cloud dashboard.
 * 5. Controls GPIO 26 relay and provides physical cooling.
 * ==============================================================================
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <DHT.h>

// ---------------- Wi-Fi Configuration ----------------
// Enter your Wi-Fi name (SSID) and Password
const char* ssid     = "Joseph";
const char* password = "22334455jm";

// ---------------- Render Cloud Server URL ----------------
// Replace with your actual Render service URL (e.g., https://smart-fan-iot.onrender.com)
const char* cloudServerUrl = "https://smart-fan-iot.onrender.com/api/telemetry";

// ---------------- Hardware Pin Definitions ----------------
#define DHTPIN         4       // GPIO 4 for DHT11 data line
#define DHTTYPE        DHT11   // Sensor type DHT11
#define RELAY_PIN      26      // GPIO 26 for 5V Relay module

// Relay Polarity (Active-LOW: LOW = ON, HIGH = OFF)
#define RELAY_ON  LOW
#define RELAY_OFF HIGH

// Temperature Thresholds for Auto Cooling
float tempThresholdOn  = 30.0;
float tempThresholdOff = 28.0;

DHT dht(DHTPIN, DHTTYPE);

bool fanState = false;
String fanMode = "auto";
unsigned long lastTelemetryTime = 0;
const unsigned long TELEMETRY_INTERVAL = 2500; // 2.5 seconds

void setFanState(bool enabled) {
  fanState = enabled;
  digitalWrite(RELAY_PIN, enabled ? RELAY_ON : RELAY_OFF);
  Serial.printf("[ACTUATOR] Fan %s (GPIO 26 = %s)\n",
    enabled ? "ON" : "OFF",
    enabled ? "LOW" : "HIGH"
  );
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n=============================================");
  Serial.println("  SmartFan ESP32 - Cloud Connected Firmware  ");
  Serial.println("=============================================");

  // Initialize relay to OFF state immediately
  digitalWrite(RELAY_PIN, RELAY_OFF);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF);

  // Initialize DHT11
  dht.begin();
  Serial.println("[DHT11] Sensor initialized on GPIO 4");

  // Connect to Wi-Fi
  Serial.printf("[WIFI] Connecting to %s", ssid);
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Connected successfully!");
    Serial.printf("[WIFI] IP Address: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WIFI WARN] Connection timeout. Running local auto-cooling fallback.");
  }
}

void loop() {
  unsigned long now = millis();
  if (now - lastTelemetryTime < TELEMETRY_INTERVAL && lastTelemetryTime != 0) {
    return;
  }
  lastTelemetryTime = now;

  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();

  if (isnan(temperature) || isnan(humidity)) {
    Serial.println("[DHT11 WARN] Sensor read failed. Retrying...");
    return;
  }

  Serial.printf("[SENSOR] Temp: %.1f °C | Humidity: %.1f %% | Fan: %s\n",
    temperature, humidity, fanState ? "ON" : "OFF"
  );

  // 1. Local Auto-Cooling Logic (ensures fan works even if Wi-Fi drops)
  if (fanMode == "auto") {
    if (temperature >= tempThresholdOn && !fanState) {
      setFanState(true);
    } else if (temperature <= tempThresholdOff && fanState) {
      setFanState(false);
    }
  }

  // 2. Synchronize with Render Cloud Backend
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure client;
    client.setInsecure(); // Skip TLS certificate fingerprint validation for Render
    HTTPClient http;

    if (http.begin(client, cloudServerUrl)) {
      http.addHeader("Content-Type", "application/json");

      // Build outgoing telemetry payload
      JsonDocument doc;
      doc["temperature"] = temperature;
      doc["humidity"] = humidity;
      doc["relayState"] = fanState;
      doc["fanState"] = fanState;
      doc["fanMode"] = fanMode;
      doc["wifiRssi"] = WiFi.RSSI();

      String requestBody;
      serializeJson(doc, requestBody);

      int httpResponseCode = http.POST(requestBody);

      if (httpResponseCode == 200) {
        String responseText = http.getString();
        JsonDocument responseDoc;
        DeserializationError err = deserializeJson(responseDoc, responseText);
        if (!err) {
          if (responseDoc["fanMode"].is<String>()) {
            fanMode = responseDoc["fanMode"].as<String>();
          }
          if (responseDoc["fanState"].is<bool>()) {
            bool targetState = responseDoc["fanState"].as<bool>();
            if (fanMode == "manual") {
              setFanState(targetState);
            }
          }
          if (responseDoc["tempThresholdOn"].is<float>()) {
            tempThresholdOn = responseDoc["tempThresholdOn"].as<float>();
          }
          if (responseDoc["tempThresholdOff"].is<float>()) {
            tempThresholdOff = responseDoc["tempThresholdOff"].as<float>();
          }
        }
      } else {
        Serial.printf("[CLOUD] HTTP POST failed, code: %d\n", httpResponseCode);
      }
      http.end();
    }
  }
}

