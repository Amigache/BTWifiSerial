 -- components/modal.lua
-- Overlay modal dialogs: Alert, Info, Confirm.
--
-- Usage:
--   local m = Modal.new({ type="alert", severity="error",
--                         title="Error!", message="Details here" })
--   m:show()           -- make visible
--   m:handleEvent(ev)  -- feed events while open
--   m:render()         -- draw on top of everything
--   m:isOpen()         -- still visible?
--   m:close()          -- force close (for "info" type)
--
-- Types:
--   "alert"   – title + message + [OK] button.  Closed by ENTER or EXIT.
--   "info"    – title + message + loading spinner.  Closed programmatically.
--   "confirm" – title + [Accept] [Cancel].  Result via onResult(bool).
--
-- Severities:
--   "success"  → green       "error"   → red
--   "warning"  → yellow      "info"    → blue
--
-- Props:
--   type        string   "alert" | "info" | "confirm"  (default "alert")
--   severity    string   "success"|"error"|"warning"|"info" (default "info")
--   title       string   big/medium text               (default "")
--   message     string   small text below title         (default "")
--   overlay     bool     show dark overlay behind modal (default true)
--   onResult    fn(bool) confirm callback               (nil)
--   onClose     fn()     called when any modal closes    (nil)

return function(ctx)
  local theme   = ctx.theme
  local scale   = ctx.scale
  local Button  = ctx.Button
  local Loading = ctx.Loading

  -- Severity → color mapping
  local SEV_COLORS = {
    success = theme.C.modalSuccess,
    error   = theme.C.modalError,
    warning = theme.C.modalWarning,
    info    = theme.C.modalInfo,
  }

  -- Event helpers (same as list.lua)
  local function evEnter(e)
    return (EVT_VIRTUAL_ENTER ~= nil and e == EVT_VIRTUAL_ENTER)
  end
  local function evExit(e)
    return (EVT_VIRTUAL_EXIT ~= nil and e == EVT_VIRTUAL_EXIT)
        or (EVT_EXIT_BREAK   ~= nil and e == EVT_EXIT_BREAK)
  end
  local function evNext(e)
    return (EVT_VIRTUAL_NEXT ~= nil and e == EVT_VIRTUAL_NEXT)
        or (EVT_ROT_RIGHT    ~= nil and e == EVT_ROT_RIGHT)
  end
  local function evPrev(e)
    return (EVT_VIRTUAL_PREV ~= nil and e == EVT_VIRTUAL_PREV)
        or (EVT_ROT_LEFT     ~= nil and e == EVT_ROT_LEFT)
  end

  local Modal = {}
  Modal.__index = Modal

  function Modal.new(props)
    local self      = setmetatable({}, Modal)
    self._type      = props.type     or "alert"
    self._severity  = props.severity or "info"
    self._title     = props.title    or ""
    self._message   = props.message  or ""
    self._onResult  = props.onResult
    self._onClose   = props.onClose
    self._overlay   = (props.overlay ~= false)  -- default true
    self._open      = false
    self._focusIdx  = 1   -- 1=left(Accept/OK), 2=right(Cancel)

    -- Pre-split message into lines for centered multi-line rendering
    self._lines = {}
    local msg = self._message
    if msg ~= "" then
      for line in string.gmatch(msg .. "\n", "([^\n]*)\n") do
        self._lines[#self._lines + 1] = line
      end
    end

    -- Color for this severity
    self._sevColor = SEV_COLORS[self._severity] or SEV_COLORS.info

    -- Modal box dimensions
    local mw = scale.sx(420)
    local mh = scale.sy(200)
    self._mx = math.floor((LCD_W - mw) / 2)
    self._my = math.floor((LCD_H - mh) / 2)
    self._mw = mw
    self._mh = mh

    -- Accent bar at top of modal
    self._accentH = scale.sy(4)

    -- Button dimensions
    local btnW = scale.sx(110)
    local btnH = scale.sy(32)
    local btnY = self._my + self._mh - btnH - scale.sy(16)

    if self._type == "alert" then
      -- Single centered OK button
      local okX = self._mx + math.floor((mw - btnW) / 2)
      self._btnOk = Button.new({
        x = okX, y = btnY, w = btnW, h = btnH,
        label = "OK", focused = true, focusColor = self._sevColor,
      })
    elseif self._type == "confirm" then
      -- Two buttons: Accept + Cancel
      local gap = scale.sx(20)
      local totalW = btnW * 2 + gap
      local startX = self._mx + math.floor((mw - totalW) / 2)
      self._btnAccept = Button.new({
        x = startX, y = btnY, w = btnW, h = btnH,
        label = "Accept", focused = true, focusColor = self._sevColor,
      })
      self._btnCancel = Button.new({
        x = startX + btnW + gap, y = btnY, w = btnW, h = btnH,
        label = "Cancel", focused = false, focusColor = self._sevColor,
      })
    elseif self._type == "info" then
      -- Loading spinner centered in lower portion of modal
      local spinY = self._my + self._mh - scale.sy(50)
      self._loading = Loading.new({
        cx = self._mx + math.floor(mw / 2),
        cy = spinY,
        r  = scale.s(20),
        color = self._sevColor,
      })
    end

    return self
  end

  function Modal:show()       self._open = true  end
  function Modal:close()
    self._open = false
    if self._onClose then self._onClose() end
  end
  function Modal:isOpen()     return self._open   end

  function Modal:handleEvent(event)
    if not self._open then return false end

    if self._type == "alert" then
      if evEnter(event) or evExit(event) then
        self:close()
        return true
      end
    elseif self._type == "confirm" then
      if evNext(event) or evPrev(event) then
        -- Toggle focus between Accept and Cancel
        if self._focusIdx == 1 then
          self._focusIdx = 2
          self._btnAccept:setFocused(false)
          self._btnCancel:setFocused(true)
        else
          self._focusIdx = 1
          self._btnAccept:setFocused(true)
          self._btnCancel:setFocused(false)
        end
        return true
      elseif evEnter(event) then
        local accepted = (self._focusIdx == 1)
        if self._onResult then self._onResult(accepted) end
        self:close()
        return true
      elseif evExit(event) then
        if self._onResult then self._onResult(false) end
        self:close()
        return true
      end
    elseif self._type == "info" then
      -- Info modal: no user interaction, closed by code
      return true   -- swallow events while open
    end

    return true   -- always consume events when modal is open
  end

  function Modal:render()
    if not self._open then return end

    if not theme.isColor then
      -- B&W: simple framed box
      lcd.drawFilledRectangle(self._mx, self._my, self._mw, self._mh, ERASE)
      lcd.drawRectangle(self._mx, self._my, self._mw, self._mh, SOLID)
      lcd.drawText(self._mx + 4, self._my + 4, self._title, BOLD)
      lcd.drawText(self._mx + 4, self._my + 24, self._message, 0)
      return
    end

    -- Overlay
    if self._overlay then
      lcd.setColor(CUSTOM_COLOR, theme.C.overlay)
      lcd.drawFilledRectangle(0, 0, LCD_W, LCD_H, CUSTOM_COLOR)
    end

    -- Drop shadow (offset dark rect behind modal)
    local sh = scale.s(4)
    lcd.setColor(CUSTOM_COLOR, theme.C.overlay)
    lcd.drawFilledRectangle(self._mx + sh, self._my + sh, self._mw, self._mh, CUSTOM_COLOR)

    -- Modal background
    lcd.setColor(CUSTOM_COLOR, theme.C.header)
    lcd.drawFilledRectangle(self._mx, self._my, self._mw, self._mh, CUSTOM_COLOR)

    -- 1px border
    lcd.setColor(CUSTOM_COLOR, theme.C.panel)
    lcd.drawRectangle(self._mx, self._my, self._mw, self._mh, CUSTOM_COLOR)

    -- Accent bar at top
    lcd.setColor(CUSTOM_COLOR, self._sevColor)
    lcd.drawFilledRectangle(self._mx, self._my, self._mw, self._accentH, CUSTOM_COLOR)

    -- Title text
    local titleFont = (self._type == "confirm") and theme.F.body or theme.F.title
    local titleFH   = (self._type == "confirm") and theme.FH.body or theme.FH.title

    -- Vertically center text block between accent bar bottom and button/spinner top
    local accentBot = self._my + self._accentH
    local hasMsg = #self._lines > 0 and self._type ~= "confirm"
    local lineGap = scale.sy(4)
    local textBlockH = titleFH
    if hasMsg then
      textBlockH = textBlockH + scale.sy(10)
                 + theme.FH.small * #self._lines
                 + lineGap * (#self._lines - 1)
    end

    local btnTop
    if self._type == "info" then
      btnTop = self._my + self._mh - scale.sy(50) - scale.s(20)
    else
      btnTop = self._my + self._mh - scale.sy(32) - scale.sy(16)
    end
    local zoneH = btnTop - accentBot
    local titleY = accentBot + math.floor((zoneH - textBlockH) / 2)

    lcd.setColor(CUSTOM_COLOR, self._sevColor)
    -- Center title horizontally
    local tw = lcd.sizeText and lcd.sizeText(self._title, titleFont) or 0
    local tx = self._mx + math.floor((self._mw - tw) / 2)
    lcd.drawText(tx, titleY, self._title, titleFont + CUSTOM_COLOR)

    -- Message text (for alert and info types) — each line centered
    if hasMsg then
      lcd.setColor(CUSTOM_COLOR, theme.C.subtext)
      local lineY = titleY + titleFH + scale.sy(10)
      for _, line in ipairs(self._lines) do
        local lw = lcd.sizeText and lcd.sizeText(line, theme.F.small) or 0
        local lx = self._mx + math.floor((self._mw - lw) / 2)
        lcd.drawText(lx, lineY, line, theme.F.small + CUSTOM_COLOR)
        lineY = lineY + theme.FH.small + lineGap
      end
    end

    -- Buttons / loading
    if self._type == "alert" then
      self._btnOk:render()
    elseif self._type == "confirm" then
      self._btnAccept:render()
      self._btnCancel:render()
    elseif self._type == "info" then
      self._loading:render()
    end
  end

  return Modal
end
