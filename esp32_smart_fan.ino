#include <DHT.h>

// ==============================================================================
// ESP32 SMART FAN CONTROLLER - PRODUCTION FIRMWARE
// Hardware Configuration:
// - DHT11 Data Pin       : GPIO 4
// - Relay Control Pin    : GPIO 26
// - Relay Type           : Active-LOW 5V Relay Module (LOW = ON, HIGH = OFF)
// - Baud Rate            : 115200 baud (COM9)
// ==============================================================================

// ---------------- PIN CONFIGURATION ----------------
#define DHT_PIN     4
#define DHT_TYPE    DHT11
#define RELAY_PIN   26

// Active-LOW Relay Logic:
// LOW  = Relay Energized / Contact Closed -> 12V Fan ON
// HIGH = Relay De-energized / Contact Open -> 12V Fan OFF
#define RELAY_ON    LOW
#define RELAY_OFF   HIGH

// ---------------- TEMPERATURE THRESHOLDS ----------------
// Automatic Mode:
// - Fan ON at 30.0°C or above
// - Fan OFF at 28.0°C or below
// - Maintain previous state between 28.0°C and 30.0°C (Deadband/Hysteresis)
const float TEMP_THRESHOLD_ON  = 30.0;
const float TEMP_THRESHOLD_OFF = 28.0;

// Sensor reading interval (2000 ms = 2 seconds for DHT11)
const unsigned long SENSOR_INTERVAL = 2000;

// ---------------- OBJECTS & STATE VARIABLES ----------------
DHT dht(DHT_PIN, DHT_TYPE);

bool fanState = false;          // false = OFF, true = ON
bool manualMode = false;        // false = AUTOMATIC, true = MANUAL

float temperature = NAN;
float humidity = NAN;

unsigned long lastSensorReadTime = 0;

// ==============================================================================
// FAN HARDWARE CONTROL FUNCTION
// Sets the physical GPIO 26 pin immediately
// ==============================================================================
void setFanState(bool enabled) {
  fanState = enabled;
  digitalWrite(RELAY_PIN, enabled ? RELAY_ON : RELAY_OFF);
}

// ==============================================================================
// ACKNOWLEDGEMENT EMITTER
// Sends immediate JSON and text confirmation of hardware execution to Serial
// ==============================================================================
void sendAck(const char* cmd, bool state, const char* pinLevel, const char* mode) {
  // 1. Machine-readable JSON acknowledgement for Flask backend
  Serial.print("{\"ack\":true,\"status\":\"ok\",\"command\":\"");
  Serial.print(cmd);
  Serial.print("\",\"fanState\":");
  Serial.print(state ? "true" : "false");
  Serial.print(",\"relayState\":");
  Serial.print(state ? "true" : "false");
  Serial.print(",\"relayPhysicalLevel\":\"");
  Serial.print(pinLevel);
  Serial.print("\",\"relayPolarity\":\"Active-LOW\"");
  Serial.print(",\"fanMode\":\"");
  Serial.print(mode);
  Serial.print("\",\"temperature\":");
  if (isnan(temperature)) Serial.print("null"); else Serial.print(temperature, 1);
  Serial.print(",\"humidity\":");
  if (isnan(humidity)) Serial.print("null"); else Serial.print(humidity, 1);
  Serial.println("}");

  // 2. Human-readable confirmation log
  Serial.print("[ACK] ");
  Serial.print(cmd);
  Serial.print(": Fan ");
  Serial.print(state ? "ON" : "OFF");
  Serial.print(" | GPIO26=");
  Serial.print(pinLevel);
  Serial.print(" | Mode=");
  Serial.println(mode);
}

// ==============================================================================
// SERIAL COMMAND PROCESSOR
// Handles commands from Flask backend over COM9 @ 115200 baud
// ==============================================================================
void handleSerialCommands() {
  while (Serial.available() > 0) {
    String command = Serial.readStringUntil('\n');
    command.trim();

    if (command.length() == 0) {
      continue;
    }

    // ---------------- 1. FAN OFF (Manual Mode) ----------------
    // User clicked FAN OFF:
    // - manualMode set to true
    // - GPIO 26 set HIGH immediately (deactivates Active-LOW relay)
    // - 12V fan power disconnected
    // - Returns acknowledgement to Flask
    if (
      command.equalsIgnoreCase("FAN_OFF") ||
      command == "0" ||
      command.indexOf("FAN_OFF") != -1 ||
      command.indexOf("\"fanState\":false") != -1 ||
      command.indexOf("\"fanState\": false") != -1
    ) {
      manualMode = true;
      setFanState(false); // GPIO 26 -> HIGH
      sendAck("fanControl", false, "HIGH", "manual");
    }

    // ---------------- 2. FAN ON (Manual Mode) ----------------
    // User clicked FAN ON:
    // - manualMode set to true
    // - GPIO 26 set LOW immediately (activates Active-LOW relay)
    // - 12V fan power connected
    // - Returns acknowledgement to Flask
    else if (
      command.equalsIgnoreCase("FAN_ON") ||
      command == "1" ||
      command.indexOf("FAN_ON") != -1 ||
      command.indexOf("\"fanState\":true") != -1 ||
      command.indexOf("\"fanState\": true") != -1
    ) {
      manualMode = true;
      setFanState(true); // GPIO 26 -> LOW
      sendAck("fanControl", true, "LOW", "manual");
    }

    // ---------------- 3. AUTOMATIC MODE ----------------
    // Switch to Automatic Mode:
    // Evaluates current temperature against thresholds immediately
    else if (
      command.equalsIgnoreCase("MODE_AUTO") ||
      command.indexOf("MODE_AUTO") != -1 ||
      command.indexOf("\"fanMode\":\"auto\"") != -1 ||
      command.indexOf("\"fanMode\": \"auto\"") != -1
    ) {
      manualMode = false;
      if (!isnan(temperature)) {
        if (temperature >= TEMP_THRESHOLD_ON && !fanState) {
          setFanState(true); // GPIO 26 -> LOW
        } else if (temperature <= TEMP_THRESHOLD_OFF && fanState) {
          setFanState(false); // GPIO 26 -> HIGH
        }
      }
      sendAck("modeControl", fanState, (digitalRead(RELAY_PIN) == LOW) ? "LOW" : "HIGH", "auto");
    }

    // ---------------- 4. MANUAL MODE ----------------
    // Switch to Manual Mode:
    // Freezes current fan state; disables temperature overrides
    else if (
      command.equalsIgnoreCase("MODE_MANUAL") ||
      command.indexOf("MODE_MANUAL") != -1 ||
      command.indexOf("\"fanMode\":\"manual\"") != -1 ||
      command.indexOf("\"fanMode\": \"manual\"") != -1
    ) {
      manualMode = true;
      sendAck("modeControl", fanState, (digitalRead(RELAY_PIN) == LOW) ? "LOW" : "HIGH", "manual");
    }

    // ---------------- 5. STATUS / HEARTBEAT PING ----------------
    else if (
      command.equalsIgnoreCase("STATUS") ||
      command.equalsIgnoreCase("GET_STATUS")
    ) {
      sendAck("status", fanState, (digitalRead(RELAY_PIN) == LOW) ? "LOW" : "HIGH", manualMode ? "manual" : "auto");
    }

    else {
      Serial.print("[WARN] Unknown serial command: ");
      Serial.println(command);
    }
  }
}

