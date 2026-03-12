-- components/list.lua
-- Multi-column list with optional inline edit mode.
--
-- Props:
--   x, y        number   top-left position              (default scale.sx(17), 0)
--   w           number   total row width                (default scale.W - 2*PAD)
--   h           number   total list height              (default scale.H - y)
--   rowH        number   height of each row             (default scale.sy(32))
--   font        flag     text font flag                  (default theme.F.small)
--   fontH       number   font pixel height               (default theme.FH.small)
--   padX        number   left/right text padding        (default scale.sx(7))
--   selectable  bool     enable row selection           (default false)
--   showScroll  bool     show scroll arrows column      (default true)
--   editCol     number   column index whose value is editable (default nil=no edit)
--   onEdit      function callback(row, colKey, oldVal, newVal) fired on confirm
--   rows        array    data tables, one per row
--   cols        array    column definitions
--   maxVisible  number   max rows shown (default h/rowH)
--
-- Column:  { key, xFrac, align }
--   key     field name in row table
--   xFrac   fraction of w where column starts (0 = first col, uses padX)
--   align   "left" | "right"
--
-- Edit mode (when editCol is set):
--   Each row may carry a _options = {...} array for the editable column.
--   Rows without _options are skipped (ENTER does nothing on them).
--   - Row highlight changes to theme.C.editBg (dimmer blue)
--   - The editCol column blinks (BLINK flag)
--   - Scroll wheel cycles through row._options[]
--   - ENTER confirms; EXIT (RTN) cancels
--   - onEdit callback fires on confirm with changed value

