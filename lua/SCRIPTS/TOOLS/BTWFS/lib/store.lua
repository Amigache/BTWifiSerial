-- store.lua
-- Application state fed exclusively by the serial protocol dispatcher.
-- Pages and consumers register callbacks for data-change events.
-- No EdgeTX API calls — pure Lua.

local M = {}

-- ── Preferences ───────────────────────────────────────────────────
-- prefs[id]    = pref table (from serial_proto.decodePrefPayload)
-- prefsOrder   = ordered id list (insertion order = firmware send order)
-- prefsReady   = true once PREF_END has been received
M.prefs      = {}
M.prefsOrder = {}
M.prefsReady = false

-- ── Info fields ───────────────────────────────────────────────────
-- info[id]     = info table (from serial_proto.decodeInfoPayload)
-- infoOrder    = ordered id list
-- infoReady    = true once INFO_END has been received
M.info      = {}
M.infoOrder = {}
M.infoReady = false

-- ── BLE / WiFi status ─────────────────────────────────────────────
M.status = {
  bleConnected  = false,
  wifiClients   = false,
  bleConnecting = false,
}

-- ── Channel data ──────────────────────────────────────────────────
-- channels[1..8]: signed int16 (-1024..+1024), nil until first frame
M.channels = {}

-- ── BLE scan ──────────────────────────────────────────────────────
-- scanState: 0=idle, 1=scanning, 2=complete
-- scanResults[1..N]: scan entry tables
M.scanState   = 0
M.scanResults = {}
-- WiFi scan ──────────────────────────────────────────────────────────
-- wifiScanState: 0=idle/fail, 1=scanning, 2=complete
-- wifiScanResults[1..N]: { idx, rssi, ssid }
M.wifiScanState   = 0
M.wifiScanResults = {}
-- ── Pending PREF_SET ──────────────────────────────────────────────
-- Set to the pref id when a PREF_SET frame is sent, cleared on PREF_ACK.
M.pendingPrefId = nil

-- ── Event bus ─────────────────────────────────────────────────────
local _cbs = {}

local function emit(ev, data)
  local list = _cbs[ev]
  if list then
    for i = 1, #list do list[i](data) end
  end
end

-- Register a callback for an event.
-- Events: "prefs_ready"  "pref_changed"  "pref_ack"
--         "info_ready"   "info_changed"
--         "status"       "channels"
--         "scan_status"  "scan_entry"
function M.on(ev, fn)
  _cbs[ev] = _cbs[ev] or {}
  _cbs[ev][#_cbs[ev] + 1] = fn
end

-- ── Mutators (called by main.lua's onFrame dispatcher) ────────────

function M.reset()
  M.prefs = {}; M.prefsOrder = {}; M.prefsReady = false
  M.info  = {}; M.infoOrder  = {}; M.infoReady  = false
end

-- PREF channel ─────────────────────────────────────────────────────

function M.beginPrefs()
  M.prefs = {}; M.prefsOrder = {}; M.prefsReady = false
end

function M.addPref(item)
  if not item then return end
  M.prefs[item.id] = item
  M.prefsOrder[#M.prefsOrder + 1] = item.id
end

function M.endPrefs()
  M.prefsReady = true
  emit("prefs_ready", nil)
end

-- PREF_UPDATE: value-only patch for an existing pref.
function M.updatePref(item)
  if not item then return end
  local p = M.prefs[item.id]
  if not p then return end
  -- Merge only value fields; label/options stay from the original PREF_ITEM.
  if p.type == 0 then   -- FT_ENUM
    p.curIdx = item.curIdx
  else
    p.value = item.value
  end
  emit("pref_changed", p)
end

-- PREF_ACK: result of the last PREF_SET.
function M.prefAck(id, result)
  M.pendingPrefId = nil
  emit("pref_ack", { id = id, result = result })
end

-- INFO channel ─────────────────────────────────────────────────────

function M.beginInfo()
  M.info = {}; M.infoOrder = {}; M.infoReady = false
end

function M.addInfo(item)
  if not item then return end
  M.info[item.id] = item
  M.infoOrder[#M.infoOrder + 1] = item.id
end

function M.endInfo()
  M.infoReady = true
  emit("info_ready", nil)
end

-- INFO_UPDATE: value-only patch for an existing info field.
function M.updateInfo(item)
  if not item then return end
  local inf = M.info[item.id]
  if not inf then return end
  inf.value = item.value
  emit("info_changed", inf)
end

-- INFO_STATUS byte.
function M.updateStatus(byte)
  M.status.bleConnected  = bit32.band(byte, 0x01) ~= 0
  M.status.wifiClients   = bit32.band(byte, 0x02) ~= 0
  M.status.bleConnecting = bit32.band(byte, 0x04) ~= 0
  emit("status", M.status)
end

-- INFO_CHANNELS: decoded table of 8 values.
function M.updateChannels(chTable)
  if not chTable then return end
  M.channels = chTable
  emit("channels", chTable)
end

-- BLE scan ─────────────────────────────────────────────────────────

function M.updateScanStatus(state, count)
  M.scanState = state
  if state == 0 then M.scanResults = {} end
  emit("scan_status", { state = state, count = count })
end

function M.addScanResult(entry)
  if not entry then return end
  M.scanResults[entry.idx + 1] = entry
  emit("scan_entry", entry)
end

-- WiFi scan ──────────────────────────────────────────────────────────

function M.updateWifiScanStatus(state, count)
  M.wifiScanState = state
  if state == 1 then M.wifiScanResults = {} end  -- clear on new scan start
  emit("wifi_scan_status", { state = state, count = count })
end

function M.addWifiScanResult(entry)
  if not entry then return end
  M.wifiScanResults[entry.idx + 1] = entry
  emit("wifi_scan_entry", entry)
end

return M
