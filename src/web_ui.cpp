/**
 * @file web_ui.cpp
 * @brief WiFi AP + Async WebServer + WebSocket + OTA for configuration
 *
 * The ESP32-C3 runs as a WiFi AP when activated.
 * mDNS hostname: btwifiserial.local
 * WebSocket endpoint: /ws
 *
 * WebSocket JSON protocol:
 *   Client -> Server:
 *     {"cmd":"getStatus"}
 *     {"cmd":"setSerialMode","value":"frsky"|"sbus"|"sport_bt"|"sport_mirror"|"lua_serial"}
 *     {"cmd":"setBtName","value":"..."}
 *     {"cmd":"setDeviceMode","value":"trainer_in"|"trainer_out"|"telemetry"}
 *     {"cmd":"setTelemOutput","value":"wifi_udp"|"ble"}
 *     {"cmd":"setMirrorBaud","value":"57600"|"115200"}
 *     {"cmd":"setUdpPort","value":"5010"}
 *     {"cmd":"scanBt"}
 *     {"cmd":"connectBt","value":"XX:XX:XX:XX:XX:XX"}
 *     {"cmd":"disconnectBt"}
 *     {"cmd":"save"}
 *     {"cmd":"reboot"}
 *
 *   Server -> Client:
 *     {"type":"status", ...}
 *     {"type":"scanResult", "devices":[...]}
 *     {"type":"ack","cmd":"...","ok":true|false}
 */

#include "web_ui.h"
#include "config.h"
#include "channel_data.h"
#include "ble_module.h"
#include "sport_telemetry.h"
#include "log.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <Preferences.h>

// ─── AP configuration note ─────────────────────────────────────────
// AP SSID and password are stored in g_config (NVS). Default password: "12345678".
// Minimum 8 chars required by WPA2.

// ─── Server instances (static — never heap-allocated to avoid fragmentation) ──
static AsyncWebServer s_server(80);
static AsyncWebSocket s_ws("/ws");
static bool s_active           = false;
static bool s_wsAdded          = false;  // handler registered only once
static bool s_otaPendingRestart = false; // set after successful OTA; restarted from webUiLoop