return function(ctx)
  local theme    = ctx.theme
  local scale    = ctx.scale

  local PAD      = scale.sx(17)
  local SCROLL_W = scale.sx(28)   -- right column reserved for arrows
  local ROW_GAP  = scale.sx(8)    -- gap between last row pixel and arrow area
  local ARROW_SZ = scale.sy(5)    -- half-base of triangle (sz in reference)

  -- ── Triangle helper (mirrors reference drawTri) ────────────────
  local function drawTri(cx, cy, sz, up, color)
    if theme.isColor then
      lcd.setColor(CUSTOM_COLOR, color)
    end
    local fl = theme.isColor and CUSTOM_COLOR or 0
    for i = 0, sz do
      local hw = up and i or (sz - i)
      lcd.drawLine(cx - hw, cy + i, cx + hw, cy + i, SOLID, fl)
    end
  end

  -- ── Truncate text to fit maxW pixels ───────────────────────────
  local function fitText(s, font, maxW)
    if not lcd.sizeText or maxW <= 0 then return s end
    if lcd.sizeText(s, font) <= maxW then return s end
    local ew = lcd.sizeText("...", font)
    while #s > 0 and lcd.sizeText(s, font) + ew > maxW do
      s = string.sub(s, 1, #s - 1)
    end
    return s .. "..."
  end

  -- ── Event helpers ──────────────────────────────────────────────
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
  local function evEnter(e)
    return (EVT_VIRTUAL_ENTER ~= nil and e == EVT_VIRTUAL_ENTER)
        or (EVT_ENTER_BREAK   ~= nil and e == EVT_ENTER_BREAK)
  end
  local function evExit(e)
    return (EVT_VIRTUAL_EXIT ~= nil and e == EVT_VIRTUAL_EXIT)
        or (EVT_EXIT_BREAK   ~= nil and e == EVT_EXIT_BREAK)
  end

  local List = {}
  List.__index = List

  function List.new(props)
    local self      = setmetatable({}, List)
    self.x          = props.x          or PAD
    self.y          = props.y          or 0
    self.w          = props.w          or (scale.W - 2 * PAD)
    self.h          = props.h          or (scale.H - self.y)
    self.rowH       = props.rowH       or scale.sy(32)
    self.font       = props.font       or theme.F.small
    self.fontH      = props.fontH      or theme.FH.small
    self.padX       = props.padX       or scale.sx(7)
    self.selectable = props.selectable or false
    self._showScroll = (props.showScroll ~= false)   -- default true
    self._editCol   = props.editCol                  -- column index for inline edit (nil=disabled)
    self._onEdit    = props.onEdit                   -- callback(row, key, oldVal, newVal)
    self._rowBg     = props.rowBg                    -- custom non-selected row bg (nil=theme.C.bg)
    self.rows       = props.rows       or {}
    self.cols       = props.cols       or {}

    -- Content width (excluding scroll arrow column when visible)
    self._contentW = self._showScroll and (self.w - SCROLL_W) or self.w
    -- Row fill width: leaves a gap before the arrows
    self._rowFillW = self._showScroll and (self._contentW - ROW_GAP) or self.w

    self.maxVisible = props.maxVisible or math.floor(self.h / self.rowH)

    -- Vertical text offset inside row: centre + leading correction
    self._txtOff = math.floor((self.rowH - self.fontH) / 2) - scale.sy(3)

    -- Selection & edit state
    self._sel      = 1
    self._offset   = 0
    self._editing  = false   -- true while in inline-edit mode
    self._editOrig = nil     -- original value before editing (for cancel)

    -- Arrow horizontal centre
    self._arrowX = self.x + self._contentW + math.floor(SCROLL_W / 2)

    -- Number of columns (cached for render loop)
    self._numCols = #self.cols

    return self
  end

  -- ── Column x helper (called in render for each column) ─────────
  -- Returns colX, colMaxW for a given column index.
  function List:_colPos(ci)
    local col  = self.cols[ci]
    local frac = col.xFrac or 0
    local cx

    -- Compute column start x
    if frac > 0 then
      cx = self.x + math.floor(self._contentW * frac)
    else
      cx = self.x + self.padX
    end

    -- Compute max text width: from cx to next column start (or row end)
    local endX
    if ci < self._numCols then
      local nf = self.cols[ci + 1].xFrac or 0
      if nf > 0 then
        endX = self.x + math.floor(self._contentW * nf) - self.padX
      else
        endX = self.x + self.padX
      end
    else
      endX = self.x + self._rowFillW - self.padX
    end

    return cx, math.max(0, endX - cx)
  end

  -- ── Selection API ──────────────────────────────────────────────
  function List:setSel(n)
    local total = math.max(1, #self.rows)
    n = math.max(1, math.min(n, total))
    self._sel = n
    if n <= self._offset then
      self._offset = n - 1
    elseif n > self._offset + self.maxVisible then
      self._offset = n - self.maxVisible
    end
  end

  function List:getSel()      return self.rows[self._sel] end
  function List:getSelIdx()   return self._sel end
  function List:isEditing()   return self._editing end

  -- Enter edit mode on the current row
  function List:_startEdit()
    if not self._editCol then return false end
    local row = self.rows[self._sel]
    if not row or not row._options then return false end
    self._editing  = true
    local col = self.cols[self._editCol]
    self._editOrig = row[col.key]  -- save for cancel
    return true
  end

  -- Confirm edit: fire callback, exit edit mode
  function List:_confirmEdit()
    self._editing = false
    if not self._editCol then return end
    local col = self.cols[self._editCol]
    local row = self.rows[self._sel]
    if not row or not col then return end
    local cur = row[col.key]
    if cur ~= self._editOrig and self._onEdit then
      self._onEdit(row, col.key, self._editOrig, cur)
    end
    self._editOrig = nil
  end

  -- Cancel edit: revert value, exit edit mode
  function List:_cancelEdit()
    if self._editCol then
      local col = self.cols[self._editCol]
      local row = self.rows[self._sel]
      if row and col and self._editOrig ~= nil then
        row[col.key] = self._editOrig
      end
    end
    self._editing  = false
    self._editOrig = nil
  end

  -- Cycle the edit column value +1 or -1 within row._options[]
  function List:_cycleEdit(dir)
    if not self._editCol then return end
    local col = self.cols[self._editCol]
    if not col then return end
    local row = self.rows[self._sel]
    if not row or not row._options then return end
    local cur = row[col.key]
    -- Find current index in row._options
    local ci = 1
    for i, v in ipairs(row._options) do
      if v == cur then ci = i; break end
    end
    ci = ci + dir
    if ci > #row._options then ci = 1 end
    if ci < 1 then ci = #row._options end
    row[col.key] = row._options[ci]
  end

  function List:handleEvent(event)
    if not self.selectable then
      -- Purely informative list: scroll the viewport directly, no row highlight.
      local total   = #self.rows
      local maxOff  = math.max(0, total - self.maxVisible)
      if maxOff == 0 then return false end
      if evNext(event) then
        self._offset = math.min(self._offset + 1, maxOff)
        return true
      elseif evPrev(event) then
        self._offset = math.max(self._offset - 1, 0)
        return true
      end
      return false
    end

    if self._editing then
      -- In edit mode: scroll cycles options, ENTER confirms, EXIT cancels
      if evNext(event) then
        self:_cycleEdit(1)
        return true
      elseif evPrev(event) then
        self:_cycleEdit(-1)
        return true
      elseif evEnter(event) then
        self:_confirmEdit()
        return true
      elseif evExit(event) then
        self:_cancelEdit()
        return true
      end
      return false
    end

    -- Normal mode: scroll moves selection, ENTER starts edit
    if evNext(event) then self:setSel(self._sel + 1); return true end
    if evPrev(event) then self:setSel(self._sel - 1); return true end
    if evEnter(event) then return self:_startEdit() end
    return false
  end

  -- ── Render ─────────────────────────────────────────────────────
  function List:render()
    local total  = #self.rows
    local last   = math.min(self._offset + self.maxVisible, total)
    local canUp  = self._offset > 0
    local canDn  = self._offset + self.maxVisible < total

    -- Rows
    for slot = self._offset + 1, last do
      local row   = self.rows[slot]
      local ry    = self.y + (slot - 1 - self._offset) * self.rowH
      local isSel = self.selectable and (slot == self._sel)

      -- Row background: editBg when editing this row, accent when selected, bg otherwise
      local isEditingThis = self._editing and isSel
      if theme.isColor then
        if isEditingThis then
          lcd.setColor(CUSTOM_COLOR, theme.C.editBg)
        elseif isSel then
          lcd.setColor(CUSTOM_COLOR, theme.C.accent)
        else
          lcd.setColor(CUSTOM_COLOR, self._rowBg or theme.C.bg)
        end
        lcd.drawFilledRectangle(self.x, ry, self._rowFillW, self.rowH, CUSTOM_COLOR)
      else
        if isSel then
          lcd.drawFilledRectangle(self.x, ry, self._rowFillW, self.rowH, FORCE)
        end
      end

      -- Columns — position computed fresh per column via _colPos
      local ty = ry + self._txtOff
      for ci = 1, self._numCols do
        local col       = self.cols[ci]
        local cx, maxW  = self:_colPos(ci)
        local raw       = tostring(row[col.key] or "")
        local val       = fitText(raw, self.font, maxW)

        -- Determine text flags: add BLINK on the edit column while editing
        local isBlinkCol = isEditingThis and (ci == self._editCol)
        if theme.isColor then
          lcd.setColor(CUSTOM_COLOR, theme.C.text)
          if col.align == "right" then
            local tw = (lcd.sizeText and lcd.sizeText(val, self.font)) or 0
            cx = cx + maxW - tw
          end
          local fl = self.font + CUSTOM_COLOR
          if isBlinkCol then fl = fl + BLINK end
          lcd.drawText(cx, ty, val, fl)
        else
          local fl = isSel and INVERS or 0
          if isBlinkCol then fl = fl + BLINK end
          lcd.drawText(cx, ty, val, self.font + fl)
        end
      end
    end

    -- Scroll arrows (only when showScroll=true).
    if self._showScroll then
      local sz     = ARROW_SZ
      local margin = math.floor((self.rowH - sz) / 2)
      local ax     = self._arrowX

      local upColor = canUp and theme.C.accent or theme.C.panel
      local dnColor = canDn and theme.C.accent or theme.C.panel

      local dnY = self.y + self.h - self.rowH + margin - scale.sy(6)
      drawTri(ax, self.y + margin, sz, true,  upColor)
      drawTri(ax, dnY,             sz, false, dnColor)
    end
  end

  -- Update rows at runtime
  function List:setRows(rows)
    self.rows    = rows
    self._sel    = math.min(self._sel, math.max(1, #rows))
    self._offset = 0
    -- Adjust offset so current selection remains visible
    if self._sel > self.maxVisible then
      self._offset = self._sel - self.maxVisible
    end
  end

  return List
end