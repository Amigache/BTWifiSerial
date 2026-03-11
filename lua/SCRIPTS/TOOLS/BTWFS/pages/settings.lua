-- pages/settings.lua
-- Settings page — fully data-driven from store.prefs.
-- Rows are built when PREF_END arrives; updated on PREF_UPDATE.
-- Edit confirm sends PREF_SET via ctx.sendFrame.

return function(ctx)
  local Page      = ctx.Page
  local Section   = ctx.Section
  local List      = ctx.List
  local Loading   = ctx.Loading
  local Modal     = ctx.Modal
  local scale     = ctx.scale
  local theme     = ctx.theme
  local proto     = ctx.proto
  local store     = ctx.store
  local sendFrame = ctx.sendFrame

  -- ── Charsets for text editing ─────────────────────────────────────
  local CHARSET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 -_@."
  local NUMSET  = "0123456789"

  -- ── Event helpers ─────────────────────────────────────────────────
  local function evEnter(e)
    return (EVT_VIRTUAL_ENTER ~= nil and e == EVT_VIRTUAL_ENTER)
        or (EVT_ENTER_BREAK   ~= nil and e == EVT_ENTER_BREAK)
  end
  local function evExit(e)
    return (EVT_VIRTUAL_EXIT ~= nil and e == EVT_VIRTUAL_EXIT)
        or (EVT_EXIT_BREAK   ~= nil and e == EVT_EXIT_BREAK)
  end
  local function evNext(e)
    return (EVT_VIRTUAL_NEXT ~= nil and e == EVT_VIRTUAL_NEXT)
        or (EVT_ROT_RIGHT    ~= nil and e == EVT_ROT_RIGHT)
        or (EVT_PLUS_BREAK   ~= nil and e == EVT_PLUS_BREAK)
        or (EVT_PLUS_REPT    ~= nil and e == EVT_PLUS_REPT)
  end
  local function evPrev(e)
    return (EVT_VIRTUAL_PREV  ~= nil and e == EVT_VIRTUAL_PREV)
        or (EVT_ROT_LEFT      ~= nil and e == EVT_ROT_LEFT)
        or (EVT_MINUS_BREAK   ~= nil and e == EVT_MINUS_BREAK)
        or (EVT_MINUS_REPT    ~= nil and e == EVT_MINUS_REPT)
  end

  local Settings = {}
  Settings.__index = Settings

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

    local secY  = self._page.contentY + scale.sy(8)
    self._section = Section.new({ title = "Configuration", y = secY })

    local listY = self._section.contentY
    local listH = (self._page.contentY + self._page.contentH) - listY

    self._savingId   = nil   -- pref id currently being saved (waiting for ACK)
    self._modal      = nil   -- active feedback modal
    self._textEdit   = nil   -- active text edit state (nil when inactive)

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

    self._page:addChild(self._section)
    self._page:addChild(self._list)

    -- ── React to store events ──────────────────────────────────
    store.on("prefs_ready", function()
      self._list:setRows(buildRows(self))
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
        if needsRestart then
          self._modal = Modal.new({
            type     = "alert",
            severity = "info",
            title    = "Restart Required",
            message  = "Settings saved.\nDevice will restart.",
          })
          self._modal:show()
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
    self._savingId = id
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


  -- ── Public API ────────────────────────────────────────────────────

  function Settings:handleEvent(event)
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
      if row and row._prefType == proto.FT_STRING then
        local p = row._prefId and store.prefs[row._prefId]
        local rdOnly = p and bit32.band(p.flags, proto.PF_RDONLY) ~= 0
        if p and not rdOnly then
          self:_startTextEdit(row._prefId)
          return true
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

    if not store.prefsReady then
      local lx = scale.sx(17)
      local ly = self._section.contentY + scale.sy(8)
      lcd.drawText(lx, ly, "Waiting for device…",
                   (theme.isColor and CUSTOM_COLOR or 0))
    end
  end

  function Settings:contentY()
    return self._page.contentY
  end

  return Settings
end

