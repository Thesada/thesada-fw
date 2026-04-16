// thesada-fw - dashboard.html.h
// Embedded dashboard HTML served from PROGMEM.
// Single-page app: polls /api/state every 5 s, renders sensor table.
// Config editor fetches /api/config and POSTs back on save.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

static const char DASHBOARD_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Thesada Node</title>
<style>
  body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:0;padding:1rem}
  h1{font-size:1.2rem;margin:0 0 1rem;color:#7cf}
  .card{background:#1e1e1e;border-radius:8px;padding:1rem;margin-bottom:1rem}
  table{width:100%;border-collapse:collapse}
  td,th{padding:.4rem .6rem;text-align:left;border-bottom:1px solid #333}
  th{color:#7cf;font-weight:600}
  .val{font-family:monospace;color:#afa}
  textarea{width:100%;height:320px;background:#111;color:#eee;border:1px solid #444;
           border-radius:4px;padding:.5rem;font-family:monospace;font-size:.85rem;box-sizing:border-box}
  button{background:#7cf;color:#111;border:none;padding:.5rem 1.2rem;
         border-radius:4px;cursor:pointer;font-weight:600;margin-top:.5rem}
  button:hover{background:#5af}
  .msg{margin-top:.5rem;font-size:.85rem;color:#afa}
  .err{color:#f88}
  nav button{margin-right:.5rem;background:#333;color:#eee}
  nav button.active{background:#7cf;color:#111}
  footer{margin-top:1.5rem;font-size:.75rem;color:#555;text-align:center}
  .section{display:none}.section.active{display:block}
  #termOut{height:280px;overflow-y:auto;background:#0a0a0a;border-radius:4px;padding:.5rem;
           font-family:monospace;font-size:.8rem;color:#afa;white-space:pre-wrap}
  #termRow{display:flex;gap:.5rem;margin-top:.5rem}
  #termIn{flex:1;background:#111;color:#eee;border:1px solid #444;border-radius:4px;
          padding:.4rem .6rem;font-family:monospace}
  #authOverlay{display:none;position:fixed;inset:0;background:rgba(0,0,0,.7);z-index:100;align-items:center;justify-content:center}
  #authOverlay.show{display:flex}
  #authBox{background:#1e1e1e;border-radius:8px;padding:1.5rem;min-width:260px}
  #authBox h2{margin:0 0 1rem;font-size:1rem;color:#7cf}
  #authBox input{width:100%;background:#111;color:#eee;border:1px solid #444;border-radius:4px;padding:.4rem .6rem;margin-bottom:.6rem;box-sizing:border-box}
  #authBox .row{display:flex;gap:.5rem;margin-top:.5rem}
  #authBox .row button{flex:1}
  #authErr{color:#f88;font-size:.82rem;min-height:1em;margin-bottom:.3rem}
</style>
</head>
<body>
<h1>&#x1F4E1; Thesada Node</h1>
<nav>
  <button class="active" onclick="show('dash',this)">Dashboard</button>
  <button id="adminBtn" onclick="showAdmin(this)">Admin</button>
</nav>
<nav id="adminNav" style="display:none;padding-top:.3rem">
  <button onclick="showSub('cfg',this)">Config</button>
  <button onclick="showSub('ota',this)">OTA</button>
  <button onclick="showSub('term',this)">Terminal</button>
  <button onclick="showSub('files',this)">Files</button>
</nav>

<!-- Auth modal -->
<div id="authOverlay">
  <div id="authBox">
    <h2>&#x1F512; Login</h2>
    <input type="text"     id="authUser" placeholder="Username" autocomplete="username">
    <input type="password" id="authPass" placeholder="Password" autocomplete="current-password">
    <div id="authErr"></div>
    <div class="row">
      <button id="authBtn" onclick="authSubmit()">Login</button>
      <button style="background:#333;color:#eee" onclick="authCancel()">Cancel</button>
    </div>
  </div>
</div>

<!-- Dashboard -->
<div id="dash" class="section active card">
  <div id="mqttStatus" style="margin-bottom:.6rem;font-size:.82rem;color:#888"></div>
  <table><thead><tr><th>Sensor</th><th>Value</th><th>Updated</th></tr></thead>
  <tbody id="rows"><tr><td colspan="3" style="color:#888">Loading…</td></tr></tbody>
  </table>
</div>

<!-- Config editor -->
<div id="cfg" class="section card">
  <textarea id="cfgText" spellcheck="false"></textarea>
  <button onclick="saveConfig()">Save &amp; Restart</button>
  <button onclick="backupConfig()" style="background:#555;color:#eee;margin-left:.4rem">Backup to SD</button>
  <div id="cfgMsg" class="msg"></div>
</div>

<!-- OTA -->
<div id="ota" class="section card">
  <p>Upload a compiled <code>.bin</code> to update firmware.</p>
  <form id="otaForm">
    <input type="file" id="otaFile" accept=".bin">
    <button type="button" onclick="uploadOta()">Flash</button>
  </form>
  <div id="otaMsg" class="msg"></div>
</div>

<!-- Terminal -->
<div id="term" class="section card">
  <div style="display:flex;gap:.5rem;align-items:center;margin-bottom:.4rem;flex-wrap:wrap">
    <span style="font-size:.82rem;color:#888">Log level:</span>
    <select id="logLevel" onchange="termFilter()" style="background:#111;color:#eee;border:1px solid #444;border-radius:4px;padding:.2rem .4rem;font-size:.82rem">
      <option value="">ALL</option>
      <option value="INF">INF</option>
      <option value="WRN">WRN</option>
      <option value="ERR">ERR</option>
      <option value="DBG">DBG</option>
    </select>
    <button onclick="termClear()" style="background:#333;color:#eee;margin-top:0;padding:.25rem .7rem;font-size:.82rem">Clear</button>
  </div>
  <div id="termOut"></div>
  <div id="termRow">
    <input id="termIn" type="text" placeholder="Type a shell command (help for list)…">
    <button onclick="termSend()">Send</button>
  </div>
</div>

<!-- Files -->
<div id="files" class="section card">
  <div style="display:flex;gap:.5rem;align-items:center;margin-bottom:.5rem;flex-wrap:wrap">
    <select id="fileSource" style="background:#111;color:#eee;border:1px solid #444;border-radius:4px;padding:.35rem" onchange="refreshFiles()">
      <option value="littlefs">LittleFS</option>
      <option value="sd">SD card</option>
      <option value="scripts">LittleFS / Scripts</option>
    </select>
    <select id="fileList" style="flex:1;min-width:180px;background:#111;color:#eee;border:1px solid #444;border-radius:4px;padding:.35rem" onchange="loadFile()">
      <option value="">- select file -</option>
    </select>
    <button onclick="refreshFiles()" style="background:#333;color:#eee;margin-top:0">Refresh</button>
    <button onclick="copyFileToOther()" style="background:#363;color:#eee;margin-top:0">Copy to other</button>
    <button onclick="deleteFile()" style="background:#633;color:#eee;margin-top:0">Delete</button>
  </div>
  <textarea id="fileContent" spellcheck="false" style="height:240px"></textarea>
  <div style="display:flex;gap:.5rem;align-items:center;margin-top:.5rem;flex-wrap:wrap">
    <button onclick="saveFile()">Save</button>
    <label style="display:flex;align-items:center;gap:.4rem;cursor:pointer;margin-top:0">
      <span style="color:#aaa;font-size:.85rem">Upload new file:</span>
      <input type="file" id="uploadFile" style="font-size:.82rem;color:#eee">
    </label>
    <button onclick="uploadFile()" style="background:#555;color:#eee;margin-top:0">Upload</button>
  </div>
  <div id="fileMsg" class="msg"></div>
</div>

<footer id="fwInfo">thesada-fw</footer>

<script>
// ── Auth modal - credentials entered once per session ─────────────────────────
let _authHeader = null;
let _authResolve = null;

// Restore token from sessionStorage and validate on page load
(function() {
  const saved = sessionStorage.getItem('thesada_token');
  if (saved) {
    _authHeader = 'Bearer ' + saved;
    // Validate the token is still accepted by the server
    fetch('/api/ws/token', { headers: { 'Authorization': _authHeader } })
      .then(r => { if (r.status === 401) clearAuth(); });
  }
})();

function authHeader() {
  if (_authHeader) return Promise.resolve(_authHeader);
  return new Promise(resolve => {
    _authResolve = resolve;
    document.getElementById('authOverlay').classList.add('show');
    document.getElementById('authUser').focus();
  });
}
function authSubmit() {
  const u = document.getElementById('authUser').value;
  const p = document.getElementById('authPass').value;
  const basicHdr = 'Basic ' + btoa(u + ':' + p);
  const btn = document.getElementById('authBtn');
  const err = document.getElementById('authErr');
  btn.disabled = true;
  btn.textContent = '...';
  err.textContent = '';
  fetch('/api/login', { method: 'POST', headers: { 'Authorization': basicHdr } })
    .then(r => {
      btn.disabled = false;
      btn.textContent = 'Login';
      if (r.status === 401) {
        err.textContent = 'Wrong username or password';
        document.getElementById('authPass').value = '';
        document.getElementById('authPass').focus();
        return;
      }
      if (r.status === 429) {
        err.textContent = 'Too many attempts - wait 30s';
        return;
      }
      return r.json();
    })
    .then(data => {
      if (!data || !data.token) return;
      _authHeader = 'Bearer ' + data.token;
      sessionStorage.setItem('thesada_token', data.token);
      document.getElementById('authOverlay').classList.remove('show');
      document.getElementById('authPass').value = '';
      if (_authResolve) { _authResolve(_authHeader); _authResolve = null; }
    })
    .catch(() => {
      btn.disabled = false;
      btn.textContent = 'Login';
      err.textContent = 'Connection error';
    });
}
function authCancel() {
  document.getElementById('authOverlay').classList.remove('show');
  document.getElementById('authErr').textContent = '';
  if (_authResolve) { _authResolve(null); _authResolve = null; }
}
function clearAuth() {
  _authHeader = null;
  sessionStorage.removeItem('thesada_token');
  document.getElementById('authUser').value = '';
}
document.getElementById('authPass').addEventListener('keydown', e => {
  if (e.key === 'Enter') authSubmit();
});

const _escEl = document.createElement('span');
function esc(s) { _escEl.textContent = s; return _escEl.innerHTML; }

function show(id, btn) {
  document.getElementById('adminNav').style.display = 'none';
  document.querySelectorAll('.section').forEach(s => s.classList.remove('active'));
  document.querySelectorAll('nav button').forEach(b => b.classList.remove('active'));
  document.getElementById(id).classList.add('active');
  btn.classList.add('active');
}
async function showAdmin(btn) {
  const auth = await authHeader();
  if (!auth) return;
  document.querySelectorAll('.section').forEach(s => s.classList.remove('active'));
  document.querySelectorAll('nav button').forEach(b => b.classList.remove('active'));
  btn.classList.add('active');
  document.getElementById('adminNav').style.display = '';
  showSub('cfg', document.querySelector('#adminNav button'));
}
function showSub(id, btn) {
  document.querySelectorAll('.section').forEach(s => s.classList.remove('active'));
  document.querySelectorAll('#adminNav button').forEach(b => b.classList.remove('active'));
  document.getElementById(id).classList.add('active');
  btn.classList.add('active');
  if (id === 'cfg') loadConfig();
  if (id === 'term') wsConnect();
  if (id === 'files') refreshFiles();
}

// ── Dashboard polling ────────────────────────────────────────────────────────
function refreshState() {
  const tbody = document.getElementById('rows');
  fetch('/api/state')
    .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); })
    .then(data => {
      if (!data || typeof data !== 'object') {
        tbody.innerHTML = '<tr><td colspan="3" style="color:#888">No data yet - waiting for first sensor read</td></tr>';
        return;
      }
      // ── MQTT status bar ──
      const mqtt = data._mqtt;
      const statusEl = document.getElementById('mqttStatus');
      if (mqtt && statusEl) {
        const dot  = mqtt.connected ? '&#9679;' : '&#9679;';
        const col  = mqtt.connected ? '#4c4' : '#c44';
        const lp   = mqtt.last_publish ? ('Last publish: ' + mqtt.last_publish) : 'No publish yet';
        statusEl.innerHTML =
          `<span style="color:${col}">&#9679;</span> MQTT ${mqtt.connected ? 'connected' : 'disconnected'} &nbsp;·&nbsp; ${lp}`;
      }
      const now = new Date().toLocaleTimeString();
      let html = '';
      for (const [key, val] of Object.entries(data)) {
        if (key.startsWith('_')) continue;  // skip metadata keys
        if (typeof val === 'object' && val !== null) {
          if (Array.isArray(val.sensors)) {
            val.sensors.forEach(s => {
              const sensorName = esc(s.name || s.address || key);
              const metrics = [];
              const addMetric = (metricKey, label, unit) => {
                const v = s[metricKey];
                if (v === undefined || v === null) return;
                metrics.push({ label: label, value: unit ? `${v} ${unit}` : String(v) });
              };

              if (s.temp !== undefined && s.temp !== null) {
                const unit = (s.temp_c !== undefined && s.temp !== s.temp_c) ? '\u00B0F' : '\u00B0C';
                addMetric('temp', 'temperature', unit);
              } else {
                addMetric('temp_c', 'temperature', '\u00B0C');
              }
              addMetric('humidity', 'humidity', '%');
              addMetric('current_a', 'current', 'A');
              addMetric('voltage_v', 'voltage', 'V');
              addMetric('power_w', 'power', 'W');
              addMetric('percent', 'percent', '%');

              const known = new Set(['name', 'address', 'temp', 'temp_c', 'humidity', 'current_a', 'voltage_v', 'power_w', 'percent']);
              Object.entries(s).forEach(([metricKey, raw]) => {
                if (known.has(metricKey)) return;
                if (raw === undefined || raw === null) return;
                if (typeof raw === 'object') return;
                metrics.push({ label: esc(metricKey.replace(/_/g, ' ')), value: esc(String(raw)) });
              });

              if (!metrics.length) {
                html += `<tr><td>${sensorName}</td>
                             <td class="val">-</td>
                             <td>${now}</td></tr>`;
                return;
              }

              metrics.forEach(m => {
                html += `<tr><td>${sensorName} - ${m.label}</td>
                             <td class="val">${m.value}</td>
                             <td>${now}</td></tr>`;
              });
            });
          } else if (Array.isArray(val.channels)) {
            val.channels.forEach(c => {
              const a = c.current_a !== undefined ? c.current_a + ' A' : '-';
              const w = c.power_w !== undefined ? c.power_w + ' W' : '-';
              const v = c.voltage_v !== undefined ? c.voltage_v + ' V' : '-';
              html += `<tr><td>${esc(c.name)}</td>
                           <td class="val">${a} &nbsp; ${w} &nbsp; <span style="color:#888">${v}</span></td>
                           <td>${now}</td></tr>`;
            });
          } else if (val.voltage_v !== undefined && val.percent !== undefined) {
            const state = val.charging ? 'Charging' : 'Discharging';
            const col = val.percent <= 20 ? '#c44' : val.charging ? '#4c4' : '#fff';
            html += `<tr><td>Battery %</td>
                         <td class="val" style="color:${col}">${val.percent}%</td>
                         <td>${now}</td></tr>`;
            html += `<tr><td>Battery V</td>
                         <td class="val">${val.voltage_v} V</td>
                         <td>${now}</td></tr>`;
            html += `<tr><td>Battery Charge State</td>
                         <td class="val" style="color:${col}">${state}</td>
                         <td>${now}</td></tr>`;
          } else {
            html += `<tr><td>${key}</td>
                         <td class="val">${JSON.stringify(val)}</td>
                         <td>${now}</td></tr>`;
          }
        }
      }
      tbody.innerHTML = html || '<tr><td colspan="3" style="color:#888">No data yet - waiting for first sensor read</td></tr>';
    })
    .catch(e => {
      tbody.innerHTML = `<tr><td colspan="3" style="color:#f88">Error: ${e.message}</td></tr>`;
    });
}
refreshState();
setInterval(refreshState, 5000);

fetch('/api/info').then(r=>r.json()).then(d=>{
  document.getElementById('fwInfo').textContent =
    'thesada-fw v' + d.version + '  ·  ' + d.build + '  ·  ' + d.device;
  // Hide SD card option if not available
  if (!d.sd) {
    const sel = document.getElementById('fileSource');
    for (let i = 0; i < sel.options.length; i++) {
      if (sel.options[i].value === 'sd') { sel.remove(i); break; }
    }
    if (sel.value !== 'littlefs' && sel.value !== 'scripts') sel.value = 'littlefs';
  }
});

// ── Config editor ────────────────────────────────────────────────────────────
async function loadConfig() {
  const auth = await authHeader();
  if (!auth) return;
  fetch('/api/config', { headers: { 'Authorization': auth } })
    .then(r => { if (r.status === 401) { clearAuth(); return null; } return r.text(); })
    .then(t => {
      if (t === null) return;
      try { t = JSON.stringify(JSON.parse(t), null, 2); } catch(e) {}
      document.getElementById('cfgText').value = t;
    });
}

async function saveConfig() {
  const body = document.getElementById('cfgText').value;
  try { JSON.parse(body); } catch(e) {
    document.getElementById('cfgMsg').innerHTML = '<span class="err">Invalid JSON: ' + e.message + '</span>';
    return;
  }
  const auth = await authHeader();
  if (!auth) return;
  fetch('/api/config', {
    method: 'POST', body,
    headers: { 'Content-Type': 'application/json', 'Authorization': auth }
  })
    .then(r => {
      if (r.status === 401) { clearAuth(); document.getElementById('cfgMsg').innerHTML = '<span class="err">Wrong credentials</span>'; return null; }
      return r.json();
    })
    .then(r => {
      if (!r) return;
      if (r.ok) {
        document.getElementById('cfgMsg').textContent = 'Saved - restarting… page will reload in 20s';
        setTimeout(() => location.reload(), 20000);
      } else {
        document.getElementById('cfgMsg').textContent = 'Error: ' + r.error;
      }
    }).catch(e => {
      document.getElementById('cfgMsg').innerHTML = '<span class="err">' + e + '</span>';
    });
}

// ── Config backup to SD ──────────────────────────────────────────────────────
async function backupConfig() {
  const auth = await authHeader();
  if (!auth) return;
  const msg = document.getElementById('cfgMsg');
  fetch('/api/backup', { method: 'POST', headers: { 'Authorization': auth } })
    .then(r => {
      if (r.status === 401) { clearAuth(); msg.innerHTML = '<span class="err">Wrong credentials</span>'; return null; }
      return r.json();
    })
    .then(r => {
      if (!r) return;
      msg.textContent = r.ok ? 'Config backed up to /config_backup.json on SD' : ('Backup failed: ' + r.error);
    });
}

// ── WebSocket serial terminal ─────────────────────────────────────────────────
let _ws = null;
let _wsConnecting = false;
let _termLines = [];
const _TERM_MAX  = 500;
const _LOG_RE    = /^\[(INF|WRN|ERR|DBG)\]\[/;

async function wsConnect() {
  if (_ws && (_ws.readyState === WebSocket.OPEN || _ws.readyState === WebSocket.CONNECTING)) return;
  if (_wsConnecting) return;
  if (!_authHeader) { termAppend('[not authenticated]'); return; }
  _wsConnecting = true;
  try {
    const r = await fetch('/api/ws/token', { headers: { 'Authorization': _authHeader } });
    if (r.status === 401) { _wsConnecting = false; clearAuth(); termAppend('[session expired - log in again]'); return; }
    if (!r.ok) { _wsConnecting = false; setTimeout(wsConnect, 5000); return; }
  } catch(e) { _wsConnecting = false; setTimeout(wsConnect, 5000); return; }
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  _ws = new WebSocket(proto + '//' + location.host + '/ws/serial');
  _ws.onopen    = () => { _wsConnecting = false; termAppend('[connected]'); };
  _ws.onmessage = e => {
    // Replay arrives as single message with newline-separated lines
    if (e.data.includes('\n')) {
      e.data.split('\n').forEach(l => { if (l) termAppend(l); });
    } else {
      termAppend(e.data);
    }
  };
  _ws.onclose   = () => { _wsConnecting = false; setTimeout(wsConnect, 3000); };
  _ws.onerror   = () => { _ws.close(); };
}
function _termRender() {
  const out = document.getElementById('termOut');
  if (!out) return;
  const level = (document.getElementById('logLevel') || {}).value || '';
  const lines = level
    ? _termLines.filter(l => { const m = l.match(_LOG_RE); return !m || m[1] === level; })
    : _termLines;
  out.textContent = lines.join('\n') + (lines.length ? '\n' : '');
}
function termAppend(line) {
  const out = document.getElementById('termOut');
  if (!out) return;
  const atBottom = out.scrollTop + out.clientHeight >= out.scrollHeight - 5;
  _termLines.push(line);
  if (_termLines.length > _TERM_MAX) _termLines.shift();
  _termRender();
  if (atBottom) out.scrollTop = out.scrollHeight;
}
function termFilter() {
  _termRender();
  const out = document.getElementById('termOut');
  if (out) out.scrollTop = out.scrollHeight;
}
function termClear() {
  _termLines = [];
  const out = document.getElementById('termOut');
  if (out) out.textContent = '';
}
function termSend() {
  const inp = document.getElementById('termIn');
  const cmd = inp.value.trim();
  if (!cmd) return;
  if (_ws && _ws.readyState === WebSocket.OPEN) { _ws.send(cmd); inp.value = ''; }
  else termAppend('[not connected]');
}
document.getElementById('termIn').addEventListener('keydown', e => {
  if (e.key === 'Enter') termSend();
});

// ── File browser (SD + LittleFS) ─────────────────────────────────────────────
function fileSource() { return document.getElementById('fileSource').value; }
function fileSourceParam() { return '&source=' + fileSource(); }

async function refreshFiles() {
  const auth = await authHeader();
  if (!auth) return;
  fetch('/api/files?source=' + fileSource(), { headers: { 'Authorization': auth } })
    .then(r => { if (r.status === 401) { clearAuth(); return null; } return r.json(); })
    .then(files => {
      if (!files) return;
      const sel = document.getElementById('fileList');
      const cur = sel.value;
      sel.innerHTML = '<option value="">- select file -</option>';
      files.filter(f => !f.dir).sort((a,b) => a.name.localeCompare(b.name)).forEach(f => {
        const opt = document.createElement('option');
        opt.value = f.name.startsWith('/') ? f.name : '/' + f.name;
        opt.textContent = (f.name.startsWith('/') ? f.name : '/' + f.name) +
                          '  (' + (f.size >= 1024 ? (f.size/1024).toFixed(1)+' KB' : f.size+' B') + ')';
        if (opt.value === cur) opt.selected = true;
        sel.appendChild(opt);
      });
      document.getElementById('fileMsg').textContent = files.length + ' file(s)';
    });
}
async function loadFile() {
  const path = document.getElementById('fileList').value;
  if (!path) return;
  const auth = await authHeader();
  if (!auth) return;
  fetch('/api/file?path=' + encodeURIComponent(path) + fileSourceParam(), { headers: { 'Authorization': auth } })
    .then(r => { if (r.status === 401) { clearAuth(); return null; } return r.text(); })
    .then(t => { if (t !== null) document.getElementById('fileContent').value = t; });
}
async function saveFile() {
  const path = document.getElementById('fileList').value;
  const content = document.getElementById('fileContent').value;
  const msg = document.getElementById('fileMsg');
  if (!path) { msg.textContent = 'Select a file first'; return; }
  const auth = await authHeader();
  if (!auth) return;
  fetch('/api/file?path=' + encodeURIComponent(path) + fileSourceParam(), {
    method: 'POST', body: content,
    headers: { 'Content-Type': 'application/octet-stream', 'Authorization': auth }
  })
    .then(r => { if (r.status === 401) { clearAuth(); return null; } return r.json(); })
    .then(r => { if (r) msg.textContent = r.ok ? 'Saved' : ('Error: ' + r.error); });
}
async function deleteFile() {
  const path = document.getElementById('fileList').value;
  const msg = document.getElementById('fileMsg');
  if (!path) { msg.textContent = 'Select a file first'; return; }
  if (!confirm('Delete ' + path + '?')) return;
  const auth = await authHeader();
  if (!auth) return;
  fetch('/api/file?path=' + encodeURIComponent(path) + fileSourceParam(), {
    method: 'DELETE', headers: { 'Authorization': auth }
  })
    .then(r => { if (r.status === 401) { clearAuth(); return null; } return r.json(); })
    .then(r => {
      if (!r) return;
      msg.textContent = r.ok ? 'Deleted' : ('Error: ' + r.error);
      if (r.ok) { document.getElementById('fileContent').value = ''; refreshFiles(); }
    });
}
async function copyFileToOther() {
  const path = document.getElementById('fileList').value;
  const msg  = document.getElementById('fileMsg');
  if (!path) { msg.textContent = 'Select a file first'; return; }
  const src  = fileSource();
  const dest = src === 'sd' ? 'littlefs' : 'sd';
  const auth = await authHeader();
  if (!auth) return;
  // Read from current source, write to other
  const r = await fetch('/api/file?path=' + encodeURIComponent(path) + '&source=' + src,
                        { headers: { 'Authorization': auth } });
  if (r.status === 401) { clearAuth(); return; }
  const content = await r.text();
  const w = await fetch('/api/file?path=' + encodeURIComponent(path) + '&source=' + dest,
                        { method: 'POST', body: content,
                          headers: { 'Content-Type': 'text/plain', 'Authorization': auth } });
  if (w.status === 401) { clearAuth(); return; }
  const j = await w.json();
  msg.textContent = j.ok ? ('Copied to ' + dest) : ('Copy failed: ' + j.error);
}
async function uploadFile() {
  const input = document.getElementById('uploadFile');
  const msg   = document.getElementById('fileMsg');
  if (!input.files.length) { msg.textContent = 'Choose a file first'; return; }
  const file = input.files[0];
  const path = '/' + file.name;
  const auth = await authHeader();
  if (!auth) return;
  msg.textContent = 'Uploading…';
  const text = await file.text();
  fetch('/api/file?path=' + encodeURIComponent(path) + fileSourceParam(), {
    method: 'POST', body: text,
    headers: { 'Content-Type': 'text/plain', 'Authorization': auth }
  })
    .then(r => { if (r.status === 401) { clearAuth(); return null; } return r.json(); })
    .then(r => {
      if (!r) return;
      msg.textContent = r.ok ? ('Uploaded: ' + path) : ('Error: ' + r.error);
      if (r.ok) { input.value = ''; refreshFiles(); }
    });
}

// ── OTA upload ───────────────────────────────────────────────────────────────
async function uploadOta() {
  const file = document.getElementById('otaFile').files[0];
  if (!file) { document.getElementById('otaMsg').textContent = 'Select a .bin file first'; return; }
  const msg = document.getElementById('otaMsg');
  const auth = await authHeader();
  if (!auth) return;
  msg.textContent = 'Uploading…';
  const form = new FormData();
  form.append('firmware', file, file.name);
  fetch('/ota', { method: 'POST', body: form, headers: { 'Authorization': auth } })
    .then(r => {
      if (r.status === 401) { clearAuth(); msg.innerHTML = '<span class="err">Wrong credentials</span>'; return null; }
      return r.json();
    })
    .then(j => {
      if (j === null) return;
      if (j.ok) { msg.textContent = 'Done - device rebooting… page will reload in 20s'; setTimeout(() => location.reload(), 20000); }
      else { msg.innerHTML = '<span class="err">OTA failed on device - check serial log</span>'; }
    })
    .catch(e => { msg.innerHTML = '<span class="err">' + e + '</span>'; });
}
</script>
</body>
</html>
)rawhtml";
