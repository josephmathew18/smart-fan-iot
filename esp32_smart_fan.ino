#include <DHT.h>

// ==================================================
// ESP32 SMART FAN CONTROLLER
// Bidirectional Automatic + Manual Control
// ==================================================

// ---------------- PIN CONFIGURATION ----------------

#define DHT_PIN     4
#define DHT_TYPE    DHT11
#define RELAY_PIN   26

// ---------------- RELAY POLARITY ----------------
// Default: Active-HIGH (HIGH = Fan ON, LOW = Fan OFF)
// If your relay / fan was spinning when OFF was clicked,
// LOW correctly de-energizes the relay and turns OFF the fan!
bool relayActiveLow = false; 

#define RELAY_ON    (relayActiveLow ? LOW : HIGH)
#define RELAY_OFF   (relayActiveLow ? HIGH : LOW)

// ---------------- TEMPERATURE SETTINGS ----------------

const float TEMP_THRESHOLD_ON  = 30.0;
const float TEMP_THRESHOLD_OFF = 28.0;

// ---------------- SENSOR SETTINGS ----------------

const unsigned long SENSOR_INTERVAL = 2000;

// ---------------- OBJECTS ----------------

DHT dht(DHT_PIN, DHT_TYPE);

// ---------------- SYSTEM VARIABLES ----------------

bool fanState = false;
bool manualMode = false;

float temperature = NAN;
float humidity = NAN;

unsigned long lastSensorReadTime = 0;

// ==================================================
// FAN CONTROL FUNCTION
// ==================================================

void setFanState(bool enabled) {
  fanState = enabled;
  digitalWrite(RELAY_PIN, enabled ? RELAY_ON : RELAY_OFF);
}

// ==================================================
// EMIT JSON TELEMETRY TO SERIAL
// ==================================================

void printJsonState() {
  Serial.print("{\"temperature\":");
  if (isnan(temperature)) {
    Serial.print("null");
  } else {
    Serial.print(temperature, 1);
  }
  Serial.print(",\"humidity\":");
  if (isnan(humidity)) {
    Serial.print("null");
  } else {
    Serial.print(humidity, 1);
  }
  Serial.print(",\"fanState\":");
  Serial.print(fanState ? "true" : "false");
  Serial.print(",\"relayState\":");
  Serial.print(fanState ? "true" : "false");
  Serial.print(",\"fanMode\":\"");
  Serial.print(manualMode ? "manual" : "auto");
  Serial.print("\",\"polarity\":\"");
  Serial.print(relayActiveLow ? "Active-LOW" : "Active-HIGH");
  Serial.println("\"}");
}

// ==================================================
// SERIAL COMMAND HANDLER
// ==================================================

void handleSerialCommands() {
  while (Serial.available() > 0) {
    String command = Serial.readStringUntil('\n');
    command.trim();

    if (command.length() == 0) {
      continue;
    }

    // 1. FAN OFF COMMANDS
    if (
      command.equalsIgnoreCase("FAN_OFF") ||
      command == "0" ||
      command.indexOf("FAN_OFF") != -1 ||
      command.indexOf("\"fanState\":false") != -1 ||
      command.indexOf("\"fanState\": false") != -1
    ) {
      manualMode = true;
      setFanState(false);
      Serial.println("[MANUAL] FAN OFF - RELAY DE-ENERGIZED");
      printJsonState();
    }

    // 2. FAN ON COMMANDS
    else if (
      command.equalsIgnoreCase("FAN_ON") ||
      command == "1" ||
      command.indexOf("FAN_ON") != -1 ||
      command.indexOf("\"fanState\":true") != -1 ||
      command.indexOf("\"fanState\": true") != -1
    ) {
      manualMode = true;
      setFanState(true);
      Serial.println("[MANUAL] FAN ON - RELAY ENERGIZED");
      printJsonState();
    }

    // 3. AUTOMATIC MODE
    else if (
      command.equalsIgnoreCase("MODE_AUTO") ||
      command.indexOf("MODE_AUTO") != -1 ||
      command.indexOf("\"fanMode\":\"auto\"") != -1 ||
      command.indexOf("\"fanMode\": \"auto\"") != -1
    ) {
      manualMode = false;
      Serial.println("[MODE] AUTOMATIC MODE ENABLED");
      printJsonState();
    }

    // 4. MANUAL MODE
    else if (
      command.equalsIgnoreCase("MODE_MANUAL") ||
      command.indexOf("MODE_MANUAL") != -1 ||
      command.indexOf("\"fanMode\":\"manual\"") != -1 ||
      command.indexOf("\"fanMode\": \"manual\"") != -1
    ) {
      manualMode = true;
      Serial.println("[MODE] MANUAL MODE ENABLED");
      printJsonState();
    }

    // 5. STATUS / PING REQUEST
    else if (
      command.equalsIgnoreCase("STATUS") ||
      command.equalsIgnoreCase("GET_STATUS")
    ) {
      Serial.println("[STATUS] Heartbeat acknowledged");
      printJsonState();
    }

    // 6. DYNAMIC POLARITY TOGGLE (Safety Fallback)
    else if (command.equalsIgnoreCase("POLARITY_LOW") || command.equalsIgnoreCase("ACTIVE_LOW")) {
      relayActiveLow = true;
      setFanState(fanState);
      Serial.println("[CONFIG] Polarity set to Active-LOW (LOW=ON, HIGH=OFF)");
      printJsonState();
    }
    else if (command.equalsIgnoreCase("POLARITY_HIGH") || command.equalsIgnoreCase("ACTIVE_HIGH")) {
      relayActiveLow = false;
      setFanState(fanState);
      Serial.println("[CONFIG] Polarity set to Active-HIGH (HIGH=ON, LOW=OFF)");
      printJsonState();
    }

    else {
      Serial.print("[ERROR] Unknown command: ");
      Serial.println(command);
    }
  }
}

