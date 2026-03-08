-- btwfs.lua — BTWifiSerial background Function script
-- Reads serial channel data from ESP32-C3 and writes to GV1–GV8 (Flight Mode 0)
-- Optionally forwards radio S.PORT telemetry to ESP32 via T_TLM frames.
-- Yields incoming serial to the BTWifiSerial Tools script when it is active.
--
-- SETUP:
--   1. Copy to /SCRIPTS/FUNCTIONS/btwfs.lua
--   2. Special Functions → SF1 → Switch: ON → Lua Script → btwfs

local SYNC  = 0xAA

-- Frame types (must match lua_serial.cpp)
local T_CH           = 0x43
local T_ST           = 0x53
local T_ACK          = 0x41
local T_CFG          = 0x47
local T_INF          = 0x49
local T_SYS          = 0x59
local T_SCAN_STATUS  = 0x44
local T_SCAN_ENTRY   = 0x52
local T_TLM          = 0x54  -- telemetry forward: Radio → ESP32

local NUM_CH = 8

-- Shared memory slot used by Tools script as heartbeat
local SHM_TOOLS_HB = 1

local rxState  = 0
local rxType   = 0
local rxBuf    = {}
local rxNeeded = 0

local function processByte(b)
  if rxState == 0 then
    if b == SYNC then rxState = 1 end
  elseif rxState == 1 then
    rxType = b; rxBuf = {b}
    if     b == T_CH           then rxNeeded = 17; rxState = 2
    elseif b == T_ST           then rxNeeded = 2;  rxState = 2
    elseif b == T_ACK          then rxNeeded = 2;  rxState = 2
    elseif b == T_CFG          then rxNeeded = 4;  rxState = 2
    elseif b == T_INF          then rxNeeded = 13; rxState = 2
    elseif b == T_SYS          then rxNeeded = 89; rxState = 2
    elseif b == T_SCAN_STATUS  then rxNeeded = 3;  rxState = 2
    elseif b == T_SCAN_ENTRY   then rxNeeded = 38; rxState = 2
    else   rxState = (b == SYNC) and 1 or 0 end
  elseif rxState == 2 then
    rxBuf[#rxBuf + 1] = b
    rxNeeded = rxNeeded - 1
    if rxNeeded == 0 then
      if rxType == T_CH then
        local crc = 0
        for i = 1, 17 do crc = bit32.bxor(crc, rxBuf[i]) end
        if crc == rxBuf[18] then
          for i = 0, NUM_CH - 1 do
            local v = rxBuf[2 + i*2] * 256 + rxBuf[3 + i*2]
            if v >= 32768 then v = v - 65536 end
            -- Clamp to EdgeTX GVar range; setGlobalVariable silently ignores
            -- values outside ±1024.
            v = math.max(-1024, math.min(1024, v))
            pcall(model.setGlobalVariable, i, 0, v)
          end
        end
      end
      -- All other frame types: consumed to keep parser in sync, not acted on
      rxState = 0
    end
  end
end

local function init()
end

-- sendTelemetry() — pop pending S.PORT packets and send as T_TLM frames.
-- Wrapped so it silently does nothing if sportTelemetryPop is unavailable.
local hasTlmPop = nil  -- cached availability flag

local function sendTelemetry()
  if type(serialWrite) ~= "function" then return end
  -- Check for sportTelemetryPop once (cache result)
  if hasTlmPop == nil then
    hasTlmPop = (type(sportTelemetryPop) == "function")
  end
  if not hasTlmPop then return end

  -- Pop up to 8 packets per frame to avoid stalling
  for _ = 1, 8 do
    local physId, primId, dataId, value = sportTelemetryPop()
    if physId == nil then break end

    -- Ensure value fits in uint32 (Lua may return negative for high bit set)
    if value < 0 then value = value + 0x100000000 end

    local di_lo = dataId % 256
    local di_hi = math.floor(dataId / 256) % 256
    local v0 = value % 256
    local v1 = math.floor(value / 256) % 256
    local v2 = math.floor(value / 65536) % 256
    local v3 = math.floor(value / 16777216) % 256

    -- CRC = XOR of all bytes from TYPE through last payload byte
    local crc = bit32.bxor(T_TLM, physId, primId, di_lo, di_hi, v0, v1, v2, v3)
    serialWrite(string.char(SYNC, T_TLM, physId, primId, di_lo, di_hi, v0, v1, v2, v3, crc))
  end
end

-- run(active) — called every frame by EdgeTX Function-script engine.
-- active: true when the Special Function switch is ON (ignored here; always run).
local function run(active)
  -- Yield *incoming* serial to Tools script while its heartbeat is fresh.
  -- (writng T_TLM frames is independent and continues regardless.)
  local toolsActive = false
  if getShmVar then
    local hb = getShmVar(SHM_TOOLS_HB)
    if hb and hb > 0 and (getTime() - hb) < 8 then
      toolsActive = true
    end
  end

  if not toolsActive then
    if type(serialRead) ~= "function" then return end
    local d = serialRead(64)
    if d and #d > 0 then
      for i = 1, #d do processByte(string.byte(d, i)) end
    end
  end

  -- Forward radio telemetry regardless of Tools activity
  sendTelemetry()
end

-- Export both run and background so the script works on all EdgeTX versions.
return { init=init, run=run, background=run }
