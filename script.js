/**
 * ==============================================================================
 * SmartFan IoT System - Client Dashboard Controller (Corrected Relay Logic)
 * ==============================================================================
 * Features:
 * 1. Live sensor telemetry (Temperature °C, Humidity %) with 2.5s polling.
 * 2. Strict Relay ON / OFF Actuator Control (Active-HIGH, GPIO26).
 * 3. Manual OFF cannot be overridden by automatic temperature control.
 * 4. Automatic Mode triggers relay ON at >= 30.0°C and OFF at <= 28.0°C.
 * 5. Clearly distinguishes confirmed relay state from actual fan rotation.
 * 6. Speed slider & presets disabled pending external MOSFET hardware installation.
 * ==============================================================================
 */

const $ = (id) => document.getElementById(id);

// Determine backend base URL:
// If running on file:// protocol, default to Flask at http://localhost:5000
let backendUrl = (window.location.protocol === 'file:') ? 'http://localhost:5000' : '';

let pollTimer = null;
let isConnected = false;
let currentMode = 'auto';

// Clean / format URL helper
function cleanUrl(raw) {
  if (!raw) return '';
  let url = raw.trim();
  if (!/^https?:\/\//i.test(url)) {
    url = 'http://' + url;
  }
  return url.replace(/\/+$/, '');
}

// ==============================================================================
// 1. UI Render Functions
// ==============================================================================

function setConnectionStatus(ok, message, channel) {
  isConnected = ok;

  // Header Pill
  $('connectionDot').className = ok ? 'dot connected' : 'dot';
  $('connectionText').textContent = ok 
    ? (channel ? `Connected (${channel})` : 'Connected') 
    : 'Not connected';

  // Device status card
  $('wifiStatus').textContent = ok 
    ? (channel ? `Active · ${channel}` : 'Active') 
    : 'Disconnected';
  $('wifiStatus').className = ok ? 'status-value ok' : 'status-value err';

  if (channel) {
    $('commChannelHint').textContent = `Bridge: ${channel}`;
  }

  // Fan control buttons remain active and clickable at all times

  if (message) {
    $('message').textContent = message;
    $('connectMessage').textContent = message;
  }

  if (!ok) {
    $('temperature').innerHTML = '--<span class="unit"> °C</span>';
    $('humidity').innerHTML = '--<span class="unit"> %</span>';
    $('tempHint').textContent = 'Waiting for data';
    $('sensorStatus').textContent = 'No data';
    $('sensorStatus').className = 'status-value';
    renderFanVisual(false);
    const isFanOn = Boolean(window._lastFanState);
    $('fanState').innerHTML = isFanOn 
      ? 'Fan: ON<small id="fanSubstate">Commanded ON · Waiting for ESP32 sync</small>'
      : 'Fan: OFF<small id="fanSubstate">Fan standby · Waiting for device</small>';
    $('confirmedRelayVal').textContent = isFanOn ? 'COMMANDED (ON)' : 'STOPPED (OFF)';
    $('confirmedRelayVal').className = isFanOn ? 'status-value ok' : 'status-value';
    $('connectBtn').textContent = 'Connect';
  } else {
    $('connectBtn').textContent = 'Save IP';
  }
}

function renderFanVisual(running) {
  const visual = $('fanVisual');
  if (running) {
    visual.classList.add('running');
    visual.style.setProperty('--spin-duration', '0.6s');
  } else {
    visual.classList.remove('running');
    visual.style.removeProperty('--spin-duration');
  }
}

function updateModeUI(mode) {
  currentMode = (mode || 'auto').toLowerCase();
  if (currentMode === 'auto') {
    $('modeAutoBtn').classList.add('active');
    $('modeManualBtn').classList.remove('active');
    $('toggleAutoBtn').textContent = 'Auto: ON';
    $('toggleAutoBtn').className = 'mode-btn active';
    $('controlModeHint').textContent = 'Auto-starts fan when temp ≥ 30°C';
  } else {
    $('modeAutoBtn').classList.remove('active');
    $('modeManualBtn').classList.add('active');
    $('toggleAutoBtn').textContent = 'Auto: OFF';
    $('toggleAutoBtn').className = 'mode-btn';
    $('controlModeHint').textContent = 'Manual ON/OFF (Auto disabled)';
  }
}

