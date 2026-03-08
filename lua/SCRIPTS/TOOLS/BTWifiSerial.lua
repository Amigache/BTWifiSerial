-- BTWifiSerial.lua  –  EdgeTX Tools script
-- Pixel-perfect UI for BTWifiSerial ESP32-C3 BLE bridge
-- Reference design: 800x480 – auto-scales to any color LCD
--
-- SETUP:
--   1. Web UI -> Serial Mode -> "LUA Serial (EdgeTX)"
--   2. Radio  -> AUX port -> LUA  (115200 baud)
--   3. Copy BTWifiSerial.lua  to  /SCRIPTS/TOOLS/BTWifiSerial.lua
--   4. Copy btwfs.lua          to  /SCRIPTS/FUNCTIONS/btwfs.lua
--   5. Special Functions → SF1 → Switch: ON → Lua Script → btwfs
--   6. MIXES: CH1 = 100% GV1, CH2 = 100% GV2 … CH8 = 100% GV8
--
-- btwfs.lua runs in background and writes channel data to GVars.
-- This Tools script is for configuration/monitoring UI only.
-- Both scripts share the serial port; coordination via ShmVar 1.

-- ═══════════════════════════════════════════════════════════════════
-- PROTOCOL
-- ═══════════════════════════════════════════════════════════════════
local SYNC           = 0xAA
local T_CH           = 0x43
local T_ST           = 0x53
local T_ACK          = 0x41
local T_CFG          = 0x47  -- config push from ESP32 (apMode, deviceMode)
local T_INF          = 0x49  -- info frame: 12-byte build timestamp (DDMMYYYYHHMM)
local T_CMD          = 0x02
local CMD_AP_ON      = 0x02
local CMD_AP_OFF     = 0x03
local CMD_DEV_TRAINER_IN  = 0x20
local CMD_DEV_TRAINER_OUT = 0x21
local CMD_DEV_TELEMETRY   = 0x22
local CMD_REQ_INFO   = 0x06  -- request ESP32 to send CFG+INF+SYS immediately
local T_SYS          = 0x59  -- system info frame from ESP32
local T_SCAN_STATUS  = 0x44  -- scan state: 0=idle, 1=scanning, 2=complete
local T_SCAN_ENTRY   = 0x52  -- scan result entry (40 bytes)
local CMD_BLE_SCAN      = 0x07
local CMD_HEARTBEAT     = 0x08  -- keep firmware "Tools active" flag alive (no other effect)
local CMD_BLE_DISCONNECT= 0x09
local CMD_BLE_FORGET    = 0x0A
local CMD_BLE_RECONNECT = 0x0B
local CMD_TELEM_WIFI    = 0x0C  -- switch telemetry output → WiFi UDP (saves config, restarts)
local CMD_TELEM_BLE     = 0x0D  -- switch telemetry output → BLE         (saves config, restarts)
local CMD_TELEM_OFF     = 0x0E  -- switch telemetry output → Off          (saves config, restarts)
local CMD_BAUD_57600    = 0x0F  -- set mirror baud → 57600  (saves config, restarts)
local CMD_BAUD_115200   = 0x23  -- set mirror baud → 115200 (saves config, restarts)
local CMD_BLE_CONNECT_BASE = 0x10  -- +index (0..15)
local T_STR_SET          = 0x4E  -- string-set frame (Lua→ESP32)
local STR_BT_NAME        = 0x01
local STR_SSID           = 0x02
local STR_UDP_PORT       = 0x03
local STR_AP_PASS        = 0x04
local CHARSET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
local NUMSET  = "0123456789"
local NUM_CH         = 8
local MODAL_TIMEOUT  = 1000   -- 10 s (getTime ticks at 10 ms each)
local SHM_TOOLS_HB   = 1      -- shared memory slot: Tools heartbeat (getTime value)

-- ═══════════════════════════════════════════════════════════════════
-- DISPLAY
-- ═══════════════════════════════════════════════════════════════════
local isColor = (lcd.setColor ~= nil)
local W, H = LCD_W, LCD_H
local big = (W >= 600)

-- Scale from 800x480 reference to actual screen
local function sx(v) return math.floor(v * W / 800) end
local function sy(v) return math.floor(v * H / 480) end

-- ═══════════════════════════════════════════════════════════════════
-- PALETTE  (matches SVG design)
-- ═══════════════════════════════════════════════════════════════════
local C = {}
if isColor then
  C.bg     = lcd.RGB(30, 34, 42)       -- #1E222A  background
  C.topBg  = lcd.RGB(18, 26, 42)       -- #121A2A  top bar
  C.accent = lcd.RGB(0, 140, 255)      -- #008CFF  accent / highlight / bars
  C.panel  = lcd.RGB(49, 60, 81)       -- #313C51  status bar / lines / bar bg
  C.white  = lcd.RGB(255, 255, 255)    -- #FFFFFF
  C.gray   = lcd.RGB(130, 130, 130)    -- #828282  status text
  C.red    = lcd.RGB(234, 33, 0)       -- #EA2100  badge / dot
  C.green  = lcd.RGB(46, 204, 113)     -- connected / toast
  C.editBg = lcd.RGB(0, 85, 170)       -- dimmer blue for edit mode
  C.orange = lcd.RGB(255, 140, 0)       -- #FF8C00  AP mode overlay
end

-- ═══════════════════════════════════════════════════════════════════
-- FONTS  (adapt to screen resolution)
-- ═══════════════════════════════════════════════════════════════════
local F_TITLE  -- title font flag
local F_BODY   -- section headers / menu / status / badge
local F_SMALL  -- channel labels & values

if big then
  F_TITLE = MIDSIZE
  F_BODY  = 0
  F_SMALL = SMLSIZE
else
  F_TITLE = BOLD
  F_BODY  = SMLSIZE
  F_SMALL = SMLSIZE
end

-- Font-cell height estimates (for modal vertical centering)
local FH_TITLE = big and 24 or 16
local FH_BODY  = big and 16 or 12
local FH_SMALL = big and 12 or 10
local FH_LGAP  = big and 10 or 7   -- gap after title row
local FH_SGAP  = big and  8 or 5   -- gap between body and hint rows

-- ═══════════════════════════════════════════════════════════════════
-- LAYOUT  (computed once, all derived from 800x480 reference)
-- ═══════════════════════════════════════════════════════════════════
local L = {}

local function computeLayout()
  L.pad = sx(17)
  L.cw  = W - 2 * L.pad                              -- content width (766 ref)

  -- All Y positions below are taken directly from the 800x480 SVG
  -- reference design and scaled via sy().  This avoids centering
  -- formulas that depend on font-cell-height guesswork.

  -- ── Top bar ──────────────────────────────────────────────────────
  L.topH    = sy(50)
  L.titleX  = sx(15)
  L.titleY  = sy(5)                                    -- raised slightly for visual centering
  L.accentH = math.max(1, sy(2))

  -- Badge (top-right)
  L.badgeW    = sx(219)
  L.badgeH    = sy(32)
  L.badgeX    = W - L.badgeW - sx(17)
  L.badgeY    = sy(9)                                  -- SVG: badge y = 9
  L.badgeTxtX = L.badgeX + sx(10)
  L.badgeTxtY = L.badgeY + sy(4)                       -- SVG: text offset ≈ 4

  -- ── Page header (replaces status bar – shows current page title) ─
  L.pageHdrY    = L.topH + L.accentH
  L.pageHdrH    = sy(38)
  L.pageHdrTxtY = L.pageHdrY + math.floor((L.pageHdrH - FH_BODY) / 2) - sy(3)

  -- ── BLE status indicator inside topbar (left of badge) ──────────
  L.bleDotSz = math.max(4, sy(8))
  local bleTxtW = sx(28)                                   -- "BLE" label width estimate
  L.bleDotX  = L.badgeX - sx(12) - bleTxtW - sx(4) - L.bleDotSz
  L.bleDotY  = math.floor((L.topH - L.bleDotSz) / 2)
  L.bleTxtX  = L.bleDotX + L.bleDotSz + sx(4)
  L.bleTxtY  = math.floor((L.topH - FH_SMALL) / 2) - sy(5)

  -- ── AP status indicator inside topbar (left of BLE) ─────────────
  local apTxtW = sx(20)                                    -- "AP" label width estimate
  L.apDotX  = L.bleDotX - sx(18) - apTxtW - sx(4) - L.bleDotSz
  L.apDotY  = L.bleDotY
  L.apTxtX  = L.apDotX + L.bleDotSz + sx(4)
  L.apTxtY  = L.bleTxtY

  -- ── Content area (below page header) ─────────────────────────────
  local contentY = L.pageHdrY + L.pageHdrH
  L.secGapTL   = sy(14)                                   -- title-bottom to line (shared)
  L.secGapLC   = sy(12)                                   -- line to content (shared)
  L.secTxtY    = contentY + sy(10)
  L.secLineY   = L.secTxtY + FH_BODY + L.secGapTL

  -- ── Menu / info rows ──────────────────────────────────────────────
  L.menuY      = L.secLineY + L.secGapLC
  L.menuRH     = sy(36)
  L.menuHL     = sy(36)
  L.menuTxtOff = math.floor((sy(36) - FH_BODY) / 2) - sy(3)
  L.menuValX   = sx(432)
  L.settingsMenuY = contentY + sy(10)  -- Settings rows align with section text Y

  -- ── Channel bars (pinned to bottom of screen) ─────────────────────
  L.chRH     = sy(25)
  L.chBH     = sy(17)
  L.chCW     = math.max(1, sx(2))
  L.chTxtOff = 0
  L.chBarOff = sy(3)
  L.chY        = H - 4 * L.chRH - sy(4)  -- 4 rows, small bottom padding
  L.chSecLineY = L.chY - L.secGapLC
  L.chSecTxtY  = L.chSecLineY - L.secGapTL - FH_BODY

  -- ── Scroll areas ────────────────────────────────────────────────────
  -- Dashboard: rows fit between menuY and channel section header
  local sysAreaH = L.chSecTxtY - sy(6) - L.menuY
  L.sysVisRows = math.max(1, math.floor(sysAreaH / L.menuRH))
  -- Telemetry mode: system info fills full height (no channel section)
  local sysFullH = H - sy(20) - L.menuY
  L.sysFullRows = math.max(1, math.floor(sysFullH / L.menuRH))
  L.sysAreaBottom = L.menuY + sysFullH  -- bottom edge of dashboard when no channel bars
  -- Settings: rows fit between settingsMenuY and bottom margin
  local setAreaH = H - L.settingsMenuY - sy(20)
  L.menuVisRows        = math.max(1, math.floor(setAreaH / L.menuRH))
  L.settingsAreaBottom = L.settingsMenuY + setAreaH  -- bottom edge of settings content

  -- Bluetooth page: total draw area from menuY to bottom margin
  local btAreaH = H - L.menuY - sy(20)
  L.btVisRows    = math.max(1, math.floor(btAreaH / L.menuRH))  -- kept for scroll arrows
  L.btAreaBottom = L.menuY + btAreaH
  -- Find Devices section: rows available below its sub-header
  -- Layout: Saved Device section ~2 rows + gap + Find header → leaves ~btVisRows-4 rows
  L.btFindRows = math.max(1, L.btVisRows - 4)

  -- ── Scroll arrow geometry (shared by both pages) ─────────────────
  L.triSz      = sy(4)              -- half-width → triangle 2*triSz+1 wide, triSz+1 tall
  L.scrollColW = L.triSz * 2 + sx(10)  -- reserved column width for arrows
  L.triX       = W - L.pad - L.triSz   -- horizontal centre of arrows

  -- Left column
  L.chLblX1  = sx(20)
  L.chBarX1  = sx(66)
  L.chPctX1  = sx(353)
  -- Right column
  L.chLblX2  = sx(406)
  L.chBarX2  = sx(452)
  L.chPctX2  = sx(739)
  L.chBW     = sx(274)
