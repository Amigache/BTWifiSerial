-- components/pick_modal.lua
-- List-picker overlay modal.
-- Shows a scrollable list; ENTER confirms selection, EXIT cancels.
--
-- Props:
--   title    string        header label
--   onResult fn(item|nil)  callback: selected item on confirm, nil on cancel
--   overlay  bool          dark backdrop behind modal (default true)
--
-- API:
--   :show()              make visible and reset to loading state
--   :close()             force dismiss (no callback)
--   :isOpen()            bool
--   :setItems(items)     populate list, exit loading state
--                        items = { {label, sublabel, value}, ... }
--   :handleEvent(ev)     returns true if consumed
--   :render()            draw (call after page render)

return function(ctx)
  local theme   = ctx.theme
  local scale   = ctx.scale
  local Loading = ctx.Loading
  local List    = ctx.List

  local function evEnter(e)
    return (EVT_VIRTUAL_ENTER ~= nil and e == EVT_VIRTUAL_ENTER)
        or (EVT_ENTER_BREAK   ~= nil and e == EVT_ENTER_BREAK)
  end
  local function evExit(e)
    return (EVT_VIRTUAL_EXIT ~= nil and e == EVT_VIRTUAL_EXIT)
        or (EVT_EXIT_BREAK   ~= nil and e == EVT_EXIT_BREAK)
  end

  local PickModal = {}
  PickModal.__index = PickModal

  function PickModal.new(props)
    local self     = setmetatable({}, PickModal)
    self._title    = props.title    or "Select"
    self._onResult = props.onResult
    self._overlay  = (props.overlay ~= false)
    self._open     = false
    self._loading  = true
    self._items    = {}

    -- Modal box dimensions
    local mw = scale.sx(440)
    local mh = scale.sy(300)
    local mx = math.floor((LCD_W - mw) / 2)
    local my = math.floor((LCD_H - mh) / 2)
    self._mx = mx
    self._my = my
    self._mw = mw
    self._mh = mh

    -- Title bar height
    local titleH = scale.sy(38)
    self._titleH = titleH

    -- Content area below title bar
    local contentY = my + titleH
    local contentH = mh - titleH
    self._contentY0 = contentY
    self._contentH0 = contentH

    -- Loading spinner (position recalculated in render for correct centering)
    self._spinner = Loading.new({
      cx    = mx + math.floor(mw / 2),
      cy    = contentY + math.floor(contentH / 2),
      r     = scale.s(16),
      color = theme.C.accent,
    })

    -- List (full content area below title bar)
    local listPad = scale.sx(4)
    self._list = List.new({
      x          = mx + listPad,
      y          = contentY + scale.sy(4),
      w          = mw - listPad * 2,
      h          = contentH - scale.sy(4),
      selectable = true,
      showScroll = true,
      rowBg      = theme.C.header,
      cols = {
        { key = "label" },
        { key = "sublabel", xFrac = 0.65 },
      },
      rows = {},
    })

    return self
  end

  function PickModal:show()
    self._open    = true
    self._loading = true
    self._items   = {}
    self._list:setRows({})
  end

  function PickModal:close()
    self._open = false
  end

  function PickModal:isOpen()
    return self._open
  end

  function PickModal:setItems(items)
    self._items   = items
    self._loading = false
    self._list:setRows(items)
  end

  function PickModal:handleEvent(event)
    if not self._open then return false end

    if self._loading then
      if evExit(event) then
        self._open = false
        if self._onResult then self._onResult(nil) end
      end
      return true   -- swallow all events while loading
    end

    if evEnter(event) then
      local item = self._list:getSel()
      self._open = false
      if self._onResult then self._onResult(item) end
      return true
    end

    if evExit(event) then
      self._open = false
      if self._onResult then self._onResult(nil) end
      return true
    end

    return self._list:handleEvent(event)
  end

  function PickModal:render()
    if not self._open then return end

    if not theme.isColor then
      -- B&W fallback
      lcd.drawFilledRectangle(self._mx, self._my, self._mw, self._mh, ERASE)
      lcd.drawRectangle(self._mx, self._my, self._mw, self._mh, SOLID)
      lcd.drawText(self._mx + 4, self._my + 4, self._title, BOLD)
      if self._loading then
        lcd.drawText(self._mx + 4, self._my + 24, "Scanning...", SMLSIZE)
      elseif #self._items == 0 then
        lcd.drawText(self._mx + 4, self._my + 24, "No networks found", SMLSIZE)
      else
        self._list:render()
      end
      return
    end

    -- Dark overlay
    if self._overlay then
      lcd.setColor(CUSTOM_COLOR, theme.C.overlay)
      lcd.drawFilledRectangle(0, 0, LCD_W, LCD_H, CUSTOM_COLOR)
    end

    -- Drop shadow
    local sh = scale.s(4)
    lcd.setColor(CUSTOM_COLOR, theme.C.overlay)
    lcd.drawFilledRectangle(self._mx + sh, self._my + sh, self._mw, self._mh, CUSTOM_COLOR)

    -- Modal background
    lcd.setColor(CUSTOM_COLOR, theme.C.header)
    lcd.drawFilledRectangle(self._mx, self._my, self._mw, self._mh, CUSTOM_COLOR)

    -- Border
    lcd.setColor(CUSTOM_COLOR, theme.C.panel)
    lcd.drawRectangle(self._mx, self._my, self._mw, self._mh, CUSTOM_COLOR)

    -- Title bar
    local titleBg = lcd.RGB(0, 80, 160)
    lcd.setColor(CUSTOM_COLOR, titleBg)
    lcd.drawFilledRectangle(self._mx, self._my, self._mw, self._titleH, CUSTOM_COLOR)
    lcd.setColor(CUSTOM_COLOR, theme.C.text)
    local ttw = (lcd.sizeText and lcd.sizeText(self._title, theme.F.body)) or scale.sx(100)
    local ty = self._my + math.floor((self._titleH - theme.FH.body) / 2) - scale.sy(3)
    lcd.drawText(self._mx + math.floor((self._mw - ttw) / 2), ty,
                 self._title, theme.F.body + CUSTOM_COLOR)

    -- Content area
    if self._loading then
      -- Center spinner + label as a group vertically in content area
      local r   = self._spinner.r
      local gap = scale.sy(6)
      local msg = "Scanning..."
      local tw  = (lcd.sizeText and lcd.sizeText(msg, theme.F.small)) or scale.sx(60)
      local groupH = r * 2 + gap + theme.FH.small
      local spinCy = self._contentY0 + math.floor((self._contentH0 - groupH) / 2) + r
      self._spinner.cx = self._mx + math.floor(self._mw / 2)
      self._spinner.cy = spinCy
      self._spinner:render()
      lcd.setColor(CUSTOM_COLOR, theme.C.subtext)
      lcd.drawText(self._mx + math.floor((self._mw - tw) / 2),
                   spinCy + r + gap, msg, theme.F.small + CUSTOM_COLOR)

    elseif #self._items == 0 then
      local cy = self._contentY0 + math.floor((self._contentH0 - theme.FH.small) / 2)
      lcd.setColor(CUSTOM_COLOR, theme.C.subtext)
      local msg = "No networks found"
      local nrW = (lcd.sizeText and lcd.sizeText(msg, theme.F.small)) or scale.sx(100)
      lcd.drawText(self._mx + math.floor((self._mw - nrW) / 2), cy,
                   msg, theme.F.small + CUSTOM_COLOR)

    else
      self._list:render()
    end
  end

  return PickModal
end