// Update dashboard with verified state from backend
function applyStatusData(data) {
  const devConnected = (data.deviceConnected !== false && data.esp32_online !== false);
  let statusMsg = null;
  if (!devConnected && data.serial_error) {
    statusMsg = data.serial_error;
  }
  setConnectionStatus(devConnected, statusMsg, data.commChannel || data.channel || '');

  // 1. Temperature & Humidity Readings
  if (typeof data.temperature === 'number' && !isNaN(data.temperature) && data.temperature > 0) {
    const temp = data.temperature;
    $('temperature').innerHTML = temp.toFixed(1) + '<span class="unit"> °C</span>';

    if (temp >= 32.0) {
      $('tempHint').textContent = 'Room is very hot (Cooling Active)';
    } else if (temp >= 30.0) {
      $('tempHint').textContent = 'Room is warm (Auto Cooling Threshold)';
    } else if (temp <= 28.0) {
      $('tempHint').textContent = 'Cool room (Auto Standby Threshold)';
    } else {
      $('tempHint').textContent = 'Comfortable (28°C - 30°C Deadband)';
    }

    $('sensorStatus').textContent = 'Reading OK';
    $('sensorStatus').className = 'status-value ok';
  } else {
    $('temperature').innerHTML = '--<span class="unit"> °C</span>';
    $('tempHint').textContent = 'Sensor disconnected';
    $('sensorStatus').textContent = 'Check DHT11 wiring';
    $('sensorStatus').className = 'status-value err';
  }

  if (typeof data.humidity === 'number' && !isNaN(data.humidity) && data.humidity > 0) {
    $('humidity').innerHTML = data.humidity.toFixed(1) + '<span class="unit"> %</span>';
  } else {
    $('humidity').innerHTML = '--<span class="unit"> %</span>';
  }

  // 2. Fan Actuator State (Tracks confirmed or commanded fan state)
  const isRelayOn = Boolean(data.relayState !== undefined ? data.relayState : (data.fanState || data.fan));
  window._lastFanState = isRelayOn;

  if (isRelayOn) {
    $('fanState').innerHTML = `Fan: ON <small id="fanSubstate">${devConnected ? 'Fan running · Active cooling' : 'Commanded ON · Waiting for ESP32 sync'}</small>`;
    $('onBtn').classList.add('active');
    $('offBtn').classList.remove('active');
    $('confirmedRelayVal').textContent = devConnected ? 'ACTIVE (RUNNING)' : 'COMMANDED (ON)';
    $('confirmedRelayVal').className = 'status-value ok';
    renderFanVisual(true);
  } else {
    $('fanState').innerHTML = `Fan: OFF <small id="fanSubstate">${devConnected ? 'Fan stopped · Standby' : 'Fan standby · Standby'}</small>`;
    $('onBtn').classList.remove('active');
    $('offBtn').classList.add('active');
    $('confirmedRelayVal').textContent = 'STOPPED (OFF)';
    $('confirmedRelayVal').className = 'status-value';
    renderFanVisual(false);
  }

  // 3. Physical Fan Rotation Disclosure
  $('rotationFeedbackVal').textContent = 'Unmeasured (2-wire fan without tachometer)';

  // 4. Mode Sync
  updateModeUI(data.fanMode || data.mode || (data.auto_mode ? 'auto' : 'manual'));

  // 5. Thresholds & Timestamps
  if (data.tempThresholdOn && data.tempThresholdOff) {
    $('autoThresholdVal').textContent = `ON ≥ ${data.tempThresholdOn}°C · OFF ≤ ${data.tempThresholdOff}°C`;
  }
  $('lastUpdatedVal').textContent = data.lastUpdated ? `${data.lastUpdated}` : 'Just now';

  // 6. Database Status
  if (data.database && data.database.connected) {
    $('dbStatus').textContent = `Connected (${data.database.name || 'PostgreSQL'})`;
    $('dbStatus').className = 'status-value ok';
  } else {
    $('dbStatus').textContent = 'Offline';
    $('dbStatus').className = 'status-value err';
  }

  // Sync IP input if present
  if (data.esp32_ip && !$('deviceUrl').value) {
    $('deviceUrl').value = data.esp32_ip;
  }
}

// ==============================================================================
// 2. API Communication
// ==============================================================================

async function fetchStatus() {
  try {
    const res = await fetch(`${backendUrl}/api/status`, {
      method: 'GET',
      headers: { 'Accept': 'application/json' },
      cache: 'no-store'
    });

    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const data = await res.json();
    applyStatusData(data);
  } catch (err) {
    setConnectionStatus(false, 'Waiting for backend / ESP32 controller...');
  }
}

