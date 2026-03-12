-- serial_proto.lua
-- Binary serial protocol for BTWifiSerial multi-channel framing.
-- Provides: protocol constants, frame parser, frame builders, payload decoders.
-- No EdgeTX API calls — pure Lua logic (Lua 5.2 / bit32 compatible).

local M = {}

-- ── Frame structure ────────────────────────────────────────────────
-- [SYNC:0xAA] [CH:1] [TYPE:1] [LEN:1] [PAYLOAD:LEN] [CRC:1]
-- CRC = XOR(CH, TYPE, LEN, payload[1..LEN])

M.SYNC = 0xAA

-- ── Logical channels ──────────────────────────────────────────────
M.CH_PREF  = 0x01   -- Preferences / configuration  (bidirectional)
M.CH_INFO  = 0x02   -- Status / channels / BLE scan  (bidirectional)
M.CH_TRANS = 0x03   -- Transparent byte passthrough  (bidirectional)

-- ── CH_PREF frame types (ESP32 → Lua) ────────────────────────────
M.PT_PREF_BEGIN  = 0x01   -- Start of pref list;  payload: count(1)
M.PT_PREF_ITEM   = 0x02   -- One pref descriptor;  payload: variable (see decoder)
M.PT_PREF_END    = 0x03   -- End of pref list;    no payload
M.PT_PREF_UPDATE = 0x04   -- One pref value changed; payload: id(1) type(1) value(var)
M.PT_PREF_ACK    = 0x05   -- Result of PREF_SET;  payload: id(1) result(1)

-- ── CH_PREF frame types (Lua → ESP32) ────────────────────────────
M.PT_PREF_REQUEST = 0x10  -- Request full pref list;  no payload
M.PT_PREF_SET     = 0x11  -- Set a preference;         payload: id(1) type(1) value(var)

-- ── CH_INFO frame types (ESP32 → Lua) ────────────────────────────
M.PT_INFO_CHANNELS    = 0x01  -- Channel data;  payload: 8×int16 BE = 16 bytes
M.PT_INFO_STATUS      = 0x02  -- Status bits;   payload: status(1)
                               --   bit0: BLE connected
                               --   bit1: WiFi AP has clients
                               --   bit2: BLE connecting
M.PT_INFO_BEGIN       = 0x03  -- Info list start;  payload: count(1)
M.PT_INFO_ITEM        = 0x04  -- One info descriptor;   payload: variable
M.PT_INFO_END         = 0x05  -- Info list end;   no payload
M.PT_INFO_UPDATE      = 0x06  -- One info value changed; payload: id(1) type(1) value(var)
M.PT_INFO_SCAN_STATUS = 0x07  -- BLE scan state;  payload: state(1) count(1)
                               --   state: 0=idle, 1=scanning, 2=complete
M.PT_INFO_SCAN_ITEM   = 0x08  -- BLE scan entry;  payload: idx(1) rssi_s8(1) flags(1) name_len(1) name(N) addr(17)
                               --   flags bit0: hasFrsky
M.PT_INFO_WIFI_SCAN_STATUS = 0x09  -- WiFi scan state; payload: state(1) count(1)
                                    --   state: 0=fail, 1=scanning, 2=done
M.PT_INFO_WIFI_SCAN_ITEM   = 0x0A  -- WiFi scan entry; payload: idx(1) rssi_s8(1) ssid_len(1) ssid(N)

-- ── CH_INFO frame types (Lua → ESP32) ────────────────────────────
M.PT_INFO_REQUEST        = 0x10  -- Request all info + prefs;  no payload
M.PT_INFO_HEARTBEAT      = 0x11  -- Tools script alive;         no payload
M.PT_INFO_BLE_SCAN       = 0x12  -- Start BLE scan;             no payload
M.PT_INFO_BLE_CONNECT    = 0x13  -- Connect to scan result;     payload: idx(1)
M.PT_INFO_BLE_DISCONNECT = 0x14  -- Disconnect;                 no payload
M.PT_INFO_BLE_FORGET     = 0x15  -- Forget saved device;        no payload
M.PT_INFO_BLE_RECONNECT  = 0x16  -- Reconnect to saved device;  no payload
M.PT_INFO_WIFI_SCAN      = 0x17  -- Start WiFi scan;             no payload

-- ── CH_TRANS frame types (bidirectional) ─────────────────────────
M.PT_TRANS_SBUS  = 0x01
M.PT_TRANS_SPORT = 0x02
M.PT_TRANS_FRSKY = 0x03