// ─── Embedded HTML UI ───────────────────────────────────────────────
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>BTWifiSerial</title>
<style>
:root{--bg:#0d1117;--sf:#161b22;--bd:#30363d;--ac:#58a6ff;--tx:#e6edf3;--mu:#8b949e;--ok:#3fb950;--er:#f85149;--wn:#d29922}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',system-ui,sans-serif;background:var(--bg);color:var(--tx);padding:12px;max-width:460px;margin:0 auto;font-size:15px}
h1{text-align:center;color:var(--ac);margin-bottom:14px;font-size:1.1em;letter-spacing:.12em;text-transform:uppercase;font-weight:600}
.card{background:var(--sf);border:1px solid var(--bd);border-radius:4px;padding:13px;margin-bottom:9px}
.ct{color:var(--ac);font-size:.78em;font-weight:700;letter-spacing:.12em;text-transform:uppercase;margin-bottom:9px;padding-bottom:6px;border-bottom:1px solid var(--bd)}
.row{display:flex;justify-content:space-between;align-items:center;padding:3px 0;font-size:.88em}
.row .l{color:var(--mu)}.row .v{color:var(--tx);font-family:monospace;font-size:.92em}
.dot{width:7px;height:7px;border-radius:50%;display:inline-block;margin-right:5px;vertical-align:middle}
.ok{background:var(--ok)}.er{background:var(--er)}
label{display:block;font-size:.82em;color:var(--mu);margin-top:9px;margin-bottom:3px}
select,input[type=text]{width:100%;padding:6px 8px;border:1px solid var(--bd);background:var(--bg);color:var(--tx);border-radius:3px;font-size:.9em}
select:focus,input:focus{outline:none;border-color:var(--ac)}
.btn{display:inline-flex;align-items:center;gap:4px;padding:5px 13px;border:1px solid var(--bd);border-radius:3px;cursor:pointer;font-size:.85em;background:var(--sf);color:var(--tx);transition:border-color .15s,color .15s;margin-top:7px;white-space:nowrap}
.btn:hover{border-color:var(--ac);color:var(--ac)}
.btn.ac{background:var(--ac);color:#000;border-color:var(--ac)}.btn.ac:hover{background:#79b8ff;border-color:#79b8ff}
.btn.er{border-color:var(--er);color:var(--er)}.btn.er:hover{background:var(--er);color:#fff;border-color:var(--er)}
.btn.sm{padding:3px 8px;font-size:.8em;margin-top:0}
.irow{display:flex;gap:6px;margin-top:3px}
.irow input{margin-top:0}
.irow .btn{margin-top:0}
#scanList{margin-top:8px;max-height:210px;overflow-y:auto;scrollbar-width:thin;scrollbar-color:var(--bd) transparent}
#scanList::-webkit-scrollbar{width:5px}
#scanList::-webkit-scrollbar-track{background:transparent}
#scanList::-webkit-scrollbar-thumb{background:var(--bd);border-radius:3px}
#scanList::-webkit-scrollbar-thumb:hover{background:var(--mu)}
.dev{display:flex;justify-content:space-between;align-items:center;padding:5px 7px;background:var(--bg);border:1px solid var(--bd);border-radius:3px;margin-bottom:3px;font-size:.83em}
.dev .inf{display:flex;flex-direction:column;gap:2px}
.dev .da{font-family:monospace;color:var(--ac)}
.dev .dm{color:var(--mu)}
.peer{display:flex;justify-content:space-between;align-items:center;padding:6px 8px;background:var(--bg);border:1px solid var(--ok);border-radius:3px;margin-top:8px}
.peer .da{font-family:monospace;color:var(--ok);font-size:.9em}
.ota-drop{border:1px dashed var(--bd);border-radius:3px;padding:18px;text-align:center;font-size:.85em;color:var(--mu);margin-top:8px;cursor:pointer;transition:border-color .15s,color .15s;user-select:none}
.ota-drop:hover,.drag{border-color:var(--ac);color:var(--ac)}
#otaFile{display:none}
.ofn{font-size:.8em;color:var(--tx);margin-top:5px;font-family:monospace;word-break:break-all}
progress{width:100%;height:8px;margin-top:10px;display:none;accent-color:var(--ac);border-radius:4px}
.msg{font-size:.82em;color:var(--wn);margin-top:6px;min-height:1.1em}
.toast{position:fixed;top:0;left:0;right:0;padding:11px 16px;background:var(--ok);color:#fff;font-size:.85em;text-align:center;z-index:200;transform:translateY(-100%);transition:transform .3s ease}
.toast.show{transform:translateY(0)}
.mbg{position:fixed;inset:0;background:rgba(13,17,23,.82);z-index:100;display:flex;align-items:center;justify-content:center}
.mbox{background:var(--sf);border:1px solid var(--bd);border-radius:4px;padding:20px 18px;max-width:300px;width:92%}
.mbox h3{color:var(--tx);font-size:.95em;margin-bottom:7px}
.mbox p{color:var(--mu);font-size:.83em;margin-bottom:15px;line-height:1.4}
.mbtns{display:flex;gap:8px;justify-content:flex-end}
</style>
</head>
<body>
<h1>&#x25C8; BTWifiSerial</h1>

<div class="card">
  <div class="ct">Status</div>
  <div class="row"><span class="l">BLE</span><span class="v"><span id="bDot" class="dot er"></span><span id="bSt">--</span></span></div>
  <div class="row"><span class="l">Serial Mode</span><span class="v" id="sMode">--</span></div>
  <div class="row"><span class="l">Device Mode</span><span class="v" id="bRole">--</span></div>
  <div class="row"><span class="l">BT Mode</span><span class="v" id="btMode">--</span></div>
  <div class="row"><span class="l">BT Name</span><span class="v" id="bName">--</span></div>
  <div class="row"><span class="l">Local Addr</span><span class="v" id="lAddr">--</span></div>
  <div class="row"><span class="l">Remote Addr</span><span class="v" id="rAddr">--</span></div>
  <div class="row"><span class="l">Build</span><span class="v" id="bTs">--</span></div>
</div>

<div class="card">
  <div class="ct">System Config</div>
  <label>Device Mode</label>
  <select id="selRole" onchange="selRoleChange(this)">
    <option value="trainer_in">Trainer IN (Central)</option>
    <option value="trainer_out">Trainer OUT (Peripheral)</option>
    <option value="telemetry">Telemetry</option>
  </select>
  <label>Serial Mode</label>
  <select id="selMode" onchange="setSerialMode(this.value)">
    <option value="frsky">FrSky Trainer (CC2540)</option>
    <option value="sbus">SBUS Trainer</option>
    <option value="sport_bt">S.PORT Telemetry (CC2540)</option>
    <option value="sport_mirror">S.PORT Telemetry (Mirror)</option>
    <option value="lua_serial">LUA Serial (EdgeTX)</option>
  </select>
  <label>BT Name</label>
  <div class="irow">
    <input type="text" id="inName" maxlength="15" placeholder="BTWifiSerial">
    <button class="btn ac" onclick="setBtName()">Set</button>
  </div>
  <label>AP SSID</label>
  <div class="irow">
    <input type="text" id="inSsid" maxlength="15" placeholder="BTWifiSerial">
    <button class="btn ac" onclick="setSsid()">Set</button>
  </div>
  <label>AP Password</label>
  <div class="irow">
    <input type="text" id="inApPass" maxlength="15" placeholder="12345678">
    <button class="btn ac" onclick="setApPass()">Set</button>
  </div>
</div>

<div class="card" id="telemCard" style="display:none">
  <div class="ct">Telemetry Output</div>
  <label>Forward to</label>
  <select id="selTelemOut" onchange="setTelemOutput(this.value)">
    <option value="wifi_udp">WiFi UDP (AP: BTWifiSerial)</option>
    <option value="ble">BLE Notification</option>
  </select>
  <div id="mirrorBaudRow" style="display:none">
    <label>Mirror Baud Rate</label>
    <select id="selBaud" onchange="setMirrorBaud(this.value)">
      <option value="57600">57600 (default)</option>
      <option value="115200">115200 (TX16S/F16)</option>
    </select>
  </div>
  <div id="udpCfg" style="display:none">
    <label>UDP Port</label>
    <div class="irow">
      <input type="text" id="inUdpPort" maxlength="5" placeholder="5010" style="width:80px">
      <button class="btn ac" onclick="setUdpPort()">Set</button>
    </div>
  </div>
  <div id="telemStats" style="margin-top:8px">
    <div class="row"><span class="l">Packets</span><span class="v" id="tPkts">0</span></div>
    <div class="row"><span class="l">Rate</span><span class="v" id="tPps">0 pkt/s</span></div>
    <div class="row"><span class="l">Output Status</span><span class="v" id="tOutSt">--</span></div>
  </div>
</div>

<div class="card">
  <div class="ct">Bluetooth</div>
</div>

<div class="card" id="scanCard" style="display:none">
  <div class="ct">BLE Scan</div>
  <div id="cPeer" style="display:none"></div>
  <button class="btn ac" id="btnScan" onclick="scanBt()" style="margin-top:8px">&#x25B6; Scan</button>
  <div id="scanList"></div>
</div>

<div class="card" id="perCard" style="display:none">
  <div class="ct">Connected Clients</div>
  <div id="perClients"><span style="color:var(--mu);font-size:.82em">No clients connected</span></div>
</div>

<div class="card">
  <div class="ct">Firmware Update</div>
  <div class="ota-drop" id="otaDrop" onclick="document.getElementById('otaFile').click()">
    <div>&#x21A5;&nbsp;Drop .bin here or click to browse</div>
    <div class="ofn" id="ofn"></div>
  </div>
  <input type="file" id="otaFile" accept=".bin">
  <progress id="otaPrg" max="100" value="0"></progress>
  <div id="otaMsg" class="msg"></div>
  <button class="btn ac" id="btnOta" onclick="doOta()" style="display:none">&#x21A5;&nbsp;Upload</button>
</div>

<div class="card" style="text-align:right">
  <button class="btn er" onclick="reboot()">&#x21BA;&nbsp;Reboot</button>
</div>

<div id="rbOverlay" style="display:none;position:fixed;inset:0;background:#0d1117;z-index:99;align-items:center;justify-content:center;flex-direction:column;gap:14px">
  <span style="color:#e6edf3;font-size:1.3em;font-weight:600;letter-spacing:.04em">&#x25C8;&nbsp;BTWifiSerial</span>
  <span style="color:#6e7681;font-size:.82em;max-width:280px;text-align:center;line-height:1.5">
    Device has rebooted in normal mode.<br>
    To return to the configuration page, briefly press the Boot button, connect to the AP and reload.
  </span>
</div>

<div id="modal" class="mbg" style="display:none">
  <div class="mbox">
    <h3 id="mTitle">Confirm</h3>
    <p id="mMsg"></p>
    <div class="mbtns">
      <button class="btn" onclick="modalCancel()">Cancel</button>
      <button class="btn er" onclick="modalOk()">Confirm</button>
    </div>
  </div>
</div>

<script>
let ws,rTimer,prevRole='';

// ── WebSocket ──
function initWS(){
  if(ws&&ws.readyState<2)return;
  if(ws){ws.onclose=null;ws.close();}
  ws=new WebSocket('ws://'+location.host+'/ws');
  ws.onopen=()=>{clearTimeout(rTimer);getStatus();};
  ws.onclose=()=>{rTimer=setTimeout(initWS,3000);};
  ws.onmessage=(e)=>handle(JSON.parse(e.data));
}
function send(o){if(ws&&ws.readyState===1)ws.send(JSON.stringify(o));}
function getStatus(){send({cmd:'getStatus'});}

// ── Reboot overlay: show static goodbye screen, close WS ──
function showReboot(){
  document.getElementById('rbOverlay').style.display='flex';
  if(ws){ws.onclose=null;ws.close();ws=null;}
}

// ── Confirm modal ──
let mCb=null;
function showConfirm(title,msg,cb){
  document.getElementById('mTitle').textContent=title;
  document.getElementById('mMsg').textContent=msg;
  mCb=cb;
  document.getElementById('modal').style.display='flex';
}
function modalOk(){document.getElementById('modal').style.display='none';if(mCb)mCb();mCb=null;}
function modalCancel(){
  document.getElementById('modal').style.display='none';
  mCb=null;
  // If role select was reverted, restore prevRole
  const s=document.getElementById('selRole');
  if(prevRole)s.value=prevRole;
}

// ── Message handler ──
function handle(m){
  if(m.type==='ack'&&m.reboot){showReboot();return;}
  if(m.type==='status'){
    const ok=m.bleConnected;
    document.getElementById('bDot').className='dot '+(ok?'ok':'er');
    document.getElementById('bSt').textContent=ok?'Connected':'Disconnected';
    document.getElementById('sMode').textContent=m.serialMode||'--';
    const dmLabels={'trainer_in':'Trainer IN','trainer_out':'Trainer OUT','telemetry':'Telemetry'};
    document.getElementById('bRole').textContent=dmLabels[m.deviceMode]||m.deviceMode||'--';
    const btm=m.deviceMode==='trainer_in'?'Master (Central)':'Slave (Peripheral)';
    document.getElementById('btMode').textContent=btm;
    document.getElementById('bName').textContent=m.btName||'--';
    document.getElementById('lAddr').textContent=m.localAddr||'--';
    document.getElementById('rAddr').textContent=m.remoteAddr||'--';
    document.getElementById('bTs').textContent=m.buildTs||'--';
    document.getElementById('selMode').value=m.serialMode;
    const rs=document.getElementById('selRole');
    rs.value=m.deviceMode; prevRole=m.deviceMode;
    if(document.getElementById('inName')!==document.activeElement)
      document.getElementById('inName').value=m.btName||'';
    if(document.getElementById('inSsid')!==document.activeElement)
      document.getElementById('inSsid').value=m.apSsid||'';
    if(document.getElementById('inApPass')!==document.activeElement)
      document.getElementById('inApPass').value=m.apPass||'';
    document.getElementById('scanCard').style.display=m.deviceMode==='trainer_in'?'':'none';
    document.getElementById('perCard').style.display=m.deviceMode==='trainer_out'?'':'none';
    // Telemetry card visibility
    const isTelem=m.serialMode==='sport_bt'||m.serialMode==='sport_mirror';
    document.getElementById('telemCard').style.display=isTelem?'':'none';
    if(isTelem){
      document.getElementById('selTelemOut').value=m.telemOutput||'wifi_udp';
      document.getElementById('mirrorBaudRow').style.display=m.serialMode==='sport_mirror'?'':'none';
      if(m.serialMode==='sport_mirror') document.getElementById('selBaud').value=m.sportBaud||'57600';
      const isUdp=(m.telemOutput||'wifi_udp')==='wifi_udp';
      document.getElementById('udpCfg').style.display=isUdp?'':'none';
      if(isUdp){
        const up=document.getElementById('inUdpPort');
        if(up!==document.activeElement)up.value=m.udpPort||'5010';
      }
      document.getElementById('tPkts').textContent=m.sportPkts||'0';
      document.getElementById('tPps').textContent=(m.sportPps||'0')+' pkt/s';
      document.getElementById('tOutSt').textContent=m.sportOutSt||'--';
    }
    // Central peer panel: show saved/connected device with appropriate buttons
    const cp=document.getElementById('cPeer');
    const sa=m.savedAddr||'';
    if(m.deviceMode==='trainer_in'&&(ok||sa)){
      const addr=ok&&m.remoteAddr?m.remoteAddr:sa;
      const ac=ok?'var(--ok)':'var(--mu)';
      let btns='';
      if(ok){
        btns='<button class="btn er sm" onclick="disconnectBt()">&#x2715; Disconnect</button>';
      }else if(sa){
        btns='<button class="btn sm" style="border-color:var(--ok);color:var(--ok)" onclick="connectBt(\''+sa+'\')">';
        btns+='&#x25B6; Connect</button>';
      }
      btns+=' <button class="btn sm" style="border-color:var(--wn);color:var(--wn)" onclick="forgetBt()">&#x1F5D1; Forget</button>';
      cp.innerHTML='<div class="peer" style="border-color:'+ac+'"><span class="da" style="color:'+ac+';font-size:.9em">'+addr+'</span>'
        +'<div style="display:flex;gap:5px">'+btns+'</div></div>';
      cp.style.display='block';
    }else{cp.style.display='none';cp.innerHTML='';}
    if(m.deviceMode==='trainer_out'){
      const pc=document.getElementById('perClients');
      if(ok&&m.remoteAddr){
        pc.innerHTML='<div class="peer"><span class="da">'+m.remoteAddr+'</span>'
          +'<button class="btn er sm" onclick="disconnectClient()">&#x2715; Disconnect</button></div>';
      }else{
        pc.innerHTML='<span style="color:var(--mu);font-size:.82em">No clients connected</span>';
      }
    }
  }
  else if(m.type==='scanResult'){
    let h='';
    m.devices.forEach(d=>{
      const n=d.name?d.name:'<i>unknown</i>';
      h+='<div class="dev"><div class="inf"><span class="da">'+d.address+'</span>'
        +'<span class="dm">'+n+(d.frsky?' &#9733; FrSky':'')+' &nbsp;'+d.rssi+'dBm</span></div>'
        +'<button class="btn ac sm" onclick="connectBt(this.dataset.a)" data-a="'+d.address+'">Connect</button></div>';
    });
    document.getElementById('scanList').innerHTML=h||'<div style="color:var(--mu);font-size:.8em;margin-top:6px">No devices found</div>';
    document.getElementById('btnScan').textContent='\u25B6 Scan';
  }
  else if(m.type==='scanning'){
    document.getElementById('btnScan').textContent='Scanning\u2026';
    document.getElementById('scanList').innerHTML='';
  }
}

// ── Actions ──
function setSerialMode(v){send({cmd:'setSerialMode',value:v});}
function setTelemOutput(v){send({cmd:'setTelemOutput',value:v});}
function setMirrorBaud(v){send({cmd:'setMirrorBaud',value:v});}
function setUdpPort(){send({cmd:'setUdpPort',value:document.getElementById('inUdpPort').value});}
function setBtName(){send({cmd:'setBtName',value:document.getElementById('inName').value});}
function setSsid(){showConfirm('Change SSID','Change AP SSID? The device will restart.',function(){send({cmd:'setSsid',value:document.getElementById('inSsid').value});});}
function setApPass(){showConfirm('Change AP Password','Change password (min 8 chars)? The device will restart.',function(){send({cmd:'setApPass',value:document.getElementById('inApPass').value});});}
function selRoleChange(sel){
  const nv=sel.value;
  sel.value=prevRole; // revert until confirmed
  showConfirm('Change Device Mode','Change mode to "'+nv+'"? The device will restart.',function(){
    prevRole=nv; sel.value=nv;
    send({cmd:'setDeviceMode',value:nv});
  });
}
function scanBt(){send({cmd:'scanBt'});}
function connectBt(a){send({cmd:'connectBt',value:a});setTimeout(getStatus,1500);}
function disconnectBt(){send({cmd:'disconnectBt'});setTimeout(getStatus,500);}
function forgetBt(){send({cmd:'forgetBt'});setTimeout(getStatus,500);}
function disconnectClient(){send({cmd:'disconnectClient'});}
function reboot(){
  showConfirm('Reboot Device',
    'After rebooting the device will start in normal mode and will not return to the configuration page.',
    function(){
      send({cmd:'reboot'});
      showReboot();
    });
}
const drop=document.getElementById('otaDrop');
const fin=document.getElementById('otaFile');
['dragover','dragenter'].forEach(ev=>drop.addEventListener(ev,e=>{e.preventDefault();drop.classList.add('drag');}));
['dragleave','drop'].forEach(ev=>drop.addEventListener(ev,e=>{e.preventDefault();drop.classList.remove('drag');}));
drop.addEventListener('drop',e=>{const f=e.dataTransfer.files[0];if(!f)return;fin.files=e.dataTransfer.files;setFile(f);});
fin.addEventListener('change',()=>{if(fin.files[0])setFile(fin.files[0]);});
function setFile(f){
  document.getElementById('ofn').textContent=f.name+' ('+Math.round(f.size/1024)+' KB)';
  document.getElementById('btnOta').style.display='inline-flex';
}
function showToast(m,d){const t=document.getElementById('toast');t.textContent=m;t.classList.add('show');setTimeout(()=>t.classList.remove('show'),d||4000);}
function doOta(){
  const f=fin.files[0];if(!f)return;
  const pg=document.getElementById('otaPrg'),msg=document.getElementById('otaMsg'),btn=document.getElementById('btnOta');
  pg.style.display='block';msg.textContent='Uploading\u2026 0%';btn.style.display='none';
  const x=new XMLHttpRequest();
  x.open('POST','/update',true);
  x.upload.onprogress=(ev)=>{if(ev.lengthComputable){const p=Math.round(ev.loaded/ev.total*100);pg.value=p;msg.textContent='Uploading\u2026 '+p+'%';}};
  x.onload=()=>{
    if(x.status===200 && x.responseText==='OK'){
      pg.value=100;msg.textContent='Upload complete \u2014 rebooting into AP mode\u2026';
      let att=0;
      const id=setInterval(()=>{
        att++;
        fetch('/',{cache:'no-cache'}).then(r=>{if(r.ok){clearInterval(id);location.href='/?otaDone=1';}}).catch(()=>{if(att>=20){clearInterval(id);msg.textContent='Reboot timeout \u2014 refresh manually.';btn.style.display='inline-flex';}});
      },1500);
    } else {
      msg.textContent='Update failed (HTTP '+x.status+': '+x.responseText+').';btn.style.display='inline-flex';
    }
  };
  x.onerror=()=>{msg.textContent='Upload error.';btn.style.display='inline-flex';};
  const fd=new FormData();fd.append('update',f);x.send(fd);
}
initWS();
</script>
<div class="toast" id="toast"></div>
<script>
(function(){const p=new URLSearchParams(location.search);if(p.get('otaDone')==='1'){showToast('\u2713 Firmware updated successfully!',5000);history.replaceState(null,'','/');}})();
</script>
</body>
</html>
)rawliteral";

// ─── WebSocket event handler ────────────────────────────────────────

static void handleWebSocketMessage(AsyncWebSocketClient* client, uint8_t* data, size_t len) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, data, len);
    if (err) {
        LOG_W("WEB", "JSON parse error: %s", err.c_str());
        return;
    }

    const char* cmd = doc["cmd"];
    if (!cmd) return;

    JsonDocument resp;

    // ─── getStatus ──────────────────────────────────────────────
    if (strcmp(cmd, "getStatus") == 0) {
        resp["type"]         = "status";
        resp["bleConnected"] = bleIsConnected();

        // Serial mode string
        const char* modeStr = "frsky";
        switch (g_config.serialMode) {
            case OutputMode::SBUS:         modeStr = "sbus";         break;
            case OutputMode::SPORT_BT:     modeStr = "sport_bt";     break;
            case OutputMode::SPORT_MIRROR: modeStr = "sport_mirror"; break;
            case OutputMode::LUA_SERIAL:   modeStr = "lua_serial";   break;
            default: break;
        }
        resp["serialMode"] = modeStr;
        resp["btName"]     = g_config.btName;
        resp["apSsid"]     = g_config.apSsid;
        resp["apPass"]     = g_config.apPass;
        resp["buildTs"]    = BUILD_TIMESTAMP;

        // Use live address if BLE is running, otherwise fall back to config cache
        const char* lAddr = bleGetLocalAddress();
        resp["localAddr"]  = (lAddr && lAddr[0]) ? lAddr : g_config.localBtAddr;

        const char* rAddr = bleGetRemoteAddress();
        resp["remoteAddr"] = (rAddr && rAddr[0]) ? rAddr : "";

        // Always send the saved address so the UI can show Forget button
        if (g_config.hasRemoteAddr && g_config.remoteBtAddr[0]) {
            resp["savedAddr"] = g_config.remoteBtAddr;
        }

        switch (g_config.deviceMode) {
            case DeviceMode::TRAINER_IN:  resp["deviceMode"] = "trainer_in";  break;
            case DeviceMode::TRAINER_OUT: resp["deviceMode"] = "trainer_out"; break;
            case DeviceMode::TELEMETRY:   resp["deviceMode"] = "telemetry";   break;
        }

        // Telemetry output settings
        resp["telemOutput"] = g_config.telemetryOutput == TelemetryOutput::BLE ? "ble" : "wifi_udp";
        resp["sportBaud"]   = String(g_config.sportBaud);
        resp["udpPort"]     = String(g_config.udpPort);
        resp["sportPkts"]   = String(sportGetPacketCount());
        resp["sportPps"]    = String(sportGetPacketsPerSec());

        // Telemetry output status
        if (g_config.serialMode == OutputMode::SPORT_BT ||
            g_config.serialMode == OutputMode::SPORT_MIRROR) {
            if (g_config.telemetryOutput == TelemetryOutput::WIFI_UDP) {
                resp["sportOutSt"] = sportUdpIsActive() ? "UDP Active" : "AP Starting...";
            } else {
                resp["sportOutSt"] = sportBleIsForwarding() ? "BLE Active" : "Waiting for client";
            }
        }
    }
    // ─── setSerialMode ──────────────────────────────────────────
    else if (strcmp(cmd, "setSerialMode") == 0) {
        const char* val = doc["value"];
        if (val) {
            if      (strcmp(val, "sbus") == 0)         g_config.serialMode = OutputMode::SBUS;
            else if (strcmp(val, "sport_bt") == 0)     g_config.serialMode = OutputMode::SPORT_BT;
            else if (strcmp(val, "sport_mirror") == 0) g_config.serialMode = OutputMode::SPORT_MIRROR;
            else if (strcmp(val, "lua_serial") == 0)   g_config.serialMode = OutputMode::LUA_SERIAL;
            else                                       g_config.serialMode = OutputMode::FRSKY;
            LOG_I("WEB", "Serial mode set to %s", val);
            configSave();
        }
        resp["type"] = "ack";
        resp["cmd"]  = cmd;
        resp["ok"]   = true;
    }
    // ─── setTelemOutput ─────────────────────────────────────────
    else if (strcmp(cmd, "setTelemOutput") == 0) {
        const char* val = doc["value"];
        if (val) {
            g_config.telemetryOutput = (strcmp(val, "ble") == 0)
                                       ? TelemetryOutput::BLE
                                       : TelemetryOutput::WIFI_UDP;
            LOG_I("WEB", "Telemetry output set to %s", val);
            configSave();
        }
        resp["type"] = "ack";
        resp["cmd"]  = cmd;
        resp["ok"]   = true;
    }
    // ─── setMirrorBaud ──────────────────────────────────────────
    else if (strcmp(cmd, "setMirrorBaud") == 0) {
        const char* val = doc["value"];
        if (val) {
            g_config.sportBaud = (uint32_t)atol(val);
            LOG_I("WEB", "Mirror baud set to %lu", g_config.sportBaud);
            configSave();
        }
        resp["type"] = "ack";
        resp["cmd"]  = cmd;
        resp["ok"]   = true;
    }
    // ─── setUdpPort ─────────────────────────────────────────────
    else if (strcmp(cmd, "setUdpPort") == 0) {
        const char* val = doc["value"];
        if (val) {
            g_config.udpPort = (uint16_t)atoi(val);
            LOG_I("WEB", "UDP port set to %u", g_config.udpPort);
            configSave();
        }
        resp["type"] = "ack";
        resp["cmd"]  = cmd;
        resp["ok"]   = true;
    }
    // ─── setBtName ──────────────────────────────────────────────
    else if (strcmp(cmd, "setBtName") == 0) {
        const char* val = doc["value"];
        if (val && strlen(val) > 0) {
            strlcpy(g_config.btName, val, sizeof(g_config.btName));
            LOG_I("WEB", "BT name set to %s", val);
            configSave();
            // Lightweight: just restart advertising with new name, no BLE stack restart
            bleUpdateAdvertisingName();
        }
        resp["type"] = "ack";
        resp["cmd"]  = cmd;
        resp["ok"]   = true;
    }
    // ─── setSsid ────────────────────────────────────────────────
    else if (strcmp(cmd, "setSsid") == 0) {
        const char* val = doc["value"];
        if (val && strlen(val) > 0) {
            strlcpy(g_config.apSsid, val, sizeof(g_config.apSsid));
            LOG_I("WEB", "AP SSID set to %s — restarting", val);
            configSave();
        }
        resp["type"]   = "ack";
        resp["cmd"]    = cmd;
        resp["ok"]     = true;
        resp["reboot"] = true;
        { String out; serializeJson(resp, out); client->text(out); }
        delay(400);
        ESP.restart();
        return;
    }
    // ─── setApPass ──────────────────────────────────────────────
    else if (strcmp(cmd, "setApPass") == 0) {
        const char* val = doc["value"];
        bool ok = (val && strlen(val) >= 8);
        if (ok) {
            strlcpy(g_config.apPass, val, sizeof(g_config.apPass));
            LOG_I("WEB", "AP pass updated — restarting");
            configSave();
        }
        resp["type"]   = "ack";
        resp["cmd"]    = cmd;
        resp["ok"]     = ok;
        resp["reboot"] = true;
        { String out; serializeJson(resp, out); client->text(out); }
        delay(400);
        ESP.restart();
        return;
    }
    // ─── setDeviceMode ────────────────────────────────────────────
    else if (strcmp(cmd, "setDeviceMode") == 0) {
        const char* val = doc["value"];
        if (val) {
            if (strcmp(val, "trainer_in") == 0)       g_config.deviceMode = DeviceMode::TRAINER_IN;
            else if (strcmp(val, "trainer_out") == 0) g_config.deviceMode = DeviceMode::TRAINER_OUT;
            else if (strcmp(val, "telemetry") == 0)   g_config.deviceMode = DeviceMode::TELEMETRY;
            LOG_I("WEB", "Device mode set to %s — restarting to AP mode", val);
            configSave();
        }
        // Role change requires full BLE stack reinit (NimBLE deinit is unsafe from
        // loop task while connected). Restart ESP keeping AP mode flag set.
        resp["type"]   = "ack";
        resp["cmd"]    = cmd;
        resp["ok"]     = true;
        resp["reboot"] = true;
        { String out; serializeJson(resp, out); client->text(out); }
        delay(400);
        { Preferences p; p.begin("btwboot", false); p.putUChar("mode", 1); p.end(); }
        ESP.restart();
        return;
    }
    // ─── scanBt ─────────────────────────────────────────────────
    else if (strcmp(cmd, "scanBt") == 0) {
        bleScanStart();
        resp["type"] = "scanning";
    }
    // ─── connectBt ──────────────────────────────────────────────
    else if (strcmp(cmd, "connectBt") == 0) {
        const char* addr = doc["value"];
        bool ok = false;
        if (addr) ok = bleConnectTo(addr);
        resp["type"] = "ack";
        resp["cmd"]  = cmd;
        resp["ok"]   = ok;
    }
    // ─── disconnectBt (central: stop auto-reconnect + disconnect) ──
    else if (strcmp(cmd, "disconnectBt") == 0) {
        bleDisconnect();
        resp["type"] = "ack";
        resp["cmd"]  = cmd;
        resp["ok"]   = true;
    }
    // ─── forgetBt (central: clear saved address, disable auto-reconnect) ──
    else if (strcmp(cmd, "forgetBt") == 0) {
        bleForget();
        resp["type"] = "ack";
        resp["cmd"]  = cmd;
        resp["ok"]   = true;
    }
    // ─── disconnectClient (peripheral: kick connected central) ──
    else if (strcmp(cmd, "disconnectClient") == 0) {
        bleKickClient();
        resp["type"] = "ack";
        resp["cmd"]  = cmd;
        resp["ok"]   = true;
    }
    // ─── save ───────────────────────────────────────────────────
    else if (strcmp(cmd, "save") == 0) {
        configSave();
        resp["type"] = "ack";
        resp["cmd"]  = cmd;
        resp["ok"]   = true;
    }
    // ─── reboot ─────────────────────────────────────────────────
    else if (strcmp(cmd, "reboot") == 0) {
        resp["type"] = "ack";
        resp["cmd"]  = cmd;
        resp["ok"]   = true;

        String out;
        serializeJson(resp, out);
        client->text(out);
        delay(300);
        // Write the correct boot mode before restart so the device lands in
        // the right state: LUA Serial + WiFi UDP → TELEM_AP (WebUI stays up,
        // Lua not blocked); everything else → BOOT_NORMAL (setup() decides).
        if (g_config.serialMode == OutputMode::LUA_SERIAL &&
            g_config.telemetryOutput == TelemetryOutput::WIFI_UDP) {
            Preferences p; p.begin("btwboot", false);
            p.putUChar("mode", 2);  // BOOT_TELEM_AP
            p.end();
        }
        ESP.restart();
        return;
    }
    else {
        resp["type"] = "ack";
        resp["cmd"]  = cmd;
        resp["ok"]   = false;
    }

    String out;
    serializeJson(resp, out);
    client->text(out);
}

static void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                      AwsEventType type, void* arg, uint8_t* data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT:
            LOG_D("WEB", "WebSocket client #%u connected", client->id());
            // Discard messages instead of closing connection when queue is full
            // (queue pressure from WiFi/BLE coexistence should not kill the session)
            client->setCloseClientOnQueueFull(false);
            // Send a WS ping every 20 s so TCP keepalive timers reset even if
            // the application-level traffic pauses
            client->keepAlivePeriod(20);
            break;
        case WS_EVT_DISCONNECT:
            LOG_D("WEB", "WebSocket client #%u disconnected", client->id());
            break;
        case WS_EVT_DATA: {
            AwsFrameInfo* info = (AwsFrameInfo*)arg;
            if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
                handleWebSocketMessage(client, data, len);
            }
            break;
        }
        case WS_EVT_ERROR:
            LOG_W("WEB", "WebSocket error #%u", client->id());
            break;
        case WS_EVT_PONG:
            break;
    }
}

