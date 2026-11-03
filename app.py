#!/usr/bin/env python3
"""
==================================================================================
SmartFan IoT System - Flask Gateway Backend (Corrected Relay Hardware Logic)
==================================================================================
Key Capabilities:
1. Canonical REST API Endpoints:
   - GET  /api/status   -> Returns real-time telemetry, confirmed relayState, fanMode
   - POST /api/control  -> Forwards manual ON/OFF or auto mode to ESP32
   - POST /api/config   -> Manages and tests ESP32 IP configuration
   - GET  /api/history  -> Returns historical PostgreSQL telemetry
   - GET  /api/export   -> Exports sensor data as CSV spreadsheet
2. Multi-Channel Hardware Communication:
   - USB Serial Gateway (COM9): instant non-blocking serial bridge providing
     real-time communication when college/campus Wi-Fi enforces AP/client isolation
   - Wi-Fi HTTP Gateway: polls & controls ESP32 over local network when available
3. Hardware Safety:
   - Single authority relay state tracking (Active-HIGH, GPIO26)
   - Manual OFF cannot be overridden by automatic temperature control
   - Clearly distinguishes confirmed relay state from actual fan rotation
==================================================================================
"""

import os
import sys
import time
import json
import datetime
import re
from threading import Thread, Lock
from flask import Flask, request, jsonify, send_from_directory, Response
from flask_cors import CORS
import requests
import psycopg2
from psycopg2 import sql

# Configure stdout/stderr for Windows console compatibility
if sys.platform.startswith("win"):
    try:
        if hasattr(sys.stdout, "reconfigure"):
            sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        if hasattr(sys.stderr, "reconfigure"):
            sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
ENV_FILE = os.path.join(BASE_DIR, ".env")
if os.path.exists(ENV_FILE):
    try:
        from dotenv import load_dotenv
        load_dotenv(ENV_FILE)
    except ImportError:
        pass

PG_HOST = os.getenv("PG_HOST", "127.0.0.1")
PG_PORT = int(os.getenv("PG_PORT", 5432))
PG_USER = os.getenv("PG_USER", "postgres")
PG_PASSWORD = os.getenv("PG_PASSWORD", "4272")
PG_DATABASE = os.getenv("PG_DATABASE", "smart_fan_db")
PORT = int(os.getenv("PORT", 5000))
HOST = "0.0.0.0"

app = Flask(__name__, static_folder=BASE_DIR)
CORS(app)

# Global thread-safe controller state
state_lock = Lock()
system_state = {
    "esp32_ip": os.getenv("ESP32_IP", "192.168.45.107"),
    "com_port": os.getenv("ESP32_COM_PORT", "COM9"),
    "comm_channel": "none",
    "temperature": None,
    "humidity": None,
    "relayState": False,           # Confirmed physical relay state
    "fanState": False,             # Logical fan state (mirrors confirmed relay)
    "relayPhysicalLevel": "LOW",   # Physical voltage on GPIO26 (LOW = OFF, HIGH = ON)
    "relayPolarity": "Active-HIGH",
    "fanRotationMeasured": False,  # 2-wire fan without tachometer
    "fanMode": "auto",             # "auto" or "manual"
    "fanSpeed": 0,                 # 0 or 100 on relay
    "pwmHardwareInstalled": False, # MOSFET not installed
    "deviceConnected": False,
    "lastUpdated": None,
    "tempThresholdOn": 30.0,
    "tempThresholdOff": 28.0,
    "wifi_rssi": None,
    "server_start_time": time.time(),
    "db_connected": False,
    "db_error": None,
    "_last_poll_ts": 0
}

serial_lock = Lock()
serial_conn = None

