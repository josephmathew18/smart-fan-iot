# 🌀 SmartFan IoT - ESP32 Smart Fan Controller Dashboard

A modern, responsive, high-performance web dashboard for monitoring indoor climate and remotely controlling a 12V fan via an **ESP32 microcontroller**, a **DHT11 temperature & humidity sensor**, and a **relay module**.

Built with pure **HTML5, CSS3, and JavaScript (ES6+)**, utilizing **Chart.js** for live environmental telemetry and **Font Awesome 6** for crisp IoT iconography.

---

## 📸 Features

- **Real-Time Sensor Telemetry**: Displays live room temperature in °C and relative humidity in % with dynamic comfort badges (*Optimal*, *Warm*, *Dry*, *Humid*).
- **Interactive Fan Visualization**: High-speed multi-blade animated SVG fan graphic that spins continuously when the fan is energized and stops when the relay is idle.
- **Manual Relay Control**: High-contrast, glowing **Fan ON** and **Fan OFF** control buttons with loading spinners, active state indicators, and disable states.
- **Real-Time Telemetry Chart**: Responsive dual-axis time-series chart powered by Chart.js with pause/resume and history clearing controls.
- **ESP32 Wi-Fi & Diagnostic Metrics**: Displays ESP32 connection status, Wi-Fi RSSI signal strength (in dBm), HTTP round-trip latency (ping), free heap memory, and board uptime.
- **Graceful Error Handling & Real Status**: Strictly does not mock sensor values or fake success when the ESP32 is offline; displays clear **Disconnected** states and diagnostic alerts.
- **Settings & Persistence**: Configurable target IP/Hostname, polling rate (1s to 5s), and timeout threshold, saved across browser sessions via `localStorage`.
- **Integrated Hardware Guide**: Built-in modal with pinout wiring diagrams and copy-pasteable Arduino C++ firmware.
- **Optional Preview / Demo Mode**: Toggle switch in settings to safely test all dashboard UI animations without physical hardware.

---

## 📁 Project Structure

```text
smart-fan/
├── app.py               # Python Flask backend server with PostgreSQL auto-provisioning
├── .env                 # Database credentials & port configuration
├── requirements.txt     # Python dependencies (flask, psycopg2-binary, requests)
├── index.html           # Main dashboard structure & layout with DB controls
├── style.css            # Custom responsive styling & cyber-IoT theme
├── script.js            # Dashboard logic, HTTP REST API client, & Chart.js
├── esp32_smart_fan.ino  # Ready-to-flash Arduino C++ sketch for ESP32
└── README.md            # Complete project documentation & setup guide
```

---

## 🔌 Hardware Circuit & Pinout

| Component | Pin | ESP32 Pin | Purpose |
| :--- | :--- | :--- | :--- |
| **DHT11 Sensor** | VCC (+) | **3.3V** or **5V** | Power supply |
| **DHT11 Sensor** | GND (-) | **GND** | Ground reference |
| **DHT11 Sensor** | DATA (Out) | **GPIO 4** | Digital 1-wire data communication |
| **Relay Module** | VCC (+) | **5V / VIN** | Relay coil power |
| **Relay Module** | GND (-) | **GND** | Ground reference |
| **Relay Module** | IN (Signal) | **GPIO 26** | Active-HIGH switching trigger |
| **Status LED** | Anode (+) | **GPIO 2** | Built-in ESP32 status indicator |
| **12V DC Fan** | 12V (+) | **Relay NO / COM** | Switched by relay contacts |

> [!TIP]
> If using a 3-pin DHT11 module with a built-in resistor, connect DATA directly to GPIO 4. If using a bare 4-pin DHT11 sensor, place a 10kΩ pull-up resistor between VCC and the DATA pin.

---

## 💻 ESP32 Firmware Setup

### 1. Requirements in Arduino IDE
1. Open **Arduino IDE**.
2. Go to **Tools > Board > Boards Manager...**, search for `esp32` by Espressif Systems, and install it.
3. Go to **Sketch > Include Library > Manage Libraries...** and install:
   - **DHT sensor library** by Adafruit
   - **Adafruit Unified Sensor** by Adafruit