// ==============================================================================
// SENSOR READING & AUTOMATIC CONTROL
// ==============================================================================
void readSensor() {
  float newTemp = dht.readTemperature();
  float newHum = dht.readHumidity();

  if (isnan(newTemp) || isnan(newHum)) {
    Serial.println("[ERROR] DHT11 SENSOR NOT RESPONDING");
    return;
  }

  temperature = newTemp;
  humidity = newHum;

  // AUTOMATIC MODE:
  // - Fan ON at 30°C or above.
  // - Fan OFF at 28°C or below.
  // - Maintain previous state between thresholds.
  //
  // MANUAL MODE:
  // - Automatic temperature control must NEVER override manual commands.
  if (!manualMode) {
    if (temperature >= TEMP_THRESHOLD_ON && !fanState) {
      setFanState(true); // GPIO 26 -> LOW
      Serial.println("[AUTO] Temp >= 30.0°C -> Fan turned ON (GPIO26 LOW)");
    } else if (temperature <= TEMP_THRESHOLD_OFF && fanState) {
      setFanState(false); // GPIO 26 -> HIGH
      Serial.println("[AUTO] Temp <= 28.0°C -> Fan turned OFF (GPIO26 HIGH)");
    }
  }

  // 1. JSON Telemetry for Dashboard backend
  Serial.print("{\"temperature\":");
  Serial.print(temperature, 1);
  Serial.print(",\"humidity\":");
  Serial.print(humidity, 1);
  Serial.print(",\"fanState\":");
  Serial.print(fanState ? "true" : "false");
  Serial.print(",\"relayState\":");
  Serial.print(fanState ? "true" : "false");
  Serial.print(",\"relayPhysicalLevel\":\"");
  Serial.print((digitalRead(RELAY_PIN) == LOW) ? "LOW" : "HIGH");
  Serial.print("\",\"relayPolarity\":\"Active-LOW\"");
  Serial.print(",\"fanMode\":\"");
  Serial.print(manualMode ? "manual" : "auto");
  Serial.println("\"}");

  // 2. Human-readable diagnostic output
  Serial.println("--------------------------------");
  Serial.print("Temperature: ");
  Serial.print(temperature, 1);
  Serial.println(" °C");

  Serial.print("Humidity: ");
  Serial.print(humidity, 1);
  Serial.println(" %");

  Serial.print("Fan Status: ");
  Serial.println(fanState ? "ON (GPIO26=LOW, 12V ACTIVE)" : "OFF (GPIO26=HIGH, 12V CUT)");

  Serial.print("Operating Mode: ");
  Serial.println(manualMode ? "MANUAL (Auto overridden)" : "AUTOMATIC");
  Serial.println("--------------------------------");
}

// ==============================================================================
// ARDUINO SETUP
// ==============================================================================
void setup() {
  Serial.begin(115200);

  // Active-LOW relay safety: set HIGH before OUTPUT to avoid power-on fan glitch
  digitalWrite(RELAY_PIN, RELAY_OFF);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF);

  // Initialize DHT11 sensor
  dht.begin();

  Serial.println();
  Serial.println("==================================================");
  Serial.println("   ESP32 SMART FAN CONTROLLER - FIRMWARE READY    ");
  Serial.println("==================================================");
  Serial.println("DHT11 Data Pin   : GPIO 4");
  Serial.println("Relay Output Pin : GPIO 26");
  Serial.println("Relay Type       : Active-LOW 5V Relay");
  Serial.println("Initial Fan State: OFF (GPIO 26 = HIGH)");
  Serial.println("Initial Mode     : AUTOMATIC");
  Serial.println("==================================================");
}

// ==============================================================================
// MAIN LOOP
// ==============================================================================
void loop() {
  // 1. Process incoming commands from Serial immediately
  handleSerialCommands();

  // 2. Read sensor and apply automatic control periodically
  unsigned long currentTime = millis();
  if (currentTime - lastSensorReadTime >= SENSOR_INTERVAL) {
    lastSensorReadTime = currentTime;
    readSensor();
  }

  // 3. Small loop delay for CPU stability
  delay(15);
}