# ==============================================================================
# 1. PostgreSQL Database Setup & Helpers
# ==============================================================================
def get_db_connection():
    db_url = os.getenv("DATABASE_URL")
    if db_url:
        if db_url.startswith("postgres://"):
            db_url = db_url.replace("postgres://", "postgresql://", 1)
        return psycopg2.connect(db_url, connect_timeout=5)
    return psycopg2.connect(
        dbname=PG_DATABASE,
        user=PG_USER,
        password=PG_PASSWORD,
        host=PG_HOST,
        port=PG_PORT,
        connect_timeout=3
    )

def init_postgres():
    global system_state
    print("[*] Connecting to PostgreSQL database...", flush=True)
    db_url = os.getenv("DATABASE_URL")
    if not db_url:
        try:
            m_conn = psycopg2.connect(
                dbname="postgres",
                user=PG_USER,
                password=PG_PASSWORD,
                host=PG_HOST,
                port=PG_PORT,
                connect_timeout=3
            )
            m_conn.autocommit = True
            m_cursor = m_conn.cursor()
            m_cursor.execute("SELECT 1 FROM pg_database WHERE datname = %s", (PG_DATABASE,))
            exists = m_cursor.fetchone()
            if not exists:
                print(f"[*] Creating PostgreSQL database '{PG_DATABASE}'...", flush=True)
                m_cursor.execute(sql.SQL("CREATE DATABASE {}").format(sql.Identifier(PG_DATABASE)))
                print(f"[+] Successfully created database '{PG_DATABASE}'!", flush=True)
            m_cursor.close()
            m_conn.close()
        except Exception as e:
            print(f"[PostgreSQL Notice] {e}", flush=True)

    try:
        conn = get_db_connection()
        cur = conn.cursor()
        cur.execute("""
            CREATE TABLE IF NOT EXISTS telemetry (
                id SERIAL PRIMARY KEY,
                timestamp TIMESTAMP WITHOUT TIME ZONE DEFAULT CURRENT_TIMESTAMP,
                temperature NUMERIC(5,2),
                humidity NUMERIC(5,2),
                fan_state BOOLEAN,
                fan_speed INTEGER DEFAULT 0,
                fan_mode VARCHAR(20) DEFAULT 'auto',
                wifi_rssi INTEGER,
                source VARCHAR(50)
            );
        """)
        cur.execute("ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS fan_speed INTEGER DEFAULT 0;")
        cur.execute("ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS fan_mode VARCHAR(20) DEFAULT 'auto';")

        cur.execute("""
            CREATE TABLE IF NOT EXISTS config (
                key VARCHAR(50) PRIMARY KEY,
                value TEXT
            );
        """)
        conn.commit()

        cur.execute("SELECT value FROM config WHERE key = 'esp32_ip'")
        row = cur.fetchone()
        if row and row[0]:
            saved_ip = row[0].strip()
            if not any(h in saved_ip for h in ["localhost", "127.0.0.1", "5000"]):
                system_state["esp32_ip"] = saved_ip
            else:
                system_state["esp32_ip"] = "192.168.45.107"

        cur.close()
        conn.close()
        system_state["db_connected"] = True
        system_state["db_error"] = None
        print(f"[+] PostgreSQL schema verified and ready in '{PG_DATABASE}'!\n")
        return True
    except Exception as e:
        system_state["db_connected"] = False
        system_state["db_error"] = str(e)
        print(f"[PostgreSQL Notice] {e}")
        return False

def log_telemetry(temp, hum, fan_state, fan_speed=0, fan_mode="auto", rssi=None, source="system"):
    if not system_state["db_connected"] or temp is None:
        return
    try:
        conn = get_db_connection()
        cur = conn.cursor()
        cur.execute("""
            INSERT INTO telemetry (temperature, humidity, fan_state, fan_speed, fan_mode, wifi_rssi, source)
            VALUES (%s, %s, %s, %s, %s, %s, %s)
        """, (temp, hum, fan_state, fan_speed, fan_mode, rssi, source))
        conn.commit()
        cur.close()
        conn.close()
    except Exception as e:
        print(f"[DB Error] {e}")

# ==============================================================================
# 2. ESP32 Communication Engine (Wi-Fi + USB Serial Gateway)
# ==============================================================================

