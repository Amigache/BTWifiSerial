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

  -- Cache theme values as locals (avoid 2-level hash lookups per frame)
  local isColor  = theme.isColor
  local C_text   = theme.C.text
  local C_accent = theme.C.accent
  local C_bg     = theme.C.bg
  local C_editBg = theme.C.editBg
  local C_panel  = theme.C.panel
  local F_small  = theme.F.small

  -- Pre-computed font + flag combos
  local F_SMALL_CC = F_small + CUSTOM_COLOR
  local TRI_FL     = isColor and CUSTOM_COLOR or 0

  local PAD      = scale.sx(17)
  local SCROLL_W = scale.sx(28)   -- right column reserved for arrows
  local ROW_GAP  = scale.sx(8)    -- gap between last row pixel and arrow area
  local ARROW_SZ = scale.sy(5)    -- half-base of triangle (sz in reference)

  -- ── Triangle helper (mirrors reference drawTri) ────────────────
  local function drawTri(cx, cy, sz, up, color)
    if isColor then
      lcd.setColor(CUSTOM_COLOR, color)
    end
    for i = 0, sz do
      local hw = up and i or (sz - i)
      lcd.drawLine(cx - hw, cy + i, cx + hw, cy + i, SOLID, TRI_FL)
    end
  end

  -- ── Truncate text to fit maxW pixels ───────────────────────────
  local _ellipsisW = lcd.sizeText and lcd.sizeText("...", SMLSIZE) or 0
  local function fitText(s, font, maxW)
    if not lcd.sizeText or maxW <= 0 then return s end
    if lcd.sizeText(s, font) <= maxW then return s end
    while #s > 0 and lcd.sizeText(s, font) + _ellipsisW > maxW do
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

    -- Pre-compute column positions (avoids math.floor per col per row per frame)
    self._colX = {}
    self._colMaxW = {}
    for ci = 1, self._numCols do
      local col  = self.cols[ci]
      local frac = col.xFrac or 0
      local cx
      if frac > 0 then
        cx = self.x + math.floor(self._contentW * frac)
      else
        cx = self.x + self.padX
      end
      self._colX[ci] = cx

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
      self._colMaxW[ci] = math.max(0, endX - cx)
    end

    -- Pre-extract column keys and alignment flags (avoid dot-chains per cell)
    self._colKeys    = {}
    self._colIsRight = {}
    for ci = 1, self._numCols do
      self._colKeys[ci]    = self.cols[ci].key
      self._colIsRight[ci] = (self.cols[ci].align == "right")
    end

    -- Pre-compute scroll arrow positions (all immutable)
    local arrowMargin     = math.floor((self.rowH - ARROW_SZ) / 2)
    self._upArrowY        = self.y + arrowMargin
    self._dnArrowY        = self.y + self.h - self.rowH + arrowMargin - scale.sy(6)

    -- Pre-computed font+flag combo for render
    self._fontCC  = self.font + CUSTOM_COLOR

    -- Render cache: fitted text per cell, invalidated by setRows()
    self._rcache  = {}

    return self
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
    -- Invalidate render cache for this row
    self._rcache[self._sel] = nil
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

    local colKeys    = self._colKeys
    local colIsRight = self._colIsRight
    local colX       = self._colX
    local colMaxW    = self._colMaxW
    local numCols    = self._numCols
    local fontCC     = self._fontCC
    local font       = self.font
    local rcache     = self._rcache
    local rowBg      = self._rowBg or C_bg

    -- Rows
    for slot = self._offset + 1, last do
      local row   = self.rows[slot]
      local ry    = self.y + (slot - 1 - self._offset) * self.rowH
      local isSel = self.selectable and (slot == self._sel)

      -- Row background
      local isEditingThis = self._editing and isSel
      if isColor then
        if isEditingThis then
          lcd.setColor(CUSTOM_COLOR, C_editBg)
        elseif isSel then
          lcd.setColor(CUSTOM_COLOR, C_accent)
        else
          lcd.setColor(CUSTOM_COLOR, rowBg)
        end
        lcd.drawFilledRectangle(self.x, ry, self._rowFillW, self.rowH, CUSTOM_COLOR)
      else
        if isSel then
          lcd.drawFilledRectangle(self.x, ry, self._rowFillW, self.rowH, FORCE)
        end
      end

      -- Columns — use render cache for fitted text
      local ty = ry + self._txtOff
      if isColor then
        lcd.setColor(CUSTOM_COLOR, C_text)  -- set once per row, not per column
      end
      for ci = 1, numCols do
        local cx   = colX[ci]
        local maxW = colMaxW[ci]

        -- Lookup cached fitted text; build on miss
        local rowCache = rcache[slot]
        local val
        if rowCache then
          val = rowCache[ci]
        end
        if not val then
          local raw = tostring(row[colKeys[ci]] or "")
          val = fitText(raw, font, maxW)
          if not rowCache then rowCache = {}; rcache[slot] = rowCache end
          rowCache[ci] = val
        end

        local isBlinkCol = isEditingThis and (ci == self._editCol)
        if isColor then
          if colIsRight[ci] then
            local tw = (lcd.sizeText and lcd.sizeText(val, font)) or 0
            cx = cx + maxW - tw
          end
          local fl = fontCC
          if isBlinkCol then fl = fl + BLINK end
          lcd.drawText(cx, ty, val, fl)
        else
          local fl = isSel and INVERS or 0
          if isBlinkCol then fl = fl + BLINK end
          lcd.drawText(cx, ty, val, font + fl)
        end
      end
    end

    -- Scroll arrows (pre-computed positions)
    if self._showScroll then
      local ax = self._arrowX
      local upColor = canUp and C_accent or C_panel
      local dnColor = canDn and C_accent or C_panel
      drawTri(ax, self._upArrowY, ARROW_SZ, true,  upColor)
      drawTri(ax, self._dnArrowY, ARROW_SZ, false, dnColor)
    end
  end

  -- Update rows at runtime
  function List:setRows(rows)
    self.rows    = rows
    self._rcache = {}  -- invalidate render cache
    self._sel    = math.min(self._sel, math.max(1, #rows))
    self._offset = 0
    -- Adjust offset so current selection remains visible
    if self._sel > self.maxVisible then
      self._offset = self._sel - self.maxVisible
    end
  end

  -- Invalidate render cache for a single row (or all rows if idx is nil)
  function List:dirtyCache(idx)
    if idx then
      self._rcache[idx] = nil
    else
      self._rcache = {}
    end
  end

  return List
end