### 2. Flashing the Code
1. Open [`esp32_smart_fan.ino`](file:///c:/Users/mathu/OneDrive/Desktop/smart%20fan/esp32_smart_fan.ino) in Arduino IDE.
2. Update your Wi-Fi credentials:
   ```cpp
   const char* ssid     = "YOUR_WIFI_SSID";      // Must be 2.4GHz network
   const char* password = "YOUR_WIFI_PASSWORD";
   ```
3. Connect your ESP32 board to your PC via USB cable.
4. Select your board (e.g., `DOIT ESP32 DEVKIT V1`) and COM Port under **Tools > Port**.
5. Click **Upload**.
6. Open the **Serial Monitor** at **115200 baud**.
7. Note down the assigned IP address shown in the output:
   ```text
   [WIFI] Connected successfully!
   [WIFI] IP Address: 192.168.1.50
   [HTTP] REST API Server listening on port 80
   ```

---

## 🌐 ESP32 HTTP REST API Specification

The dashboard communicates directly with the ESP32 via HTTP:

### 1. `GET /api/status`
Retrieves live sensor telemetry and controller diagnostics.
- **Response Format**: `application/json`
- **Example Response**:
  ```json
  {
    "status": "ok",
    "temperature": 27.5,
    "humidity": 58,
    "fan": true,
    "wifi_rssi": -62,
    "uptime": 1420,
    "free_heap": 182400
  }
  ```

### 2. `GET /api/fan?state=on` or `GET /api/fan?state=off`
Controls the relay and updates the fan state.
- **Parameters**: `state=on` or `state=off`
- **Response Format**: `application/json`
- **Example Response**:
  ```json
  {
    "status": "ok",
    "fan": true,
    "message": "Fan is now ON"
  }
  ```

### 3. Cross-Origin Resource Sharing (CORS)
The ESP32 firmware includes CORS response headers:
```cpp
server.sendHeader("Access-Control-Allow-Origin", "*");
server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
```
This allows modern web browsers to send HTTP requests to the ESP32 without cross-origin security blocks.

---

## 🚀 How to Run the System

### Option 1: Run with Python Backend (Recommended)
Running with the Python Flask backend gives you persistent SQLite logging, historical chart memory, CSV data export, and automatic proxying to your ESP32:

1. Install dependencies (if not already installed):
   ```powershell
   pip install -r requirements.txt
   ```
2. Start the Python server:
   ```powershell
   python app.py
   ```
3. Open your browser at **[http://localhost:5000](http://localhost:5000)**.
4. Enter your ESP32 IP address in the settings and click **Save & Connect**.

### Option 2: VS Code Live Server
1. Open this project folder in Visual Studio Code.
2. Right-click [`index.html`](file:///c:/Users/mathu/OneDrive/Desktop/smart%20fan/index.html) and select **Open with Live Server**.

### Option 2: Python Local HTTP Server
Run this command inside the project directory:
```powershell
python -m http.server 8000
```
Then open [http://localhost:8000](http://localhost:8000) in any web browser.

### Option 3: Direct Browser File Opening
You can double-click [`index.html`](file:///c:/Users/mathu/OneDrive/Desktop/smart%20fan/index.html) to open it in Chrome, Edge, Firefox, or Safari.

### Connecting to Your ESP32:
1. Ensure your laptop/phone is connected to the **same Wi-Fi network** as the ESP32.
2. In the dashboard's **Network Settings** section, enter the ESP32 IP address (e.g., `192.168.1.50`).
3. Click **Save & Connect**.
4. The dashboard status indicator turns green, sensor cards update, and the **Fan ON / Fan OFF** buttons become active!

---

## 🛠️ Troubleshooting

- **Status Shows "Disconnected" / Cannot Reach ESP32**:
  - Verify your device and ESP32 are on the **exact same Wi-Fi subnet**.
  - Check the Arduino Serial Monitor to confirm the ESP32 connected and received an IP.
  - Verify that your router doesn't enable "AP Isolation" or "Guest Mode" (which blocks devices on the same Wi-Fi from talking to each other).
- **Fan Buttons Don't Click**:
  - The buttons are automatically disabled when disconnected from the ESP32 to prevent deceptive mock states. Ensure the connection dot is green.
- **Testing Without Hardware**:
  - Enable **Simulation / Preview Mode** in the Settings panel to preview the animated fan propeller, toast notifications, and telemetry chart immediately.

