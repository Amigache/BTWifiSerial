-- btwfs.lua — BTWifiSerial background Function script
-- New multi-channel serial protocol (CH/TYPE/LEN framing).
--
-- Responsibilities:
--   1. Own the serial port when the Tools script is NOT active.
--   2. Parse INFO_CHANNELS frames → inject channels into GV1–GV8 or TR1–TR8.
--   3. Parse PREF_ITEM / PREF_UPDATE for MAP_MODE (pref id 5) to know GV/TR mode.
--   4. Forward radio S.PORT telemetry to ESP32 as CH_TRANS / TRANS_SPORT frames.
--   5. Update SHM slot 1 with getTime() so the Tools script can detect us.
--
-- SETUP:
--   1. Copy to /SCRIPTS/FUNCTIONS/btwfs.lua
--   2. Special Functions → SF1 → Switch: ON → Lua Script → btwfs

local BASE_TOOLS = "/SCRIPTS/TOOLS/BTWFS"
local proto = loadScript(BASE_TOOLS .. "/lib/serial_proto.lua")()

-- ── Shared memory ─────────────────────────────────────────────────
local SHM_TOOLS_HB = 1   -- Tools script writes getTime() here each frame
local HB_STALE_MS  = 80  -- 8 ticks × 10 ms = 80 ms

-- ── MAP_MODE cache (updated from PREF frames) ─────────────────────
-- 0 = GV (global variables),  1 = TR (trainer channels)
local PREF_ID_MAP_MODE = 0x05
local mapMode = 0

-- ── Channel injection ─────────────────────────────────────────────
local NUM_CH = 8
local CH_STALE_TICKS = 300   -- ≈ 3 s without a channels frame → stop injecting
local _lastChannelTick = 0

local function channelsAreFresh()
  return _lastChannelTick > 0 and (getTime() - _lastChannelTick) < CH_STALE_TICKS
end
local function injectChannels(vals)
  if mapMode == 1 then
    -- TR mode: setTrainerChannels expects a table indexed 1-based, values -1024..1024
    if setTrainerChannels then
      setTrainerChannels(vals)
    end
  else
    -- GV mode: write GV1..GV8 in flight-mode 0
    for i = 1, NUM_CH do
      local v = vals[i] or 0
      if v < -1024 then v = -1024 elseif v > 1024 then v = 1024 end
      pcall(model.setGlobalVariable, i - 1, 0, v)
    end
  end
end

-- ── Frame handler ─────────────────────────────────────────────────
local function onFrame(ch, typ, payload)
  if ch == proto.CH_INFO and typ == proto.PT_INFO_CHANNELS then
    local vals = proto.decodeChannels(payload)
    if vals then
      _lastChannelTick = getTime()
      injectChannels(vals)
    end

  elseif ch == proto.CH_PREF then
    -- Extract MAP_MODE from full PREF_ITEM or PREF_UPDATE
    if typ == proto.PT_PREF_ITEM then
      local p = proto.decodePrefPayload(payload, false)
      if p and p.id == PREF_ID_MAP_MODE and p.type == proto.FT_ENUM then
        mapMode = p.curIdx
      end
    elseif typ == proto.PT_PREF_UPDATE then
      local p = proto.decodePrefPayload(payload, true)
      if p and p.id == PREF_ID_MAP_MODE then
        mapMode = p.curIdx or mapMode
      end
    end
  end
  -- All other channels/types are ignored by btwfs
end

local _parser = proto.newParser(onFrame)

-- ── Telemetry forwarding (S.PORT → ESP32) ─────────────────────────
local function forwardTelemetry()
  if not sportTelemetryPop then return end
  local physId, primId, dataId, value = sportTelemetryPop()
  if physId then
    -- Build TRANS_SPORT payload: physId(1) primId(1) dataId_lo(1) dataId_hi(1) value(4 LE)
    local dlo = bit32.band(dataId,          0xFF)
    local dhi = bit32.band(bit32.rshift(dataId, 8), 0xFF)
    local v0  = bit32.band(value,           0xFF)
    local v1  = bit32.band(bit32.rshift(value,  8), 0xFF)
    local v2  = bit32.band(bit32.rshift(value, 16), 0xFF)
    local v3  = bit32.band(bit32.rshift(value, 24), 0xFF)
    if serialWrite then
      serialWrite(proto.buildTrans(proto.PT_TRANS_SPORT,
                                   { physId, primId, dlo, dhi, v0, v1, v2, v3 }))
    end
  end
end

-- ── Tools-script heartbeat check ──────────────────────────────────
local function toolsIsActive()
  if not getShmVar then return false end
  local hb = getShmVar(SHM_TOOLS_HB)
  if not hb or hb == 0 then return false end
  -- getTime() returns 10 ms ticks; compare difference
  local diff = getTime() - hb
  return diff >= 0 and diff < (HB_STALE_MS / 10)
end

-- ── Reconnect probe (when stale and Tools is not active) ─────────
local RECONNECT_INTERVAL = 100   -- ticks (≈ 1 s)
local _reconnectTick = 0

-- ── EdgeTX callbacks ──────────────────────────────────────────────

local function run(event)
  if toolsIsActive() then
    -- Tools script owns the serial port; only forward telemetry.
    forwardTelemetry()
    return 0
  end

  -- Drain up to 64 bytes per tick
  if serialRead then
    local data = serialRead(64)
    if data and #data > 0 then
      for i = 1, #data do
        _parser(string.byte(data, i))
      end
    end
  end

  -- If channel data has gone stale, probe the board every second so we
  -- resume injection automatically when it comes back.
  if not channelsAreFresh() then
    local now = getTime()
    if serialWrite and now - _reconnectTick >= RECONNECT_INTERVAL then
      _reconnectTick = now
      serialWrite(proto.buildInfoRequest())
      serialWrite(proto.buildPrefRequest())
    end
  end

  forwardTelemetry()
  return 0
end

return { run = run }

