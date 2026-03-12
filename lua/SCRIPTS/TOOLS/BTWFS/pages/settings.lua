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

  -- ── Charsets for text editing ─────────────────────────────────────
  -- Full printable ASCII (0x20-0x7E, 95 chars) so any WPA2 password can be entered.
  local CHARSET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"
  local NUMSET  = "0123456789"

  -- ── Event helpers ─────────────────────────────────────────────────
  local evEnter = input.evEnter
  local evExit  = input.evExit
  local evNext  = input.evNext
  local evPrev  = input.evPrev

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
    for _, row in ipairs(self._list.rows) do
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
          -- Device is about to restart; close spinner silently.
          -- main.lua will show "Reconnecting..." once the device goes offline.
          self._modal = nil
        end
        -- Auto-open password editor after SSID save from WiFi picker
        if self._pendingPassEdit and ev.id == 0x0A then
          self._pendingPassEdit = false
          self._wifiConnCheck   = true  -- check wifi after password save + restart
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
    local isNumeric = bit32.band(p.flags, proto.PF_NUMERIC) ~= 0
    local cs  = isNumeric and NUMSET or CHARSET
    local val = p.value or ""
    local maxLen = p.maxLen or 15
    -- Populate chars from current value
    local chars = {}
    for i = 1, math.min(#val, maxLen) do
      local ch = string.sub(val, i, i)
      local ci = 1
      for j = 1, #cs do
        if string.sub(cs, j, j) == ch then ci = j; break end
      end
      chars[i] = ci
    end
    -- Add end-of-input marker after current content
    if #chars < maxLen then
      chars[#chars + 1] = #cs + 1
    end
    self._textEdit = {
      prefId  = id,
      maxLen  = maxLen,
      charset = cs,
      chars   = chars,
      cursor  = 1,
    }
  end

  -- Commit the edited string: close editor, send PREF_SET, show spinner.
  function Settings:_commitTextEdit(id, value)
    self._textEdit = nil
    local p = store.prefs[id]
    if not p then return end
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
    local cs    = te.charset
    local csLen = #cs
    if evEnter(event) then
      local ci = te.chars[te.cursor]
      if ci == nil or ci > csLen then
        -- End-of-input marker selected: commit everything up to cursor-1
        local result = ""
        for i = 1, te.cursor - 1 do
          local c = te.chars[i]
          if c and c >= 1 and c <= csLen then
            result = result .. string.sub(cs, c, c)
          end
        end
        self:_commitTextEdit(te.prefId, result)
      else
        -- Advance cursor
        te.cursor = te.cursor + 1
        if te.cursor > te.maxLen then
          -- Reached max length: commit entire buffer
          local result = ""
          for i = 1, te.maxLen do
            local c = te.chars[i]
            if c and c >= 1 and c <= csLen then
              result = result .. string.sub(cs, c, c)
            end
          end
          self:_commitTextEdit(te.prefId, result)
        elseif te.cursor > #te.chars then
          -- Past end of pre-loaded text: place end marker
          te.chars[te.cursor] = csLen + 1
        end
      end
      return true
    elseif evExit(event) then
      self._textEdit = nil
      -- Restore the row's visible value (it was cleared for inline rendering)
      local p = store.prefs[te.prefId]
      if p then
        for _, row in ipairs(self._list.rows) do
          if row._prefId == te.prefId then row.value = p.value or ""; break end
        end
      end
      return true
    elseif evNext(event) then
      local ci = te.chars[te.cursor] or 1
      ci = ci + 1
      if ci > csLen + 1 then ci = 1 end
      te.chars[te.cursor] = ci
      return true
    elseif evPrev(event) then
      local ci = te.chars[te.cursor] or 1
      ci = ci - 1
      if ci < 1 then ci = csLen + 1 end
      te.chars[te.cursor] = ci
      return true
    end
    return false
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
        if (wmp and wmp.curIdx == 2) and not store.status.wifiClients then
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
    -- While text-editing: clear the row's value so the list draws nothing in
    -- the value column; we render pre/cursor/post ourselves right after.
    if self._textEdit then
      for _, row in ipairs(self._list.rows) do
        if row._prefId == self._textEdit.prefId then
          row.value = ""
          break
        end
      end
    end

    self._page:render()

    -- Inline character-by-character text editor drawn on top of the row
    if self._textEdit then
      local te   = self._textEdit
      local list = self._list
      local slot = list._sel - list._offset
      if slot >= 1 and slot <= list.maxVisible then
        local ty    = list.y + (slot - 1) * list.rowH + list._txtOff
        local valX  = list.x + math.floor(list._contentW * 0.54)
        local cs    = te.charset
        local csLen = #cs
        -- Pre-cursor: confirmed characters
        local pre = ""
        for i = 1, te.cursor - 1 do
          local c = te.chars[i]
          if c and c >= 1 and c <= csLen then
            pre = pre .. string.sub(cs, c, c)
          end
        end
        -- Cursor character (will blink)
        local ci    = te.chars[te.cursor]
        local isEnd = (ci == nil or ci > csLen)
        local curCh = isEnd and "_" or string.sub(cs, ci, ci)
        -- Post-cursor: pre-loaded characters after cursor
        local post = ""
        for i = te.cursor + 1, #te.chars do
          local c = te.chars[i]
          if c and c >= 1 and c <= csLen then
            post = post .. string.sub(cs, c, c)
          end
        end
        -- Draw pre (solid) + cursor (BLINK) + post (solid)
        local font    = list.font
        local colFlag = theme.isColor and CUSTOM_COLOR or 0
        if theme.isColor then lcd.setColor(CUSTOM_COLOR, theme.C.text) end
        local preW = 0
        if pre ~= "" then
          lcd.drawText(valX, ty, pre, font + colFlag)
          preW = (lcd.sizeText and lcd.sizeText(pre, font)) or (#pre * 8)
        end
        lcd.drawText(valX + preW, ty, curCh, font + BLINK + colFlag)
        if post ~= "" then
          local curW = (lcd.sizeText and lcd.sizeText(curCh, font)) or 8
          lcd.drawText(valX + preW + curW, ty, post, font + colFlag)
        end
      end
    end

    if self._modal then
      self._modal:render()
      if not self._modal:isOpen() then self._modal = nil end
    end

    if self._pickModal then
      self._pickModal:render()
      if not self._pickModal:isOpen() then self._pickModal = nil end
    end

    if not store.prefsReady then
      local lx = scale.sx(17)
      local ly = self._page.contentY + scale.sy(8)
      lcd.drawText(lx, ly, "Waiting for device…",
                   (theme.isColor and CUSTOM_COLOR or 0))
    end
  end

  function Settings:contentY()
    return self._page.contentY
  end

  return Settings
end

