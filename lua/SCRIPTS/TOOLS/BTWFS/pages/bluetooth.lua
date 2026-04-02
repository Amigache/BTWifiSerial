-- pages/bluetooth.lua
-- Bluetooth management page.
--
-- Section 1 — Saved Device
--   Shows the saved remote BT address (store.info[INFO_REM_ADDR]).
--   Edit mode on that row cycles Disconnect / Reconnect → Forget (toggle).
--
-- Section 2 — Find Devices
--   BLE scan: idle → scan button,  scanning → animation,
--             complete + results  → scrollable list → ENTER connects.

return function(ctx)
  local Page    = ctx.Page
  local Section = ctx.Section
  local List    = ctx.List
  local Modal   = ctx.Modal
  local scale   = ctx.scale
  local theme   = ctx.theme
  local proto   = ctx.proto
  local store   = ctx.store
  local input   = ctx.input
  local sendFrame = ctx.sendFrame

  -- ── Event helpers ─────────────────────────────────────────────────
  local evEnter = input.evEnter
  local evExit  = input.evExit
  local evNext  = input.evNext
  local evPrev  = input.evPrev

  -- ── Async operation state ────────────────────────────────────────
  local OP_NONE          = "none"
  local OP_SCANNING      = "scanning"
  local OP_CONNECTING    = "connecting"
  local OP_CONNECTED_INFO = "connected_info"
  local OP_DISCONNECTING = "disconnecting"
  local OP_RECONNECTING  = "reconnecting"
  local OP_FORGETTING    = "forgetting"

  -- ── Layout constants ──────────────────────────────────────────────
  local PAD    = scale.sx(17)
  local rowH   = scale.sy(32)
  local rowW   = scale.W - 2 * PAD
  -- Text vertical offset inside a row (same formula as List component)
  local txtOff = math.floor((rowH - theme.FH.small) / 2) - scale.sy(3)

  -- Cache theme values
  local isColor    = theme.isColor
  local C_text     = theme.C.text
  local C_subtext  = theme.C.subtext
  local C_accent   = theme.C.accent
  local C_editBg   = theme.C.editBg
  local C_green    = theme.C.green
  local C_red      = theme.C.red
  local F_SMALL_CC = theme.F.small + CUSTOM_COLOR

  -- Pre-compute layout constants for drawRow
  local LABEL_X    = PAD + scale.sx(7)
  local VAL_X      = PAD + math.floor(rowW * 0.54)
  local BW_VAL_X   = math.floor(scale.W / 2)

  -- ── Helper: read saved remote address from info store ─────────────
  local function remoteAddr()
    local inf = store.info[proto.INFO_REM_ADDR]
    if inf and inf.value and inf.value ~= "" and inf.value ~= "(none)" then
      return inf.value
    end
    return nil
  end

  -- ── Helper: read local BT address from info store ─────────────────
  local function localAddr()
    local inf = store.info[proto.INFO_BT_ADDR]
    if inf and inf.value and inf.value ~= "" and inf.value ~= "?" then
      return inf.value
    end
    return nil
  end

  -- ── Helper: build scan-results rows for the List component ────────
  local function buildScanRows()
    local rows = {}
    for i = 1, #store.scanResults do
      local dev = store.scanResults[i]
      if dev then
        local nm = (dev.name and dev.name ~= "") and dev.name or dev.addr
        rows[#rows + 1] = {
          label = nm,
          rssi  = tostring(dev.rssi) .. " dB",
          _idx  = dev.idx,  -- 0-based, used for BLE_CONNECT command
        }
      end
    end
    return rows
  end

  -- ── Draw helper: filled row background + label + value ─────────────
  local function drawRow(ry, rw, isFocused, isEditing, labelStr, valueStr, valueColor)
    if isColor then
      if isEditing then
        lcd.setColor(CUSTOM_COLOR, C_editBg)
        lcd.drawFilledRectangle(PAD, ry, rw, rowH, CUSTOM_COLOR)
      elseif isFocused then
        lcd.setColor(CUSTOM_COLOR, C_accent)
        lcd.drawFilledRectangle(PAD, ry, rw, rowH, CUSTOM_COLOR)
      end
      local ty = ry + txtOff
      lcd.setColor(CUSTOM_COLOR, C_text)
      lcd.drawText(LABEL_X, ty, labelStr, F_SMALL_CC)
      local valFont = F_SMALL_CC
      if isEditing then valFont = valFont + BLINK end
      lcd.setColor(CUSTOM_COLOR, isFocused and C_text or (valueColor or C_subtext))
      lcd.drawText(VAL_X, ty, valueStr, valFont)
    else
      local fl = isFocused and INVERS or 0
      lcd.drawText(PAD, ry + 1, labelStr, SMLSIZE + fl)
      lcd.drawText(BW_VAL_X, ry + 1, valueStr, SMLSIZE + fl)
    end
  end

  local Bluetooth = {}
  Bluetooth.__index = Bluetooth

  function Bluetooth.new()
    local self = setmetatable({}, Bluetooth)

    -- ── Page shell ────────────────────────────────────────────────
    self._page = Page.new({
      hasHeader    = true,
      title        = "BTWifiSerial",
      hasPageTitle = true,
      pageTitle    = "Bluetooth",
      hasFooter    = true,
      indicators   = ctx.indicators,
    })

    local contentY = self._page.contentY
    local contentH = self._page.contentH
    local gap      = scale.sy(8)

    -- ── Section 1: Saved Device ───────────────────────────────────
    local savedSecY       = contentY + gap
    self._savedSection    = Section.new({ title = "Saved Device", y = savedSecY })
    self._savedRowY       = self._savedSection.contentY
    self._localAddrRowY   = self._savedRowY + rowH   -- read-only local BT addr row

    -- ── Section 2: Find Devices ───────────────────────────────────
    local findSecY        = self._localAddrRowY + rowH + gap
    self._findSection     = Section.new({ title = "Find Devices", y = findSecY, marginTop = scale.sy(8) })
    local findListY       = self._findSection.contentY
    local findListH       = (contentY + contentH) - findListY

    -- Scan results list (only visible when scanState == 2 and #results > 0)
    self._findList = List.new({
      y          = findListY,
      h          = findListH,
      selectable = true,
      showScroll = true,
      cols       = {
        { key = "label" },
        { key = "rssi", xFrac = 0.65 },
      },
      rows = {},
    })

    -- ── Section components as page children (draw headers) ────────
    self._page:addChild(self._savedSection)
    self._page:addChild(self._findSection)

    -- ── Page-level state ─────────────────────────────────────────
    self._focus         = remoteAddr() and 0 or 1
    self._savedEdit     = false  -- true while edit mode on saved device row
    self._savedToggled  = false  -- true = "Forget" option selected in edit mode
    self._modal         = nil    -- active feedback modal (spinner / error / confirm)
    self._opState       = OP_NONE
    self._opTick        = 0      -- getTime() when async BLE op started (0 = no op)
    self._scanSentTick  = 0      -- getTime() when last scan was requested

    -- ── Store events ──────────────────────────────────────────────
    store.on("scan_status", function(s)
      if s.state == 1 then
        -- Firmware confirmed scan started: clear old results.
        -- Modal is already showing from ENTER press, so only create it if missing.
        self._findList:setRows({})
        if self._opState ~= OP_SCANNING then
          self._modal = Modal.new({
            type     = "info",
            severity = "info",
            title    = "Scanning...",
            message  = "Looking for BLE devices...",
          })
          self._modal:show()
          self._opState = OP_SCANNING
        end
      elseif s.state == 0 then
        -- Scan rejected or idle: close scanning modal (show error if we just requested it)
        self._findList:setRows({})
        if self._opState == OP_SCANNING then
          self._modal = Modal.new({
            type     = "alert",
            severity = "error",
            title    = "Scan Failed",
            message  = "Could not start BLE scan.",
          })
          self._modal:show()
          self._opState = OP_NONE
          self._scanSentTick = 0
        end
      elseif s.state == 2 then
        -- Scan complete: close spinner.
        -- Use s.count (from the frame) rather than #store.scanResults,
        -- because scan-entry frames haven't arrived yet at this point.
        self._scanSentTick = 0
        if self._opState == OP_SCANNING then
          self._modal = nil
          self._opState = OP_NONE
        end
        if s.count == 0 then
          self._modal = Modal.new({
            type     = "alert",
            severity = "warning",
            title    = "No Devices Found",
            message  = "No BLE devices in range.",
          })
          self._modal:show()
        end
      end
    end)

    store.on("scan_entry", function()
      self._findList:setRows(buildScanRows())
    end)

    -- When remote addr info changes (connect / forget), update focus if needed
    local function onRemoteAddrUpdate()
      if not remoteAddr() then
        -- Device was forgotten: exit edit mode, focus Find section
        self._savedEdit    = false
        self._savedToggled = false
        self._focus        = 1
      else
        -- Device info loaded: focus Saved Device section, clean up scan
        self._focus = 0
        store.scanResults = {}; store.scanState = 0
        self._findList:setRows({})
      end
      -- Close any pending BLE operation modal
      if self._opState == OP_CONNECTED_INFO or self._opState == OP_FORGETTING or self._opState == OP_CONNECTING then
        self._modal = nil
        self._opTick = 0
        self._opState = OP_NONE
      end
    end

    store.on("info_changed", function(inf)
      if inf.id == proto.INFO_REM_ADDR then onRemoteAddrUpdate() end
      -- INFO_BT_ADDR: no layout change needed, render reads localAddr() live
    end)

    -- Initial info dump uses addInfo (no info_changed), so also listen for info_ready
    store.on("info_ready", function()
      onRemoteAddrUpdate()
    end)

    -- Auto-close BLE spinners when status changes
    store.on("status", function(s)
      if self._opState == OP_CONNECTING then
        if s.bleConnected then
          self._savedEdit = false; self._savedToggled = false
          store.scanResults = {}; store.scanState = 0
          self._findList:setRows({})
          self._modal = Modal.new({
            type     = "info",
            severity = "info",
            title    = "Connected",
            message  = "Loading device info...",
          })
          self._modal:show()
          self._opState = OP_CONNECTED_INFO
          self._opTick = getTime()
        elseif not s.bleConnecting and self._opTick > 0
               and (getTime() - self._opTick) > 300 then
          -- Grace period (3 s) before declaring failure, to allow FW
          -- to start the connection task after receiving the command.
          self._opTick = 0
          self._modal = Modal.new({
            type     = "alert",
            severity = "error",
            title    = "Connection Failed",
            message  = "Could not connect to device.",
          })
          self._modal:show()
          self._opState = OP_NONE
        end
      elseif self._opState == OP_DISCONNECTING then
        if not s.bleConnected then
          self._modal  = nil
          self._opTick = 0
          self._opState = OP_NONE
          self._savedEdit = false; self._savedToggled = false
        end
      elseif self._opState == OP_RECONNECTING then
        if s.bleConnected then
          self._modal  = nil
          self._opTick = 0
          self._opState = OP_NONE
          self._savedEdit = false; self._savedToggled = false
        end
      end
    end)

    return self
  end

  -- ── Public API ────────────────────────────────────────────────────

  function Bluetooth:setPagination(n, total)
    self._page:setPagination(n, total)
  end

  function Bluetooth:handleEvent(event)
    -- ELRS mode: no BLE operations available
    local devPref = store.prefs[0x02]
    if devPref and devPref.curIdx == 3 then
      if self._modal then
        self._modal:handleEvent(event)
        return true
      end
      return false
    end

    -- Modal takes priority
    if self._modal then
      self._modal:handleEvent(event)
      return true
    end

    local addr = remoteAddr()
    -- If no saved device, always keep focus on Find section
    if not addr then self._focus = 1 end

    -- ── Edit mode on Saved Device row ────────────────────────────
    if self._savedEdit then
      if evEnter(event) then
        if self._savedToggled then
          -- Forget: ask for confirmation first
          local savedSelf = self
          self._modal = Modal.new({
            type     = "confirm",
            severity = "error",
            title    = "Forget Device?",
            message  = "Remove saved pairing?",
            onResult = function(accepted)
              if accepted then
                savedSelf._modal = Modal.new({
                  type     = "info",
                  severity = "warning",
                  title    = "Forgetting...",
                  message  = "Clearing saved device...",
                })
                savedSelf._modal:show()
                savedSelf._opState = OP_FORGETTING
                sendFrame(proto.buildInfoBleForget())
              end
            end,
          })
          self._modal:show()
        else
          -- Primary action: Disconnect or Reconnect
          if store.status.bleConnected then
            self._modal = Modal.new({
              type     = "info",
              severity = "warning",
              title    = "Disconnecting...",
              message  = "Closing BLE connection...",
            })
            self._modal:show()
            self._opState = OP_DISCONNECTING
            self._opTick = getTime()
            sendFrame(proto.buildInfoBleDisconnect())
          else
            self._modal = Modal.new({
              type     = "info",
              severity = "info",
              title    = "Reconnecting...",
              message  = "Connecting to saved device...",
            })
            self._modal:show()
            self._opState = OP_RECONNECTING
            self._opTick = getTime()
            sendFrame(proto.buildInfoBleReconnect())
          end
        end
        self._savedEdit = false; self._savedToggled = false
        return true
      elseif evExit(event) then
        self._savedEdit = false; self._savedToggled = false
        return true
      elseif evNext(event) or evPrev(event) then
        self._savedToggled = not self._savedToggled
        return true
      end
      return true
    end

    -- ── Saved Device section (focus = 0) ─────────────────────────
    if self._focus == 0 then
      if evEnter(event) then
        if addr then self._savedEdit = true end
        return true
      elseif evNext(event) then
        self._focus = 1
        return true
      end
      return false
    end

    -- ── Find Devices section (focus = 1) ─────────────────────────
    if store.scanState == 2 and #store.scanResults > 0 then
      -- Results list is active ────────────────────────────────────
      if evPrev(event) then
        if self._findList._sel == 1 and self._findList._offset == 0 and addr then
          -- At very top: jump up to Saved Device section
          self._focus = 0
        else
          self._findList:handleEvent(event)
        end
        return true
      end
      if evNext(event) then
        self._findList:handleEvent(event)
        return true
      end
      if evEnter(event) then
        local row = self._findList:getSel()
        if row then
          sendFrame(proto.buildInfoBleConnect(row._idx))
          self._modal = Modal.new({
            type     = "info",
            severity = "info",
            title    = "Connecting...",
            message  = "Opening BLE connection...",
          })
          self._modal:show()
          self._opState = OP_CONNECTING
          self._opTick = getTime()
        end
        return true
      end
      if evExit(event) then
        store.scanResults = {}; store.scanState = 0
        self._findList:setRows({})
        return true
      end
    else
      -- Idle / scanning (modal covers input while scanning) ────────
      if evPrev(event) and addr then
        self._focus = 0
        return true
      end
      if evEnter(event) then
        sendFrame(proto.buildInfoBleScan())
        self._scanSentTick = getTime()
        -- Show modal immediately so user gets feedback.
        -- Will remain open until scan_status=2 (or state=0 after timeout).
        self._modal = Modal.new({
          type     = "info",
          severity = "info",
          title    = "Scanning...",
          message  = "Looking for BLE devices...",
        })
        self._modal:show()
        self._opState = OP_SCANNING
        return true
      end
    end

    return false
  end

  function Bluetooth:render()
    -- Render Page shell (background + header + page title + footer + section headers)
    self._page:render()

    -- ── ELRS mode: Bluetooth not available ───────────────────────
    local devPref = store.prefs[0x02]
    if devPref and devPref.curIdx == 3 then
      local msgY = self._page.contentY + scale.sy(30)
      if isColor then
        lcd.setColor(CUSTOM_COLOR, C_subtext)
        lcd.drawText(LABEL_X, msgY, "Bluetooth not available", F_SMALL_CC)
        lcd.drawText(LABEL_X, msgY + theme.FH.small + scale.sy(4),
                     "ELRS mode uses WiFi radio", F_SMALL_CC)
      else
        lcd.drawText(PAD, msgY, "BT N/A (ELRS mode)", SMLSIZE)
      end
      if self._modal then
        self._modal:render()
        if not self._modal:isOpen() then self._modal = nil end
      end
      return
    end

    -- ── Saved Device row ─────────────────────────────────────────
    local addr    = remoteAddr()
    local ry      = self._savedRowY
    local focused = (self._focus == 0)

    if not addr then
      if isColor then
        lcd.setColor(CUSTOM_COLOR, C_subtext)
        lcd.drawText(LABEL_X, ry + txtOff, "No saved device", F_SMALL_CC)
      else
        lcd.drawText(PAD, ry + 1, "No saved device", SMLSIZE)
      end
    else
      local isEditing = self._savedEdit
      -- Value text
      local valStr, valColor
      if isEditing then
        local primaryLabel = store.status.bleConnected and "Disconnect" or "Reconnect"
        valStr   = self._savedToggled and "Forget" or primaryLabel
        valColor = C_text
      else
        local isConn  = store.status.bleConnected
        valStr   = isConn and "Connected" or "Disconnected"
        valColor = isConn and C_green or C_red
      end
      drawRow(ry, rowW, focused and not isEditing, isEditing, addr, valStr, valColor)
    end

    -- ── Local BT Address row (read-only, always rendered) ────────
    local la = localAddr() or "--"
    drawRow(self._localAddrRowY, rowW, false, false, "Local Address", la, nil)

    -- ── Find Devices content ──────────────────────────────────────
    local findFocused = (self._focus == 1)
    local findRowY    = self._findSection.contentY

    if store.scanState == 2 and #store.scanResults > 0 then
      -- Results: delegate rendering to the List component
      self._findList:render()
    else
      -- Idle, scanning, or no results: always show the scan button row
      drawRow(findRowY, rowW, findFocused, false, "Scan for Devices", "", C_subtext)
    end

    -- ── Scan timeout (30 s) ───────────────────────────────────────
    if self._scanSentTick > 0 and self._opState == OP_SCANNING then
      if (getTime() - self._scanSentTick) > 3000 then
        self._scanSentTick = 0
        self._modal = Modal.new({
          type     = "alert",
          severity = "warning",
          title    = "Scan Timeout",
          message  = "No response from device.\nTry again.",
        })
        self._modal:show()
        self._opState = OP_NONE
      end
    end

    -- ── Operation timeout ─────────────────────────────────────────
    if self._opTick > 0 and self._modal then
      local timedOp = self._opState == OP_CONNECTING
                   or self._opState == OP_CONNECTED_INFO
                   or self._opState == OP_RECONNECTING
                   or self._opState == OP_DISCONNECTING
                   or self._opState == OP_FORGETTING
      -- Connect/reconnect can legitimately take longer because firmware may
      -- retry with alternate BLE address type on first attempt.
      local timeoutTicks = 1500
      if self._opState == OP_CONNECTING or self._opState == OP_RECONNECTING then
        timeoutTicks = 2200
      end
      if timedOp and (getTime() - self._opTick) > timeoutTicks then
        self._opTick = 0
        self._opState = OP_NONE
        self._modal = Modal.new({
          type     = "alert",
          severity = "error",
          title    = "Timeout",
          message  = "Operation timed out.",
        })
        self._modal:show()
      end
    end

    -- ── Modal overlay ─────────────────────────────────────────────
    if self._modal then
      self._modal:render()
      if not self._modal:isOpen() then self._modal = nil end
    end
  end

  return Bluetooth
end
