-- components/status_dot.lua
-- A filled dot + label pair for status indicators.
--
-- Usage:
--   local sd = StatusDot.new({ label="BLE", x=10, y=5 })
--   sd:setColor(theme.C.green)  -- update state colour
--   sd:render()
--
-- Props:
--   label   string   text shown to the right of the dot  (required)
--   x       number   left edge of the dot                (required)
--   y       number   vertical centre of the dot          (required)
--   r       number   dot radius                          (default scale.s(4))
--   color   rgb      initial dot fill color              (default theme.C.subtext)

return function(ctx)
  local theme = ctx.theme
  local scale = ctx.scale

  local DOT_GAP = scale.sx(5)   -- gap between right edge of dot and label text

  local StatusDot = {}
  StatusDot.__index = StatusDot

  function StatusDot.new(props)
    local self   = setmetatable({}, StatusDot)
    self._label  = props.label or ""
    self._x      = props.x
    self._y      = props.y
    self._r      = props.r or scale.s(4)
    self._color  = props.color or theme.C.subtext
    return self
  end

  function StatusDot:setColor(c)
    self._color = c
  end

  function StatusDot:setLabel(lbl)
    self._label = lbl
  end

  function StatusDot:render()
    if not theme.isColor then
      lcd.drawText(self._x, self._y, self._label, SMLSIZE)
      return
    end

    local r  = self._r
    local cx = self._x + r
    -- _y = footer ty, which already has -sy(3) baked in for text rendering offset.
    -- Add FH.small/2 + sy(3) so the circle lands at the true footer centre (same
    -- as the text optical centre), cancelling the -sy(3) shift in ty.
    local cy = self._y + math.floor(theme.FH.small / 2) + scale.sy(3)

    -- Filled circle
    lcd.setColor(CUSTOM_COLOR, self._color)
    lcd.drawFilledCircle(cx, cy, r, CUSTOM_COLOR)

    -- Label text to the right, top-aligned with _y (same as footer ty)
    local tx = self._x + r * 2 + DOT_GAP
    lcd.setColor(CUSTOM_COLOR, theme.C.subtext)
    lcd.drawText(tx, self._y, self._label, theme.F.small + CUSTOM_COLOR)
  end

  -- Returns total pixel width of the component (dot diameter + gap + text)
  function StatusDot:width()
    local tw = (lcd.sizeText and lcd.sizeText(self._label, theme.F.small)) or
               (#self._label * 7)
    return self._r * 2 + DOT_GAP + tw
  end

  return StatusDot
end