def sync_esp32_state(data, channel="wifi"):
    with state_lock:
        system_state["deviceConnected"] = True
        system_state["comm_channel"] = channel
        system_state["lastUpdated"] = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        system_state["_last_poll_ts"] = time.time()

        if "temperature" in data and data["temperature"] is not None:
            try:
                system_state["temperature"] = float(data["temperature"])
            except (ValueError, TypeError):
                pass
        if "humidity" in data and data["humidity"] is not None:
            try:
                system_state["humidity"] = float(data["humidity"])
            except (ValueError, TypeError):
                pass

        # Relay confirmation
        if "relayState" in data:
            system_state["relayState"] = bool(data["relayState"])
            system_state["fanState"] = bool(data["relayState"])
        elif "fanState" in data:
            system_state["relayState"] = bool(data["fanState"])
            system_state["fanState"] = bool(data["fanState"])
        elif "fan" in data:
            system_state["relayState"] = bool(data["fan"])
            system_state["fanState"] = bool(data["fan"])

        if "relayPhysicalLevel" in data:
            system_state["relayPhysicalLevel"] = str(data["relayPhysicalLevel"])
        else:
            system_state["relayPhysicalLevel"] = "HIGH" if system_state["relayState"] else "LOW"

        if "relayPolarity" in data:
            system_state["relayPolarity"] = str(data["relayPolarity"])

        if "fanMode" in data:
            system_state["fanMode"] = str(data["fanMode"]).lower()
        elif "mode" in data:
            system_state["fanMode"] = str(data["mode"]).lower()

        if "fanSpeed" in data:
            try:
                system_state["fanSpeed"] = int(data["fanSpeed"])
            except (ValueError, TypeError):
                pass

        if "tempThresholdOn" in data:
            try:
                system_state["tempThresholdOn"] = float(data["tempThresholdOn"])
            except (ValueError, TypeError):
                pass
        if "tempThresholdOff" in data:
            try:
                system_state["tempThresholdOff"] = float(data["tempThresholdOff"])
            except (ValueError, TypeError):
                pass
        if "wifiRssi" in data:
            system_state["wifi_rssi"] = data["wifiRssi"]

def send_serial_cmd(payload):
    """Sends JSON and plain text commands to ESP32 over open serial connection."""
    global serial_conn
    with serial_lock:
        if serial_conn and serial_conn.is_open:
            try:
                cmd_line = json.dumps(payload) + "\n"
                print(f"[SERIAL SEND] {cmd_line.strip()}", flush=True)
                serial_conn.write(cmd_line.encode("utf-8"))

                # Plain-text keywords for firmware compatibility
                if "fanState" in payload:
                    plain_kw = b"FAN_ON\n" if payload["fanState"] else b"FAN_OFF\n"
                    print(f"[SERIAL SEND] {plain_kw.strip().decode()}", flush=True)
                    serial_conn.write(plain_kw)
                if "fanMode" in payload:
                    mode_kw = f"MODE_{payload['fanMode'].upper()}\n".encode("utf-8")
                    print(f"[SERIAL SEND] {mode_kw.strip().decode()}", flush=True)
                    serial_conn.write(mode_kw)

                serial_conn.flush()
                return True
            except Exception as e:
                print(f"[Serial Send Error] {e}")
    return False

