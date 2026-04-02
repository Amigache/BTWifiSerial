-- main.lua  –  EdgeTX Tools script
-- BTWifiSerial – BTWFS tool entry point

-- ── Module base path (folder where this file lives) ────────────────
-- loadScript() requires absolute paths on the radio SD card.
-- BTWFS scripts live at /SCRIPTS/TOOLS/BTWFS/
local BASE = "/SCRIPTS/TOOLS/BTWFS"

-- ── Debug flag ────────────────────────────────────────────────────
-- Set true to display page render time (in 10 ms ticks) in the top-right corner.
-- Keep false in production — it adds an extra getTime() call and text draw per frame.
local DEBUG_TIMING = false

-- ── Load shared libs ───────────────────────────────────────────────
local scale = loadScript(BASE .. "/lib/scale.lua")()
local theme = loadScript(BASE .. "/lib/theme.lua")()
local proto = loadScript(BASE .. "/lib/serial_proto.lua")()
local store = loadScript(BASE .. "/lib/store.lua")()
local input = loadScript(BASE .. "/lib/input.lua")()

-- ── Build component context (injected into every component) ────────
local ctx = { scale = scale, theme = theme, proto = proto, store = store, input = input }

-- ── Serial helpers ─────────────────────────────────────────────────
-- sendFrame(str): write a pre-built frame string to the AUX serial port.
ctx.sendFrame = function(frame)
  if serialWrite then serialWrite(frame) end
end

-- ── Load component classes ─────────────────────────────────────────
-- loadScript(path)  → chunk function
-- ()                → executes chunk, returns the factory function(ctx)
-- (ctx)             → executes factory, returns the class table
local Label     = loadScript(BASE .. "/components/label.lua")()(ctx)
ctx.Label       = Label

local Header    = loadScript(BASE .. "/components/header.lua")()(ctx)
ctx.Header      = Header

local PageTitle = loadScript(BASE .. "/components/page_title.lua")()(ctx)
ctx.PageTitle   = PageTitle

local Footer    = loadScript(BASE .. "/components/footer.lua")()(ctx)
ctx.Footer      = Footer

local Section   = loadScript(BASE .. "/components/section.lua")()(ctx)
ctx.Section     = Section

local List      = loadScript(BASE .. "/components/list.lua")()(ctx)
ctx.List        = List

local ChannelBar = loadScript(BASE .. "/components/channel_bar.lua")()(ctx)
ctx.ChannelBar   = ChannelBar

local Grid       = loadScript(BASE .. "/components/grid.lua")()(ctx)
ctx.Grid         = Grid

local Page      = loadScript(BASE .. "/components/page.lua")()(ctx)
ctx.Page        = Page

local Button    = loadScript(BASE .. "/components/button.lua")()(ctx)
ctx.Button      = Button

local Loading   = loadScript(BASE .. "/components/loading.lua")()(ctx)
ctx.Loading     = Loading

local Modal     = loadScript(BASE .. "/components/modal.lua")()(ctx)
ctx.Modal       = Modal

local PickModal = loadScript(BASE .. "/components/pick_modal.lua")()(ctx)
ctx.PickModal   = PickModal

local StatusDot = loadScript(BASE .. "/components/status_dot.lua")()(ctx)
ctx.StatusDot   = StatusDot

