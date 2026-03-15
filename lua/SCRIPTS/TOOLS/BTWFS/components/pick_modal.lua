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

  -- Cache theme values
  local isColor    = theme.isColor
  local C_overlay  = theme.C.overlay
  local C_header   = theme.C.header
  local C_panel    = theme.C.panel
  local C_text     = theme.C.text
  local C_subtext  = theme.C.subtext
  local C_accent   = theme.C.accent
  local F_body     = theme.F.body
  local F_small    = theme.F.small
  local F_BODY_CC  = F_body + CUSTOM_COLOR
  local F_SMALL_CC = F_small + CUSTOM_COLOR
  local FH_body    = theme.FH.body
  local FH_small   = theme.FH.small

  -- Pre-compute scale values
  local SHADOW_OFF = scale.s(4)
  local SPINNER_GAP = scale.sy(6)
  local SCANNING_MSG = "Scanning..."
  local SCANNING_TW  = (lcd.sizeText and lcd.sizeText(SCANNING_MSG, F_small)) or scale.sx(60)
  local NORESULT_MSG = "No networks found"
  local NORESULT_TW  = (lcd.sizeText and lcd.sizeText(NORESULT_MSG, F_small)) or scale.sx(100)

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

    -- Pre-compute title text position
    local ttw = (lcd.sizeText and lcd.sizeText(self._title, F_body)) or scale.sx(100)
    self._titleTextX = mx + math.floor((mw - ttw) / 2)
    self._titleTextY = my + math.floor((titleH - FH_body) / 2) - scale.sy(3)

    -- Pre-compute shadow position
    self._shX = mx + SHADOW_OFF
    self._shY = my + SHADOW_OFF

    -- Pre-compute empty-state center Y
    self._emptyCY = contentY + math.floor((contentH - FH_small) / 2)
    self._emptyTX = mx + math.floor((mw - NORESULT_TW) / 2)

    -- Pre-compute scanning label X
    self._scanLabelX = mx + math.floor((mw - SCANNING_TW) / 2)

    -- Title bar color
    self._titleBg = lcd.RGB(0, 80, 160)

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

    if not isColor then
      -- B&W fallback
      lcd.drawFilledRectangle(self._mx, self._my, self._mw, self._mh, ERASE)
      lcd.drawRectangle(self._mx, self._my, self._mw, self._mh, SOLID)
      lcd.drawText(self._mx + 4, self._my + 4, self._title, BOLD)
      if self._loading then
        lcd.drawText(self._mx + 4, self._my + 24, SCANNING_MSG, SMLSIZE)
      elseif #self._items == 0 then
        lcd.drawText(self._mx + 4, self._my + 24, NORESULT_MSG, SMLSIZE)
      else
        self._list:render()
      end
      return
    end

    -- Dark overlay
    if self._overlay then
      lcd.setColor(CUSTOM_COLOR, C_overlay)
      lcd.drawFilledRectangle(0, 0, LCD_W, LCD_H, CUSTOM_COLOR)
    end

    -- Drop shadow (pre-computed)
    lcd.setColor(CUSTOM_COLOR, C_overlay)
    lcd.drawFilledRectangle(self._shX, self._shY, self._mw, self._mh, CUSTOM_COLOR)

    -- Modal background
    lcd.setColor(CUSTOM_COLOR, C_header)
    lcd.drawFilledRectangle(self._mx, self._my, self._mw, self._mh, CUSTOM_COLOR)

    -- Border
    lcd.setColor(CUSTOM_COLOR, C_panel)
    lcd.drawRectangle(self._mx, self._my, self._mw, self._mh, CUSTOM_COLOR)

    -- Title bar
    lcd.setColor(CUSTOM_COLOR, self._titleBg)
    lcd.drawFilledRectangle(self._mx, self._my, self._mw, self._titleH, CUSTOM_COLOR)
    lcd.setColor(CUSTOM_COLOR, C_text)
    lcd.drawText(self._titleTextX, self._titleTextY, self._title, F_BODY_CC)

    -- Content area
    if self._loading then
      -- Center spinner + label as a group vertically in content area
      local r      = self._spinner.r
      local groupH = r * 2 + SPINNER_GAP + FH_small
      local spinCy = self._contentY0 + math.floor((self._contentH0 - groupH) / 2) + r
      self._spinner.cx = self._mx + math.floor(self._mw / 2)
      self._spinner.cy = spinCy
      self._spinner:render()
      lcd.setColor(CUSTOM_COLOR, C_subtext)
      lcd.drawText(self._scanLabelX, spinCy + r + SPINNER_GAP, SCANNING_MSG, F_SMALL_CC)

    elseif #self._items == 0 then
      lcd.setColor(CUSTOM_COLOR, C_subtext)
      lcd.drawText(self._emptyTX, self._emptyCY, NORESULT_MSG, F_SMALL_CC)

    else
      self._list:render()
    end
  end

  return PickModal
end