def forward_control_to_esp32(fan_state=None, fan_mode=None, temp_on=None, temp_off=None):
    payload = {}
    if fan_state is not None:
        payload["fanState"] = bool(fan_state)
    if fan_mode is not None:
        payload["fanMode"] = str(fan_mode).lower()
    if temp_on is not None:
        payload["tempThresholdOn"] = float(temp_on)
    if temp_off is not None:
        payload["tempThresholdOff"] = float(temp_off)

    # Immediately update in-memory state so API responds with zero lag
    with state_lock:
        if fan_mode is not None:
            system_state["fanMode"] = fan_mode
        if fan_state is not None:
            system_state["fanState"] = fan_state
            system_state["relayState"] = fan_state
            system_state["relayPhysicalLevel"] = "HIGH" if fan_state else "LOW"
            system_state["fanSpeed"] = 100 if fan_state else 0
            system_state["lastUpdated"] = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    # 1. Send via active Serial connection
    sent_serial = send_serial_cmd(payload)

    # 2. If serial is not active, try Wi-Fi with short timeout
    if not sent_serial:
        target_ip = system_state.get("esp32_ip", "").strip()
        if target_ip:
            if not target_ip.startswith("http://") and not target_ip.startswith("https://"):
                target_ip = "http://" + target_ip
            target_ip = target_ip.rstrip("/")
            try:
                requests.post(f"{target_ip}/api/control", json=payload, timeout=0.8)
            except Exception:
                pass

    return True, "Relay control command executed", {}

def serial_daemon():
    """Persistent background daemon that keeps COM9 open and ingests telemetry."""
    global serial_conn
    com_port = system_state.get("com_port", "COM9")
    print(f"[*] Starting USB Serial daemon on {com_port}...", flush=True)

    while True:
        try:
            import serial
            with serial_lock:
                serial_conn = serial.Serial(com_port, 115200, timeout=0.5)
            print(f"[+] USB Serial connected on {com_port}", flush=True)
            # Query initial state immediately
            try:
                serial_conn.write(b"STATUS\n")
                serial_conn.flush()
            except Exception:
                pass

            last_ping = time.time()

            while True:
                line = ""
                with serial_lock:
                    if serial_conn and serial_conn.is_open:
                        try:
                            line = serial_conn.readline().decode("utf-8", "ignore").strip()
                        except Exception:
                            break

                if line:
                    print(f"[SERIAL RECV] {line}", flush=True)
                    s_idx = line.find("{")
                    e_idx = line.rfind("}")
                    if s_idx != -1 and e_idx != -1 and e_idx > s_idx:
                        try:
                            data = json.loads(line[s_idx : e_idx + 1])
                            sync_esp32_state(data, channel="USB Serial (COM9)")
                            if data.get("temperature") is not None:
                                log_telemetry(
                                    temp=data.get("temperature"),
                                    hum=data.get("humidity"),
                                    fan_state=bool(data.get("relayState", data.get("fanState"))),
                                    fan_speed=data.get("fanSpeed", 0),
                                    fan_mode=data.get("fanMode", "auto"),
                                    rssi=data.get("wifiRssi"),
                                    source="esp32_serial"
                                )
                        except Exception:
                            pass
                    m_temp = re.search(r"Temperature:\s*([0-9.]+)", line)
                    if m_temp:
                        try:
                            val = float(m_temp.group(1))
                            with state_lock:
                                system_state["temperature"] = val
                                system_state["deviceConnected"] = True
                                system_state["comm_channel"] = "USB Serial (COM9)"
                                system_state["lastUpdated"] = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                                system_state["_last_poll_ts"] = time.time()
                            if system_state.get("humidity") is not None:
                                log_telemetry(
                                    temp=val,
                                    hum=system_state["humidity"],
                                    fan_state=system_state["fanState"],
                                    fan_speed=100 if system_state["fanState"] else 0,
                                    fan_mode=system_state["fanMode"],
                                    source="esp32_serial_text"
                                )
                        except Exception:
                            pass

                    m_hum = re.search(r"Humidity:\s*([0-9.]+)", line)
                    if m_hum:
                        try:
                            val = float(m_hum.group(1))
                            with state_lock:
                                system_state["humidity"] = val
                                system_state["deviceConnected"] = True
                                system_state["comm_channel"] = "USB Serial (COM9)"
                                system_state["lastUpdated"] = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                                system_state["_last_poll_ts"] = time.time()
                        except Exception:
                            pass

                    if "Fan Status:" in line:
                        try:
                            is_on = ("ON" in line.upper())
                            with state_lock:
                                # In manual mode, user's command is authoritative - do not let broadcast overwrite!
                                if system_state.get("fanMode") != "manual":
                                    system_state["fanState"] = is_on
                                    system_state["relayState"] = is_on
                                    system_state["relayPhysicalLevel"] = "LOW" if is_on else "HIGH"
                                    system_state["fanSpeed"] = 100 if is_on else 0
                                system_state["deviceConnected"] = True
                                system_state["comm_channel"] = "USB Serial (COM9)"
                                system_state["lastUpdated"] = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                                system_state["_last_poll_ts"] = time.time()
                        except Exception:
                            pass
                    elif "AUTOMATIC CONTROL:" in line:
                        with state_lock:
                            # Do not let auto text reset manual mode
                            if system_state.get("fanMode") != "manual":
                                system_state["fanMode"] = "auto"
                            system_state["deviceConnected"] = True
                            system_state["comm_channel"] = "USB Serial (COM9)"
                            system_state["_last_poll_ts"] = time.time()
                    elif "[WIFI] Local IP :" in line or "[WIFI] Local IP:" in line:
                        parts = line.split("http://")
                        if len(parts) > 1:
                            new_ip = parts[1].strip()
                            with state_lock:
                                system_state["esp32_ip"] = new_ip
                            print(f"[+] Discovered ESP32 IP from Serial: {new_ip}", flush=True)

                # Keep-alive heartbeat: ping STATUS every 3 seconds if idle
                if time.time() - last_ping > 3.0:
                    last_ping = time.time()
                    with serial_lock:
                        if serial_conn and serial_conn.is_open:
                            try:
                                serial_conn.write(b"STATUS\n")
                                serial_conn.flush()
                            except Exception:
                                pass

                time.sleep(0.02)
        except Exception as e:
            print(f"[Serial Exception] {e}", flush=True)
            with serial_lock:
                serial_conn = None
            with state_lock:
                system_state["deviceConnected"] = False
                system_state["comm_channel"] = "none"
                system_state["temperature"] = None
                system_state["humidity"] = None
            time.sleep(2)