end

-- ═══════════════════════════════════════════════════════════════════
-- STATE
-- ═══════════════════════════════════════════════════════════════════
local rxState, rxType, rxBuf, rxNeeded = 0, 0, {}, 0
local bleConnected   = false
local boardConnected = false
local lastRxTime     = 0
local channels       = {0,0,0,0,0,0,0,0}
local buildTs        = ""      -- 12-char build timestamp received from ESP32
local sysMode        = 0       -- OutputMode enum value (0=FrSky..4=LUA)
local btName         = ""      -- BT device name
local apSsid         = ""      -- WiFi AP SSID
local apPass         = ""      -- WiFi AP password
local udpPort        = 5010    -- WiFi AP UDP port
local apHasClients   = false   -- true if ≥1 STA connected to AP
local mirrorBaudIdx  = 0       -- 0=57600, 1=115200 (S.PORT Mirror baud)
local localAddr      = ""      -- local BLE MAC address
local remoteAddr     = ""      -- saved remote BLE MAC address
local sysScroll      = 0       -- scroll offset for system info rows
local currentPage    = 1       -- 1 = Dashboard, 2 = Bluetooth, 3 = Settings
local deviceMode     = 0      -- 0=Trainer IN, 1=Trainer OUT, 2=Telemetry
local apMode         = 1      -- 0=AP blocked, 1=normal, 2=telemetry AP
local tlmOutput      = 0      -- 0=WiFi UDP, 1=BLE (last value from T_CFG byte 4)
local scanState      = 0       -- 0=idle, 1=scanning, 2=complete
local scanStartT     = 0       -- getTime() when scan started (for timeout)
local scanResults    = {}      -- array of {name, rssi, hasFrsky, addr}
local bleConnecting  = false   -- true while ESP32 connection attempt is in progress

-- ─── App mode state machine ────────────────────────────────────────
local APP = {
  INIT        = 1,  -- waiting for first board data   → blue "Getting Data" modal
  RUNNING     = 2,  -- normal operation
  DISCONNECTED = 3, -- board not responding             → "Board not Connected" badge
  AP_ACTIVE   = 4,  -- (unused, kept for enum compat)
}
local appState     = APP.INIT
local appInitTime  = 0      -- set in init(); drives the INIT modal animation
local INIT_TIMEOUT = 500    -- ticks (5 s) before giving up → DISCONNECTED
local gotConfig    = false  -- received T_CFG at least once
local gotInfo      = false  -- received T_INF at least once
local gotSys       = false  -- received T_SYS at least once
local lastReqTime  = 0      -- last time we sent CMD_REQ_INFO
local lastHbT      = 0      -- last time we sent CMD_HEARTBEAT to firmware


-- ═══════════════════════════════════════════════════════════════════
-- MENU
-- ═══════════════════════════════════════════════════════════════════
local menuItems = {
  -- { label, {option texts}, {commands}, currentIdx, savedIdx }
  { "AP Mode",     {"On",         "Off"},       {CMD_AP_ON, CMD_AP_OFF},           2, 2 },
  { "Device Mode", {"Trainer IN", "Trainer OUT", "Telemetry"}, {CMD_DEV_TRAINER_IN, CMD_DEV_TRAINER_OUT, CMD_DEV_TELEMETRY}, 1, 1 },
  { "Telem Output", {"WiFi", "BLE", "Off"}, {CMD_TELEM_WIFI, CMD_TELEM_BLE, CMD_TELEM_OFF}, 1, 1 },
  { "Mirror Baud", {"57600", "115200"}, {CMD_BAUD_57600, CMD_BAUD_115200}, 1, 1 },
  -- text items: { label, "text", subCmd }
  { "BT Name",     "text", STR_BT_NAME },
  { "SSID",        "text", STR_SSID },
  { "UDP Port",    "text", STR_UDP_PORT, 5 },
  { "AP Password", "text", STR_AP_PASS },
}

local menu = {
  sel     = 1,
  editing = false,
  offset  = 0,
}

-- Text-editing state for BT Name / SSID
local textEdit = {
  active  = false,
  itemIdx = 0,       -- which menuItem (4 or 5)
  subCmd  = 0,       -- STR_BT_NAME or STR_SSID
  chars   = {},      -- array of charset indices (1-based into charset)
  cursor  = 1,       -- current editing position (1-based)
  saved   = "",      -- original value before editing (for revert)
  reboot  = false,   -- requires reboot on save?
  maxLen  = 15,
  charset = CHARSET, -- active character set (CHARSET or NUMSET)
}

-- Modal overlay state (replaces simple toast)
local modal = {
  active    = false,     -- is the modal visible?
  state     = "saving",  -- "saving" | "error" | "text_confirm" | "rebooting" | "reboot_error"
  sentTime  = 0,
  itemIdx   = 0,         -- which menuItem triggered the save
  prevVal   = 0,         -- savedIdx before the command was sent (for revert)
  newName   = "",        -- proposed new name (text edit only)
  textSave  = false,     -- is this a text-field save?
  confirmSel = 1,        -- 1=Yes, 2=No (for text_confirm)
}

-- Bluetooth page state
local bt = {
  pageFocus    = 0,         -- 0 = Saved Device row focused, 1 = Find Devices focused
  -- Saved Device section
  savedEditing = false,   -- true while user is editing the saved-device row
  savedToggled = false,   -- true = Forget selected; false = Disconnect/Reconnect
  -- Find Devices section
  findSel    = 1,         -- selected row index inside scan results
  findScroll = 0,
}

-- Info modal shared by Reconnecting / Connecting / errors
local infoModal = {
  active  = false,
  kind    = "spin",   -- "spin" | "ok" | "err"
  title   = "",
  body    = "",
  startT  = 0,
}

local function getPageKeys()
  if deviceMode == 0 or deviceMode == 1 then
    return {"dashboard", "settings", "bluetooth"}
  end
  -- deviceMode == 2 (Telemetry)
  local tIdx = menuItems[3] and menuItems[3][5] or 1
  if tIdx == 1 then return {"dashboard", "settings", "telemetry", "wifi"} end
  if tIdx == 2 then return {"dashboard", "settings", "telemetry", "bluetooth"} end
  return {"dashboard", "settings", "telemetry"}
end

local function getCurrentPageKey()
  local pages = getPageKeys()
  if currentPage > #pages then currentPage = #pages end
  if currentPage < 1 then currentPage = 1 end
  return pages[currentPage], pages
end

-- Confirmation modal state (Forget)
local confirm = {
  active = false,
  sel    = 2,     -- 1=Yes, 2=No (default No)
  title  = "",
  cmd    = 0,
}

-- ═══════════════════════════════════════════════════════════════════
-- EVENT HELPERS
-- ═══════════════════════════════════════════════════════════════════
local function evEnter(e)
  return (EVT_VIRTUAL_ENTER and e == EVT_VIRTUAL_ENTER)
      or (EVT_ENTER_BREAK   and e == EVT_ENTER_BREAK)
end
local function evExit(e)
  return (EVT_VIRTUAL_EXIT and e == EVT_VIRTUAL_EXIT)
      or (EVT_EXIT_BREAK   and e == EVT_EXIT_BREAK)
end
local function evNext(e)
  return (EVT_VIRTUAL_NEXT and e == EVT_VIRTUAL_NEXT)
      or (EVT_ROT_RIGHT    and e == EVT_ROT_RIGHT)
      or (EVT_PLUS_BREAK   and e == EVT_PLUS_BREAK)
      or (EVT_PLUS_REPT    and e == EVT_PLUS_REPT)
end
local function evPrev(e)
  return (EVT_VIRTUAL_PREV and e == EVT_VIRTUAL_PREV)
      or (EVT_ROT_LEFT     and e == EVT_ROT_LEFT)
      or (EVT_MINUS_BREAK  and e == EVT_MINUS_BREAK)
      or (EVT_MINUS_REPT   and e == EVT_MINUS_REPT)
end
local function evPage(e)
  return (EVT_VIRTUAL_NEXT_PAGE ~= nil and e == EVT_VIRTUAL_NEXT_PAGE)
      or (EVT_VIRTUAL_PREV_PAGE ~= nil and e == EVT_VIRTUAL_PREV_PAGE)
end
local function evPageNext(e)
  return EVT_VIRTUAL_NEXT_PAGE ~= nil and e == EVT_VIRTUAL_NEXT_PAGE
end
local function evPagePrev(e)
  return EVT_VIRTUAL_PREV_PAGE ~= nil and e == EVT_VIRTUAL_PREV_PAGE
end

-- ═══════════════════════════════════════════════════════════════════
-- TEXT-EDIT HELPERS
-- ═══════════════════════════════════════════════════════════════════
local function charsetIndex(ch)
  local cs = textEdit.charset
  for i = 1, #cs do
    if string.sub(cs, i, i) == ch then return i end
  end
  return 1
end

local function charsToText(chars)
  local cs = textEdit.charset
  local s = ""
  for _, ci in ipairs(chars) do
    if ci >= 1 and ci <= #cs then
      s = s .. string.sub(cs, ci, ci)
    end
  end
  return s
end

local function getTextValue(subCmd)
  if subCmd == STR_BT_NAME then return btName end
  if subCmd == STR_SSID then return apSsid end
  if subCmd == STR_UDP_PORT then return tostring(udpPort) end
  if subCmd == STR_AP_PASS then return apPass end
  return ""
