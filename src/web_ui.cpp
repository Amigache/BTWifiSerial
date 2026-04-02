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
 *     {"cmd":"factoryReset"}
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
#include <esp_mac.h>
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
static bool s_cfgPendingRestart = false; // config saved but restart not applied yet

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
body{font-family:'Segoe UI',system-ui,sans-serif;background:var(--bg);color:var(--tx);padding:12px;max-width:980px;margin:0 auto;font-size:15px}
h1{text-align:center;color:var(--ac);margin-bottom:14px;font-size:1.1em;letter-spacing:.12em;text-transform:uppercase;font-weight:600}
#cards{column-count:2;column-gap:10px}
.card{background:var(--sf);border:1px solid var(--bd);border-radius:4px;padding:13px;margin:0 0 10px;break-inside:avoid;display:inline-block;width:100%}
.ct{color:var(--ac);font-size:.78em;font-weight:700;letter-spacing:.12em;text-transform:uppercase;margin-bottom:9px;padding-bottom:6px;border-bottom:1px solid var(--bd)}
.row{display:flex;justify-content:space-between;align-items:center;padding:3px 0;font-size:.88em}
.row .l{color:var(--mu)}.row .v{color:var(--tx);font-family:monospace;font-size:.92em}
.dot{width:7px;height:7px;border-radius:50%;display:inline-block;margin-right:5px;vertical-align:middle}
.ok{background:var(--ok)}.er{background:var(--er)}
label{display:block;font-size:.82em;color:var(--mu);margin-top:9px;margin-bottom:3px}
select,input[type=text]{width:100%;padding:6px 8px;border:1px solid var(--bd);background:var(--bg);color:var(--tx);border-radius:3px;font-size:.9em}
select:focus,input:focus{outline:none;border-color:var(--ac)}
.btn{display:inline-flex;align-items:center;gap:4px;padding:5px 13px;border:1px solid var(--bd);border-radius:3px;cursor:pointer;font-size:.85em;line-height:1;background:var(--sf);color:var(--tx);transition:border-color .15s,color .15s;margin-top:7px;white-space:nowrap}
.btn:hover{border-color:var(--ac);color:var(--ac)}
.btn.ac{background:var(--ac);color:#000;border-color:var(--ac)}.btn.ac:hover{background:#79b8ff;border-color:#79b8ff}
.btn.er{border-color:var(--er);color:var(--er)}.btn.er:hover{background:var(--er);color:#fff;border-color:var(--er)}
.btn.sys{min-width:132px;height:32px;justify-content:center}
.btn.sys .ico{width:14px;display:inline-flex;align-items:center;justify-content:center;line-height:1}
.btn.sys .lbl{line-height:1}
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
.toast.ok{background:var(--ok);color:#fff}
.toast.info{background:var(--ac);color:#000}
.toast.warn{background:var(--wn);color:#000}
.toast.err{background:var(--er);color:#fff}
.toast.show{transform:translateY(0)}
.mbg{position:fixed;inset:0;background:rgba(13,17,23,.82);z-index:100;display:flex;align-items:center;justify-content:center}
.mbox{background:var(--sf);border:1px solid var(--bd);border-radius:4px;padding:20px 18px;max-width:300px;width:92%}
.mbox h3{color:var(--tx);font-size:.95em;margin-bottom:7px}
.mbox p{color:var(--mu);font-size:.83em;margin-bottom:15px;line-height:1.4}
.mbtns{display:flex;gap:8px;justify-content:flex-end}
@media (max-width:760px){
  #cards{column-count:1}
}
</style>
</head>
<body>
<h1>&#x25C8; BTWifiSerial</h1>

<div id="cards">

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
  <div class="row"><span class="l">Pending Restart</span><span class="v" id="rpSt">No</span></div>
</div>

<div class="card">
  <div class="ct">System Config</div>
  <label>Device Mode</label>
  <select id="selRole" onchange="markSystemDirty()">
    <option value="trainer_in">Trainer IN (Central)</option>
    <option value="trainer_out">Trainer OUT (Peripheral)</option>
    <option value="telemetry">Telemetry</option>
    <option value="elrs_ht">ELRS HT (Head Tracking)</option>
  </select>
  <label>Serial Mode</label>
  <select id="selMode" onchange="systemSerialChanged()">
    <option value="frsky">FrSky Trainer (CC2540)</option>
    <option value="sbus">SBUS Trainer</option>
    <option value="sport_bt">S.PORT Telemetry (CC2540)</option>
    <option value="sport_mirror">S.PORT Telemetry (Mirror)</option>
    <option value="lua_serial">LUA Serial (EdgeTX)</option>
  </select>
  <div id="mapModeRow" style="display:none">
    <label>Trainer Map</label>
    <select id="selMapMode" onchange="markSystemDirty()">
      <option value="gv">GV (Global Variables)</option>
      <option value="tr">TR (Trainer Channels)</option>
    </select>
  </div>
  <div style="text-align:right;margin-top:10px">
    <button class="btn ac" onclick="saveSystem()">&#x1F4BE; Save &amp; Restart</button>
  </div>
</div>

<div class="card">
  <div class="ct">WiFi Config</div>
  <label>WiFi Mode</label>
  <select id="selWifi" onchange="wifiModeChanged(this.value)">
    <option value="off">Off (BLE only)</option>
    <option value="ap">AP (Access Point)</option>
    <option value="sta">STA (Connect to network)</option>
  </select>
  <div id="apCfg" style="display:none">
    <label>AP SSID</label>
    <div class="irow">
      <input type="text" id="inSsid" maxlength="15" placeholder="BTWifiSerial" oninput="markWifiDirty()">
    </div>
    <label>AP Password</label>
    <div class="irow">
      <input type="text" id="inApPass" maxlength="15" placeholder="12345678" oninput="markWifiDirty()">
    </div>
  </div>
  <div id="staCfg" style="display:none">
    <label>STA SSID</label>
    <div class="irow">
      <input type="text" id="inStaSsid" maxlength="31" placeholder="MyHomeWiFi" oninput="markWifiDirty()">
    </div>
    <label>STA Password</label>
    <div class="irow">
      <input type="text" id="inStaPass" maxlength="63" placeholder="" oninput="markWifiDirty()">
    </div>
  </div>
  <div style="text-align:right;margin-top:10px">
    <button class="btn ac" onclick="saveWifi()">&#x1F4BE; Save &amp; Restart</button>
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

<div class="card" id="btCard">
  <div class="ct">Bluetooth</div>
  <label>BT Name</label>
  <div class="irow">
    <input type="text" id="inName" maxlength="15" placeholder="BTWifiSerial" oninput="markBluetoothDirty()">
  </div>
  <div style="text-align:right;margin-top:10px">
    <button class="btn ac" onclick="saveBluetooth()">&#x1F4BE; Save</button>
  </div>
  <div id="scanCard" style="display:none;margin-top:10px">
    <label class="ct">BLE Scan</label>
    <div id="cPeer" style="display:none"></div>
    <button class="btn ac" id="btnScan" onclick="scanBt()" style="margin-top:8px">&#x25B6; Scan</button>
    <div id="scanList"></div>
  </div>
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
  <div class="ct" style="margin-top:10px">System Actions</div>
  <button class="btn sys" style="border-color:var(--wn);color:var(--wn)" onclick="factoryReset()"><span class="ico">&#x26A0;</span><span class="lbl">Factory Reset</span></button>
  <button class="btn er sys" onclick="reboot()"><span class="ico">&#x21BA;</span><span class="lbl">Reboot</span></button>
</div>

</div>

<div id="rbOverlay" style="display:none;position:fixed;inset:0;background:#0d1117;z-index:99;align-items:center;justify-content:center;flex-direction:column;gap:14px">
  <span style="color:#e6edf3;font-size:1.3em;font-weight:600;letter-spacing:.04em">&#x25C8;&nbsp;BTWifiSerial</span>
  <span style="color:#6e7681;font-size:.82em;max-width:280px;text-align:center;line-height:1.5">
    Device is rebooting. Reconnect to access the configuration page.
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
let ws,rTimer;
let systemDirty=false;
let bluetoothDirty=false;
let wifiDirty=false;
let telemDirty=false;

// BLE connection state tracking
let lastBleConnected=false;
let bleConnectPending=false;
let bleConnectTimer=null;
let toastTimer=null;

// ── WebSocket ──
function initWS(){
  if(ws&&ws.readyState<2)return;
  if(ws){ws.onclose=null;ws.close();}
  ws=new WebSocket('ws://'+location.host+'/ws');
  ws.onopen=()=>{clearTimeout(rTimer);getStatus();};
  ws.onclose=()=>{rTimer=setTimeout(initWS,3000);};
  ws.onmessage=(e)=>handle(JSON.parse(e.data));
}
function send(o){
  if(ws&&ws.readyState===1){
    ws.send(JSON.stringify(o));
    return true;
  }
  showToast('WebSocket disconnected. Reconnecting...',5000,'warn');
  initWS();
  return false;
}
function getStatus(){send({cmd:'getStatus'});}

// ── Reboot overlay: show static goodbye screen, close WS ──
function showReboot(){
  document.getElementById('rbOverlay').style.display='flex';
  if(ws){ws.onclose=null;ws.close();ws=null;}
}

// ── Confirm modal ──
let mCb=null;
let mCbCancel=null;
function showConfirm(title,msg,cb){
  document.getElementById('mTitle').textContent=title;
  document.getElementById('mMsg').textContent=msg;
  mCb=cb;
  mCbCancel=null;
  document.getElementById('modal').style.display='flex';
}
function modalOk(){document.getElementById('modal').style.display='none';if(mCb)mCb();mCb=null;mCbCancel=null;}
function modalCancel(){
  document.getElementById('modal').style.display='none';
  if(mCbCancel)mCbCancel();
  mCb=null;
  mCbCancel=null;
}

// ── Message handler ──
function handle(m){
  if(m.type==='ack'&&m.reboot){showReboot();return;}
  if(m.type==='ack'){
    if(m.cmd==='connectBt' && m.ok===false){
      clearTimeout(bleConnectTimer);
      bleConnectPending=false;
      showToast('✗ Connection failed',5000,'err');
      return;
    }
    if(m.cmd==='disconnectBt' && m.ok===false){
      showToast('✗ Disconnect failed',4000,'err');
      return;
    }
    if(m.cmd==='forgetBt' && m.ok===false){
      showToast('✗ Forget failed',4000,'err');
      return;
    }
  }
  if(m.type==='status'){
    const ok=m.bleConnected;
    
    // Detect BLE connection state changes
    if(bleConnectPending && !ok){
      // Still waiting for connection (timeout not yet reached)
    } else if(bleConnectPending && ok){
      // Connection succeeded!
      clearTimeout(bleConnectTimer);
      bleConnectPending=false;
      showToast('✓ Connected successfully',3000,'ok');
    } else if(!lastBleConnected && ok){
      // Spontaneous connection (auto-reconnect)
      showToast('✓ Connected',3000,'ok');
    } else if(lastBleConnected && !ok){
      // Just disconnected
      if(!bleConnectPending){
        showToast('⚠ Disconnected',4000,'warn');
      }
    }
    lastBleConnected=ok;
    
    document.getElementById('bDot').className='dot '+(ok?'ok':'er');
    document.getElementById('bSt').textContent=ok?'Connected':'Disconnected';
    document.getElementById('sMode').textContent=m.serialMode||'--';
    const dmLabels={'trainer_in':'Trainer IN','trainer_out':'Trainer OUT','telemetry':'Telemetry','elrs_ht':'ELRS HT'};
    document.getElementById('bRole').textContent=dmLabels[m.deviceMode]||m.deviceMode||'--';
    const btm=m.deviceMode==='trainer_in'?'Master (Central)':'Slave (Peripheral)';
    document.getElementById('btMode').textContent=btm;
    document.getElementById('bName').textContent=m.btName||'--';
    document.getElementById('lAddr').textContent=m.localAddr||'--';
    document.getElementById('rAddr').textContent=m.remoteAddr||'--';
    document.getElementById('bTs').textContent=m.buildTs||'--';
    document.getElementById('rpSt').textContent=m.restartPending?'Yes':'No';
    if(!systemDirty){
      document.getElementById('selMode').value=m.serialMode;
      document.getElementById('selRole').value=m.deviceMode;
      document.getElementById('selMapMode').value=m.mapMode||'gv';
      const isLuaDraft=m.serialMode==='lua_serial';
      document.getElementById('mapModeRow').style.display=isLuaDraft?'':'none';
    }
    if(!bluetoothDirty && document.getElementById('inName')!==document.activeElement)
      document.getElementById('inName').value=m.btName||'';
    if(!wifiDirty && m.apSsid!==undefined && document.getElementById('inSsid')!==document.activeElement)
      document.getElementById('inSsid').value=m.apSsid||'';
    if(!wifiDirty && m.apPass!==undefined && document.getElementById('inApPass')!==document.activeElement)
      document.getElementById('inApPass').value=m.apPass||'';
    // WiFi mode select + section visibility — only update when field is present
    if(!wifiDirty && m.wifiMode!==undefined){
      document.getElementById('selWifi').value=m.wifiMode;
      wifiModeToggle(m.wifiMode);
    }
    if(!wifiDirty && m.staSsid!==undefined && document.getElementById('inStaSsid')!==document.activeElement)
      document.getElementById('inStaSsid').value=m.staSsid||'';
    if(!wifiDirty && m.staPass!==undefined && document.getElementById('inStaPass')!==document.activeElement)
      document.getElementById('inStaPass').value=m.staPass||'';
    document.getElementById('scanCard').style.display=m.deviceMode==='trainer_in'?'':'none';
    document.getElementById('perCard').style.display=m.deviceMode==='trainer_out'?'':'none';
    // Trainer map mode visibility (LUA Serial only)
    const isLua=m.serialMode==='lua_serial';
    if(!systemDirty){
      document.getElementById('mapModeRow').style.display=isLua?'':'none';
      if(isLua) document.getElementById('selMapMode').value=m.mapMode||'gv';
    }
    // Telemetry card visibility
    const isTelem=m.serialMode==='sport_bt'||m.serialMode==='sport_mirror';
    document.getElementById('telemCard').style.display=isTelem?'':'none';
    if(isTelem){
      if(!telemDirty) document.getElementById('selTelemOut').value=m.telemOutput||'wifi_udp';
      document.getElementById('mirrorBaudRow').style.display=m.serialMode==='sport_mirror'?'':'none';
      if(m.serialMode==='sport_mirror' && !telemDirty) document.getElementById('selBaud').value=m.sportBaud||'57600';
      const isUdp=(m.telemOutput||'wifi_udp')==='wifi_udp';
      document.getElementById('udpCfg').style.display=isUdp?'':'none';
      if(isUdp && !telemDirty){
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
function setTelemOutput(v){
  if(send({cmd:'setTelemOutput',value:v})) showToast('Telemetry output updated',2500,'ok');
}
function setMirrorBaud(v){
  if(send({cmd:'setMirrorBaud',value:v})) showToast('Mirror baud updated',2500,'ok');
}
function setUdpPort(){
  telemDirty=true;
  const val=document.getElementById('inUdpPort').value;
  showConfirm('UDP Port','Save UDP port and restart now?',function(){
    if(send({cmd:'setUdpPort',value:val,restartNow:true})) showReboot();
  });
  mCbCancel=function(){
    if(send({cmd:'setUdpPort',value:val,restartNow:false}))
      showToast('Saved. Changes apply after restart.',3500,'warn');
  };
}
function markSystemDirty(){systemDirty=true;}
function systemSerialChanged(){
  markSystemDirty();
  const isLua=document.getElementById('selMode').value==='lua_serial';
  document.getElementById('mapModeRow').style.display=isLua?'':'none';
}
function markBluetoothDirty(){bluetoothDirty=true;}
function markWifiDirty(){wifiDirty=true;}
function wifiModeChanged(v){markWifiDirty();wifiModeToggle(v);}
function saveSystem(){
  const d={
    cmd:'saveSystem',
    deviceMode:document.getElementById('selRole').value,
    serialMode:document.getElementById('selMode').value,
    mapMode:document.getElementById('selMapMode').value
  };
  showConfirm('Save System Config','Save settings and restart now?',function(){
    systemDirty=false;
    d.restartNow=true;
    if(send(d)) showReboot();
  });
  mCbCancel=function(){
    systemDirty=false;
    d.restartNow=false;
    if(send(d)) showToast('Saved. Changes apply after restart.',3500,'warn');
  };
}
function saveBluetooth(){
  const d={cmd:'saveBluetooth',btName:document.getElementById('inName').value};
  showConfirm('Save Bluetooth Config','Save Bluetooth settings?',function(){
    bluetoothDirty=false;
    send(d);
  });
}
function wifiModeToggle(v){
  document.getElementById('apCfg').style.display=v==='ap'?'':'none';
  document.getElementById('staCfg').style.display=v==='sta'?'':'none';
}
function saveWifi(){
  const wm=document.getElementById('selWifi').value;
  const d={cmd:'saveWifi',wifiMode:wm};
  if(wm==='ap'){
    d.apSsid=document.getElementById('inSsid').value;
    d.apPass=document.getElementById('inApPass').value;
  }else if(wm==='sta'){
    d.staSsid=document.getElementById('inStaSsid').value;
    d.staPass=document.getElementById('inStaPass').value;
  }
  showConfirm('Save WiFi Config','Save WiFi settings and restart now?',function(){
    wifiDirty=false;
    d.restartNow=true;
    if(send(d)) showReboot();
  });
  mCbCancel=function(){
    wifiDirty=false;
    d.restartNow=false;
    if(send(d)) showToast('Saved. Changes apply after restart.',3500,'warn');
  };
}
function scanBt(){
  if(send({cmd:'scanBt'})) showToast('Scanning for devices...',2500,'info');
}
function connectBt(a){
  if(bleConnectPending){
    showToast('⚠ Connection already in progress',3000,'warn');
    return;
  }
  if(send({cmd:'connectBt',value:a})) {
    bleConnectPending=true;
    clearTimeout(bleConnectTimer);
    showToast('Connecting...',0,'info');
    // BLE timeout is 5s; wait 7s before declaring failure
    bleConnectTimer=setTimeout(function(){
      if(bleConnectPending && !lastBleConnected){
        bleConnectPending=false;
        showToast('✗ Connection failed (timeout)',5000,'err');
      }
    }, 7000);
  }
}
function disconnectBt(){
  if(send({cmd:'disconnectBt'})) {
    clearTimeout(bleConnectTimer);
    bleConnectPending=false;
    showToast('Disconnecting...',2500,'info');
    setTimeout(getStatus, 500);
  }
}
function forgetBt(){
  if(send({cmd:'forgetBt'})) {
    clearTimeout(bleConnectTimer);
    bleConnectPending=false;
    showToast('Device forgotten',3000,'warn');
    setTimeout(getStatus, 500);
  }
}
function disconnectClient(){send({cmd:'disconnectClient'});}
function reboot(){
  showConfirm('Reboot Device',
    'After rebooting the device will start in normal mode and will not return to the configuration page.',
    function(){
      if(send({cmd:'reboot'})) showReboot();
    });
}
function factoryReset(){
  showConfirm('Factory Reset',
    'Restore default configuration and reboot? This will erase saved settings (WiFi, BLE pairing, modes).',
    function(){
      if(send({cmd:'factoryReset'})) showReboot();
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
function showToast(m,d,type){
  const t=document.getElementById('toast');
  if(toastTimer){clearTimeout(toastTimer);toastTimer=null;}
  t.classList.remove('ok','info','warn','err');
  t.classList.add(type||'ok');
  t.textContent=m;
  t.classList.add('show');
  if(d===0)return;
  toastTimer=setTimeout(()=>{t.classList.remove('show');toastTimer=null;},d||4000);
}
function doOta(){
  const f=fin.files[0];if(!f)return;
  const pg=document.getElementById('otaPrg'),msg=document.getElementById('otaMsg'),btn=document.getElementById('btnOta');
  pg.style.display='block';msg.textContent='Uploading\u2026 0%';btn.style.display='none';
  const x=new XMLHttpRequest();
  x.open('POST','/update',true);
  x.upload.onprogress=(ev)=>{if(ev.lengthComputable){const p=Math.round(ev.loaded/ev.total*100);pg.value=p;msg.textContent='Uploading\u2026 '+p+'%';}};
  x.onload=()=>{
    if(x.status===200 && x.responseText==='OK'){
      pg.value=100;msg.textContent='Upload complete \u2014 rebooting and restoring current WiFi mode\u2026';
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
        resp["staSsid"]    = g_config.staSsid;
        resp["staPass"]    = g_config.staPass;
        // WiFi mode: report what is actually running, not just what is configured.
        // The button can boot into AP/STA without changing g_config.wifiMode.
        if (s_active) {
            wifi_mode_t wm = WiFi.getMode();
            if (wm == WIFI_AP || wm == WIFI_AP_STA)  resp["wifiMode"] = "ap";
            else if (wm == WIFI_STA)                  resp["wifiMode"] = "sta";
            else                                       resp["wifiMode"] = "off";
        } else {
            switch (g_config.wifiMode) {
                case WifiMode::AP:  resp["wifiMode"] = "ap";  break;
                case WifiMode::STA: resp["wifiMode"] = "sta"; break;
                default:            resp["wifiMode"] = "off"; break;
            }
        }
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
            case DeviceMode::ELRS_HT:     resp["deviceMode"] = "elrs_ht";     break;
        }

        // Telemetry output settings
        resp["telemOutput"] = g_config.telemetryOutput == TelemetryOutput::BLE ? "ble" : "wifi_udp";
        resp["sportBaud"]   = String(g_config.sportBaud);
        resp["udpPort"]     = String(g_config.udpPort);
        resp["sportPkts"]   = String(sportGetPacketCount());
        resp["sportPps"]    = String(sportGetPacketsPerSec());
        resp["restartPending"] = s_cfgPendingRestart;

        // Trainer map mode
        resp["mapMode"]     = g_config.trainerMapMode == TrainerMapMode::MAP_TR ? "tr" : "gv";

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
    // ─── saveSystem ─────────────────────────────────────────────
    else if (strcmp(cmd, "saveSystem") == 0) {
      const char* serial = doc["serialMode"];
      const char* dev    = doc["deviceMode"];
      const char* map    = doc["mapMode"];

      if (serial) {
        if      (strcmp(serial, "sbus") == 0)         g_config.serialMode = OutputMode::SBUS;
        else if (strcmp(serial, "sport_bt") == 0)     g_config.serialMode = OutputMode::SPORT_BT;
        else if (strcmp(serial, "sport_mirror") == 0) g_config.serialMode = OutputMode::SPORT_MIRROR;
        else if (strcmp(serial, "lua_serial") == 0)   g_config.serialMode = OutputMode::LUA_SERIAL;
        else                                            g_config.serialMode = OutputMode::FRSKY;
      }

      if (dev) {
        if      (strcmp(dev, "trainer_in") == 0)  g_config.deviceMode = DeviceMode::TRAINER_IN;
        else if (strcmp(dev, "trainer_out") == 0) g_config.deviceMode = DeviceMode::TRAINER_OUT;
        else if (strcmp(dev, "telemetry") == 0)   g_config.deviceMode = DeviceMode::TELEMETRY;
        else if (strcmp(dev, "elrs_ht") == 0)     g_config.deviceMode = DeviceMode::ELRS_HT;
      }

      if (map) {
        g_config.trainerMapMode = (strcmp(map, "tr") == 0)
                      ? TrainerMapMode::MAP_TR
                      : TrainerMapMode::MAP_GV;
      }

      // ELRS_HT uses WiFi radio exclusively — force WiFi/telemetry off, trainer map to GV
      if (g_config.deviceMode == DeviceMode::ELRS_HT) {
        g_config.wifiMode       = WifiMode::OFF;
        g_config.telemetryOutput = TelemetryOutput::NONE;
        g_config.trainerMapMode = TrainerMapMode::MAP_GV;
      }

      bool restartNow = doc["restartNow"] | false;
      LOG_I("WEB", "System config saved%s", restartNow ? " — restarting" : " (pending restart)");
      configSave();
      s_cfgPendingRestart = true;

      resp["type"]   = "ack";
      resp["cmd"]    = cmd;
      resp["ok"]     = true;
      resp["reboot"] = restartNow;
      { String out; serializeJson(resp, out); client->text(out); }
      if (restartNow) {
        delay(400);
        uint8_t bootMode = 1;  // BOOT_AP_MODE default
        wifi_mode_t wm = WiFi.getMode();
        if (wm == WIFI_STA || wm == WIFI_AP_STA) {
            bootMode = 3;  // BOOT_STA_MODE
        } else if (g_config.serialMode == OutputMode::LUA_SERIAL &&
                   g_config.telemetryOutput == TelemetryOutput::WIFI_UDP) {
            bootMode = 2;  // BOOT_TELEM_AP
        }
        { Preferences p; p.begin("btwboot", false); p.putUChar("mode", bootMode); p.end(); }
        s_cfgPendingRestart = false;
        ESP.restart();
        return;
      }
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
      bool restartNow = doc["restartNow"] | false;
        if (val) {
            int port = atoi(val);
            if (port > 0 && port <= 65535) {
                g_config.udpPort = (uint16_t)port;
                LOG_I("WEB", "UDP port set to %u", g_config.udpPort);
                configSave();
          s_cfgPendingRestart = true;
                resp["ok"] = true;
          resp["reboot"] = restartNow;
            } else {
                LOG_W("WEB", "Invalid UDP port: %d (must be 1-65535)", port);
                resp["ok"] = false;
            }
        } else {
            resp["ok"] = false;
        }
        resp["type"] = "ack";
        resp["cmd"]  = cmd;
        if (resp["ok"] && restartNow) {
          String out;
          serializeJson(resp, out);
          client->text(out);
          delay(300);
          uint8_t bm = 0;
          if (g_config.wifiMode == WifiMode::AP) bm = 1;
          else if (g_config.wifiMode == WifiMode::STA) bm = 3;
          { Preferences p; p.begin("btwboot", false); p.putUChar("mode", bm); p.end(); }
          s_cfgPendingRestart = false;
          ESP.restart();
          return;
        }
    }
    // ─── saveBluetooth ───────────────────────────────────────────
    else if (strcmp(cmd, "saveBluetooth") == 0) {
      const char* name = doc["btName"];
      bool ok = (name && strlen(name) > 0);
      if (ok) {
        strlcpy(g_config.btName, name, sizeof(g_config.btName));
        LOG_I("WEB", "Bluetooth config saved: name=%s", name);
        configSave();
        bleUpdateAdvertisingName();
      }
      resp["type"] = "ack";
      resp["cmd"]  = cmd;
      resp["ok"]   = ok;
    }
    // ─── saveWifi ─────────────────────────────────────────────
    else if (strcmp(cmd, "saveWifi") == 0) {
        const char* wm = doc["wifiMode"];
        WifiMode newMode = WifiMode::OFF;
        if (wm) {
            if      (strcmp(wm, "ap")  == 0) newMode = WifiMode::AP;
            else if (strcmp(wm, "sta") == 0) newMode = WifiMode::STA;
        }
        g_config.wifiMode = newMode;

        if (newMode == WifiMode::AP) {
            const char* ss = doc["apSsid"];
            const char* pw = doc["apPass"];
            if (ss && strlen(ss) > 0)
                strlcpy(g_config.apSsid, ss, sizeof(g_config.apSsid));
            if (pw && strlen(pw) >= 8)
                strlcpy(g_config.apPass, pw, sizeof(g_config.apPass));
        } else if (newMode == WifiMode::STA) {
            const char* ss = doc["staSsid"];
            const char* pw = doc["staPass"];
            if (ss && strlen(ss) > 0)
                strlcpy(g_config.staSsid, ss, sizeof(g_config.staSsid));
            if (pw)
                strlcpy(g_config.staPass, pw, sizeof(g_config.staPass));
        }

          bool restartNow = doc["restartNow"] | false;
          LOG_I("WEB", "WiFi config saved: mode=%s%s", wm ? wm : "off",
            restartNow ? " — restarting" : " (pending restart)");
        configSave();
          s_cfgPendingRestart = true;

        uint8_t bootMode = 0;  // BOOT_NORMAL
        if      (newMode == WifiMode::AP)  bootMode = 1;
        else if (newMode == WifiMode::STA) bootMode = 3;

        resp["type"]   = "ack";
        resp["cmd"]    = cmd;
        resp["ok"]     = true;
        resp["reboot"] = restartNow;
        { String out; serializeJson(resp, out); client->text(out); }
        if (restartNow) {
          delay(400);
          { Preferences p; p.begin("btwboot", false); p.putUChar("mode", bootMode); p.end(); }
          s_cfgPendingRestart = false;
          ESP.restart();
          return;
        }
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
        // the right state based on the configured WiFi mode.
        // LUA Serial + WiFi UDP with AP/STA mode: use TELEM_AP so WebUI stays up.
        uint8_t bm = 0;  // BOOT_NORMAL
        if (g_config.wifiMode == WifiMode::AP) {
            bm = (g_config.serialMode == OutputMode::LUA_SERIAL &&
                  g_config.telemetryOutput == TelemetryOutput::WIFI_UDP) ? 2 : 1;
        } else if (g_config.wifiMode == WifiMode::STA) {
            bm = 3;  // BOOT_STA_MODE
        }
        { Preferences p; p.begin("btwboot", false); p.putUChar("mode", bm); p.end(); }
        s_cfgPendingRestart = false;
        ESP.restart();
        return;
    }
      // ─── factoryReset ───────────────────────────────────────────
      else if (strcmp(cmd, "factoryReset") == 0) {
        LOG_W("WEB", "Factory reset requested from Web UI");

        g_config.setDefaults();
        configSave();
        s_cfgPendingRestart = false;

        resp["type"]   = "ack";
        resp["cmd"]    = cmd;
        resp["ok"]     = true;
        resp["reboot"] = true;

        String out;
        serializeJson(resp, out);
        client->text(out);
        delay(300);

        // Defaults use WiFi OFF + LUA_SERIAL + Trainer IN.
        // Boot in normal mode after reset.
        { Preferences p; p.begin("btwboot", false); p.putUChar("mode", 0); p.end(); }
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

    // Clean driver state before any mode start to avoid residual STA/AP state
    // after repeated restarts or previous failed connections.
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    delay(120);

    if (g_config.wifiMode == WifiMode::STA) {
        // ── Station mode: connect to an existing WiFi network ──────────
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(false);
      WiFi.setSleep(false);

        LOG_D("WEB", "STA MAC: %s", WiFi.macAddress().c_str());
      LOG_D("WEB", "STA target SSID='%s' passLen=%u",
          g_config.staSsid, (unsigned)strlen(g_config.staPass));

        // Capture disconnect reason for diagnostics
        WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
            LOG_E("WEB", "WiFi disconnected: reason=%d",
                  (int)info.wifi_sta_disconnected.reason);
        }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

        // Log assigned DHCP IP every time STA gets/re-gets an address.
        WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) {
          LOG_I("WEB", "STA got IP: %s", WiFi.localIP().toString().c_str());
        }, ARDUINO_EVENT_WIFI_STA_GOT_IP);

      LOG_I("WEB", "STA connecting SSID='%s'", g_config.staSsid);
      WiFi.disconnect(true);
      delay(120);
      WiFi.begin(g_config.staSsid, g_config.staPass);

        // Wait up to 15 s for connection
        uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
            delay(500);
            LOG_D("WEB", "  status=%d elapsed=%lus", (int)WiFi.status(), (millis()-t0)/1000);
        }

        if (WiFi.status() != WL_CONNECTED) {
          LOG_W("WEB", "STA targeted connect failed: status=%d; retrying generic connect",
              (int)WiFi.status());

          // Retry once with a fresh STA session.
          WiFi.disconnect(true);
          delay(200);
          WiFi.mode(WIFI_STA);
          delay(100);
          WiFi.setSleep(false);
          WiFi.begin(g_config.staSsid, g_config.staPass);

          uint32_t t1 = millis();
          while (WiFi.status() != WL_CONNECTED && millis() - t1 < 12000) {
            delay(500);
            LOG_D("WEB", "  retry status=%d elapsed=%lus", (int)WiFi.status(), (millis()-t1)/1000);
          }
        }

        if (WiFi.status() != WL_CONNECTED) {
          LOG_E("WEB", "STA failed after retry: status=%d",
              (int)WiFi.status());
          WiFi.disconnect(true);
          WiFi.mode(WIFI_OFF);
          return;
        } else {
          esp_wifi_set_ps(WIFI_PS_NONE);
          LOG_I("WEB", "STA connected: IP=%s", WiFi.localIP().toString().c_str());
          LOG_D("WEB", "Free heap after WiFi start: %u", ESP.getFreeHeap());
        }

    } else {
        // ── AP mode (default): create a soft access point ───────────────
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
    }

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

        // Captive portal detection endpoints — redirect to main page (AP mode only)
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
            String ip = (g_config.wifiMode == WifiMode::STA)
                        ? WiFi.localIP().toString()
                        : WiFi.softAPIP().toString();
            r->redirect("http://" + ip + "/");
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

    // Disconnect WiFi — AP or STA
    if (g_config.wifiMode == WifiMode::STA) {
        WiFi.disconnect(true);
    } else {
        WiFi.softAPdisconnect(true);
    }
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
        // After OTA, return to the WiFi mode that is currently active at runtime
        // (not just the configured one), so boot override modes are preserved.
        // STA active -> BOOT_STA_MODE (3), otherwise AP/TELEM_AP path.
        uint8_t nextMode = 1;  // BOOT_AP_MODE default
        wifi_mode_t wm = WiFi.getMode();
        if (wm == WIFI_STA || wm == WIFI_AP_STA) {
          nextMode = 3;  // BOOT_STA_MODE
        } else if (g_config.serialMode == OutputMode::LUA_SERIAL &&
               g_config.telemetryOutput == TelemetryOutput::WIFI_UDP) {
          nextMode = 2;  // BOOT_TELEM_AP
        }
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
    uint32_t pushIntervalMs = bleIsConnecting() ? 8000 : 3000;
    if (s_ws.count() > 0 && millis() - lastPush >= pushIntervalMs) {
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
        doc["apPass"]     = g_config.apPass;
        doc["staSsid"]    = g_config.staSsid;
        doc["staPass"]    = g_config.staPass;
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
            case DeviceMode::ELRS_HT:     doc["deviceMode"] = "elrs_ht";     break;
        }

        // Telemetry info for periodic push
        doc["telemOutput"] = g_config.telemetryOutput == TelemetryOutput::BLE ? "ble" : "wifi_udp";
        doc["sportBaud"]   = String(g_config.sportBaud);
        doc["udpPort"]     = String(g_config.udpPort);
        doc["sportPkts"]   = String(sportGetPacketCount());
        doc["sportPps"]    = String(sportGetPacketsPerSec());
        doc["mapMode"]     = g_config.trainerMapMode == TrainerMapMode::MAP_TR ? "tr" : "gv";
        doc["restartPending"] = s_cfgPendingRestart;

        // WiFi mode: report actual running mode, same logic as getStatus
        {
            wifi_mode_t wm = WiFi.getMode();
            if      (wm == WIFI_AP || wm == WIFI_AP_STA) doc["wifiMode"] = "ap";
            else if (wm == WIFI_STA)                     doc["wifiMode"] = "sta";
            else                                          doc["wifiMode"] = "off";
        }

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