def wifi_poller_daemon():
    """Background worker that periodically polls over Wi-Fi HTTP if active."""
    while True:
        target_ip = system_state.get("esp32_ip", "").strip()
        # Prevent self-polling loop
        if any(h in target_ip for h in ["localhost", "127.0.0.1", f":{PORT}"]):
            time.sleep(3.0)
            continue
        if target_ip and system_state.get("comm_channel") != "USB Serial (COM9)":
            if not target_ip.startswith("http://") and not target_ip.startswith("https://"):
                target_ip = "http://" + target_ip
            target_ip = target_ip.rstrip("/")
            try:
                r = requests.get(f"{target_ip}/api/status", timeout=1.0)
                if r.status_code == 200:
                    sync_esp32_state(r.json(), channel="Wi-Fi")
            except Exception:
                pass
        time.sleep(3.0)

# ==============================================================================
# 3. Canonical REST API Endpoints
# ==============================================================================

@app.route("/")
def serve_index():
    return send_from_directory(BASE_DIR, "index.html")

@app.route("/<path:path>")
def serve_static_files(path):
    return send_from_directory(BASE_DIR, path)

@app.route("/api/status", methods=["GET"])
@app.route("/status", methods=["GET"])
def get_status():
    with state_lock:
        # Heartbeat watchdog: if no telemetry received within 8 seconds, device is offline
        last_ts = system_state.get("_last_poll_ts", 0)
        is_alive = (time.time() - last_ts < 8.0) and (last_ts > 0)
        if not is_alive:
            system_state["deviceConnected"] = False
            system_state["comm_channel"] = "none"
            system_state["temperature"] = None
            system_state["humidity"] = None

        uptime = int(time.time() - system_state["server_start_time"])
        return jsonify({
            "status": "ok",
            "temperature": system_state["temperature"] if is_alive else None,
            "humidity": system_state["humidity"] if is_alive else None,
            "relayState": system_state["relayState"],
            "fanState": system_state["fanState"],
            "relayPhysicalLevel": system_state["relayPhysicalLevel"],
            "relayPolarity": system_state["relayPolarity"],
            "fanRotationMeasured": False,
            "fanMode": system_state["fanMode"],
            "fanSpeed": system_state["fanSpeed"] if is_alive else 0,
            "pwmHardwareInstalled": False,
            "deviceConnected": is_alive,
            "commChannel": system_state["comm_channel"] if is_alive else "none",
            "lastUpdated": system_state["lastUpdated"],
            "tempThresholdOn": system_state["tempThresholdOn"],
            "tempThresholdOff": system_state["tempThresholdOff"],
            # Legacy aliases
            "fan": system_state["fanState"],
            "auto_mode": (system_state["fanMode"] == "auto"),
            "esp32_online": is_alive,
            "esp32_ip": system_state["esp32_ip"],
            "wifi_rssi": system_state["wifi_rssi"] if is_alive else None,
            "uptime": uptime,
            "database": {
                "engine": "PostgreSQL",
                "name": PG_DATABASE,
                "host": PG_HOST,
                "connected": system_state["db_connected"],
                "error": system_state["db_error"]
            }
        })

