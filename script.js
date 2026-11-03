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
    $('controlModeHint').textContent = 'Auto-starts fan when temp ≥ 32°C';
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

  // 1. Temperature & Humidity Readings (with Real vs Simulation Demonstration support)
  const isSim = Boolean(data.isSimulated);
  const demoBadge = $('demoBadge');
  if (demoBadge) {
    demoBadge.textContent = isSim ? 'SIMULATED DEMO' : 'REAL SENSOR';
    demoBadge.className = isSim ? 'demo-tag active' : 'demo-tag';
  }

  if (isSim) {
    const simT = (typeof data.simulatedTemperature === 'number') ? data.simulatedTemperature : data.temperature;
    $('temperature').innerHTML = simT.toFixed(1) + '<span class="unit"> °C</span> <small style="display:block;font-size:11px;color:var(--yellow);font-weight:600;margin-top:2px;">[SIMULATED DEMO]</small>';
    const actualStr = (typeof data.actualTemperature === 'number') ? `${data.actualTemperature.toFixed(1)} °C` : 'DHT11 offline';
    $('tempHint').textContent = `Demonstration Mode (${simT.toFixed(1)}°C) · Actual DHT11: ${actualStr}`;
    $('sensorStatus').textContent = 'Demo Mode (Simulated)';
    $('sensorStatus').className = 'status-value ok';
  } else if (typeof data.temperature === 'number' && !isNaN(data.temperature) && data.temperature > 0) {
    const temp = data.temperature;
    $('temperature').innerHTML = temp.toFixed(1) + '<span class="unit"> °C</span>';

    if (temp >= 32.0) {
      $('tempHint').textContent = 'Room is hot (≥32°C Auto Cooling Active)';
    } else if (temp <= 31.5) {
      $('tempHint').textContent = 'Normal / Cool (≤31.5°C Auto Standby)';
    } else {
      $('tempHint').textContent = 'Comfortable (31.5°C – 32°C Anti-chatter Deadband)';
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

  const pinLvl = data.relayPhysicalLevel || (isRelayOn ? 'LOW' : 'HIGH');
  if (isRelayOn) {
    $('fanState').innerHTML = `Fan: ON <small id="fanSubstate">${devConnected ? 'Fan running · Active cooling' : 'Waiting for ESP32 connection'}</small>`;
    $('onBtn').classList.add('active');
    $('offBtn').classList.remove('active');
    $('confirmedRelayVal').textContent = devConnected ? `ACTIVE (RUNNING) · GPIO26 ${pinLvl}` : 'OFFLINE';
    $('confirmedRelayVal').className = 'status-value ok';
    renderFanVisual(true);
  } else {
    $('fanState').innerHTML = `Fan: OFF <small id="fanSubstate">${devConnected ? 'Fan stopped · Standby' : 'Standby · Offline'}</small>`;
    $('onBtn').classList.remove('active');
    $('offBtn').classList.add('active');
    $('confirmedRelayVal').textContent = devConnected ? `STOPPED (OFF) · GPIO26 ${pinLvl}` : 'STOPPED (OFF)';
    $('confirmedRelayVal').className = 'status-value';
    renderFanVisual(false);
  }

  // Relay Polarity badge
  if (data.relayPolarity && $('polarityBadge')) {
    $('polarityBadge').textContent = data.relayPolarity;
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

let isCommandPending = false;

async function sendControl(payload) {
  if (isCommandPending) return;
  isCommandPending = true;

  // Prevent duplicate button requests while command is executing
  $('onBtn').disabled = true;
  $('offBtn').disabled = true;
  $('modeAutoBtn').disabled = true;
  $('modeManualBtn').disabled = true;

  const desc = payload.fanState === false ? 'FAN OFF (GPIO26 HIGH)' :
               payload.fanState === true ? 'FAN ON (GPIO26 LOW)' :
               `Mode ${payload.fanMode}`;
  $('message').textContent = `Transmitting command to ESP32: ${desc}...`;

  try {
    const res = await fetch(`${backendUrl}/api/control`, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'Accept': 'application/json'
      },
      body: JSON.stringify(payload)
    });

    const data = await res.json();
    if (res.ok && data.status === 'ok') {
      // ONLY update fan status and UI after receiving hardware acknowledgement!
      applyStatusData(data);
      $('message').textContent = data.message || 'Confirmed: Command executed by ESP32.';
    } else {
      // Hardware failed or timed out - do NOT alter fan status
      $('message').textContent = `Command failed: ${data.message || 'ESP32 did not acknowledge'}`;
    }
  } catch (err) {
    $('message').textContent = `Communication error: ${err.message}`;
  } finally {
    isCommandPending = false;
    $('onBtn').disabled = false;
    $('offBtn').disabled = false;
    $('modeAutoBtn').disabled = false;
    $('modeManualBtn').disabled = false;
  }
}

// ==============================================================================
// 3. Event Listeners & Initialization
// ==============================================================================

document.addEventListener('DOMContentLoaded', () => {

  // 1. Fan Turn ON / OFF Buttons (Strict hardware confirmation)
  $('onBtn').addEventListener('click', () => {
    sendControl({
      fanState: true,
      fanMode: 'manual'
    });
  });

  $('offBtn').addEventListener('click', () => {
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

  // 5. Demonstration Simulation Buttons (35°C Auto ON / 25°C Auto OFF / Real Sensor)
  const btn35 = $('demo35Btn');
  if (btn35) {
    btn35.addEventListener('click', async () => {
      $('message').textContent = 'Activating Demo: Simulating 35.0°C (Should trigger Fan ON in Auto Mode)...';
      try {
        const res = await fetch(`${backendUrl}/api/simulate`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ action: 'start', temperature: 35.0 })
        });
        const d = await res.json();
        applyStatusData(d);
        $('message').textContent = d.message || 'Simulating 35°C demo';
      } catch (err) {
        $('message').textContent = `Demo request failed: ${err.message}`;
      }
    });
  }

  const btn25 = $('demo25Btn');
  if (btn25) {
    btn25.addEventListener('click', async () => {
      $('message').textContent = 'Activating Demo: Simulating 25.0°C (Should trigger Fan OFF in Auto Mode)...';
      try {
        const res = await fetch(`${backendUrl}/api/simulate`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ action: 'start', temperature: 25.0 })
        });
        const d = await res.json();
        applyStatusData(d);
        $('message').textContent = d.message || 'Simulating 25°C demo';
      } catch (err) {
        $('message').textContent = `Demo request failed: ${err.message}`;
      }
    });
  }

  const btnReal = $('demoRealBtn');
  if (btnReal) {
    btnReal.addEventListener('click', async () => {
      $('message').textContent = 'Returning to Real DHT11 Sensor mode...';
      try {
        const res = await fetch(`${backendUrl}/api/simulate`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ action: 'stop' })
        });
        const d = await res.json();
        applyStatusData(d);
        $('message').textContent = d.message || 'Returned to Real DHT11 Sensor';
      } catch (err) {
        $('message').textContent = `Demo reset failed: ${err.message}`;
      }
    });
  }

  // --- Database Refresh Button ---
  const refreshDbBtn = $('refreshDbBtn');
  if (refreshDbBtn) {
    refreshDbBtn.addEventListener('click', async () => {
      refreshDbBtn.textContent = '⏳ Fetching...';
      await fetchDatabaseLogs();
      refreshDbBtn.textContent = '🔄 Refresh Logs';
    });
  }

  // 6. Initial Fetch and Periodic Polling
  fetchStatus();
  fetchDatabaseLogs();
  pollTimer = setInterval(fetchStatus, 2500);
  setInterval(fetchDatabaseLogs, 5000); // Sync database view every 5 seconds
});