-- ── Global status indicators (shared across all pages' footers) ───
-- Created once; dot colors are updated by store events + conn state.
local _dotBoard = StatusDot.new({ label = "BOARD", x = 0, y = 0 })
local _dotWifi  = StatusDot.new({ label = "WIFI",  x = 0, y = 0 })
local _dotBLE   = StatusDot.new({ label = "BLE",   x = 0, y = 0 })

local _indicators = { _dotBoard, _dotWifi, _dotBLE }
ctx.indicators    = _indicators   -- pages forward this to Page.new({ indicators=ctx.indicators })

local function _updateDotBoard(connected)
  _dotBoard:setColor(connected and theme.C.green or theme.C.red)
end

local _wifiActive = false

local function _updateDotWifi(wifiActive)
  _wifiActive = wifiActive
  local wmp = store.prefs and store.prefs[0x01]
  local idx = wmp and wmp.curIdx or 0
  local label = "WIFI"
  local color
  if wifiActive then
    color = theme.C.green
    if     idx == 1 then label = "WIFI (AP)"
    elseif idx == 2 then label = "WIFI (STA)"
    end
  elseif idx ~= 0 then
    -- Mode configured but not yet active (pending restart)
    color = theme.C.orange
    if     idx == 1 then label = "WIFI (AP)"
    elseif idx == 2 then label = "WIFI (STA)"
    end
  else
    color = theme.C.red
  end
  _dotWifi:setLabel(label)
  _dotWifi:setColor(color)
end

local function _updateDotBLE(connected, connecting)
  if connected then
    _dotBLE:setColor(theme.C.green)
  elseif connecting then
    _dotBLE:setColor(theme.C.orange)
  else
    _dotBLE:setColor(theme.C.red)
  end
end

local _dotELRS = StatusDot.new({ label = "ELRS HT", x = 0, y = 0 })

local function _updateDotELRS(receiving)
  _dotELRS:setColor(receiving and theme.C.green or theme.C.red)
end

-- Build indicator list based on device mode; updated when prefs arrive
local function _rebuildIndicators()
  local devPref = store.prefs and store.prefs[0x02]
  local isElrs = devPref and devPref.curIdx == 3
  if isElrs then
    _indicators[3] = _dotELRS
  else
    _indicators[3] = _dotBLE
  end
end

-- Initialise all dots to disconnected state
_updateDotBoard(false)
_updateDotWifi(false)
_updateDotBLE(false, false)
_updateDotELRS(false)

-- React to status frames from ESP32
store.on("status", function(s)
  _updateDotWifi(s.wifiClients)
  _updateDotBLE(s.bleConnected, s.bleConnecting)
  _updateDotELRS(s.bleConnected)  -- bit0 = ELRS receiving in ELRS mode
end)

-- Swap BLE/ELRS indicator when prefs arrive
store.on("prefs_ready", function()
  _rebuildIndicators()
end)

-- Re-evaluate WiFi dot when prefs arrive or WiFi Mode pref changes
store.on("prefs_ready", function()
  _updateDotWifi(_wifiActive)
end)

store.on("pref_changed", function(pref)
  if pref.id == 0x01 then
    _updateDotWifi(_wifiActive)
  end
end)

-- ── Load pages ─────────────────────────────────────────────────────
local Dashboard = loadScript(BASE .. "/pages/dashboard.lua")()(ctx)
local Settings  = loadScript(BASE .. "/pages/settings.lua")()(ctx)
local Bluetooth = loadScript(BASE .. "/pages/bluetooth.lua")()(ctx)
local Wifi      = loadScript(BASE .. "/pages/wifi.lua")()(ctx)

-- ── Serial frame dispatcher ────────────────────────────────────────
local _lastRxTick = 0

local function onFrame(ch, typ, payload)
  _lastRxTick = getTime()
  if ch == proto.CH_PREF then
    if     typ == proto.PT_PREF_BEGIN  then store.beginPrefs()
    elseif typ == proto.PT_PREF_ITEM   then store.addPref(proto.decodePrefPayload(payload, false))
    elseif typ == proto.PT_PREF_END    then store.endPrefs()
    elseif typ == proto.PT_PREF_UPDATE then store.updatePref(proto.decodePrefPayload(payload, true))
    elseif typ == proto.PT_PREF_ACK    then
      if #payload >= 2 then store.prefAck(payload[1], payload[2]) end
    end

  elseif ch == proto.CH_INFO then
    if     typ == proto.PT_INFO_CHANNELS    then store.updateChannels(proto.decodeChannels(payload))
    elseif typ == proto.PT_INFO_STATUS      then
      if #payload >= 1 then store.updateStatus(payload[1]) end
    elseif typ == proto.PT_INFO_BEGIN       then store.beginInfo()
    elseif typ == proto.PT_INFO_ITEM        then store.addInfo(proto.decodeInfoPayload(payload, false))
    elseif typ == proto.PT_INFO_END         then store.endInfo()
    elseif typ == proto.PT_INFO_UPDATE      then store.updateInfo(proto.decodeInfoPayload(payload, true))
    elseif typ == proto.PT_INFO_SCAN_STATUS then
      if #payload >= 2 then store.updateScanStatus(payload[1], payload[2]) end
    elseif typ == proto.PT_INFO_SCAN_ITEM   then store.addScanResult(proto.decodeScanItem(payload))
    elseif typ == proto.PT_INFO_WIFI_SCAN_STATUS then
      if #payload >= 2 then store.updateWifiScanStatus(payload[1], payload[2]) end
    elseif typ == proto.PT_INFO_WIFI_SCAN_ITEM then
      store.addWifiScanResult(proto.decodeWifiScanItem(payload))
    end
  end
  -- CH_TRANS is not consumed by the Tools script (passed through by btwfs)
end

local _parser = proto.newParser(onFrame)

-- ── Serial read (drain up to 64 bytes per frame) ───────────────────
local function readSerial()
  if not serialRead then return end
  local data = serialRead(256)
  if data and #data > 0 then
    for i = 1, #data do
      _parser(string.byte(data, i))
    end
  end
end

-- ── Heartbeat (keep ESP32 in "Tools active" mode) ─────────────────
local _lastHbTick = 0
local function sendHeartbeat()
  if not serialWrite then return end
  local now = getTime()
  if now - _lastHbTick >= 5 then   -- every 50 ms
    serialWrite(proto.buildInfoHeartbeat())
    _lastHbTick = now
  end
end

-- ── Initial data request (retried until prefs arrive) ─────────────
local _initTick  = 0
local _initDone  = false
local _prefsReady = false
local _infoReady  = false
local _prefsTick   = 0
local _infoTick    = 0

-- ── Connection / boot state ────────────────────────────────────────
local CONN_TIMEOUT      = 800   -- ticks (≈ 8 s, 1 tick = 10 ms)
local DISCONNECT_TIMEOUT = 300   -- ticks (≈ 3 s)
local INIT_GRACE        = 180   -- ticks (≈ 1.8 s) grace for the missing half
local _connState     = "idle"
local _connStartTick = 0
local _connModal     = nil

-- ── BG script warning ────────────────────────────────────────────
local PREF_DEV_MODE  = 0x02  -- ENUM: 0=Trainer IN, 1=Trainer OUT, 2=Telemetry
local PREF_TELEM_OUT = 0x03  -- ENUM: 0=WiFi UDP, 1=BLE, 2=None
local _bgWarnModal = nil
local _bgWarnShown = false

local function _bgScriptNeeded()
  if (ctx.sfSlot or 0) ~= 0 then return false end
  local dm  = store.prefs[PREF_DEV_MODE]
  local tel = store.prefs[PREF_TELEM_OUT]
  local dmIdx  = dm  and dm.curIdx  or 0
  local telIdx = tel and tel.curIdx or 2
  return dmIdx == 0
      or dmIdx == 1
      or (dmIdx == 2 and telIdx ~= 2)
end

local function _checkBgScriptWarning()
  if _bgWarnShown then return end
  _bgWarnShown = true
  if not _bgScriptNeeded() then return end
  local dm  = store.prefs[PREF_DEV_MODE]
  local dmIdx = dm and dm.curIdx or 0
  local modeNames = { [0]="Trainer IN", [1]="Trainer OUT", [2]="Telemetry" }
  local modeName  = modeNames[dmIdx] or "Current"
  _bgWarnModal = Modal.new({
    type     = "alert",
    severity = "warning",
    title    = "BG Script Missing",
    message  = modeName .. " mode needs the\nbtwfs background script.",
    onClose  = function() _bgWarnModal = nil end,
  })
  _bgWarnModal:show()
end

local function _onReady()
  _connState = "ready"
  _connModal = nil
  _updateDotBoard(true)
  _checkBgScriptWarning()
end

local function requestInitialData()
  if _prefsReady and _infoReady then
    _initDone = true
    return
  end
  if not serialWrite then return end
  local now = getTime()
  if now - _initTick < 200 then return end   -- retry every 2 s
  _initTick = now
  -- Split requests: send the missing piece individually to avoid a single
  -- large burst that overflows the radio’s UART RX buffer.
  if not _prefsReady then
    serialWrite(proto.buildPrefRequest())
  end
  if not _infoReady then
    serialWrite(proto.buildInfoRequest())
  end
end

store.on("prefs_ready", function()
  _prefsReady = true
  _prefsTick = getTime()
  _initDone = _prefsReady and _infoReady
  if _initDone and (_connState == "connecting" or _connState == "disconnected") then
    _onReady()
  end
end)

store.on("info_ready", function()
  _infoReady = true
  _infoTick = getTime()
  _initDone = _prefsReady and _infoReady
  if _initDone and (_connState == "connecting" or _connState == "disconnected") then
    _onReady()
  end
end)

-- Re-evaluate BG script warning when Device Mode or Telem Out changes.
store.on("pref_changed", function(pref)
  if (pref.id == PREF_DEV_MODE or pref.id == PREF_TELEM_OUT)
    and _connState == "ready" then
    _bgWarnShown = false
    if _bgWarnModal then _bgWarnModal:close(); _bgWarnModal = nil end
  end
end)

local SHM_TOOLS_HB = 1   -- slot written by Tools script; read by btwfs.lua
local function updateShmHeartbeat()
  if setShmVar then setShmVar(SHM_TOOLS_HB, getTime()) end
end

-- ── Navigator ──────────────────────────────────────────────────────
-- pages: ordered array of page instances. Add/remove at will.
-- The footer pagination updates automatically.
local pages    = {}
local pageIdx  = 1

local function currentPage()
  return pages[pageIdx]
end

local function navigateTo(idx)
  pageIdx = ((idx - 1) % #pages) + 1   -- wrap around
  currentPage():setPagination(pageIdx, #pages)
end

-- ── Background script SF detection ──────────────────────────────
-- Scans the model's Special Functions for an existing btwfs slot.
-- Writing model SFs from a TOOLS script is not reliably supported by
-- EdgeTX — the scan detects an existing manual setup only.
-- Result stored in ctx.sfSlot:
--   nil  = model API unavailable
--   0    = not found (user must set it up manually)
--   N>0  = found at slot N (1-based, i.e. SF1, SF2, …)
local BG_SCRIPT_NAME = "btwfs"

local function ensureBgScript()
  ctx.sfSlot = 0  -- default: not found
  if not (model and model.getCustomFunction) then
    ctx.sfSlot = nil  -- API unavailable
    return
  end
  if not FUNC_PLAY_SCRIPT then return end
  for i = 0, 63 do
    local cf = model.getCustomFunction(i)
    if cf and cf.func == FUNC_PLAY_SCRIPT and cf.name == BG_SCRIPT_NAME then
      ctx.sfSlot = i + 1   -- 1-based (SF1, SF2, …)
      return
    end
  end
  -- Not found — user must add it manually in Special Functions:
  -- Switch: ON  →  Lua Script  →  btwfs
end

local function init()
  _initDone    = false
  _prefsReady  = false
  _infoReady   = false
  _prefsTick   = 0
  _infoTick    = 0
  _initTick    = 0
  _lastHbTick  = 0
  _lastRxTick  = getTime()
  _bgWarnShown = false
  _bgWarnModal = nil
  store.reset()
  -- Start in connecting state: spinner shown until prefs arrive or timeout
  _connState     = "connecting"
  _connStartTick = getTime()
  _connModal     = Modal.new({
    type     = "info",
    severity = "info",
    title    = "Connecting...",
    message  = "Communicating with device...",
  })
  _connModal:show()
  ensureBgScript()
  pages[1] = Dashboard.new()
  pages[2] = Settings.new()
  pages[3] = Bluetooth.new()
  pages[4] = Wifi.new()
  navigateTo(1)
end

local function run(event, touchState)
  -- Drive serial protocol
  readSerial()
  sendHeartbeat()
  updateShmHeartbeat()
  requestInitialData()

  -- ── Connection gate ──────────────────────────────────────────────
  if _connState == "connecting" then
    -- Accept when both halves arrived
    local bothReady = _prefsReady and _infoReady
    -- Symmetric grace: if one half arrived, give the other INIT_GRACE ticks
    local graceOk = false
    if _prefsReady and not _infoReady and (getTime() - _prefsTick >= INIT_GRACE) then
      graceOk = true
    end
    if _infoReady and not _prefsReady and (getTime() - _infoTick >= INIT_GRACE) then
      graceOk = true
    end
    if bothReady or graceOk then
      _initDone = bothReady
      _onReady()
    end
    if getTime() - _connStartTick >= CONN_TIMEOUT then
      -- Timeout reached: swap spinner for error alert
      _connState = "timeout"
      _connModal = Modal.new({
        type     = "alert",
        severity = "error",
        title    = "No Connection",
        message  = "Could not communicate\nwith the device.",
        onClose  = function() _connState = "exit" end,
      })
      _connModal:show()
    end
    if _connModal and _connModal:isOpen() then
      _connModal:handleEvent(event)
      _connModal:render()
    end
    if _connState == "exit" then return 2 end
    return 0
  end

  if _connState == "timeout" then
    if _connModal and _connModal:isOpen() then
      _connModal:handleEvent(event)
      _connModal:render()
    end
    if _connState == "exit" then return 2 end
    return 0
  end

  if _connState == "disconnected" then
    -- Allow EXIT to quit while reconnecting
    if (EVT_VIRTUAL_EXIT ~= nil and event == EVT_VIRTUAL_EXIT)
    or (EVT_EXIT_BREAK   ~= nil and event == EVT_EXIT_BREAK) then
      return 2
    end
    if _connModal and _connModal:isOpen() then
      _connModal:render()
    end
    return 0
  end

  if _connState == "exit" then
    return 2
  end

  -- ── Disconnect detection (only while fully connected) ─────────────────────
  if _connState == "ready" and getTime() - _lastRxTick >= DISCONNECT_TIMEOUT then
    _connState    = "disconnected"
    _initDone     = false
    _prefsReady   = false
    _infoReady    = false
    _prefsTick    = 0
    _infoTick     = 0
    _initTick     = 0
    store.reset()
    _updateDotBoard(false)
    _connModal = Modal.new({
      type     = "info",
      severity = "warning",
      title    = "Reconnecting...",
      message  = "Waiting for device...",
    })
    _connModal:show()
  end

  -- Check BG script warning once per session (deferred until "ready")
  if not _bgWarnShown and _connState == "ready" then
    _checkBgScriptWarning()
  end

  -- Page-button navigation (consume event before passing to page)
  if EVT_VIRTUAL_NEXT_PAGE ~= nil and event == EVT_VIRTUAL_NEXT_PAGE then
    navigateTo(pageIdx + 1)
    event = 0
  elseif EVT_VIRTUAL_PREV_PAGE ~= nil and event == EVT_VIRTUAL_PREV_PAGE then
    navigateTo(pageIdx - 1)
    event = 0
  end

  -- Delegate remaining events to the current page (if it handles them)
  local pg = currentPage()
  if event ~= 0 and pg.handleEvent then
    if not (_bgWarnModal and _bgWarnModal:isOpen()) then
      pg:handleEvent(event)
    end
  end

  if DEBUG_TIMING then
    local _t0 = getTime()
    pg:render()
    local _dt = getTime() - _t0
    lcd.drawText(scale.W - scale.sx(50), 0, "T:" .. _dt, SMLSIZE)
  else
    pg:render()
  end

  -- BG script warning modal renders on top of everything
  local bgm = _bgWarnModal
  if bgm and bgm:isOpen() then
    bgm:handleEvent(event)
    if bgm:isOpen() then bgm:render() end
  end

  return 0
end

local function background()
  -- Keep reading serial and updating SHM even when script is backgrounded.
  readSerial()
  updateShmHeartbeat()
end

return { init = init, run = run, background = background }