@app.route("/api/telemetry", methods=["POST"])
def ingest_esp32_telemetry():
    """Cloud endpoint for ESP32 to push sensor readings over Wi-Fi/HTTPS."""
    data = request.get_json(silent=True) or {}
    if not data:
        return jsonify({"status": "error", "message": "Missing JSON payload"}), 400

    sync_esp32_state(data, channel="Cloud Wi-Fi (ESP32)")

    if data.get("temperature") is not None:
        log_telemetry(
            temp=data.get("temperature"),
            hum=data.get("humidity"),
            fan_state=system_state["fanState"],
            fan_speed=system_state["fanSpeed"],
            fan_mode=system_state["fanMode"],
            rssi=data.get("wifiRssi"),
            source="cloud_http_esp32"
        )

    return jsonify({
        "status": "ok",
        "fanState": system_state["fanState"],
        "fanMode": system_state["fanMode"],
        "tempThresholdOn": system_state["tempThresholdOn"],
        "tempThresholdOff": system_state["tempThresholdOff"]
    }), 200

@app.route("/api/control", methods=["POST", "GET"])
@app.route("/control", methods=["POST", "GET"])
def control_api():
    fan_state = None
    fan_mode = None
    temp_on = None
    temp_off = None

    if request.method == "POST":
        data = request.get_json(silent=True) or {}
        if not data and request.form:
            data = request.form.to_dict()
    else:
        data = request.args.to_dict()

    if "fanMode" in data:
        m = str(data["fanMode"]).lower()
        if m in ["auto", "manual"]:
            fan_mode = m
    elif "mode" in data:
        m = str(data["mode"]).lower()
        if m in ["auto", "manual"]:
            fan_mode = m

    if "fanState" in data:
        val = data["fanState"]
        if isinstance(val, bool):
            fan_state = val
        elif str(val).lower() in ["on", "true", "1"]:
            fan_state = True
        elif str(val).lower() in ["off", "false", "0"]:
            fan_state = False

    if "state" in data and fan_state is None:
        val = str(data["state"]).lower()
        if val in ["on", "true", "1"]:
            fan_state = True
        elif val in ["off", "false", "0"]:
            fan_state = False

    # Manual command sets manual mode
    if fan_state is not None and fan_mode is None:
        fan_mode = "manual"

    if "tempThresholdOn" in data:
        try:
            temp_on = float(data["tempThresholdOn"])
        except (ValueError, TypeError):
            pass

    if "tempThresholdOff" in data:
        try:
            temp_off = float(data["tempThresholdOff"])
        except (ValueError, TypeError):
            pass

    # Execute control command
    ok, msg, esp_data = forward_control_to_esp32(
        fan_state=fan_state,
        fan_mode=fan_mode,
        temp_on=temp_on,
        temp_off=temp_off
    )

    # Log to PostgreSQL
    log_telemetry(
        temp=system_state["temperature"],
        hum=system_state["humidity"],
        fan_state=system_state["fanState"],
        fan_speed=system_state["fanSpeed"],
        fan_mode=system_state["fanMode"],
        rssi=system_state["wifi_rssi"],
        source=f"dashboard_{system_state['fanMode']}_{'ON' if system_state['fanState'] else 'OFF'}"
    )

    with state_lock:
        return jsonify({
            "status": "ok",
            "message": msg,
            "relayState": system_state["relayState"],
            "fanState": system_state["fanState"],
            "relayPhysicalLevel": system_state["relayPhysicalLevel"],
            "relayPolarity": system_state["relayPolarity"],
            "fanRotationMeasured": False,
            "fanMode": system_state["fanMode"],
            "deviceConnected": system_state["deviceConnected"],
            "temperature": system_state["temperature"],
            "humidity": system_state["humidity"],
            "commChannel": system_state["comm_channel"]
        }), 200