// ─── Send scan results (called periodically when scan completes) ────
static void sendScanResults() {
    if (!s_active || s_ws.count() == 0) return;

    BleScanResult results[MAX_SCAN_RESULTS];
    uint8_t count = bleGetScanResults(results, MAX_SCAN_RESULTS);

    JsonDocument doc;
    doc["type"] = "scanResult";
    JsonArray devices = doc["devices"].to<JsonArray>();

    for (uint8_t i = 0; i < count; i++) {
        JsonObject dev = devices.add<JsonObject>();
        dev["address"] = results[i].address;
        dev["rssi"]    = results[i].rssi;
        dev["name"]    = results[i].name;
        dev["frsky"]   = results[i].hasFrskyService;
        dev["addrType"]= results[i].addrType;
    }

    String out;
    serializeJson(doc, out);
    s_ws.textAll(out);
}

// ─── OTA handler ────────────────────────────────────────────────────

static void handleOtaUpload(AsyncWebServerRequest* request, const String& filename,
                            size_t index, uint8_t* data, size_t len, bool final) {
    if (index == 0) {
        LOG_I("OTA", "Starting update: %s", filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            LOG_E("OTA", "Update.begin() failed");
            Update.abort();
            return;
        }
    }

    if (!Update.isRunning()) return;  // begin failed on a previous chunk

    if (Update.write(data, len) != len) {
        LOG_E("OTA", "Write error at index %u", index);
        Update.abort();
        return;
    }

    if (final) {
        if (Update.end(true)) {
            LOG_I("OTA", "Update success: %u bytes written", index + len);
            // Set restart flag HERE, after Update.end() succeeds.
            // If done in the response handler there is a race: the response
            // callback may run before this final chunk, causing a restart
            // before Update.end() is ever called — boot bits never set.
            s_otaPendingRestart = true;
        } else {
            LOG_E("OTA", "Update.end() failed: %s", Update.errorString());
            Update.printError(Serial);
            Update.abort();
        }
    }
}