end

-- ═══════════════════════════════════════════════════════════════════
-- SERIAL PROTOCOL
-- ═══════════════════════════════════════════════════════════════════
local function sendCmd(cmd)
  if type(serialWrite) == "function" then
    serialWrite(string.char(SYNC, T_CMD, cmd, bit32.bxor(T_CMD, cmd)))
  end
end

local function sendString(subCmd, str)
  if type(serialWrite) ~= "function" then return end
  local frame = {SYNC, T_STR_SET, subCmd}
  for i = 1, 16 do
    frame[#frame + 1] = (i <= #str) and string.byte(str, i) or 0
  end
  local crc = 0
  for i = 2, 19 do crc = bit32.bxor(crc, frame[i]) end
  frame[20] = crc
  local s = ""
  for i = 1, 20 do s = s .. string.char(frame[i]) end
  serialWrite(s)
end

local function applyChannels()
  local crc = 0
  for i = 1, 17 do crc = bit32.bxor(crc, rxBuf[i]) end
  if crc ~= rxBuf[18] then return end
  for i = 0, NUM_CH - 1 do
    local v = rxBuf[2 + i*2] * 256 + rxBuf[3 + i*2]
    if v >= 32768 then v = v - 65536 end
    channels[i + 1] = v
    -- Write to GVars (GV1..GV8). model.setGlobalVariable(idx, flightMode, value)
    -- is available in all EdgeTX script types. Using pcall for safety.
    -- NOTE: EdgeTX silently ignores values outside [-1024, 1024] (no clamping).
    pcall(model.setGlobalVariable, i, 0, v)
  end
  lastRxTime = getTime()
end

local function applyStatus()
  if bit32.bxor(rxBuf[1], rxBuf[2]) ~= rxBuf[3] then return end
  local wasConnected  = bleConnected
  local wasConnecting = bleConnecting
  bleConnected  = bit32.band(rxBuf[2], 0x01) ~= 0
  apHasClients  = bit32.band(rxBuf[2], 0x02) ~= 0
  bleConnecting = bit32.band(rxBuf[2], 0x04) ~= 0
  -- Auto-close Connecting / Reconnecting / Disconnecting spinner
  if infoModal.active and infoModal.kind == "spin" then
    if bleConnected and not wasConnected then
      -- Connected: clear scan list, close modal
      scanState = 0; scanResults = {}
      bt.savedEditing = false; bt.savedToggled = false
      infoModal.active = false
    elseif not bleConnected and wasConnected then
      -- Disconnect confirmed
      infoModal.active = false
    elseif wasConnecting and not bleConnecting and not bleConnected then
      -- Connection attempt finished without success
      infoModal.kind  = "err"
      infoModal.title = "Connection Failed"
      infoModal.body  = "Press ENTER or EXIT"
    end
  end
  lastRxTime = getTime()
end

-- Apply a config push frame: sync the menu currentIdx/savedIdx with hardware state.
-- apMode value: 0=AP blocked, 1=normal, 2=telemetry AP (WiFi up, Lua not blocked)
-- deviceMode value: 0=Trainer IN, 1=Trainer OUT, 2=Telemetry
-- tlmOutput value: 0=WiFi UDP, 1=BLE
local function applyConfig()
  -- rxBuf[1]=T_CFG, rxBuf[2]=apMode, rxBuf[3]=deviceMode, rxBuf[4]=tlmOutput, rxBuf[5]=CRC
  local crc = bit32.bxor(bit32.bxor(bit32.bxor(rxBuf[1], rxBuf[2]), rxBuf[3]), rxBuf[4])
  if crc ~= rxBuf[5] then return end
  -- AP Mode menu mapping:
  --   apMode 0 (regular AP, blocks Lua) → "On"  (index 1)
  --   apMode 1 (normal operation)        → "Off" (index 2)
  --   apMode 2 (telemetry AP, no block)  → "On"  (index 1)  ← AP is still up
  local ap  = (rxBuf[2] == 1) and 2 or 1
  local ble = rxBuf[3] + 1
  if menuItems[1] then menuItems[1][4] = ap; menuItems[1][5] = ap end
  if menuItems[2] then menuItems[2][4] = ble; menuItems[2][5] = ble end
  deviceMode = rxBuf[3]
  apMode     = rxBuf[2]
  -- Sync telemetry output selection
  tlmOutput  = rxBuf[4]        -- 0=WiFi, 1=BLE, 2=Off
  local tOut = rxBuf[4] + 1
  do
    local oldPages = getPageKeys()
    local oldKey = oldPages[currentPage] or "dashboard"
    -- Now update savedIdx so getPageKeys() returns the new set
    if menuItems[3] then menuItems[3][4] = tOut; menuItems[3][5] = tOut end
    local newPages = getPageKeys()
    -- Re-locate same page key in the new set
    local found = false
    for i, k in ipairs(newPages) do
      if k == oldKey then currentPage = i; found = true; break end
    end
    if not found then currentPage = 1 end
  end
  gotConfig = true
  -- Safety net: if a modal was stuck waiting while board restarted, close it
  if modal.active and modal.state == "saving" then modal.active = false end
  -- Board came back after BLE role reboot → close the rebooting modal
  if modal.active and modal.state == "rebooting" then modal.active = false end
  -- AP runs alongside Lua in all modes
  if appState ~= APP.RUNNING then appState = APP.RUNNING end
  lastRxTime = getTime()
end

-- Apply an info frame: 12 ASCII bytes of build timestamp (DDMMYYYYHHMM)
local function applyInfo()
  -- rxBuf[1]=T_INF, rxBuf[2..13]=12 ASCII bytes, rxBuf[14]=CRC
  local crc = rxBuf[1]
  for i = 2, 13 do crc = bit32.bxor(crc, rxBuf[i]) end
  if crc ~= rxBuf[14] then return end
  local s = ""
  for i = 2, 13 do s = s .. string.char(rxBuf[i]) end
  buildTs = s
  gotInfo = true
end

-- Apply a system info frame: serialMode, btName, localAddr, remoteAddr, apSsid, udpPort, apPass, baudIdx
local function applySys()
  -- rxBuf[1]=T_SYS, [2]=serialMode, [3..18]=btName(16), [19..36]=localAddr(18),
  -- [37..54]=remoteAddr(18), [55..70]=apSsid(16), [71..72]=udpPort(2), [73..88]=apPass(16),
  -- [89]=baudIdx(1), [90]=CRC
  local crc = rxBuf[1]
  for i = 2, 89 do crc = bit32.bxor(crc, rxBuf[i]) end
  if crc ~= rxBuf[90] then return end
  sysMode = rxBuf[2]
  local s = ""
  for i = 3, 18 do if rxBuf[i] == 0 then break end; s = s .. string.char(rxBuf[i]) end
  btName = s
  s = ""
  for i = 19, 36 do if rxBuf[i] == 0 then break end; s = s .. string.char(rxBuf[i]) end
  localAddr = s
  s = ""
  for i = 37, 54 do if rxBuf[i] == 0 then break end; s = s .. string.char(rxBuf[i]) end
  remoteAddr = s
  s = ""
  for i = 55, 70 do if rxBuf[i] == 0 then break end; s = s .. string.char(rxBuf[i]) end
  apSsid = s
  udpPort = rxBuf[71] * 256 + rxBuf[72]
  if udpPort == 0 then udpPort = 5010 end
  s = ""
  for i = 73, 88 do if rxBuf[i] == 0 then break end; s = s .. string.char(rxBuf[i]) end
  apPass = s
  mirrorBaudIdx = rxBuf[89]  -- 0=57600, 1=115200
  if menuItems[4] and type(menuItems[4][2]) == "table" then
    local baudSel = mirrorBaudIdx + 1
    menuItems[4][4] = baudSel; menuItems[4][5] = baudSel
  end
  gotSys = true
  -- Auto-close "Forgetting" spinner when remoteAddr is cleared
  if infoModal.active and infoModal.kind == "spin"
     and infoModal.title == "Forgetting" and remoteAddr == "" then
    infoModal.active = false
  end
end

local function applyAck()
  -- rxBuf[1]=type, rxBuf[2]=result, rxBuf[3]=CRC
  if bit32.bxor(rxBuf[1], rxBuf[2]) ~= rxBuf[3] then return end
  if modal.active and modal.state == "saving" then
    if modal.textSave then
      -- Text field save (BT Name / SSID)
      if rxBuf[2] == 0x00 then
        -- Success: update local variable
        if textEdit.subCmd == STR_BT_NAME then
          btName = modal.newName
          textEdit.active = false
          modal.active = false
        elseif textEdit.subCmd == STR_SSID then
          apSsid = modal.newName
          textEdit.active = false
          modal.state   = "rebooting"
          modal.sentTime = getTime()
        elseif textEdit.subCmd == STR_UDP_PORT then
          udpPort = tonumber(modal.newName) or udpPort
          textEdit.active = false
          modal.state   = "rebooting"
          modal.sentTime = getTime()
        elseif textEdit.subCmd == STR_AP_PASS then
          apPass = modal.newName
          textEdit.active = false
          modal.state   = "rebooting"
          modal.sentTime = getTime()
        end
      else
        textEdit.active = false
        modal.state = "error"
      end
    else
      -- Regular menu item save
      if rxBuf[2] == 0x00 then
        local item = menuItems[modal.itemIdx]
        if item then item[5] = item[4] end
        if modal.itemIdx == 2 or modal.itemIdx == 3 or modal.itemIdx == 4 then
          modal.state   = "rebooting"
          modal.sentTime = getTime()
        else
          modal.active = false
        end
      else
        local item = menuItems[modal.itemIdx]
        if item then item[4] = modal.prevVal end
        modal.state = "error"
      end
    end
  end
end

local function applyScanStatus()
  -- rxBuf[1]=T_SCAN_STATUS, [2]=state, [3]=count, [4]=CRC
  if bit32.bxor(bit32.bxor(rxBuf[1], rxBuf[2]), rxBuf[3]) ~= rxBuf[4] then return end
  local state = rxBuf[2]
  if state == 1 then
    scanState = 1; scanStartT = getTime()
    scanResults = {}
  elseif state == 2 then
    scanState = 2
  else
    scanState = 0
  end
end

local function applyScanEntry()
  -- rxBuf: [1]=type,[2]=idx,[3]=rssi,[4]=hasFrsky,[5..20]=name(16),[21..38]=addr(18),[39]=CRC
  local crc = rxBuf[1]
  for i = 2, 38 do crc = bit32.bxor(crc, rxBuf[i]) end
  if crc ~= rxBuf[39] then return end
  local idx = rxBuf[2] + 1  -- 0-based → 1-based
  local rssi = rxBuf[3]
  if rssi >= 128 then rssi = rssi - 256 end
  local hasFrsky = rxBuf[4] ~= 0
  local name = ""
  for i = 5, 20 do if rxBuf[i] == 0 then break end; name = name .. string.char(rxBuf[i]) end
  local addr = ""
  for i = 21, 38 do if rxBuf[i] == 0 then break end; addr = addr .. string.char(rxBuf[i]) end
  scanResults[idx] = {name = name, rssi = rssi, hasFrsky = hasFrsky, addr = addr}
end

local function processByte(b)
  if rxState == 0 then
    if b == SYNC then rxState = 1 end
  elseif rxState == 1 then
    rxType = b;  rxBuf = {b}
    if     b == T_CH  then rxNeeded = 17; rxState = 2
    elseif b == T_ST  then rxNeeded = 2;  rxState = 2
    elseif b == T_ACK then rxNeeded = 2;  rxState = 2
    elseif b == T_CFG then rxNeeded = 4;  rxState = 2
    elseif b == T_INF then rxNeeded = 13; rxState = 2
    elseif b == T_SYS then rxNeeded = 89; rxState = 2
    elseif b == T_SCAN_STATUS then rxNeeded = 3;  rxState = 2
    elseif b == T_SCAN_ENTRY  then rxNeeded = 38; rxState = 2
    else   rxState = (b == SYNC) and 1 or 0 end
  elseif rxState == 2 then
    rxBuf[#rxBuf + 1] = b
    rxNeeded = rxNeeded - 1
    if rxNeeded == 0 then
      if     rxType == T_CH  then applyChannels()
      elseif rxType == T_ST  then applyStatus()
      elseif rxType == T_ACK then applyAck()
      elseif rxType == T_CFG then applyConfig()
      elseif rxType == T_INF then applyInfo()
      elseif rxType == T_SYS then applySys()
      elseif rxType == T_SCAN_STATUS then applyScanStatus()
      elseif rxType == T_SCAN_ENTRY  then applyScanEntry() end
      rxState = 0
    end
  end
end

local function readSerial()
  if type(serialRead) ~= "function" then
    boardConnected = false
    return
  end
  -- Drain the entire UART buffer each tick so partial frames are never
  -- left pending and large bursts (CFG+INF+SYS + channel frames) are
  -- consumed in full before checking the got* flags.
  local d = serialRead(128)
  while d and #d > 0 do
    for i = 1, #d do processByte(string.byte(d, i)) end
    d = serialRead(128)
  end
  local connected = (lastRxTime > 0) and ((getTime() - lastRxTime) < 200)
  boardConnected = connected
  -- State transitions
  if appState == APP.INIT then
    -- Actively request config+info from ESP32 every 500ms
    local now = getTime()
    if now - lastReqTime >= 50 then   -- 50 ticks = 500 ms
      lastReqTime = now
      sendCmd(CMD_REQ_INFO)
    end
    if gotConfig and gotInfo and gotSys then
      appState = APP.RUNNING
    elseif (now - appInitTime) >= INIT_TIMEOUT then
      appState = connected and APP.RUNNING or APP.DISCONNECTED
    end
  elseif appState == APP.RUNNING then
    if not connected then appState = APP.DISCONNECTED end
  elseif appState == APP.DISCONNECTED then
    if connected then appState = APP.RUNNING end
  end
  -- Periodic heartbeat so firmware keeps sending CFG/INF/SYS resync frames.
  -- btwfs.lua never sends CMDs, so without this the firmware would stop the
  -- heavier periodic TX after TOOLS_IDLE_TIMEOUT (15 s) of inactivity.
  if appState == APP.RUNNING and type(serialWrite) == "function" then
    local now = getTime()
    if now - lastHbT >= 500 then   -- every 5 s  (ticks = 10 ms)
      lastHbT = now
      sendCmd(CMD_HEARTBEAT)
    end
  end
end

-- ═══════════════════════════════════════════════════════════════════
-- DRAWING HELPERS
-- ═══════════════════════════════════════════════════════════════════
local function sc(c) lcd.setColor(CUSTOM_COLOR, c) end

local function fill(x, y, w, h, c)
  if isColor then sc(c); lcd.drawFilledRectangle(x, y, w, h, CUSTOM_COLOR)
  else lcd.drawFilledRectangle(x, y, w, h, SOLID) end
end

local function txt(x, y, s, fl, c)
  s = s or ""
  if isColor then sc(c or C.white); lcd.drawText(x, y, s, (fl or 0) + CUSTOM_COLOR)
  else lcd.drawText(x, y, s, fl or 0) end
end

-- Filled triangle: cy = top of bounding box, sz = half-base-width
-- up=true → ▲ (tip at top, base at bottom)   up=false → ▽ (base at top, tip at bottom)
local function drawTri(cx, cy, sz, up, c)
  if isColor then sc(c) end
  local fl = isColor and CUSTOM_COLOR or 0
  for i = 0, sz do
    local hw = up and i or (sz - i)
    local y  = cy + i
    lcd.drawLine(cx - hw, y, cx + hw, y, SOLID, fl)
  end
end

-- ═══════════════════════════════════════════════════════════════════
-- DRAW SECTIONS
-- ═══════════════════════════════════════════════════════════════════

-- ── Top bar ─────────────────────────────────────────────────────
local function drawTopBar()
  if isColor then
    fill(0, 0, W, L.topH, C.topBg)
    fill(0, L.topH, W, L.accentH, C.accent)
    txt(L.titleX, L.titleY, "BTWifiSerial", F_TITLE, C.white)

    -- AP status indicator (dot + label)
    local apOn = (apMode == 0 or apMode == 2)
    local apc  = apOn and (apHasClients and C.green or C.red) or C.gray
    fill(L.apDotX, L.apDotY, L.bleDotSz, L.bleDotSz, apc)
    txt(L.apTxtX, L.apTxtY, "AP", F_SMALL, apOn and C.white or C.gray)

    -- BLE status indicator (dot + label)
    local bdc = bleConnected and C.green or C.red
    fill(L.bleDotX, L.bleDotY, L.bleDotSz, L.bleDotSz, bdc)
    txt(L.bleTxtX, L.bleTxtY, "BLE", F_SMALL, bleConnected and C.green or C.gray)

    -- Page indicator (dots centered between BLE label and badge)
    local _, pages = getCurrentPageKey()
    local np = #pages
    local dots = {}
    for i = 1, np do dots[i] = (i == currentPage) and "●" or "○" end
    local dotStr = table.concat(dots, " ")
    local dotW   = (lcd.sizeText and lcd.sizeText(dotStr, F_SMALL)) or 0
    local dotAreaStart = L.bleTxtX + sx(32)
    local dotX   = dotAreaStart + math.floor((L.badgeX - dotAreaStart - dotW) / 2)
    txt(dotX, L.titleY + sy(16), dotStr, F_SMALL, C.gray)

    -- Board connection badge
    local bc = boardConnected and C.green or C.red
    fill(L.badgeX, L.badgeY, L.badgeW, L.badgeH, bc)
    local label = boardConnected and "Board Connected" or "Board not Connected"
    local tw  = lcd.sizeText(label, F_BODY)
    local btx = L.badgeX + math.floor((L.badgeW - tw) / 2)
    txt(btx, L.badgeTxtY, label, F_BODY, C.white)
  else
    lcd.drawText(1, 0, "BTWifiSerial", SMLSIZE + BOLD)
    local bst = (boardConnected and "BRD " or "    ") .. (bleConnected and "BLE" or "---")
    lcd.drawText(W - 60, 0, bst, SMLSIZE)
    lcd.drawLine(0, L.topH - 1, W - 1, L.topH - 1, SOLID, 0)
  end
end

-- ── Page header band (shows current page title) ─────────────────
local function drawPageHeader(title)
  if isColor then
    local t = string.upper(title)
    fill(0, L.pageHdrY, W, L.pageHdrH, C.panel)
    local tw = (lcd.sizeText and lcd.sizeText(t, F_BODY)) or 0
    local tx = tw > 0 and math.floor((W - tw) / 2) or L.pad
    txt(tx, L.pageHdrTxtY, t, F_BODY, C.white)
  else
    lcd.drawText(1, L.pageHdrY + 5, title, SMLSIZE + BOLD)
  end
end

-- ── Settings menu (Page 2) ──────────────────────────────────────
local function drawMenu()
  local total = #menuItems
  if isColor then
    local rowW = L.cw - L.scrollColW  -- leave room for scroll arrows
    for slot = 1, math.min(L.menuVisRows, total) do
      local idx = slot + menu.offset
      if idx > total then break end
      local item = menuItems[idx]
      local ry = L.settingsMenuY + (slot - 1) * L.menuRH
      local isSel = (idx == menu.sel)
      local isText = (type(item[2]) == "string")
      local isEditingThis = isText and textEdit.active and textEdit.itemIdx == idx

      if isSel then
        fill(L.pad, ry, rowW, L.menuHL,
             (menu.editing or isEditingThis) and C.editBg or C.accent)
      end

      local ty = ry + L.menuTxtOff
      txt(L.pad + sx(7), ty, item[1], F_BODY, C.white)

      if isText then
        -- Text-type item
        if isEditingThis then
          local cs = textEdit.charset
          -- Build display string from chars with cursor
          local ds = ""
          for ci = 1, #textEdit.chars do
            if ci == textEdit.cursor then
              -- This char will blink
            else
              ds = ds .. string.sub(cs, textEdit.chars[ci], textEdit.chars[ci])
            end
          end
          -- Draw non-blinking part before cursor
          local pre = ""
          for ci = 1, textEdit.cursor - 1 do
            if textEdit.chars[ci] then
              pre = pre .. string.sub(cs, textEdit.chars[ci], textEdit.chars[ci])
            end
          end
          txt(L.menuValX, ty, pre, F_BODY, C.white)
          -- Draw blinking cursor char
          local curCh
          local ci = textEdit.chars[textEdit.cursor]
          if ci and ci >= 1 and ci <= #cs then
            curCh = string.sub(cs, ci, ci)
          else
            curCh = "_"
          end
          local preW = (lcd.sizeText and lcd.sizeText(pre, F_BODY)) or (#pre * 10)
          txt(L.menuValX + preW, ty, curCh, F_BODY + BLINK, C.white)
          -- Draw non-blinking part after cursor
          local post = ""
          for ci2 = textEdit.cursor + 1, #textEdit.chars do
            if textEdit.chars[ci2] and textEdit.chars[ci2] >= 1 and textEdit.chars[ci2] <= #cs then
              post = post .. string.sub(cs, textEdit.chars[ci2], textEdit.chars[ci2])
            end
          end
          if post ~= "" then
            local curW = (lcd.sizeText and lcd.sizeText(curCh, F_BODY)) or 10
            txt(L.menuValX + preW + curW, ty, post, F_BODY, C.white)
          end
        else
          txt(L.menuValX, ty, getTextValue(item[3]), F_BODY, C.white)
        end
      else
        -- Regular option-list item
        local vfl = F_BODY
        if menu.editing and isSel then vfl = vfl + BLINK end
        txt(L.menuValX, ty, item[2][item[4]], vfl, C.white)
      end
    end
    -- Scroll arrows: always draw both, blue when active, gray when not
    local triSz  = L.triSz
    local triX   = L.triX
    local half   = math.floor(triSz / 2)
    local midRow = math.floor(L.menuRH / 2)
    local canUp  = menu.offset > 0
    local canDn  = menu.offset + L.menuVisRows < total
    -- ▲ centred on first visible row
    drawTri(triX, L.settingsMenuY + midRow - half, triSz, true,  canUp and C.accent or C.gray)
    -- ▽ anchored to bottom of content area
    drawTri(triX, L.settingsAreaBottom - triSz,    triSz, false, canDn and C.accent or C.gray)
  else
    for slot = 1, math.min(L.menuVisRows, total) do
      local idx = slot + menu.offset
      if idx > total then break end
      local item = menuItems[idx]
      local ry = L.settingsMenuY + (slot - 1) * L.menuRH
      local isSel = (idx == menu.sel)
      local isText = (type(item[2]) == "string")
      local isEditingThis = isText and textEdit.active and textEdit.itemIdx == idx
      local fl = isSel and INVERS or 0
      lcd.drawText(2, ry + 1, item[1], SMLSIZE + fl)
      if isText then
        if isEditingThis then
          local ds = charsToText(textEdit.chars)
          local vfl = (INVERS + BLINK)
          lcd.drawText(L.menuValX, ry + 1, ds, SMLSIZE + vfl)
        else
          lcd.drawText(L.menuValX, ry + 1, getTextValue(item[3]), SMLSIZE + fl)
        end
      else
        local vfl = (menu.editing and isSel) and (INVERS + BLINK) or fl
        lcd.drawText(L.menuValX, ry + 1, item[2][item[4]], SMLSIZE + vfl)
      end
    end
  end
end

-- ── Channel bars ────────────────────────────────────────────────
local function drawChannels()
  if deviceMode == 2 then return end  -- Telemetry mode: no channel bars
  -- Trainer OUT mode: read radio outputs into channels[]
  if deviceMode == 1 then
    for i = 1, NUM_CH do channels[i] = getValue("ch" .. i) or 0 end
  end
  local chTitle = (deviceMode == 1) and "Channels (Output)" or "Channels (Input)"
  if isColor then
    -- Section header
    txt(L.pad, L.chSecTxtY, chTitle, F_BODY, C.accent)
    fill(L.pad, L.chSecLineY, L.cw, 1, C.panel)

    for i = 1, NUM_CH do
      local col = (i <= 4) and 0 or 1
      local row = (i - 1) % 4
      local lx = col == 0 and L.chLblX1 or L.chLblX2
      local bx = col == 0 and L.chBarX1 or L.chBarX2
      local px = col == 0 and L.chPctX1 or L.chPctX2
      local ry = L.chY + row * L.chRH

      local v = channels[i]
      local pct = math.floor(v * 100 / 1024 + 0.5)

      -- Label
      txt(lx, ry + L.chTxtOff, "CH" .. i, F_SMALL, C.white)

      -- Bar background
      fill(bx, ry + L.chBarOff, L.chBW, L.chBH, C.panel)

      -- Center marker
      local cx = bx + math.floor(L.chBW / 2)
      fill(cx, ry + L.chBarOff, L.chCW, L.chBH, C.topBg)

      -- Value fill (positive = right of center, negative = left)
      local halfW = math.floor(L.chBW / 2)
      if v > 0 then
        local maxFw = halfW - L.chCW  -- clamp so fill stays inside bar bg
        local fw = math.min(math.floor(v * halfW / 1024), maxFw)
        if fw > 0 then
          fill(cx + L.chCW, ry + L.chBarOff, fw, L.chBH, C.accent)
        end
      elseif v < 0 then
        local fw = math.floor(-v * halfW / 1024)
        if fw > 0 then
          fill(cx - fw, ry + L.chBarOff, fw, L.chBH, C.accent)
        end
      end

      -- Percentage (LEFT-aligned: consistent start position after bar end)
      txt(px, ry + L.chTxtOff, pct .. "%", F_SMALL, C.white)
    end
  else
    lcd.drawText(1, L.chSecTxtY, chTitle, SMLSIZE)
    lcd.drawLine(1, L.chSecLineY, W - 2, L.chSecLineY, DOTTED, 0)
    for i = 1, NUM_CH do
      local col = (i <= 4) and 0 or 1
      local row = (i - 1) % 4
      local rx = col == 0 and 0 or math.floor(W / 2)
      local ry = L.chY + row * L.chRH
      local v = channels[i]
      local pct = math.floor(v * 100 / 1024 + 0.5)
      lcd.drawText(rx + 1, ry, "C" .. i, SMLSIZE)
      lcd.drawText(rx + 22, ry, pct .. "%", SMLSIZE)
    end
  end
end

-- ═══════════════════════════════════════════════════════════════════
-- MODAL COMPONENT
-- ═══════════════════════════════════════════════════════════════════
-- Standardized centered modal. Colors: C.accent=blue  C.green=ok
--                                        C.red=error   C.orange=warning
-- animated=true shows "..." spinner; body/hint are optional text rows.
local function drawStdModal(bg, title, body, animated, startT, hint)
  if not isColor then
    lcd.drawText(1, math.floor(H / 2), title, SMLSIZE + INVERS + BLINK)
    return
  end
  local mw = sx(400)
  local mh = sy(130)
  local mx = math.floor((W - mw) / 2)
  local my = math.floor((H - mh) / 2)
  local brd = math.max(1, sy(2))
  fill(mx - brd, my - brd, mw + 2*brd, mh + 2*brd, C.editBg)
  fill(mx, my, mw, mh, bg)
  local mcx = mx + math.floor(mw / 2)
  local function ctxt(y, s, fl)
    local tw = (lcd.sizeText and lcd.sizeText(s, fl)) or 0
    local tx = tw > 0 and (mcx - math.floor(tw / 2)) or (mx + sx(20))
    txt(tx, y, s, fl, C.white)
  end
  -- Compute text block total height for vertical centering
  local blockH
  if animated then
    blockH = FH_TITLE + FH_LGAP + FH_TITLE
  else
    blockH = FH_TITLE
    if body then blockH = blockH + FH_LGAP + FH_BODY end
    if hint then blockH = blockH + (body and FH_SGAP or FH_LGAP) + FH_SMALL end
  end
  local ty = my + math.floor((mh - blockH) / 2)
  ctxt(ty, title, F_TITLE)
  ty = ty + FH_TITLE
  if animated then
    local frames = {".", "..", "..."}
    local di = (math.floor((getTime() - startT) / 50) % 3) + 1
    ctxt(ty + FH_LGAP, frames[di], F_TITLE)
  else
    if body then
      ctxt(ty + FH_LGAP, body, F_BODY)
      ty = ty + FH_LGAP + FH_BODY
    end
    if hint then ctxt(ty + (body and FH_SGAP or FH_LGAP), hint, F_SMALL) end
  end
end

local function btnLabel(bx, by, bw, bh, label, fl, col)
  local tw = (lcd.sizeText and lcd.sizeText(label, fl)) or 0
  txt(bx + math.floor((bw - tw) / 2), by + math.floor((bh - FH_BODY) / 2) - sy(3), label, fl, col)
end

-- Save-operation modal: green = saving, red = error/timeout.
local function drawSaveModal()
  if not modal.active then return end
  if modal.state == "saving" and (getTime() - modal.sentTime) >= MODAL_TIMEOUT then
    if not modal.textSave then
      local item = menuItems[modal.itemIdx]
      if item then item[4] = modal.prevVal end
    end
    textEdit.active = false
    modal.state = "error"
  end
  if modal.state == "rebooting" and (getTime() - modal.sentTime) >= MODAL_TIMEOUT then
    modal.state = "reboot_error"
  end
  if modal.state == "text_confirm" then
    if not isColor then
      lcd.drawText(1, math.floor(H/2), "Save " .. modal.newName .. "?", SMLSIZE + INVERS)
      return
    end
    local mw = sx(380); local mh = sy(160)
    local mx = math.floor((W - mw) / 2)
    local my = math.floor((H - mh) / 2)
    local brd = math.max(1, sy(2))
    fill(mx - brd, my - brd, mw + 2*brd, mh + 2*brd, C.editBg)
    fill(mx, my, mw, mh, C.accent)
    local mcx = mx + math.floor(mw / 2)
    local title = "SAVE NAME?"
    local tw = (lcd.sizeText and lcd.sizeText(title, F_TITLE)) or 0
    local tx = tw > 0 and (mcx - math.floor(tw / 2)) or (mx + sx(20))
    txt(tx, my + sy(18), title, F_TITLE, C.white)
    -- Show proposed name
    local nw = (lcd.sizeText and lcd.sizeText(modal.newName, F_BODY)) or 0
    local nx = nw > 0 and (mcx - math.floor(nw / 2)) or (mx + sx(20))
    txt(nx, my + sy(52), modal.newName, F_BODY, C.white)
    -- Reboot warning for SSID
    if textEdit.reboot then
      local warn = "(will reboot)"
      local ww = (lcd.sizeText and lcd.sizeText(warn, F_SMALL)) or 0
      local wx = ww > 0 and (mcx - math.floor(ww / 2)) or (mx + sx(20))
      txt(wx, my + sy(74), warn, F_SMALL, C.white)
    end
    -- YES / NO buttons
    local btnW = sx(100); local btnH = sy(36)
    local btnY = my + mh - sy(58)
    local yesX = mcx - btnW - sx(10)
    local noX  = mcx + sx(10)
    if modal.confirmSel == 1 then
      fill(yesX, btnY, btnW, btnH, C.white)
      btnLabel(yesX, btnY, btnW, btnH, "YES", F_BODY, C.accent)
      fill(noX,  btnY, btnW, btnH, C.panel)
      btnLabel(noX,  btnY, btnW, btnH, "NO",  F_BODY, C.white)
    else
      fill(yesX, btnY, btnW, btnH, C.panel)
      btnLabel(yesX, btnY, btnW, btnH, "YES", F_BODY, C.white)
      fill(noX,  btnY, btnW, btnH, C.white)
      btnLabel(noX,  btnY, btnW, btnH, "NO",  F_BODY, C.accent)
    end
  elseif modal.state == "saving" then
    drawStdModal(C.green, "SAVING CONFIG", nil, true, modal.sentTime, nil)
  elseif modal.state == "rebooting" then
    drawStdModal(C.accent, "REBOOTING", "Waiting for board...", true, modal.sentTime, nil)
  elseif modal.state == "reboot_error" then
    drawStdModal(C.red, "REBOOT TIMEOUT", "Board did not reconnect", false, 0, "Press ENTER or EXIT")
  else
    drawStdModal(C.red, "SAVE ERROR", "No response from board", false, 0, "Press ENTER or EXIT")
  end
end

-- AP overlay removed: Lua and WebUI coexist
local function drawApOverlay() end

-- ── Dashboard page: System Information ──────────────────────────
local SYS_MODE_NAMES = {"FrSky", "SBUS", "S.PORT CC2540", "S.PORT Mirror", "LUA Serial"}
local BT_MODE_NAMES  = {[0]="Master (Central)", [1]="Slave (Peripheral)", [2]="Slave (Peripheral)"}
local SYS_INFO_COUNT = 7  -- number of system info rows
local function drawSystemInfo()
  if isColor then
    txt(L.pad, L.secTxtY, "System Information", F_BODY, C.accent)
    fill(L.pad, L.secLineY, L.cw, 1, C.panel)
    local rowW = L.cw - L.scrollColW
    local items = {}
    items[#items+1] = {"Build",        buildTs   ~= "" and buildTs   or "--"}
    items[#items+1] = {"Serial Mode",  SYS_MODE_NAMES[sysMode + 1] or ("Mode " .. sysMode)}
    items[#items+1] = {"Device Mode",  (menuItems[2][2][menuItems[2][4]]) or "--"}
    items[#items+1] = {"BT Mode",      BT_MODE_NAMES[deviceMode] or "--"}
    items[#items+1] = {"BT Name",      btName    ~= "" and btName    or "--"}
    items[#items+1] = {"Local Addr",   localAddr ~= "" and localAddr or "--"}
    items[#items+1] = {"Remote Addr",  remoteAddr ~= "" and remoteAddr or "--"}
    local total = #items
    local visRows = (deviceMode == 2) and L.sysFullRows or L.sysVisRows
    local maxScroll = math.max(0, total - visRows)
    if sysScroll > maxScroll then sysScroll = maxScroll end
    for slot = 1, visRows do
      local idx = slot + sysScroll
      if idx > total then break end
      local item = items[idx]
      local ry = L.menuY + (slot - 1) * L.menuRH
      if item[3] then fill(L.pad, ry, rowW, L.menuRH, item[3]) end
      txt(L.pad + sx(7), ry + L.menuTxtOff, item[1], F_BODY, C.white)
      txt(L.menuValX,    ry + L.menuTxtOff, item[2], F_BODY, item[3] and C.white or C.gray)
    end
    -- Scroll arrows: always draw both, blue when active, gray when not
    local triSz  = L.triSz
    local triX   = L.triX
    local half   = math.floor(triSz / 2)
    local midRow = math.floor(L.menuRH / 2)
    local canUp  = sysScroll > 0
    local canDn  = sysScroll < maxScroll
    local arrowRows = visRows
    if total <= visRows then arrowRows = math.max(1, total) end
    -- ▲ centred on first visible row
    drawTri(triX, L.menuY + midRow - half, triSz, true,  canUp and C.accent or C.gray)
    -- ▽ anchored to bottom of visible area
    local downY
    if deviceMode == 2 then
      downY = L.sysAreaBottom - triSz
    else
      downY = L.menuY + (arrowRows - 1) * L.menuRH + midRow - half
    end
    drawTri(triX, downY, triSz, false, canDn and C.accent or C.gray)
  else
    lcd.drawText(1, L.secTxtY, "System Information", SMLSIZE)
    lcd.drawLine(1, L.secLineY, W - 2, L.secLineY, DOTTED, 0)
    local bw = math.floor(W / 2)
    local items = {
      "Build: " .. (buildTs ~= "" and buildTs or "--"),
      "Mode: " .. (SYS_MODE_NAMES[sysMode + 1] or "?"),
      "Name: " .. (btName ~= "" and btName or "--"),
    }
    for i = 1, math.min(3, #items) do
      lcd.drawText(1, L.menuY + (i-1)*10, items[i], SMLSIZE)
    end
  end
end

-- ── Bluetooth page helpers ──────────────────────────────────────
-- Draw a section sub-header and separator line, return Y after the line.
-- Uses the same spacings as the main page header: L.secGapTL (title→line)
-- and L.secGapLC (line→content).
local function drawSubHeader(label, y)
  txt(L.pad, y, label, F_BODY, C.accent)
  fill(L.pad, y + FH_BODY + L.secGapTL, L.cw, 1, C.panel)
  return y + FH_BODY + L.secGapTL + 1 + L.secGapLC
end

-- ── Info modal (Reconnecting / Connecting / error) ───────────────
local function drawInfoModal()
  if not infoModal.active then return end
  if not isColor then
    lcd.drawText(1, math.floor(H/2), infoModal.title, SMLSIZE + INVERS + BLINK)
    return
  end
  local mw = sx(400); local mh = sy(130)
  local mx = math.floor((W - mw) / 2)
  local my = math.floor((H - mh) / 2)
  local brd = math.max(1, sy(2))
  fill(mx - brd, my - brd, mw + 2*brd, mh + 2*brd, C.editBg)
  fill(mx, my, mw, mh, C.accent)
  local mcx = mx + math.floor(mw / 2)
  local function c(y, s, fl)
    local tw = (lcd.sizeText and lcd.sizeText(s, fl)) or 0
    local tx = tw > 0 and (mcx - math.floor(tw / 2)) or (mx + sx(20))
    txt(tx, y, s, fl, C.white)
  end
  if infoModal.kind == "spin" then
    local frames = {".", "..", "..."}
    local di = (math.floor((getTime() - infoModal.startT) / 40) % 3) + 1
    local blockH = FH_TITLE + FH_LGAP + FH_TITLE
    local ty = my + math.floor((mh - blockH) / 2)
    c(ty, infoModal.title, F_TITLE)
    c(ty + FH_TITLE + FH_LGAP, frames[di], F_TITLE)
  else
    local blockH = FH_TITLE + (infoModal.body ~= "" and (FH_LGAP + FH_BODY) or 0)
    local ty = my + math.floor((mh - blockH) / 2)
    c(ty, infoModal.title, F_TITLE)
    if infoModal.body ~= "" then c(ty + FH_TITLE + FH_LGAP, infoModal.body, F_BODY) end
    -- Close hint
    c(my + mh - FH_SMALL - sy(8), "Press ENTER or EXIT", F_SMALL)
  end
end

-- ── Confirm modal (Forget) ────────────────────────────────────────
local function drawConfirmModal()
  if not confirm.active then return end
  if not isColor then
    lcd.drawText(1, math.floor(H/2), confirm.title, SMLSIZE + INVERS)
    return
  end
  local mw = sx(380); local mh = sy(160)
  local mx = math.floor((W - mw) / 2)
  local my = math.floor((H - mh) / 2)
  local brd = math.max(1, sy(2))
  fill(mx - brd, my - brd, mw + 2*brd, mh + 2*brd, C.editBg)
  fill(mx, my, mw, mh, C.accent)
  local mcx = mx + math.floor(mw / 2)
  local tw = (lcd.sizeText and lcd.sizeText(confirm.title, F_TITLE)) or 0
  local tx = tw > 0 and (mcx - math.floor(tw / 2)) or (mx + sx(20))
  txt(tx, my + sy(28), confirm.title, F_TITLE, C.white)

  -- YES / NO  –  selected = white bg + blue text; idle = transparent outline
  local btnW = sx(100); local btnH = sy(36)
  local btnY = my + mh - sy(58)
  local yesX = mcx - btnW - sx(10)
  local noX  = mcx + sx(10)

  if confirm.sel == 1 then
    fill(yesX, btnY, btnW, btnH, C.white)
    btnLabel(yesX, btnY, btnW, btnH, "YES", F_BODY, C.accent)
    fill(noX,  btnY, btnW, btnH, C.panel)
    btnLabel(noX,  btnY, btnW, btnH, "NO",  F_BODY, C.white)
  else
    fill(yesX, btnY, btnW, btnH, C.panel)
    btnLabel(yesX, btnY, btnW, btnH, "YES", F_BODY, C.white)
    fill(noX,  btnY, btnW, btnH, C.white)
    btnLabel(noX,  btnY, btnW, btnH, "NO",  F_BODY, C.accent)
  end
end

-- ── Main BT page draw ─────────────────────────────────────────────
local function drawBluetooth()
  if not isColor then
    lcd.drawText(1, L.secTxtY, "Bluetooth", SMLSIZE)
    return
  end

  local rowW = L.cw - L.scrollColW
  local curY = L.secTxtY

  -- ── Trainer OUT / Telemetry+BLE (Peripheral): connected remote device ─────
  local isBlePeripheral = (deviceMode == 1) or (deviceMode == 2 and tlmOutput == 1)
  if isBlePeripheral then
    if not bleConnected then bt.savedEditing = false end
    curY = drawSubHeader("Connected Device", curY)
    if bleConnected and remoteAddr ~= "" then
      local isEditing = bt.savedEditing
      fill(L.pad, curY, L.cw, L.menuHL, isEditing and C.editBg or C.accent)
      local ty = curY + L.menuTxtOff
      txt(L.pad + sx(7), ty, remoteAddr, F_BODY, C.white)
      if isEditing then
        txt(L.menuValX, ty, "Disconnect", F_BODY + BLINK, C.white)
      else
        txt(L.menuValX, ty, "Connected", F_BODY, C.green)
      end
    else
      txt(L.pad + sx(7), curY + L.menuTxtOff, "Waiting for connection\xe2\x80\xa6", F_BODY, C.gray)
    end
    return
  end

  -- ── Section 1: Saved Device ────────────────────────────────────
  curY = drawSubHeader("Saved Device", curY)

  if deviceMode ~= 0 then
    -- deviceMode==2 but tlmOutput~=1 (WiFi/Off): BLE page is irrelevant
    txt(L.pad + sx(7), curY + L.menuTxtOff, "Telemetry mode \xe2\x80\x93 change in Settings", F_BODY, C.gray)
  elseif remoteAddr == "" then
    txt(L.pad + sx(7), curY + L.menuTxtOff, "No saved device", F_BODY, C.gray)
  else
    -- Row 1: device name + live status or action toggle
    local rowFocused = (bt.pageFocus == 0)
    local isEditing  = bt.savedEditing
    if rowFocused or isEditing then
      fill(L.pad, curY, L.cw, L.menuHL, isEditing and C.editBg or C.accent)
    end
    local ty = curY + L.menuTxtOff
    txt(L.pad + sx(7), ty, remoteAddr, F_BODY, C.white)
    if isEditing then
      local primaryLabel = bleConnected and "Disconnect" or "Reconnect"
      local vLabel = bt.savedToggled and "Forget" or primaryLabel
      txt(L.menuValX, ty, vLabel, F_BODY + BLINK, C.white)
    else
      local status = bleConnected and "Connected" or "Disconnected"
      local sCol   = rowFocused and C.white or (bleConnected and C.green or C.red)
      txt(L.menuValX, ty, status, F_BODY, sCol)
    end
    curY = curY + L.menuRH
  end

  -- Gap between sections
  local findY = L.menuY + L.menuRH + sy(10)

  -- ── Section 2: Find Devices ────────────────────────────────────
  local postLine = drawSubHeader("Find Devices", findY)

  -- Scan state = 0 or 2 with no results: show Scan button row
  if scanState == 0 or (scanState == 2 and #scanResults == 0) then
    local isSel = (bt.pageFocus == 1) or (remoteAddr == "")
    if isSel then fill(L.pad, postLine, rowW, L.menuHL, C.accent) end
    local ty = postLine + L.menuTxtOff
    if scanState == 2 and #scanResults == 0 then
      txt(L.pad + sx(7), ty, "No devices found – Scan again", F_BODY, isSel and C.white or C.gray)
    else
      txt(L.pad + sx(7), ty, "Scan for Devices", F_BODY, isSel and C.white or C.gray)
    end

  elseif scanState == 1 then
    -- Searching animation
    local frames = {".", "..", "..."}
    local di = (math.floor(getTime() / 40) % 3) + 1
    txt(L.pad + sx(7), postLine + L.menuTxtOff, "Searching" .. frames[di], F_BODY, C.gray)

  elseif scanState == 2 and #scanResults > 0 then
    -- Results list with scroll
    local results = scanResults
    local total = 0
    for _ in pairs(results) do total = total + 1 end
    if bt.findSel < 1 then bt.findSel = 1 end
    if bt.findSel > total then bt.findSel = total end
    local maxScroll = math.max(0, total - L.btFindRows)
    bt.findScroll = math.min(bt.findScroll, maxScroll)
    if bt.findSel > bt.findScroll + L.btFindRows then bt.findScroll = bt.findSel - L.btFindRows end
    if bt.findSel <= bt.findScroll then bt.findScroll = bt.findSel - 1 end
    if bt.findScroll < 0 then bt.findScroll = 0 end

    for slot = 1, L.btFindRows do
      local idx = slot + bt.findScroll
      if idx > total then break end
      local dev = results[idx]
      if dev then
        local ry = postLine + (slot - 1) * L.menuRH
        local isSel = (idx == bt.findSel)
        if isSel then fill(L.pad, ry, rowW, L.menuHL, C.accent) end
        local ty = ry + L.menuTxtOff
        local nm  = (dev.name and dev.name ~= "") and dev.name or dev.addr
        txt(L.pad + sx(7), ty, nm, F_BODY, C.white)
        txt(L.menuValX, ty, tostring(dev.rssi) .. " dBm", F_BODY, isSel and C.white or C.gray)
      end
    end
  end

  -- Scroll arrows for Find section: always visible as placeholders
  do
    local total = 0
    for _ in pairs(scanResults) do total = total + 1 end
    local canUp = bt.findScroll > 0
    local canDn = bt.findScroll + L.btFindRows < total
    local half  = math.floor(L.triSz / 2)
    local midR  = math.floor(L.menuRH / 2)
    drawTri(L.triX, postLine + midR - half, L.triSz, true,  canUp and C.accent or C.gray)
    drawTri(L.triX, L.btAreaBottom - L.triSz, L.triSz, false, canDn and C.accent or C.gray)
  end
end

-- ── WiFi page ───────────────────────────────────────────────────
local function drawWifi()
  if isColor then
    local rowY = drawSubHeader("WiFi Info", L.secTxtY)
    local ssidVal = apSsid ~= "" and apSsid or "BTWifiSerial"
    txt(L.pad + sx(7), rowY,               "SSID",     F_BODY, C.white)
    txt(L.menuValX,    rowY,               ssidVal,    F_BODY, C.gray)
    txt(L.pad + sx(7), rowY + L.menuRH,   "IP",        F_BODY, C.white)
    txt(L.menuValX,    rowY + L.menuRH,   "192.168.4.1", F_BODY, C.gray)
    txt(L.pad + sx(7), rowY + 2*L.menuRH, "Password",  F_BODY, C.white)
    txt(L.menuValX,    rowY + 2*L.menuRH, apPass ~= "" and apPass or "12345678", F_BODY, C.gray)
    txt(L.pad + sx(7), rowY + 3*L.menuRH, "UDP Port",  F_BODY, C.white)
    txt(L.menuValX,    rowY + 3*L.menuRH, tostring(udpPort), F_BODY, C.gray)
  else
    local ssidVal = apSsid ~= "" and apSsid or "BTWifiSerial"
    lcd.drawText(2, L.settingsMenuY + 1,  "SSID: " .. ssidVal, SMLSIZE)
    lcd.drawText(2, L.settingsMenuY + 12, "IP: 192.168.4.1",    SMLSIZE)
    lcd.drawText(2, L.settingsMenuY + 22, "Port: " .. tostring(udpPort), SMLSIZE)
  end
end

-- ── Telemetry page ────────────────────────────────────────────────
local function drawTelemetry()
  -- Empty for now – content will be added later
end

-- ═══════════════════════════════════════════════════════════════════
-- EVENT HANDLING — returns true to exit script
-- ═══════════════════════════════════════════════════════════════════
local function handleEvent(event)
  if event == nil or event == 0 then return false end
  -- Block all input in non-interactive states
  if appState == APP.INIT then return false end

  -- Page switch works in any interactive state, regardless of modal
  local pageKey, pages = getCurrentPageKey()
  local np = #pages
  if evPageNext(event) then
    currentPage = (currentPage % np) + 1
    menu.editing = false; confirm.active = false
    bt.pageFocus = 0; bt.savedEditing = false; bt.savedToggled = false
    return false
  elseif evPagePrev(event) then
    currentPage = (currentPage + np - 2) % np + 1
    menu.editing = false; confirm.active = false
    bt.pageFocus = 0; bt.savedEditing = false; bt.savedToggled = false
    return false
  end

  -- Info modal (Connecting / Reconnecting / error) — dismiss error with ENTER/EXIT
  if infoModal.active then
    if infoModal.kind ~= "spin" and (evEnter(event) or evExit(event)) then
      infoModal.active = false
    end
    return false
  end

  -- Confirm modal (Forget)
  if confirm.active then
    if evEnter(event) then
      if confirm.sel == 1 then
        sendCmd(confirm.cmd)
        -- Show spinner feedback for forget
        infoModal.active = true; infoModal.kind = "spin"
        infoModal.title  = "Forgetting"; infoModal.body = ""
        infoModal.startT = getTime()
      end
      confirm.active = false
    elseif evExit(event) then
      confirm.active = false
    elseif evNext(event) or evPrev(event) then
      confirm.sel = (confirm.sel % 2) + 1
    end
    return false
  end

  -- Settings save modal
  if modal.active then
    if modal.state == "text_confirm" then
      if evEnter(event) then
        if modal.confirmSel == 1 then
          -- Yes: send the string
          sendString(textEdit.subCmd, modal.newName)
          modal.state    = "saving"
          modal.sentTime = getTime()
          modal.textSave = true
        else
          -- No: revert
          textEdit.active = false
          modal.active = false
        end
      elseif evExit(event) then
        textEdit.active = false
        modal.active = false
      elseif evNext(event) or evPrev(event) then
        modal.confirmSel = (modal.confirmSel % 2) + 1
      end
    elseif (modal.state == "error" or modal.state == "reboot_error") and (evEnter(event) or evExit(event)) then
      modal.active = false
    end
    return false
  end

  -- ── Bluetooth page ─────────────────────────────────────────────
  if pageKey == "bluetooth" then

    -- ── Trainer OUT / Telemetry+BLE (Peripheral): disconnect connected device ─
    local isBlePeripheral = (deviceMode == 1) or (deviceMode == 2 and tlmOutput == 1)
    if isBlePeripheral then
      bt.pageFocus = 0
      if bt.savedEditing then
        if evEnter(event) then
          sendCmd(CMD_BLE_DISCONNECT)
          infoModal.active = true; infoModal.kind = "spin"
          infoModal.title  = "Disconnecting"; infoModal.body = ""
          infoModal.startT = getTime()
          bt.savedEditing  = false
        elseif evExit(event) then
          bt.savedEditing = false
        end
        return false
      end
      if evEnter(event) then
        if bleConnected then bt.savedEditing = true end
      elseif evExit(event) then
        return true
      end
      return false
    end

    -- If no saved device, always keep focus on Find section
    if remoteAddr == "" then bt.pageFocus = 1 end

    -- ── Edit mode: wheel/ENTER/EXIT are scoped to the action toggle ──
    if bt.savedEditing then
      if evEnter(event) then
        if bt.savedToggled then
          confirm.active = true; confirm.title = "FORGET DEVICE?"
          confirm.cmd    = CMD_BLE_FORGET; confirm.sel = 2
        else
          if bleConnected then
            sendCmd(CMD_BLE_DISCONNECT)
            infoModal.active = true; infoModal.kind = "spin"
            infoModal.title  = "Disconnecting"; infoModal.body = ""
            infoModal.startT = getTime()
          else
            sendCmd(CMD_BLE_RECONNECT)
            infoModal.active = true; infoModal.kind = "spin"
            infoModal.title  = "Reconnecting"; infoModal.body = ""
            infoModal.startT = getTime()
          end
        end
        bt.savedEditing = false; bt.savedToggled = false
      elseif evExit(event) then
        bt.savedEditing = false; bt.savedToggled = false
      elseif evNext(event) or evPrev(event) then
        bt.savedToggled = not bt.savedToggled
      end
      return false
    end

    -- ── Normal pageFocus navigation ───────────────────────────────
    if evEnter(event) then
      if bt.pageFocus == 0 then
        -- Enter edit mode on saved device row
        bt.savedEditing = true; bt.savedToggled = false
      else
        -- Find section: scan or connect
        if scanState == 0 or (scanState == 2 and #scanResults == 0) then
          sendCmd(CMD_BLE_SCAN); scanState = 1; scanStartT = getTime(); scanResults = {}
          bt.findSel = 1; bt.findScroll = 0
        elseif scanState == 2 and #scanResults > 0 then
          local dev = scanResults[bt.findSel]
          if dev then
            sendCmd(CMD_BLE_CONNECT_BASE + (bt.findSel - 1))
            infoModal.active = true; infoModal.kind = "spin"
            infoModal.title  = "Connecting"; infoModal.body = ""
            infoModal.startT = getTime()
          end
        end
      end
    elseif evExit(event) then
      if bt.pageFocus == 1 and scanState == 2 and #scanResults > 0 then
        -- Clear results → stay in Find section showing Scan button
        scanState = 0; scanResults = {}
        bt.findSel = 1; bt.findScroll = 0
      elseif bt.pageFocus == 1 and remoteAddr ~= "" then
        -- Move focus back to Saved Device section
        bt.pageFocus = 0
      else
        return true  -- exit script
      end
    elseif evNext(event) then
      if bt.pageFocus == 0 then
        bt.pageFocus = 1  -- move focus down to Find section
      elseif scanState == 2 and #scanResults > 0 then
        local total = 0; for _ in pairs(scanResults) do total = total + 1 end
        bt.findSel = math.min(bt.findSel + 1, total)
        if bt.findSel > bt.findScroll + L.btFindRows then bt.findScroll = bt.findSel - L.btFindRows end
      end
    elseif evPrev(event) then
      if bt.pageFocus == 1 then
        if scanState == 2 and #scanResults > 0 and bt.findSel > 1 then
          bt.findSel = bt.findSel - 1
          if bt.findSel <= bt.findScroll then bt.findScroll = bt.findSel - 1 end
          if bt.findScroll < 0 then bt.findScroll = 0 end
        elseif remoteAddr ~= "" then
          bt.pageFocus = 0  -- move focus up to Saved Device section
        end
      end
    end
    return false

  -- ── Settings page ──────────────────────────────────────────────
  elseif pageKey == "settings" then
    local item = menuItems[menu.sel]
    local isText = item and type(item[2]) == "string"

    -- ── Text editing active ──────────────────────────────────────
    if textEdit.active then
      if evEnter(event) then
        local ci = textEdit.chars[textEdit.cursor]
        if ci and ci >= 1 and ci <= #textEdit.charset then
          -- Lock this character, advance cursor
          textEdit.cursor = textEdit.cursor + 1
          if textEdit.cursor > textEdit.maxLen then
            -- Reached max length, finish
            local name = charsToText(textEdit.chars)
            modal.active = true; modal.state = "text_confirm"
            modal.newName = name; modal.confirmSel = 1
          elseif textEdit.cursor > #textEdit.chars then
            -- Past current length: add end marker
            textEdit.chars[textEdit.cursor] = #textEdit.charset + 1
          end
        else
          -- End marker: finish editing
          if textEdit.cursor <= 1 then
            -- Min length 1: don't allow empty
          else
            local chars = {}
            for i = 1, textEdit.cursor - 1 do chars[i] = textEdit.chars[i] end
            local name = charsToText(chars)
            modal.active = true; modal.state = "text_confirm"
            modal.newName = name; modal.confirmSel = 1
          end
        end
      elseif evExit(event) then
        textEdit.active = false
      elseif evNext(event) then
        local ci = textEdit.chars[textEdit.cursor] or 1
        ci = ci + 1
        if ci > #textEdit.charset + 1 then ci = 1 end
        textEdit.chars[textEdit.cursor] = ci
      elseif evPrev(event) then
        local ci = textEdit.chars[textEdit.cursor] or 1
        ci = ci - 1
        if ci < 1 then ci = #textEdit.charset + 1 end
        textEdit.chars[textEdit.cursor] = ci
      end

    -- ── Normal menu navigation ───────────────────────────────────
    elseif evEnter(event) then
      if isText then
        -- Start text editing
        textEdit.active  = true
        textEdit.itemIdx = menu.sel
        textEdit.subCmd  = item[3]
        textEdit.saved   = getTextValue(item[3])
        textEdit.reboot  = (item[3] == STR_SSID or item[3] == STR_UDP_PORT or item[3] == STR_AP_PASS)
        textEdit.maxLen  = item[4] or 15
        textEdit.charset = (item[3] == STR_UDP_PORT) and NUMSET or CHARSET
        textEdit.chars   = {}
        local val = textEdit.saved
        for i = 1, #val do
          textEdit.chars[i] = charsetIndex(string.sub(val, i, i))
        end
        textEdit.cursor = 1
        if #textEdit.chars == 0 then
          textEdit.chars[1] = 1  -- default first char in charset
        end
      elseif menu.editing then
        if item[4] ~= item[5] then
          sendCmd(item[3][item[4]])
          modal.active   = true
          modal.state    = "saving"
          modal.sentTime = getTime()
          modal.itemIdx  = menu.sel
          modal.prevVal  = item[5]
          modal.textSave = false
        end
        menu.editing = false
      else
        menu.editing = true
      end
    elseif evExit(event) then
      if menu.editing then
        menu.editing = false
      else
        return true
      end
    elseif evNext(event) then
      if menu.editing then
        item[4] = item[4] + 1
        if item[4] > #item[2] then item[4] = 1 end
      else
        menu.sel = menu.sel + 1
        if menu.sel > #menuItems then menu.sel = 1 end
        if menu.sel > menu.offset + L.menuVisRows then
          menu.offset = menu.sel - L.menuVisRows
        end
      end
    elseif evPrev(event) then
      if menu.editing then
        item[4] = item[4] - 1
        if item[4] < 1 then item[4] = #item[2] end
      else
        menu.sel = menu.sel - 1
        if menu.sel < 1 then menu.sel = #menuItems end
        if menu.sel <= menu.offset then menu.offset = menu.sel - 1 end
      end
    end

  -- ── Dashboard page ─────────────────────────────────────────────
  elseif pageKey == "dashboard" then
    if evNext(event) then
      local visRows = (deviceMode == 2) and L.sysFullRows or L.sysVisRows
      local maxScroll = math.max(0, SYS_INFO_COUNT - visRows)
      sysScroll = math.min(sysScroll + 1, maxScroll)
    elseif evPrev(event) then
      sysScroll = math.max(sysScroll - 1, 0)
    elseif evExit(event) then
      return true
    end
  elseif pageKey == "wifi" or pageKey == "telemetry" then
    if evExit(event) then return true end
  end

  return false
end

-- ═══════════════════════════════════════════════════════════════════
-- ENTRY POINTS
-- ═══════════════════════════════════════════════════════════════════
local function init()
  computeLayout()
  appInitTime = getTime()
  scanState = 0; scanStartT = 0; scanResults = {}
end

local function background()
  readSerial()
end

local function run(event, touchState)
  readSerial()

  -- Scan timeout: if stuck scanning for >8s, auto-reset
  if scanState == 1 and scanStartT > 0 and (getTime() - scanStartT) > 800 then
    scanState = 2; scanStartT = 0
    infoModal.active = true; infoModal.kind = "err"
    infoModal.title  = "Scan Timed Out"; infoModal.body = "Press ENTER or EXIT"
  end

  -- Safety-net timeout for all spinner modals (10 s)
  if infoModal.active and infoModal.kind == "spin" and infoModal.startT > 0 then
    if (getTime() - infoModal.startT) > 1000 then
      infoModal.kind  = "err"
      infoModal.title = "Timed Out"
      infoModal.body  = "Press ENTER or EXIT"
    end
  end

  -- Dismiss spinner if board goes offline (can't complete the action)
  if infoModal.active and infoModal.kind == "spin" and not boardConnected then
    infoModal.active = false
  end

  if isColor then sc(C.bg); lcd.clear(CUSTOM_COLOR)
  else lcd.clear() end

  -- Topbar always draws (shows connection status in all states)
  drawTopBar()

  -- INIT: waiting for first contact → show blue loading modal, skip rest
  if appState == APP.INIT then
    drawStdModal(C.accent, "Getting Data", nil, true, appInitTime, nil)
    -- Still signal active during init so Function yields while we cold-boot
    if setShmVar then setShmVar(SHM_TOOLS_HB, getTime()) end
    return 0
  end

  -- DISCONNECTED before any config received → Board not Found modal
  if appState == APP.DISCONNECTED and not gotConfig then
    drawStdModal(C.red, "Board not Found",
      "Check configuration and connection", false, 0, "Press EXIT to close")
    if event and evExit(event) then
      if setShmVar then setShmVar(SHM_TOOLS_HB, 0) end
      return 1
    end
    -- Keep requesting data in case board comes online
    local now = getTime()
    if now - lastReqTime >= 50 then
      lastReqTime = now
      sendCmd(CMD_REQ_INFO)
    end
    if setShmVar then setShmVar(SHM_TOOLS_HB, getTime()) end
    return 0
  end

  if handleEvent(event) then
    -- Exiting: clear heartbeat BEFORE return so Function resumes immediately
    if setShmVar then setShmVar(SHM_TOOLS_HB, 0) end
    return 1
  end

  -- Set heartbeat AFTER confirming we are not exiting this frame
  if setShmVar then setShmVar(SHM_TOOLS_HB, getTime()) end

  local pageKey = getCurrentPageKey()
  local pageTitles = {
    dashboard = "Dashboard",
    bluetooth = "Bluetooth",
    wifi      = "WiFi",
    settings  = "Settings",
    telemetry = "Telemetry",
  }
  local pageTitle = pageTitles[pageKey] or "Dashboard"
  drawPageHeader(pageTitle)
  if pageKey == "dashboard" then
    drawSystemInfo()
    drawChannels()
  elseif pageKey == "bluetooth" then
    drawBluetooth()
  elseif pageKey == "wifi" then
    drawWifi()
  elseif pageKey == "settings" then
    drawMenu()
  elseif pageKey == "telemetry" then
    drawTelemetry()
  end
  drawSaveModal()
  drawConfirmModal()
  drawInfoModal()
  drawApOverlay()

  return 0
end

return { init = init, run = run, background = background }