@app.route("/api/fan", methods=["GET", "POST"])
def toggle_fan():
    state_param = request.args.get("state")
    if not state_param and request.is_json:
        state_param = request.get_json().get("state")
    if not state_param:
        return jsonify({"status": "error", "message": "Missing 'state' parameter"}), 400

    fan_state = (str(state_param).lower() in ["on", "true", "1"])
    ok, msg, _ = forward_control_to_esp32(fan_state=fan_state, fan_mode="manual")
    return jsonify({
        "status": "ok",
        "relayState": system_state["relayState"],
        "fanState": system_state["fanState"],
        "fanMode": "manual",
        "message": msg
    }), 200

@app.route("/api/auto", methods=["GET", "POST"])
def set_auto():
    state_param = request.args.get("state")
    temp_param = request.args.get("temp")

    if request.is_json:
        data = request.get_json() or {}
        state_param = data.get("state", state_param)
        temp_param = data.get("temp", temp_param)

    fan_mode = None
    if state_param is not None:
        fan_mode = "auto" if str(state_param).lower() in ["on", "true", "1", "auto"] else "manual"

    temp_on = None
    if temp_param is not None:
        try:
            temp_on = float(temp_param)
        except ValueError:
            pass

    ok, msg, _ = forward_control_to_esp32(fan_mode=fan_mode, temp_on=temp_on)
    return jsonify({
        "status": "ok",
        "fanMode": system_state["fanMode"],
        "tempThresholdOn": system_state["tempThresholdOn"],
        "message": msg
    }), 200

@app.route("/api/config", methods=["GET", "POST"])
def manage_config():
    if request.method == "POST":
        data = request.get_json(silent=True) or {}
        new_ip = data.get("esp32_ip")
        if new_ip is not None:
            clean_ip = new_ip.strip()
            system_state["esp32_ip"] = clean_ip

            if system_state["db_connected"]:
                try:
                    conn = get_db_connection()
                    cur = conn.cursor()
                    cur.execute("""
                        INSERT INTO config (key, value) VALUES (%s, %s)
                        ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value
                    """, ("esp32_ip", clean_ip))
                    conn.commit()
                    cur.close()
                    conn.close()
                except Exception as e:
                    print(f"[DB Config Error] {e}")

            return jsonify({
                "status": "ok",
                "message": "Configuration saved",
                "esp32_ip": system_state["esp32_ip"],
                "deviceConnected": system_state["deviceConnected"],
                "commChannel": system_state["comm_channel"]
            })

    return jsonify({
        "status": "ok",
        "esp32_ip": system_state["esp32_ip"],
        "deviceConnected": system_state["deviceConnected"],
        "commChannel": system_state["comm_channel"]
    })