-- ── Preference / info field types ────────────────────────────────
M.FT_ENUM   = 0
M.FT_STRING = 1
M.FT_INT    = 2
M.FT_BOOL   = 3

-- ── Pref descriptor flags ─────────────────────────────────────────
M.PF_RESTART   = 0x01  -- Applying this pref requires a device restart
M.PF_RDONLY    = 0x02  -- Read-only (display only, PREF_SET is ignored)
M.PF_DASHBOARD = 0x04  -- Show this pref on the Dashboard System section
M.PF_NUMERIC   = 0x08  -- FT_STRING: only numeric digit input (0-9)

-- ── Info item IDs (ESP32 → Lua) ──────────────────────────────────
M.INFO_FIRMWARE = 0x01  -- Build timestamp string
M.INFO_BT_ADDR  = 0x02  -- Local BT MAC address
M.INFO_REM_ADDR = 0x03  -- Saved remote BT MAC address ("(none)" when empty)

-- ── PREF_ACK result codes ─────────────────────────────────────────
M.ACK_OK  = 0x00
M.ACK_ERR = 0x01

-- ── Frame builder ──────────────────────────────────────────────────
-- Builds a binary frame string ready for serialWrite().
-- payload: array of integer byte values (may be empty table).

local function buildFrame(ch, typ, payload)
  local len = #payload
  local crc = bit32.bxor(ch, typ)
  crc = bit32.bxor(crc, len)
  for i = 1, len do
    crc = bit32.bxor(crc, payload[i])
  end
  local t = { M.SYNC, ch, typ, len }
  for i = 1, len do t[#t + 1] = payload[i] end
  t[#t + 1] = crc
  local s = {}
  for i = 1, #t do s[i] = string.char(t[i]) end
  return table.concat(s)
end

-- ── Specific builders (Lua → ESP32) ──────────────────────────────

function M.buildPrefRequest()
  return buildFrame(M.CH_PREF, M.PT_PREF_REQUEST, {})
end

-- Build a PREF_SET frame.
-- value:  ENUM/BOOL → integer index (0-based),  STRING → string,  INT → number
function M.buildPrefSet(id, fieldType, value)
  local p = { id, fieldType }
  if fieldType == M.FT_ENUM or fieldType == M.FT_BOOL then
    p[#p + 1] = value
  elseif fieldType == M.FT_INT then
    local v = (value < 0) and (value + 65536) or value
    p[#p + 1] = bit32.band(v, 0xFF)
    p[#p + 1] = bit32.band(bit32.rshift(v, 8), 0xFF)
  elseif fieldType == M.FT_STRING then
    local slen = #value
    p[#p + 1] = slen
    for i = 1, slen do p[#p + 1] = string.byte(value, i) end
  end
  return buildFrame(M.CH_PREF, M.PT_PREF_SET, p)
end

function M.buildInfoRequest()
  return buildFrame(M.CH_INFO, M.PT_INFO_REQUEST, {})
end

function M.buildInfoHeartbeat()
  return buildFrame(M.CH_INFO, M.PT_INFO_HEARTBEAT, {})
end

function M.buildInfoBleScan()
  return buildFrame(M.CH_INFO, M.PT_INFO_BLE_SCAN, {})
end

function M.buildInfoBleConnect(idx)
  return buildFrame(M.CH_INFO, M.PT_INFO_BLE_CONNECT, { idx })
end

function M.buildInfoBleDisconnect()
  return buildFrame(M.CH_INFO, M.PT_INFO_BLE_DISCONNECT, {})
end

function M.buildInfoBleForget()
  return buildFrame(M.CH_INFO, M.PT_INFO_BLE_FORGET, {})
end

function M.buildInfoBleReconnect()
  return buildFrame(M.CH_INFO, M.PT_INFO_BLE_RECONNECT, {})
end

function M.buildInfoWifiScan()
  return buildFrame(M.CH_INFO, M.PT_INFO_WIFI_SCAN, {})
end

-- Build a transparent passthrough frame.
-- bytes: array of integer byte values
function M.buildTrans(transType, bytes)
  return buildFrame(M.CH_TRANS, transType, bytes)
end

-- ── Payload decoders ──────────────────────────────────────────────

-- Internal signed int16 from two little-endian bytes.
local function toS16(lo, hi)
  local v = lo + hi * 256
  return (v >= 32768) and (v - 65536) or v
end

-- Decode a PREF_ITEM (isUpdate=false) or PREF_UPDATE (isUpdate=true) payload.
-- Returns a pref table, or nil on malformed input.
--
-- Full PREF_ITEM table fields:
--   .id     number
--   .type   number (FT_*)
--   .flags  number (PF_* bits)   [only for full item]
--   .label  string               [only for full item]
--   .options  {string,...}       [ENUM only, full item]
--   .curIdx   number (0-based)   [ENUM]
--   .maxLen   number             [STRING only, full item]
--   .value    string/number/bool [STRING/INT/BOOL]
--
-- PREF_UPDATE table:  .id  .type  .curIdx or .value
function M.decodePrefPayload(payload, isUpdate)
  local n = #payload
  if n < 2 then return nil end
  local pos = 1

  local id    = payload[pos]; pos = pos + 1
  local ftype = payload[pos]; pos = pos + 1

  if isUpdate then
    local p = { id = id, type = ftype }
    if ftype == M.FT_ENUM or ftype == M.FT_BOOL then
      if pos > n then return nil end
      p.curIdx = payload[pos]
    elseif ftype == M.FT_INT then
      if pos + 1 > n then return nil end
      p.value = toS16(payload[pos], payload[pos + 1])
    elseif ftype == M.FT_STRING then
      if pos > n then return nil end
      local vlen = payload[pos]; pos = pos + 1
      local chars = {}
      for i = 1, vlen do
        if pos > n then return nil end
        chars[i] = string.char(payload[pos]); pos = pos + 1
      end
      p.value = table.concat(chars)
    end
    return p
  end

  -- Full PREF_ITEM: id(1) type(1) flags(1) label_len(1) label(N) <type-specific>
  if pos + 1 > n then return nil end
  local flags = payload[pos]; pos = pos + 1
  local llen  = payload[pos]; pos = pos + 1
  local lchars = {}
  for i = 1, llen do
    if pos > n then return nil end
    lchars[i] = string.char(payload[pos]); pos = pos + 1
  end

  local p = { id = id, type = ftype, flags = flags, label = table.concat(lchars) }

  if ftype == M.FT_ENUM then
    if pos + 1 > n then return nil end
    local optCount = payload[pos]; pos = pos + 1
    p.curIdx  = payload[pos];     pos = pos + 1
    p.options = {}
    for oi = 1, optCount do
      if pos > n then return nil end
      local olen = payload[pos]; pos = pos + 1
      local ochars = {}
      for c = 1, olen do
        if pos > n then return nil end
        ochars[c] = string.char(payload[pos]); pos = pos + 1
      end
      p.options[oi] = table.concat(ochars)
    end

  elseif ftype == M.FT_STRING then
    if pos + 1 > n then return nil end
    p.maxLen = payload[pos]; pos = pos + 1
    local vlen = payload[pos]; pos = pos + 1
    local vchars = {}
    for i = 1, vlen do
      if pos > n then return nil end
      vchars[i] = string.char(payload[pos]); pos = pos + 1
    end
    p.value = table.concat(vchars)

  elseif ftype == M.FT_INT then
    if pos + 5 > n then return nil end
    p.min   = toS16(payload[pos],     payload[pos + 1]); pos = pos + 2
    p.max   = toS16(payload[pos],     payload[pos + 1]); pos = pos + 2
    p.value = toS16(payload[pos],     payload[pos + 1])

  elseif ftype == M.FT_BOOL then
    if pos > n then return nil end
    p.value = (payload[pos] ~= 0)
  end

  return p
end

-- Decode an INFO_ITEM (isUpdate=false) or INFO_UPDATE (isUpdate=true) payload.
-- Returns an info table, or nil on malformed input.
--
-- Full INFO_ITEM table:  .id  .type  .label  .value
-- INFO_UPDATE table:     .id  .type  .value
function M.decodeInfoPayload(payload, isUpdate)
  local n = #payload
  if n < 2 then return nil end
  local pos = 1

  local id    = payload[pos]; pos = pos + 1
  local ftype = payload[pos]; pos = pos + 1

  local function readValue()
    if ftype == M.FT_STRING then
      if pos > n then return nil end
      local vlen = payload[pos]; pos = pos + 1
      local chars = {}
      for i = 1, vlen do
        if pos > n then return nil end
        chars[i] = string.char(payload[pos]); pos = pos + 1
      end
      return table.concat(chars)
    elseif ftype == M.FT_INT then
      if pos + 1 > n then return nil end
      local v = toS16(payload[pos], payload[pos + 1])
      pos = pos + 2
      return v
    elseif ftype == M.FT_BOOL then
      if pos > n then return nil end
      local v = (payload[pos] ~= 0); pos = pos + 1
      return v
    end
    return nil
  end

  if isUpdate then
    return { id = id, type = ftype, value = readValue() }
  end

  -- Full INFO_ITEM: id(1) type(1) label_len(1) label(N) value(var)
  if pos > n then return nil end
  local llen = payload[pos]; pos = pos + 1
  local lchars = {}
  for i = 1, llen do
    if pos > n then return nil end
    lchars[i] = string.char(payload[pos]); pos = pos + 1
  end

  return { id = id, type = ftype, label = table.concat(lchars), value = readValue() }
end

-- Decode INFO_CHANNELS payload: 8 × int16 big-endian = 16 bytes.
-- Returns { v1, v2, ..., v8 }, or nil if payload is too short.
function M.decodeChannels(payload)
  if #payload < 16 then return nil end
  local ch = {}
  for i = 1, 8 do
    local hi = payload[(i - 1) * 2 + 1]
    local lo = payload[(i - 1) * 2 + 2]
    local v  = hi * 256 + lo
    ch[i]    = (v >= 32768) and (v - 65536) or v
  end
  return ch
end

-- Decode INFO_SCAN_ITEM payload.
-- Returns { idx, rssi, hasFrsky, name, addr }, or nil on error.
function M.decodeScanItem(payload)
  local n = #payload
  if n < 4 then return nil end
  local pos = 1

  local idx   = payload[pos]; pos = pos + 1
  local rssi  = payload[pos]; pos = pos + 1
  rssi = (rssi >= 128) and (rssi - 256) or rssi  -- signed
  local flags = payload[pos]; pos = pos + 1
  local nlen  = payload[pos]; pos = pos + 1

  local nchars = {}
  for i = 1, nlen do
    if pos > n then return nil end
    nchars[i] = string.char(payload[pos]); pos = pos + 1
  end

  -- addr: up to 17 bytes, null-terminated
  local achars = {}
  while pos <= n and payload[pos] ~= 0 do
    achars[#achars + 1] = string.char(payload[pos]); pos = pos + 1
  end

  return {
    idx      = idx,
    rssi     = rssi,
    hasFrsky = bit32.band(flags, 0x01) ~= 0,
    name     = table.concat(nchars),
    addr     = table.concat(achars),
  }
end

-- Decode INFO_WIFI_SCAN_ITEM payload.
-- Returns { idx, rssi, ssid }, or nil on error.
function M.decodeWifiScanItem(payload)
  local n = #payload
  if n < 3 then return nil end
  local pos = 1
  local idx   = payload[pos]; pos = pos + 1
  local rssi  = payload[pos]; pos = pos + 1
  rssi = (rssi >= 128) and (rssi - 256) or rssi
  local slen  = payload[pos]; pos = pos + 1
  local chars = {}
  for i = 1, slen do
    if pos > n then break end
    chars[i] = string.char(payload[pos]); pos = pos + 1
  end
  return { idx = idx, rssi = rssi, ssid = table.concat(chars) }
end

-- ── Frame parser ──────────────────────────────────────────────────
-- Creates a stateful parser instance.
-- Returns a function(byte) that calls onFrame(ch, typ, payload_array) on
-- each validated frame. CRC mismatches are silently discarded.

function M.newParser(onFrame)
  -- States: 0=wait-sync  1=ch  2=type  3=len  4=accumulate  5=crc
  local st     = 0
  local ch, typ, needed
  local buf    = {}
  local crcAcc = 0

  return function(b)
    if st == 0 then
      if b == M.SYNC then st = 1 end

    elseif st == 1 then
      ch = b; crcAcc = b; st = 2

    elseif st == 2 then
      typ = b; crcAcc = bit32.bxor(crcAcc, b); st = 3

    elseif st == 3 then
      needed = b; crcAcc = bit32.bxor(crcAcc, b)
      buf = {}
      st = (needed == 0) and 5 or 4

    elseif st == 4 then
      buf[#buf + 1] = b
      crcAcc = bit32.bxor(crcAcc, b)
      needed = needed - 1
      if needed == 0 then st = 5 end

    elseif st == 5 then
      if b == crcAcc then onFrame(ch, typ, buf) end
      st = 0
    end
  end
end

return M