// ==================================================
// SENSOR READING FUNCTION
// ==================================================

void readSensor() {
  float newTemperature = dht.readTemperature();
  float newHumidity = dht.readHumidity();

  if (isnan(newTemperature) || isnan(newHumidity)) {
    Serial.println("[ERROR] DHT11 SENSOR NOT RESPONDING");
    return;
  }

  temperature = newTemperature;
  humidity = newHumidity;

  // AUTOMATIC FAN CONTROL (Only when in Auto Mode)
  if (!manualMode) {
    if (temperature >= TEMP_THRESHOLD_ON && !fanState) {
      setFanState(true);
      Serial.println("[AUTO] Temperature >= 30.0 °C -> FAN TURNED ON");
    } else if (temperature <= TEMP_THRESHOLD_OFF && fanState) {
      setFanState(false);
      Serial.println("[AUTO] Temperature <= 28.0 °C -> FAN TURNED OFF");
    }
  }

  // Telemetry Output (Both Human Readable and JSON)
  Serial.println("--------------------------------");
  Serial.print("Temperature: ");
  Serial.print(temperature, 1);
  Serial.println(" °C");

  Serial.print("Humidity: ");
  Serial.print(humidity, 1);
  Serial.println(" %");

  Serial.print("Fan Status: ");
  Serial.println(fanState ? "ON" : "OFF");

  Serial.print("Operating Mode: ");
  Serial.println(manualMode ? "MANUAL" : "AUTOMATIC");
  Serial.println("--------------------------------");

  // Output JSON for dashboard
  printJsonState();
}

// ==================================================
// SETUP
// ==================================================

void setup() {
  Serial.begin(115200);

  // Configure relay safely to OFF on power-up
  digitalWrite(RELAY_PIN, RELAY_OFF);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF);

  // Initialize DHT11
  dht.begin();

  Serial.println();
  Serial.println("================================");
  Serial.println(" ESP32 SMART FAN CONTROLLER");
  Serial.println("================================");
  Serial.println("System Started Successfully");
  Serial.println("DHT11: GPIO 4");
  Serial.println("Relay: GPIO 26");
  Serial.print("Relay Polarity: ");
  Serial.println(relayActiveLow ? "ACTIVE LOW" : "ACTIVE HIGH (HIGH=ON, LOW=OFF)");
  Serial.println("Initial Fan State: OFF (LOW)");
  Serial.println("Initial Mode: AUTOMATIC");
  Serial.println("================================");
}

// ==================================================
// MAIN LOOP
// ==================================================

void loop() {
  // 1. Process incoming commands immediately
  handleSerialCommands();

  // 2. Read sensor periodically
  unsigned long currentTime = millis();
  if (currentTime - lastSensorReadTime >= SENSOR_INTERVAL) {
    lastSensorReadTime = currentTime;
    readSensor();
  }

  // 3. Small delay
  delay(20);
}