// ─── Public API ─────────────────────────────────────────────────────

void webUiInit() {
    if (s_active) return;

    LOG_D("WEB", "Free heap before WiFi start: %u", ESP.getFreeHeap());

    // Prevent the WiFi driver from writing SSID/pass to NVS on its own
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP);
    delay(100);  // let radio + netif settle

    // Explicit params: channel 1, not hidden, max 4 clients
    if (!WiFi.softAP(g_config.apSsid, g_config.apPass, 1, 0, 4)) {
        LOG_E("WEB", "softAP() failed!");
        WiFi.mode(WIFI_OFF);
        return;
    }

    // Wait until the AP interface has a valid IP (DHCP server ready)
    {
        uint32_t t0 = millis();
        while (WiFi.softAPIP() == IPAddress(0, 0, 0, 0) && millis() - t0 < 3000) {
            delay(50);
        }
    }
    if (WiFi.softAPIP() == IPAddress(0, 0, 0, 0)) {
        LOG_E("WEB", "AP interface has no IP — aborting");
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
        return;
    }

    delay(200);  // let AP + DHCP stabilise before starting HTTP server

    // BLE is NOT initialized yet (lazy init), so WIFI_PS_NONE is safe here.
    // Disabling modem-sleep ensures beacons are sent on time and the SSID
    // stays visible consistently. The coex scheduler will take over once
    // ensureController() is called later.
    esp_wifi_set_ps(WIFI_PS_NONE);

    LOG_I("WEB", "AP started: SSID=%s IP=%s",
          g_config.apSsid, WiFi.softAPIP().toString().c_str());
    LOG_D("WEB", "Free heap after WiFi start: %u", ESP.getFreeHeap());

    // Register WebSocket handler and routes only once
    if (!s_wsAdded) {
        s_ws.onEvent(onWsEvent);
        s_server.addHandler(&s_ws);

        s_server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
            request->send(200, "text/html", INDEX_HTML);
        });

        s_server.on("/update", HTTP_POST,
            [](AsyncWebServerRequest* request) {
                // s_otaPendingRestart is set by handleOtaUpload after Update.end() succeeds.
                // Here we just report the outcome; never call delay()/restart() in this callback.
                bool success = !Update.hasError() && s_otaPendingRestart;
                request->send(success ? 200 : 500, "text/plain", success ? "OK" : "FAIL");
                if (!success) {
                    LOG_E("OTA", "Update reported failure in response handler");
                }
            },
            handleOtaUpload
        );

        // Captive portal detection endpoints — redirect to main page
        // Android
        s_server.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest* r) {
            r->redirect("http://" + WiFi.softAPIP().toString() + "/");
        });
        // Windows
        s_server.on("/connecttest.txt", HTTP_GET, [](AsyncWebServerRequest* r) {
            r->redirect("http://" + WiFi.softAPIP().toString() + "/");
        });
        s_server.on("/ncsi.txt", HTTP_GET, [](AsyncWebServerRequest* r) {
            r->redirect("http://" + WiFi.softAPIP().toString() + "/");
        });
        // Apple
        s_server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest* r) {
            r->redirect("http://" + WiFi.softAPIP().toString() + "/");
        });
        // Firefox
        s_server.on("/canonical.html", HTTP_GET, [](AsyncWebServerRequest* r) {
            r->redirect("http://" + WiFi.softAPIP().toString() + "/");
        });
        s_server.on("/success.txt", HTTP_GET, [](AsyncWebServerRequest* r) {
            r->send(200, "text/plain", "success");
        });

        // Catch-all: any unknown URL redirects to the config page
        s_server.onNotFound([](AsyncWebServerRequest* r) {
            r->redirect("http://" + WiFi.softAPIP().toString() + "/");
        });

        s_wsAdded = true;
    }

    s_server.begin();
    s_active = true;

    LOG_I("WEB", "Web server started");
    LOG_D("WEB", "Free heap after server start: %u", ESP.getFreeHeap());
}

