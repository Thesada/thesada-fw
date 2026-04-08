// thesada-fw - liteserver.html.h
// Minimal setup page served from PROGMEM. WiFi config, config editor, OTA upload.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

static const char LITE_HTML[] PROGMEM = R"rawhtml(<!DOCTYPE html>
<html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>thesada-fw setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:monospace;background:#1a1a2e;color:#e0e0e0;padding:16px;max-width:600px;margin:0 auto}
h1{color:#0f0;font-size:18px;margin-bottom:16px}
h2{color:#7fdbca;font-size:14px;margin:16px 0 8px;border-bottom:1px solid #333;padding-bottom:4px}
.card{background:#16213e;border:1px solid #333;border-radius:6px;padding:12px;margin-bottom:12px}
label{display:block;font-size:12px;color:#999;margin-bottom:4px}
input[type=text],input[type=password],textarea{width:100%;padding:8px;background:#0f3460;border:1px solid #444;border-radius:4px;color:#e0e0e0;font-family:monospace;font-size:13px;margin-bottom:8px}
textarea{height:200px;resize:vertical}
input[type=file]{margin-bottom:8px;font-size:12px}
button{background:#0f0;color:#000;border:none;padding:8px 16px;border-radius:4px;cursor:pointer;font-family:monospace;font-weight:bold;font-size:13px}
button:hover{background:#0c0}
.btn-warn{background:#e94560;color:#fff}
.btn-warn:hover{background:#c03050}
.info{font-size:12px;color:#666;margin-top:4px}
.status{padding:8px;border-radius:4px;margin-bottom:8px;font-size:12px;display:none}
.ok{background:#0a3d0a;color:#0f0;display:block}
.err{background:#3d0a0a;color:#f66;display:block}
#progress{width:100%;height:20px;display:none;margin:8px 0}
</style>
</head><body>
<h1>thesada-fw setup</h1>
<div id="info" class="card"><span id="ver">loading...</span></div>

<h2>WiFi</h2>
<div class="card">
<form method="POST" action="/api/wifi">
<label>SSID</label>
<select id="ssid-sel" onchange="var m=document.getElementById('ssid-man');if(this.value=='__manual__'){m.style.display='block';m.name='ssid';this.name=''}else{m.style.display='none';m.name='';this.name='ssid'}">
<option value="">Scanning...</option>
</select>
<input type="text" id="ssid-man" name="" placeholder="Enter SSID manually" style="display:none">
<label>Password</label><input type="password" name="password">
<button type="submit">Save + Reboot</button>
<div class="info">Device will reboot and connect to the new network.</div>
</form>
</div>

<h2>Config</h2>
<div class="card">
<div id="cfg-status" class="status"></div>
<textarea id="cfg"></textarea>
<button onclick="saveConfig()">Save Config</button>
<button class="btn-warn" onclick="if(confirm('Reboot now?'))fetch('/api/restart',{method:'POST'})">Reboot</button>
</div>

<h2>OTA Update</h2>
<div class="card">
<div id="ota-status" class="status"></div>
<input type="file" id="fw" accept=".bin">
<progress id="progress" max="100" value="0"></progress>
<button onclick="uploadOTA()">Upload Firmware</button>
</div>

<script>
fetch('/api/info').then(r=>r.json()).then(d=>{
  document.getElementById('ver').textContent='v'+d.version+' | '+d.device+' | heap: '+d.heap+' B';
});
fetch('/api/scan').then(r=>r.json()).then(nets=>{
  var sel=document.getElementById('ssid-sel');
  sel.innerHTML='';
  nets.forEach(function(n){
    var o=document.createElement('option');
    o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+' dBm)';
    sel.appendChild(o);
  });
  var m=document.createElement('option');
  m.value='__manual__';m.textContent='-- enter manually --';
  sel.appendChild(m);
  sel.name='ssid';
});
fetch('/api/config').then(r=>r.text()).then(t=>{
  document.getElementById('cfg').value=t;
});
function saveConfig(){
  var s=document.getElementById('cfg-status');
  fetch('/api/config',{method:'POST',body:document.getElementById('cfg').value,
    headers:{'Content-Type':'application/json'}})
  .then(r=>{s.className='status '+(r.ok?'ok':'err');s.textContent=r.ok?'Saved':'Error: '+r.status;});
}
function uploadOTA(){
  var f=document.getElementById('fw').files[0];
  if(!f){alert('Select a .bin file');return;}
  var s=document.getElementById('ota-status');
  var p=document.getElementById('progress');
  p.style.display='block';s.className='status';s.style.display='none';
  var xhr=new XMLHttpRequest();
  xhr.open('POST','/ota');
  xhr.upload.onprogress=function(e){if(e.lengthComputable)p.value=Math.round(e.loaded/e.total*100);};
  xhr.onload=function(){
    s.className='status '+(xhr.status==200?'ok':'err');
    s.textContent=xhr.status==200?'Done - rebooting...':'Error: '+xhr.responseText;
    if(xhr.status==200)setTimeout(function(){location.reload()},10000);
  };
  xhr.onerror=function(){s.className='status err';s.textContent='Upload failed';};
  var fd=new FormData();fd.append('firmware',f);
  xhr.send(fd);
}
</script>
</body></html>)rawhtml";
