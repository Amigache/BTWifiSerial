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

  -- Cache theme values
  local isColor    = theme.isColor
  local C_subtext  = theme.C.subtext
  local F_small    = theme.F.small
  local F_SMALL_CC = F_small + CUSTOM_COLOR

  local DOT_GAP = scale.sx(5)   -- gap between right edge of dot and label text

  local StatusDot = {}
  StatusDot.__index = StatusDot

  local function computeWidth(self)
    local tw = (lcd.sizeText and lcd.sizeText(self._label, F_small)) or
               (#self._label * 7)
    self._w = self._r * 2 + DOT_GAP + tw
  end

  function StatusDot.new(props)
    local self   = setmetatable({}, StatusDot)
    self._label  = props.label or ""
    self._x      = props.x
    self._y      = props.y
    self._r      = props.r or scale.s(4)
    self._color  = props.color or theme.C.subtext
    computeWidth(self)
    return self
  end

  function StatusDot:setColor(c)
    self._color = c
  end

  function StatusDot:setLabel(lbl)
    self._label = lbl
    computeWidth(self)
  end

  local _cyOff = math.floor(theme.FH.small / 2) + scale.sy(3)

  function StatusDot:render()
    if not isColor then
      lcd.drawText(self._x, self._y, self._label, SMLSIZE)
      return
    end

    local r  = self._r
    local cx = self._x + r
    local cy = self._y + _cyOff

    -- Filled circle
    lcd.setColor(CUSTOM_COLOR, self._color)
    lcd.drawFilledCircle(cx, cy, r, CUSTOM_COLOR)

    -- Label text to the right
    lcd.setColor(CUSTOM_COLOR, C_subtext)
    lcd.drawText(self._x + r * 2 + DOT_GAP, self._y, self._label, F_SMALL_CC)
  end

  -- Returns total pixel width of the component (dot diameter + gap + text)
  function StatusDot:width()
    return self._w
  end

  return StatusDot
end