// ==============================================================================
// 7. PostgreSQL Database Telemetry Logs Viewer
// ==============================================================================

function escapeHtml(str) {
  if (str === null || str === undefined) return '';
  return String(str)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#039;');
}

async function fetchDatabaseLogs() {
  try {
    const res = await fetch(`${backendUrl}/api/history?limit=25`, {
      method: 'GET',
      headers: { 'Accept': 'application/json' },
      cache: 'no-store'
    });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const data = await res.json();
    if (data.status === 'ok' && Array.isArray(data.data)) {
      renderDatabaseTable(data);
    }
  } catch (err) {
    console.warn('[DB Error]', err);
    const tbody = $('dbTableBody');
    if (tbody) {
      tbody.innerHTML = `<tr><td colspan="7" style="padding: 20px; text-align: center; color: var(--red);">Unable to fetch PostgreSQL records: ${err.message}</td></tr>`;
    }
  }
}

function renderDatabaseTable(payload) {
  const tbody = $('dbTableBody');
  if (!tbody) return;

  if (payload.total_records !== undefined && $('dbTotalBadge')) {
    $('dbTotalBadge').textContent = `${payload.total_records.toLocaleString()} Total Records`;
  }

  if (payload.database && $('dbTableInfo')) {
    $('dbTableInfo').textContent = `${payload.database.engine} · ${payload.database.name}`;
  }

  const rows = payload.data || [];
  if (rows.length === 0) {
    tbody.innerHTML = `<tr><td colspan="7" style="padding: 20px; text-align: center; color: var(--muted);">No telemetry records found in PostgreSQL table 'telemetry'.</td></tr>`;
    return;
  }

  let html = '';
  rows.forEach((row) => {
    const isHot = typeof row.temperature === 'number' && row.temperature >= 32.0;
    const tempHtml = (typeof row.temperature === 'number') 
      ? `<span class="temp-tag ${isHot ? 'hot' : 'cool'}">${row.temperature.toFixed(1)} °C</span>`
      : '<span style="color:var(--muted);">--</span>';

    const humHtml = (typeof row.humidity === 'number') 
      ? `${row.humidity.toFixed(1)} %`
      : '<span style="color:var(--muted);">--</span>';

    const isFanOn = Boolean(row.fanState);
    const fanHtml = isFanOn 
      ? `<span class="badge-state on">● ON (${row.fanSpeed || 100}%)</span>`
      : `<span class="badge-state off">○ OFF</span>`;

    const modeHtml = `<span class="mode-pill-mini">${escapeHtml(row.fanMode || 'AUTO')}</span>`;

    const rssiHtml = (row.wifi_rssi !== null && row.wifi_rssi !== undefined) 
      ? `${row.wifi_rssi} dBm`
      : '<span style="color:var(--muted);">N/A</span>';

    html += `
      <tr>
        <td style="color: var(--muted); font-family: monospace;">#${row.id || '--'}</td>
        <td style="font-family: monospace; color: var(--text);">${escapeHtml(row.timestamp || '')}</td>
        <td>${tempHtml}</td>
        <td>${humHtml}</td>
        <td>${fanHtml}</td>
        <td>${modeHtml}</td>
        <td style="color: var(--muted);">${rssiHtml}</td>
      </tr>
    `;
  });

  tbody.innerHTML = html;
  if ($('dbLastSync')) {
    $('dbLastSync').textContent = `Last synced: ${new Date().toLocaleTimeString()} · Showing latest ${rows.length} rows`;
  }
}