void webUiStop() {
    if (!s_active) return;

    // Close all WebSocket clients gracefully, then give them time to close
    s_ws.closeAll();
    delay(100);
    s_ws.cleanupClients();

    // Stop HTTP server
    s_server.end();

    // Disconnect WiFi AP
    WiFi.softAPdisconnect(true);
    delay(50);
    WiFi.mode(WIFI_OFF);
    delay(50);

    s_active = false;
    LOG_I("WEB", "Web server stopped");
    LOG_D("WEB", "Free heap after stop: %u", ESP.getFreeHeap());
}

void webUiLoop() {
    if (!s_active) return;

    // OTA completed successfully — wait for the response to be fully flushed
    // over TCP before restarting. 1500 ms is enough for the client to receive
    // the "OK" body and show the "Rebooting…" message.
    if (s_otaPendingRestart) {
        delay(1500);
        // After OTA, keep the WiFi AP up so the browser can reconnect.
        // For LUA Serial + WiFi UDP: use TELEM_AP — WiFi AP stays up AND
        // the Lua script is not blocked (no orange overlay on the radio).
        // All other configurations: use regular AP_MODE.
        bool luaWifi = (g_config.serialMode == OutputMode::LUA_SERIAL &&
                        g_config.telemetryOutput == TelemetryOutput::WIFI_UDP);
        uint8_t nextMode = luaWifi ? 2 : 1;  // BOOT_TELEM_AP : BOOT_AP_MODE
        Preferences prefs;
        prefs.begin("btwboot", false);
        prefs.putUChar("mode", nextMode);
        prefs.end();
        ESP.restart();
    }

    // Clean up disconnected WebSocket clients — rate-limited to avoid per-tick overhead
    static uint32_t lastCleanupMs = 0;
    uint32_t nowMs = millis();
    if (nowMs - lastCleanupMs >= 1000) {
        lastCleanupMs = nowMs;
        s_ws.cleanupClients(4);
    }

    // Proactive status push every 3 s — keeps the TCP connection alive
    // under WiFi/BLE coexistence and avoids relying solely on client polling.
    static uint32_t lastPush = 0;
    if (s_ws.count() > 0 && millis() - lastPush >= 3000) {
        lastPush = millis();

        JsonDocument doc;
        doc["type"]         = "status";
        doc["bleConnected"] = bleIsConnected();

        const char* mStr = "frsky";
        switch (g_config.serialMode) {
            case OutputMode::SBUS:         mStr = "sbus";         break;
            case OutputMode::SPORT_BT:     mStr = "sport_bt";     break;
            case OutputMode::SPORT_MIRROR: mStr = "sport_mirror"; break;
            case OutputMode::LUA_SERIAL:   mStr = "lua_serial";   break;
            default: break;
        }
        doc["serialMode"] = mStr;
        doc["btName"]     = g_config.btName;
        doc["apSsid"]     = g_config.apSsid;
        doc["buildTs"]    = BUILD_TIMESTAMP;

        const char* lAddr = bleGetLocalAddress();
        doc["localAddr"]  = (lAddr && lAddr[0]) ? lAddr : g_config.localBtAddr;

        const char* rAddr = bleGetRemoteAddress();
        doc["remoteAddr"] = (rAddr && rAddr[0]) ? rAddr : "";

        if (g_config.hasRemoteAddr && g_config.remoteBtAddr[0]) {
            doc["savedAddr"] = g_config.remoteBtAddr;
        }

        switch (g_config.deviceMode) {
            case DeviceMode::TRAINER_IN:  doc["deviceMode"] = "trainer_in";  break;
            case DeviceMode::TRAINER_OUT: doc["deviceMode"] = "trainer_out"; break;
            case DeviceMode::TELEMETRY:   doc["deviceMode"] = "telemetry";   break;
        }

        // Telemetry info for periodic push
        doc["telemOutput"] = g_config.telemetryOutput == TelemetryOutput::BLE ? "ble" : "wifi_udp";
        doc["sportBaud"]   = String(g_config.sportBaud);
        doc["udpPort"]     = String(g_config.udpPort);
        doc["sportPkts"]   = String(sportGetPacketCount());
        doc["sportPps"]    = String(sportGetPacketsPerSec());

        String out;
        serializeJson(doc, out);
        s_ws.textAll(out);
    }

    // Check if scan completed and send results
    static bool lastScanning = false;
    bool nowScanning = bleIsScanning();
    if (lastScanning && !nowScanning) {
        sendScanResults();
    }
    lastScanning = nowScanning;
}

bool webUiIsActive() {
    return s_active;
}
