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
 *     {"cmd":"setSerialMode","value":"frsky"|"sbus"|"sport_bt"|"sport_mirror"}
 *     {"cmd":"setBtName","value":"..."}
 *     {"cmd":"setBtRole","value":"peripheral"|"central"|"telemetry"}
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

// ─── AP configuration ───────────────────────────────────────────────
static const char* AP_SSID     = "BTWifiSerial";
static const char* AP_PASS     = "12345678";

// ─── Server instances (static — never heap-allocated to avoid fragmentation) ──
static AsyncWebServer s_server(80);
static AsyncWebSocket s_ws("/ws");
static bool s_active    = false;
static bool s_wsAdded   = false;  // handler registered only once

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
progress{width:100%;height:3px;margin-top:8px;display:none;accent-color:var(--ac)}
.msg{font-size:.82em;color:var(--wn);margin-top:4px;min-height:1.1em}
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
  <div class="row"><span class="l">BT Role</span><span class="v" id="bRole">--</span></div>
  <div class="row"><span class="l">BT Name</span><span class="v" id="bName">--</span></div>
  <div class="row"><span class="l">Local Addr</span><span class="v" id="lAddr">--</span></div>
  <div class="row"><span class="l">Remote Addr</span><span class="v" id="rAddr">--</span></div>
</div>

<div class="card">
  <div class="ct">Serial Mode</div>
  <label>Mode</label>
  <select id="selMode" onchange="setSerialMode(this.value)">
    <option value="frsky">FrSky Trainer (CC2540)</option>
    <option value="sbus">SBUS Trainer</option>
    <option value="sport_bt">S.PORT Telemetry (BT)</option>
    <option value="sport_mirror">S.PORT Telemetry (Mirror)</option>
  </select>
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
  <label>Device Name</label>
  <div class="irow">
    <input type="text" id="inName" maxlength="30" placeholder="BTWifiSerial">
    <button class="btn ac" onclick="setBtName()">Set Name</button>
  </div>
  <label>Role</label>
  <select id="selRole" onchange="selRoleChange(this)">
    <option value="peripheral">Peripheral (Slave)</option>
    <option value="central">Central (Master)</option>
    <option value="telemetry">Telemetry</option>
  </select>
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
    document.getElementById('bRole').textContent=m.btRole||'--';
    document.getElementById('bName').textContent=m.btName||'--';
    document.getElementById('lAddr').textContent=m.localAddr||'--';
    document.getElementById('rAddr').textContent=m.remoteAddr||'--';
    document.getElementById('selMode').value=m.serialMode;
    const rs=document.getElementById('selRole');
    rs.value=m.btRole; prevRole=m.btRole;
    if(document.getElementById('inName')!==document.activeElement)
      document.getElementById('inName').value=m.btName||'';
    document.getElementById('scanCard').style.display=m.btRole==='central'?'':'none';
    document.getElementById('perCard').style.display=m.btRole==='peripheral'?'':'none';
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
    if(m.btRole==='central'&&(ok||sa)){
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
    if(m.btRole==='peripheral'){
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
function selRoleChange(sel){
  const nv=sel.value;
  sel.value=prevRole; // revert until confirmed
  showConfirm('Change BLE Role','Change role to "'+nv+'"? The device will restart.',function(){
    prevRole=nv; sel.value=nv;
    send({cmd:'setBtRole',value:nv});
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
function doOta(){
  const f=fin.files[0];if(!f)return;
  const pg=document.getElementById('otaPrg'),msg=document.getElementById('otaMsg');
  pg.style.display='block';msg.textContent='Uploading\u2026';
  const x=new XMLHttpRequest();
  x.open('POST','/update',true);
  x.upload.onprogress=(ev)=>{if(ev.lengthComputable)pg.value=ev.loaded/ev.total*100;};
  x.onload=()=>{msg.textContent=x.status===200?'Update OK! Rebooting\u2026':'Update failed ('+x.status+')';
    if(x.status===200)setTimeout(()=>location.reload(),5000);};
  x.onerror=()=>{msg.textContent='Upload error.';};
  const fd=new FormData();fd.append('update',f);x.send(fd);
}
initWS();
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
            default: break;
        }
        resp["serialMode"] = modeStr;
        resp["btName"]     = g_config.btName;

        // Use live address if BLE is running, otherwise fall back to config cache
        const char* lAddr = bleGetLocalAddress();
        resp["localAddr"]  = (lAddr && lAddr[0]) ? lAddr : g_config.localBtAddr;

        const char* rAddr = bleGetRemoteAddress();
        resp["remoteAddr"] = (rAddr && rAddr[0]) ? rAddr : "";

        // Always send the saved address so the UI can show Forget button
        if (g_config.hasRemoteAddr && g_config.remoteBtAddr[0]) {
            resp["savedAddr"] = g_config.remoteBtAddr;
        }

        switch (g_config.bleRole) {
            case BleRole::PERIPHERAL: resp["btRole"] = "peripheral"; break;
            case BleRole::CENTRAL:    resp["btRole"] = "central";    break;
            case BleRole::TELEMETRY:  resp["btRole"] = "telemetry";  break;
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
    // ─── setBtRole ──────────────────────────────────────────────
    else if (strcmp(cmd, "setBtRole") == 0) {
        const char* val = doc["value"];
        if (val) {
            if (strcmp(val, "peripheral") == 0)      g_config.bleRole = BleRole::PERIPHERAL;
            else if (strcmp(val, "central") == 0)    g_config.bleRole = BleRole::CENTRAL;
            else if (strcmp(val, "telemetry") == 0)  g_config.bleRole = BleRole::TELEMETRY;
            LOG_I("WEB", "BT role set to %s — restarting to AP mode", val);
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
            Update.printError(Serial);
        }
    }

    if (Update.isRunning()) {
        if (Update.write(data, len) != len) {
            Update.printError(Serial);
        }
    }

    if (final) {
        if (Update.end(true)) {
            LOG_I("OTA", "Update success, %u bytes", index + len);
        } else {
            Update.printError(Serial);
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
    if (!WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 4)) {
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
          AP_SSID, WiFi.softAPIP().toString().c_str());
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
                request->send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
                if (!Update.hasError()) {
                    delay(500);
                    ESP.restart();
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

    // Clean up disconnected WebSocket clients (allow up to 4 simultaneous)
    s_ws.cleanupClients(4);

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
            default: break;
        }
        doc["serialMode"] = mStr;
        doc["btName"]     = g_config.btName;

        const char* lAddr = bleGetLocalAddress();
        doc["localAddr"]  = (lAddr && lAddr[0]) ? lAddr : g_config.localBtAddr;

        const char* rAddr = bleGetRemoteAddress();
        doc["remoteAddr"] = (rAddr && rAddr[0]) ? rAddr : "";

        if (g_config.hasRemoteAddr && g_config.remoteBtAddr[0]) {
            doc["savedAddr"] = g_config.remoteBtAddr;
        }

        switch (g_config.bleRole) {
            case BleRole::PERIPHERAL: doc["btRole"] = "peripheral"; break;
            case BleRole::CENTRAL:    doc["btRole"] = "central";    break;
            case BleRole::TELEMETRY:  doc["btRole"] = "telemetry";  break;
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
