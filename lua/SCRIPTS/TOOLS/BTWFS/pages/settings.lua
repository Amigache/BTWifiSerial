-- pages/settings.lua
-- Settings page — fully data-driven from store.prefs.
-- Rows are built when PREF_END arrives; updated on PREF_UPDATE.
-- Edit confirm sends PREF_SET via ctx.sendFrame.

return function(ctx)
  local Page      = ctx.Page
  local List      = ctx.List
  local Loading   = ctx.Loading
  local Modal     = ctx.Modal
  local PickModal = ctx.PickModal
  local scale     = ctx.scale
  local theme     = ctx.theme
  local proto     = ctx.proto
  local store     = ctx.store
  local input     = ctx.input
  local sendFrame = ctx.sendFrame

  -- Cache theme values
  local isColor    = theme.isColor
  local C_text     = theme.C.text
  local F_small    = theme.F.small
  local F_SMALL_CC = F_small + CUSTOM_COLOR

  -- Pre-compute layout constants
  local WAITING_X = scale.sx(17)
  local WAITING_Y_OFF = scale.sy(8)

  -- ── Charsets for text editing ─────────────────────────────────────
  local CHARSET_NUM    = "0123456789"
  local CHARSET_ALNUM  = "abcdefghijklmnopqrstuvwxyz0123456789"
  local CHARSET_COMMON = "abcdefghijklmnopqrstuvwxyz0123456789 _-./@:+,!?()#%&*="
  local COMMON_CHAR_PREF = {
    [0x06] = true, -- BT Name
    [0x07] = true, -- AP SSID
    [0x09] = true, -- AP Password
    [0x0A] = true, -- STA SSID
    [0x0B] = true, -- STA Password
    [0x0C] = true, -- ELRS Bind Phrase
  }

  -- ── Event helpers ─────────────────────────────────────────────────
  local evEnter = input.evEnter
  local evEnterLong = input.evEnterLong or function() return false end
  local evExit  = input.evExit
  local evNext  = input.evNext
  local evPrev  = input.evPrev

  local function isAsciiAlpha(ch)
    return ch and #ch == 1 and ((ch >= "a" and ch <= "z") or (ch >= "A" and ch <= "Z"))
  end

  local function findCharIndex(cs, ch)
    for j = 1, #cs do
      if string.sub(cs, j, j) == ch then return j end
    end
    return nil
  end

  local function resolveChar(te, idx)
    local ci = te.chars[idx]
    if not ci or ci < 1 or ci > te.csLen then return nil end
    local ch = string.sub(te.charset, ci, ci)
    if te.upper[idx] and ch >= "a" and ch <= "z" then
      ch = string.upper(ch)
    end
    return ch
  end

  local function pickCharset(pref)
    if bit32.band(pref.flags, proto.PF_NUMERIC) ~= 0 then
      return CHARSET_NUM
    end
    if COMMON_CHAR_PREF[pref.id] then
      return CHARSET_COMMON
    end
    return CHARSET_ALNUM
  end

  local Settings = {}
  Settings.__index = Settings
  local buildWifiItems

  -- ── Helpers ───────────────────────────────────────────────────────

  -- Build list rows from store.prefs (all prefs, in firmware-defined order).
  local function buildRows(self)
    local rows = {}
    for _, id in ipairs(store.prefsOrder) do
      local p = store.prefs[id]
      if p then
        local row = { _prefId = id, _prefType = p.type, label = p.label }
        local rdOnly = bit32.band(p.flags, proto.PF_RDONLY) ~= 0

        if p.type == proto.FT_ENUM then
          row.value = p.options and p.options[p.curIdx + 1] or "?"
          if not rdOnly then row._options = p.options end

        elseif p.type == proto.FT_STRING then
          row.value = p.value or ""
          -- STRING prefs are edited via the character-by-character overlay
          -- (intercepted in handleEvent before the list sees ENTER).

        elseif p.type == proto.FT_INT then
          row.value = tostring(p.value or 0)

        elseif p.type == proto.FT_BOOL then
          row.value   = p.value and "On" or "Off"
          if not rdOnly then row._options = { "Off", "On" } end
        end

        rows[#rows + 1] = row
      end
    end
    return rows
  end

  -- Update a single row whose value changed.
  local function refreshRow(self, pref)
    for i, row in ipairs(self._list.rows) do
      if row._prefId == pref.id then
        if pref.type == proto.FT_ENUM then
          row.value = pref.options and pref.options[pref.curIdx + 1] or "?"
          if row._options then row._options = pref.options end
        elseif pref.type == proto.FT_STRING then
          row.value = pref.value or ""
        elseif pref.type == proto.FT_INT then
          row.value = tostring(pref.value or 0)
        elseif pref.type == proto.FT_BOOL then
          row.value = pref.value and "On" or "Off"
        end
        self._list:dirtyCache(i)
        return
      end
    end
  end

  -- ── Constructor ───────────────────────────────────────────────────

  function Settings.new()
    local self = setmetatable({}, Settings)

    self._page = Page.new({
      hasHeader    = true,
      title        = "BTWifiSerial",
      hasPageTitle = true,
      pageTitle    = "Settings",
      hasFooter    = true,
      indicators   = ctx.indicators,
    })

    local gap   = scale.sy(8)
    local listY = self._page.contentY + gap
    local listH = self._page.contentH - gap

    self._savingId    = nil   -- pref id currently being saved (waiting for ACK)
    self._modal       = nil   -- active feedback modal
    self._textEdit    = nil   -- active text edit state (nil when inactive)
    self._pickModal   = nil   -- active WiFi-scan picker modal
    self._wifiScanGen = 0     -- generation counter to invalidate stale scan callbacks
    self._activeWifiScanGen = 0
    self._pendingPassEdit = false  -- auto-open password editor after SSID save
    self._wifiConnCheck  = false  -- show result modal after STA reconnect
    self._wifiConnCheckT = nil    -- deferred timer for wifi check
    self._pendingRestartWifiCheck = false  -- set when restart-required STA_PASS is accepted
    self._lastCommittedId    = nil  -- track committed text-edit value for row refresh
    self._lastCommittedValue = nil

    self._list = List.new({
      y          = listY,
      h          = listH,
      selectable = true,
      editCol    = 2,
      onEdit     = function(row, key, oldVal, newVal)
        local id = row._prefId
        local p  = store.prefs[id]
        if not p then return end

        local val
        if p.type == proto.FT_ENUM then
          for i, opt in ipairs(p.options) do
            if opt == newVal then val = i - 1; break end
          end
          if val == nil then return end
        elseif p.type == proto.FT_BOOL then
          val = (newVal == "On") and 1 or 0
        elseif p.type == proto.FT_INT then
          val = tonumber(newVal) or 0
        elseif p.type == proto.FT_STRING then
          val = newVal
        else
          return
        end

        self._savingId = id
        sendFrame(proto.buildPrefSet(id, p.type, val))
        store.pendingPrefId = id
        -- Show saving spinner immediately while waiting for ACK
        self._modal = Modal.new({
          type     = "info",
          severity = "info",
          title    = "Saving...",
          message  = "Applying change...",
        })
        self._modal:show()
      end,
      cols = {
        { key = "label" },
        { key = "value", xFrac = 0.54 },
      },
      rows = store.prefsReady and buildRows(self) or {},
    })

    self._page:addChild(self._list)

    -- ── React to store events ──────────────────────────────────
    store.on("prefs_ready", function()
      -- Clear any stale post-save modal (device may have just reconnected after restart).
      self._modal    = nil
      self._savingId = nil
      -- Don't rebuild rows while inline text editor is active (would reset scroll)
      if not self._textEdit then
        self._list:setRows(buildRows(self))
      end
      -- After STA reconnect from scan-pick flow, check WiFi status
      if self._wifiConnCheck then
        self._wifiConnCheck  = false
        self._wifiConnCheckT = getTime()  -- deferred: status frame not yet processed
      end
      if self._pendingRestartWifiCheck then
        self._pendingRestartWifiCheck = false
        self._wifiConnCheck  = false
        self._wifiConnCheckT = getTime()
      end
    end)

    store.on("pref_changed", function(pref)
      refreshRow(self, pref)
    end)

    store.on("pref_ack", function(ev)
      if ev.id ~= self._savingId then return end
      self._savingId = nil
      self._modal    = nil   -- close spinner

      local p = store.prefs[ev.id]
      local needsRestart = p and (bit32.band(p.flags, proto.PF_RESTART) ~= 0)

      if ev.result == proto.ACK_OK then
        -- Update the row value immediately from the committed text (firmware won't
        -- send PREF_UPDATE for STRING prefs that trigger a restart)
        if self._lastCommittedId == ev.id and self._lastCommittedValue ~= nil then
          local p2 = store.prefs[ev.id]
          if p2 then
            p2.value = self._lastCommittedValue
            refreshRow(self, p2)
          end
          self._lastCommittedId    = nil
          self._lastCommittedValue = nil
        end
        if needsRestart then
          local shouldCheckWifiAfterRestart = false
          if ev.id == 0x0B then
            local wmp = store.prefs[0x01]
            local ssid = store.prefs[0x0A]
            if wmp and wmp.curIdx == 2 and ssid and (ssid.value or "") ~= "" then
              shouldCheckWifiAfterRestart = true
            end
          end
          self._modal = Modal.new({
            type     = "confirm",
            severity = "warning",
            title    = "Restart Required",
            message  = "Apply now or later?",
            onResult = function(accepted)
              if accepted then
                self._pendingRestartWifiCheck = shouldCheckWifiAfterRestart
                sendFrame(proto.buildInfoRestart())
                self._modal = Modal.new({
                  type     = "info",
                  severity = "info",
                  title    = "Restarting...",
                  message  = "Applying changes...",
                })
                self._modal:show()
              else
                self._pendingRestartWifiCheck = false
              end
            end,
          })
          self._modal:show()
          return
        end
        -- Auto-open password editor after SSID save from WiFi picker
        if self._pendingPassEdit and ev.id == 0x0A then
          self._pendingPassEdit = false
          self._wifiConnCheckT  = nil
          -- Navigate list to STA Password row and open text editor
          for i, row in ipairs(self._list.rows) do
            if row._prefId == 0x0B then
              self._list:setSel(i)
              self:_startTextEdit(0x0B)
              break
            end
          end
        end
        -- No restart: silent success, spinner already closed.
      else
        -- Firmware rejected the change: revert the list row to the stored value.
        if p then refreshRow(self, p) end
        self._modal = Modal.new({
          type     = "alert",
          severity = "error",
          title    = "Save Failed",
          message  = "Device rejected the change.",
        })
        self._modal:show()
      end
    end)

    -- WiFi scan listeners are registered once; active scan is generation-gated.
    store.on("wifi_scan_status", function(ev)
      if not (self._pickModal and self._pickModal:isOpen()) then return end
      if self._activeWifiScanGen ~= self._wifiScanGen then return end
      if ev.state ~= 1 then
        self._wifiScanStartT = nil
        self._pickModal:setItems(buildWifiItems())
      end
    end)

    store.on("wifi_scan_entry", function(_)
      if not (self._pickModal and self._pickModal:isOpen()) then return end
      if self._activeWifiScanGen ~= self._wifiScanGen then return end
      self._pickModal:setItems(buildWifiItems())
    end)

    return self
  end

  -- ── Text editor ───────────────────────────────────────────────────

  -- Start character-by-character editing for a FT_STRING pref.
  function Settings:_startTextEdit(id)
    local p = store.prefs[id]
    if not p or p.type ~= proto.FT_STRING then return end
    local cs  = pickCharset(p)
    local val = p.value or ""
    local maxLen = p.maxLen or 15
    -- Populate chars from current value
    local chars = {}
    local upper = {}
    for i = 1, math.min(#val, maxLen) do
      local ch = string.sub(val, i, i)
      local base = string.lower(ch)
      local ci = findCharIndex(cs, base)
      if ci then
        chars[i] = ci
        upper[i] = (ch ~= base) and isAsciiAlpha(ch)
      end
    end
    -- Add end-of-input marker after current content
    if #chars < maxLen then
      chars[#chars + 1] = #cs + 1
    end
    self._textEdit = {
      prefId         = id,
      title          = p.label or "Edit",
      maxLen         = maxLen,
      charset        = cs,
      csLen          = #cs,
      chars          = chars,
      upper          = upper,
      cursor         = 1,
      _teRenderCache = nil,   -- cached render strings; nil = dirty
    }
  end

  -- Commit the edited string: close editor, send PREF_SET, show spinner.
  function Settings:_commitTextEdit(id, value)
    local p = store.prefs[id]
    if not p then return end
    self._textEdit = nil
    value = string.match(value or "", "^%s*(.-)%s*$") or ""   -- trim
    if value == (p.value or "") then
      return
    end
    self._savingId           = id
    self._lastCommittedId    = id
    self._lastCommittedValue = value
    sendFrame(proto.buildPrefSet(id, proto.FT_STRING, value))
    store.pendingPrefId = id
    self._modal = Modal.new({
      type     = "info",
      severity = "info",
      title    = "Saving...",
      message  = "Applying change...",
    })
    self._modal:show()
  end

  -- Handle events while text editor is active.
  function Settings:_handleTextEdit(event)
    local te = self._textEdit
    te._teRenderCache = nil   -- any event may change visible state; rebuild on next render
    local cs    = te.charset
    local csLen = te.csLen
    if evEnterLong(event) then
      local ci = te.chars[te.cursor]
      if ci and ci >= 1 and ci <= csLen then
        local ch = string.sub(cs, ci, ci)
        if ch >= "a" and ch <= "z" then
          te.upper[te.cursor] = not te.upper[te.cursor]
        end
      end
      return true
    elseif evEnter(event) then
      local ci = te.chars[te.cursor]
      if ci == nil or ci > csLen then
        -- End-of-input marker selected: commit everything up to cursor-1
        local result = ""
        for i = 1, te.cursor - 1 do
          local ch = resolveChar(te, i)
          if ch then result = result .. ch end
        end
        self:_commitTextEdit(te.prefId, result)
      else
        -- Advance cursor
        te.cursor = te.cursor + 1
        if te.cursor > te.maxLen then
          -- Reached max length: commit entire buffer (trim handled in _commitTextEdit)
          local result = ""
          for i = 1, te.maxLen do
            local ch = resolveChar(te, i)
            if ch then result = result .. ch end
          end
          self:_commitTextEdit(te.prefId, result)
        elseif te.cursor > #te.chars then
          -- Past end of pre-loaded text: place end marker
          te.chars[te.cursor] = csLen + 1
          te.upper[te.cursor] = nil
        end
      end
      return true
    elseif evExit(event) then
      if te.cursor > 1 then
        te.cursor = te.cursor - 1
      else
        self._textEdit = nil
        -- Restore the row's visible value (it was cleared for inline rendering)
        local p = store.prefs[te.prefId]
        if p then
          for i, row in ipairs(self._list.rows) do
            if row._prefId == te.prefId then
              row.value = p.value or ""
              self._list:dirtyCache(i)
              break
            end
          end
        end
      end
      return true
    elseif evNext(event) then
      local ci = te.chars[te.cursor] or 1
      ci = ci + 1
      if ci > csLen + 1 then ci = 1 end
      te.chars[te.cursor] = ci
      if ci > csLen then te.upper[te.cursor] = nil end
      return true
    elseif evPrev(event) then
      local ci = te.chars[te.cursor] or 1
      ci = ci - 1
      if ci < 1 then ci = csLen + 1 end
      te.chars[te.cursor] = ci
      if ci > csLen then te.upper[te.cursor] = nil end
      return true
    end
    return false
  end

  function Settings:_renderTextEditModal()
    local te = self._textEdit
    if not te then return end

    local mw = scale.sx(320)
    local mh = scale.sy(140)
    local mx = math.floor((scale.W - mw) / 2)
    local my = math.floor((scale.H - mh) / 2)
    local th = scale.sy(34)
    local title = te.title or "Edit"
    local titleW = (lcd.sizeText and lcd.sizeText(title, F_small)) or (#title * 7)
    local titleX = mx + math.floor((mw - titleW) / 2)

    if isColor then
      lcd.setColor(CUSTOM_COLOR, theme.C.overlay)
      lcd.drawFilledRectangle(0, 0, scale.W, scale.H, CUSTOM_COLOR)
      lcd.setColor(CUSTOM_COLOR, theme.C.header)
      lcd.drawFilledRectangle(mx, my, mw, mh, CUSTOM_COLOR)
      lcd.setColor(CUSTOM_COLOR, theme.C.panel)
      lcd.drawRectangle(mx, my, mw, mh, CUSTOM_COLOR)
      lcd.setColor(CUSTOM_COLOR, theme.C.accent)
      lcd.drawFilledRectangle(mx, my, mw, th, CUSTOM_COLOR)
      lcd.setColor(CUSTOM_COLOR, C_text)
      lcd.drawText(titleX, my + scale.sy(6), title, F_SMALL_CC)
    else
      lcd.drawFilledRectangle(mx, my, mw, mh, ERASE)
      lcd.drawRectangle(mx, my, mw, mh, SOLID)
      lcd.drawText(titleX, my + scale.sy(4), title, SMLSIZE + BOLD)
    end

    local rowY = my + th + scale.sy(40)
    local font = F_small

    -- Rebuild render cache only when dirty (invalidated by _handleTextEdit on any event).
    -- This avoids 3 lcd.sizeText() calls + string building on no-input frames.
    if not te._teRenderCache then
      local pre = ""
      for i = 1, te.cursor - 1 do
        local ch = resolveChar(te, i)
        if ch then pre = pre .. ch end
      end
      local ci = te.chars[te.cursor]
      local isEnd = (ci == nil or ci > te.csLen)
      local curCh = isEnd and " " or (resolveChar(te, te.cursor) or " ")
      local post = ""
      for i = te.cursor + 1, #te.chars do
        local ch = resolveChar(te, i)
        if ch then post = post .. ch end
      end
      local allTxt = pre .. curCh .. post
      local allW = (lcd.sizeText and lcd.sizeText(allTxt, font)) or (#allTxt * 8)
      local preW
      if pre ~= "" then
        preW = (lcd.sizeText and lcd.sizeText(pre, font)) or (#pre * 8)
      else
        preW = 0
      end
      local curW = (lcd.sizeText and lcd.sizeText(curCh, font)) or 8
      te._teRenderCache = {
        pre = pre, curCh = curCh, post = post,
        isEnd = isEnd, allW = allW, preW = preW, curW = curW,
      }
    end
    local c = te._teRenderCache
    local sx = mx + math.floor((mw - c.allW) / 2)

    if isColor then lcd.setColor(CUSTOM_COLOR, C_text) end
    local colFlag = isColor and CUSTOM_COLOR or 0
    if c.pre ~= "" then
      lcd.drawText(sx, rowY, c.pre, font + colFlag)
    end
    lcd.drawText(sx + c.preW, rowY, c.curCh, font + colFlag)
    if c.post ~= "" then
      lcd.drawText(sx + c.preW + c.curW, rowY, c.post, font + colFlag)
    end

    local caretY = rowY + theme.FH.small + scale.sy(2)
    lcd.drawText(sx + c.preW, caretY, "_", font + BLINK + colFlag)
  end


  -- ── WiFi scan picker ───────────────────────────────────────────────

  buildWifiItems = function()
    local entries = {}
    for _, r in ipairs(store.wifiScanResults) do
      if r then entries[#entries + 1] = r end
    end
    table.sort(entries, function(a, b)
      return (a.rssi or -999) > (b.rssi or -999)
    end)
    local items = {}
    for _, r in ipairs(entries) do
      items[#items + 1] = {
        label    = (r.ssid and r.ssid ~= "") and r.ssid or "(hidden)",
        sublabel = tostring(r.rssi) .. " dBm",
        value    = (r.ssid and r.ssid ~= "") and r.ssid or "",
      }
    end
    return items
  end

  function Settings:_startWifiScanPick()
    -- Guard: WiFi mode must be On (AP or STA)
    local wmp = store.prefs[0x01]
    local idx = wmp and wmp.curIdx or 0
    if idx == 0 then
      self._modal = Modal.new({
        type = "alert", severity = "warning",
        title = "WiFi Off",
        message = "Enable WiFi mode (AP or STA)\nbefore scanning.",
      })
      self._modal:show()
      return
    end
    -- Guard: BLE must not be active (single-radio device)
    if store.status.bleConnected or store.status.bleConnecting then
      self._modal = Modal.new({
        type = "alert", severity = "warning",
        title = "BLE Active",
        message = "WiFi scan not available\nwhile BLE is connected.",
      })
      self._modal:show()
      return
    end

    -- Bump generation so stale callbacks from a previous scan are ignored
    self._wifiScanGen = self._wifiScanGen + 1
    self._activeWifiScanGen = self._wifiScanGen
    self._wifiConnCheck = false
    self._wifiConnCheckT = nil

    self._pickModal = PickModal.new({
      title    = "Select WiFi Network",
      onResult = function(item)
        self._pickModal = nil
        if item and item.value ~= "" then
          local p = store.prefs[0x0A]
          if p then
            -- Save SSID (firmware won't restart); then auto-open password editor
            self._savingId = 0x0A
            self._pendingPassEdit = true
            self._lastCommittedId = 0x0A
            self._lastCommittedValue = item.value
            sendFrame(proto.buildPrefSet(0x0A, proto.FT_STRING, item.value))
            store.pendingPrefId = 0x0A
            self._modal = Modal.new({
              type = "info", severity = "info",
              title = "Saving...", message = "Applying change...",
            })
            self._modal:show()
          end
        end
      end,
    })
    self._pickModal:show()
    self._wifiScanStartT = getTime()
    sendFrame(proto.buildInfoWifiScan())
  end

  -- ── Public API ────────────────────────────────────────────────────

  function Settings:handleEvent(event)
    -- Deferred WiFi connection check (2s after prefs_ready to let status frames arrive)
    if self._wifiConnCheckT then
      if getTime() - self._wifiConnCheckT > 200 then
        self._wifiConnCheckT = nil
        local wmp = store.prefs[0x01]
        if self._pendingRestartWifiCheck then
          -- Device just restarted; give it another 2 s before checking STA status.
          self._pendingRestartWifiCheck = false
          self._wifiConnCheckT = getTime()
        elseif (wmp and wmp.curIdx == 2) and not store.status.wifiClients then
          self._modal = Modal.new({
            type = "alert", severity = "warning",
            title = "Connection Failed",
            message = "Could not connect to WiFi.\nCheck SSID and password.",
          })
          self._modal:show()
        end
      end
    end
    -- WiFi scan timeout (10 seconds) — only while still loading
    if self._pickModal and self._pickModal:isOpen() and self._wifiScanStartT then
      if (getTime() - self._wifiScanStartT) > 1000 then
        self._pickModal:setItems({})
        self._wifiScanStartT = nil
      end
    end
    -- PickModal has topmost priority while open
    if self._pickModal and self._pickModal:isOpen() then
      self._pickModal:handleEvent(event)
      return true
    end
    if self._modal then
      self._modal:handleEvent(event)
      return true
    end
    -- Text editor takes full priority
    if self._textEdit then
      return self:_handleTextEdit(event)
    end
    -- Intercept ENTER for FT_STRING rows before the list handles it
    if evEnter(event) then
      local row = self._list:getSel()
      if row then
        -- Special case: STA SSID opens the WiFi scan picker
        if row._prefId == 0x0A then
          self:_startWifiScanPick()
          return true
        end
        if row._prefType == proto.FT_STRING then
          local p = row._prefId and store.prefs[row._prefId]
          local rdOnly = p and bit32.band(p.flags, proto.PF_RDONLY) ~= 0
        if p and not rdOnly then
            self:_startTextEdit(row._prefId)
            return true
          end
        end
      end
    end
    if self._list:handleEvent(event) then return true end
    return false
  end

  function Settings:setPagination(n, total)
    self._page:setPagination(n, total)
  end

  function Settings:render()
    self._page:render()
    if self._textEdit then self:_renderTextEditModal() end

    if self._modal then
      self._modal:render()
      if not self._modal:isOpen() then self._modal = nil end
    end

    if self._pickModal then
      self._pickModal:render()
      if not self._pickModal:isOpen() then self._pickModal = nil end
    end

    if not store.prefsReady then
      local ly = self._page.contentY + WAITING_Y_OFF
      lcd.drawText(WAITING_X, ly, "Waiting for device…",
                   (isColor and CUSTOM_COLOR or 0))
    end
  end

  function Settings:contentY()
    return self._page.contentY
  end

  return Settings
end