async function sendControl(payload) {
  try {
    $('message').textContent = 'Sending command to ESP32...';
    const res = await fetch(`${backendUrl}/api/control`, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'Accept': 'application/json'
      },
      body: JSON.stringify(payload)
    });

    const data = await res.json();
    if (res.ok) {
      applyStatusData(data);
      $('message').textContent = data.message || 'Command confirmed by ESP32.';
    } else {
      $('message').textContent = data.message || 'Command warning received.';
    }
  } catch (err) {
    $('message').textContent = `Failed to send command: ${err.message}`;
  }
}

// ==============================================================================
// 3. Event Listeners & Initialization
// ==============================================================================

document.addEventListener('DOMContentLoaded', () => {

  // 1. Fan Turn ON / OFF Buttons (Always clickable with instant response)
  $('onBtn').addEventListener('click', () => {
    updateModeUI('manual');
    window._lastFanState = true;
    $('onBtn').classList.add('active');
    $('offBtn').classList.remove('active');
    $('fanState').innerHTML = 'Fan: ON <small id="fanSubstate">Turning on fan · Command sent</small>';
    $('confirmedRelayVal').textContent = 'ACTIVE (RUNNING)';
    $('confirmedRelayVal').className = 'status-value ok';
    renderFanVisual(true);
    sendControl({
      fanState: true,
      fanMode: 'manual'
    });
  });

  $('offBtn').addEventListener('click', () => {
    updateModeUI('manual');
    window._lastFanState = false;
    $('offBtn').classList.add('active');
    $('onBtn').classList.remove('active');
    $('fanState').innerHTML = 'Fan: OFF <small id="fanSubstate">Turning off fan · Command sent</small>';
    $('confirmedRelayVal').textContent = 'STOPPED (OFF)';
    $('confirmedRelayVal').className = 'status-value';
    renderFanVisual(false);
    sendControl({
      fanState: false,
      fanMode: 'manual'
    });
  });

  // 2. Mode Switch Buttons
  $('modeAutoBtn').addEventListener('click', () => {
    updateModeUI('auto');
    sendControl({ fanMode: 'auto' });
  });

  $('modeManualBtn').addEventListener('click', () => {
    updateModeUI('manual');
    sendControl({ fanMode: 'manual' });
  });

  $('toggleAutoBtn').addEventListener('click', () => {
    const nextMode = currentMode === 'auto' ? 'manual' : 'auto';
    updateModeUI(nextMode);
    sendControl({ fanMode: nextMode });
  });

  // 3. Connect & Save ESP32 IP
  $('connectBtn').addEventListener('click', async () => {
    const ipVal = $('deviceUrl').value.trim();
    if (!ipVal) return;

    $('connectMessage').textContent = 'Saving ESP32 IP and testing link...';
    try {
      const res = await fetch(`${backendUrl}/api/config`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ esp32_ip: cleanUrl(ipVal) })
      });
      const data = await res.json();
      $('connectMessage').textContent = data.message;
      fetchStatus();
    } catch (e) {
      $('connectMessage').textContent = `Configuration failed: ${e.message}`;
    }
  });

  // 4. Test Connection Button
  $('checkConnBtn').addEventListener('click', async () => {
    const resultBox = $('checkResult');
    resultBox.style.display = 'block';
    resultBox.className = 'check-box info';
    resultBox.innerHTML = '<span class="spinner"></span> Testing ESP32 connection latency...';

    const t0 = performance.now();
    try {
      const res = await fetch(`${backendUrl}/api/status?_t=${Date.now()}`);
      const t1 = performance.now();
      const latency = Math.round(t1 - t0);

      if (res.ok) {
        const data = await res.json();
        if (data.deviceConnected !== false) {
          resultBox.className = 'check-box success';
          resultBox.innerHTML = `✅ <strong>ESP32 Online!</strong> Ping: <strong>${latency} ms</strong> | Fan: <strong>${(data.relayState || data.fanState) ? 'ON' : 'OFF'}</strong> | Temp: <strong>${data.temperature ? data.temperature + ' °C' : 'N/A'}</strong>`;
        } else {
          resultBox.className = 'check-box error';
          resultBox.innerHTML = `⚠️ <strong>Backend reachable, but ESP32 not responding.</strong> Ensure ESP32 is powered and IP/COM is set.`;
        }
      } else {
        resultBox.className = 'check-box error';
        resultBox.innerHTML = `❌ <strong>HTTP ${res.status}:</strong> Failed to retrieve status.`;
      }
    } catch (err) {
      resultBox.className = 'check-box error';
      resultBox.innerHTML = `❌ <strong>Connection Error:</strong> ${err.message}. Ensure Flask is running at http://localhost:5000`;
    }
  });

  // 5. Initial Fetch and Periodic Polling (every 2.5s)
  fetchStatus();
  pollTimer = setInterval(fetchStatus, 2500);
});