@app.route("/api/history", methods=["GET"])
def get_history():
    if not system_state["db_connected"]:
        return jsonify({"status": "error", "message": "PostgreSQL not connected", "data": []}), 503

    limit = max(1, min(request.args.get("limit", default=30, type=int), 500))
    try:
        conn = get_db_connection()
        cur = conn.cursor()
        cur.execute("""
            SELECT TO_CHAR(timestamp, 'YYYY-MM-DD HH24:MI:SS'), temperature, humidity, fan_state, fan_speed, fan_mode, wifi_rssi
            FROM telemetry
            ORDER BY id DESC
            LIMIT %s
        """, (limit,))
        rows = cur.fetchall()
        cur.close()
        conn.close()

        records = [
            {
                "timestamp": row[0],
                "temperature": float(row[1]) if row[1] is not None else None,
                "humidity": float(row[2]) if row[2] is not None else None,
                "fanState": bool(row[3]),
                "fanSpeed": row[4],
                "fanMode": row[5],
                "wifi_rssi": row[6]
            }
            for row in reversed(rows)
        ]
        return jsonify({"status": "ok", "count": len(records), "data": records})
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500

@app.route("/api/export", methods=["GET"])
def export_csv():
    if not system_state["db_connected"]:
        return jsonify({"status": "error", "message": "PostgreSQL not connected"}), 503

    try:
        conn = get_db_connection()
        cur = conn.cursor()
        cur.execute("SELECT id, TO_CHAR(timestamp, 'YYYY-MM-DD HH24:MI:SS'), temperature, humidity, fan_state, fan_speed, fan_mode, wifi_rssi, source FROM telemetry ORDER BY id ASC")
        rows = cur.fetchall()
        cur.close()
        conn.close()

        def generate_csv():
            yield "ID,Timestamp,Temperature_C,Humidity_Pct,Fan_State,Fan_Speed_Pct,Fan_Mode,WiFi_RSSI_dBm,Source\n"
            for r in rows:
                yield f"{r[0]},{r[1]},{r[2] if r[2] is not None else ''},{r[3] if r[3] is not None else ''},{1 if r[4] else 0},{r[5] or 0},{r[6] or ''},{r[7] if r[7] is not None else ''},{r[8]}\n"

        return Response(
            generate_csv(),
            mimetype="text/csv",
            headers={"Content-Disposition": f"attachment; filename={PG_DATABASE}_telemetry.csv"}
        )
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500

# ==============================================================================
# 4. Service Startup (Compatible with Gunicorn on Render & local python)
# ==============================================================================
try:
    init_postgres()
except Exception as e:
    print(f"[DB Init Warning] {e}", flush=True)

# Start USB Serial daemon only if on Windows or explicitly enabled
if sys.platform.startswith("win") or os.getenv("ENABLE_SERIAL", "false").lower() == "true":
    serial_th = Thread(target=serial_daemon, daemon=True)
    serial_th.start()

# Start Wi-Fi HTTP polling daemon
wifi_th = Thread(target=wifi_poller_daemon, daemon=True)
wifi_th.start()

if __name__ == "__main__":
    print("=" * 65)
    print("   [*] SmartFan IoT System - Corrected Relay Gateway Backend")
    print("=" * 65)
    print(f" -> PostgreSQL Target   : postgresql://{PG_USER}@{PG_HOST}:{PG_PORT}/{PG_DATABASE}")
    print(f" -> Local Dashboard URL : http://localhost:{PORT}")
    print(f" -> Network Dashboard   : http://127.0.0.1:{PORT}")
    print(f" -> REST API Endpoints  : http://localhost:{PORT}/api/status")
    print(f"                          http://localhost:{PORT}/api/control")
    print(f"                          http://localhost:{PORT}/api/telemetry")
    print("=" * 65 + "\n", flush=True)

    try:
        app.run(host=HOST, port=PORT, debug=False, threaded=True)
    except Exception as e:
        print(f"[FATAL FLASK ERROR] {e}", flush=True)
