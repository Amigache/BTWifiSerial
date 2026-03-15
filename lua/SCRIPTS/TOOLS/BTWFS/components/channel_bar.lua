-- components/channel_bar.lua
-- Horizontal bar gauge for a single channel value.
--
-- Props:
--   x, y        number   top-left position              (required)
--   w           number   total width incl. labels        (required)
--   h           number   row height                      (default scale.sy(25))
--   barH        number   bar height inside row           (default scale.sy(17))
--   labelW      number   left label area width           (default scale.sx(40))
--   pctW        number   right % text area width         (default scale.sx(50))
--   textGap     number   gap between text and bar        (default scale.sx(6))
--   min         number   min value                       (default -1024)
--   max         number   max value                       (default  1024)
--   label       string   left text, e.g. "CH1"           (default "")
--   value       number   current value                   (default 0)
--   barColor    rgb      fill color                      (default theme.C.accent)
--   bgColor     rgb      bar background                  (default theme.C.panel)
--   centerColor rgb      center line color               (default theme.C.header)
--   textColor   rgb      label and % text color          (default theme.C.text)
--
-- Read-only after construction:
--   bar.h       → total height consumed

return function(ctx)
  local theme = ctx.theme
  local scale = ctx.scale

  -- Cache theme values as locals
  local isColor    = theme.isColor
  local F_small    = theme.F.small
  local F_SMALL_CC = F_small + CUSTOM_COLOR

  local ChannelBar = {}
  ChannelBar.__index = ChannelBar

  function ChannelBar.new(props)
    local self        = setmetatable({}, ChannelBar)
    self.x            = props.x           or 0
    self.y            = props.y           or 0
    self.w            = props.w           or scale.sx(274)
    self.h            = props.h           or scale.sy(25)
    self.barH         = props.barH        or scale.sy(17)
    self.labelW       = props.labelW      or scale.sx(40)
    self.pctW         = props.pctW        or scale.sx(50)
    self.textGap      = props.textGap     or scale.sx(6)
    self.min          = props.min         or -1024
    self.max          = props.max         or 1024
    self.label        = props.label       or ""
    self.value        = props.value       or 0
    self.barColor     = props.barColor    or theme.C.accent
    self.bgColor      = props.bgColor     or theme.C.panel
    self.centerColor  = props.centerColor or theme.C.header
    self.textColor    = props.textColor   or theme.C.text

    -- Derived layout: symmetric gap between label/bar and bar/pct
    self._barX    = self.x + self.labelW + self.textGap
    self._barW    = self.w - self.labelW - self.pctW - 2 * self.textGap
    self._pctX    = self._barX + self._barW + self.textGap
    self._barOff  = math.floor((self.h - self.barH) / 2)
    self._txtOff  = math.floor((self.h - theme.FH.small) / 2) - scale.sy(3)
    self._centerW = math.max(1, scale.sx(2))
    self._cx      = self._barX + math.floor(self._barW / 2)
    self._halfW   = math.floor(self._barW / 2)
    self._maxFillW = self._halfW - self._centerW  -- constant limit for fill width
    self._pctStr  = "0%"
    self._norm    = 0

    -- Pre-compute immutable derived positions
    self._by = self.y + self._barOff
    self._ty = self.y + self._txtOff

    return self
  end

  function ChannelBar:setValue(v)
    if v == self.value then return end
    self.value = v

    -- Recompute cached derived values only when value changes
    local range = self.max - self.min
    if range > 0 then
      self._pctStr = math.floor((v - self.min) / range * 200 - 100 + 0.5) .. "%"
      self._norm   = (v - self.min) / range * 2 - 1
    else
      self._pctStr = "0%"
      self._norm   = 0
    end
  end

  function ChannelBar:render()
    local by  = self._by
    local ty  = self._ty
    local bw  = self._barW
    local bx  = self._barX
    local bh  = self.barH

    if isColor then
      -- Batch text draws: label + pct (same textColor) → one setColor
      lcd.setColor(CUSTOM_COLOR, self.textColor)
      lcd.drawText(self.x, ty, self.label, F_SMALL_CC)
      lcd.drawText(self._pctX, ty, self._pctStr, F_SMALL_CC)

      -- Bar background
      lcd.setColor(CUSTOM_COLOR, self.bgColor)
      lcd.drawFilledRectangle(bx, by, bw, bh, CUSTOM_COLOR)

      -- Center line
      local cx = self._cx
      lcd.setColor(CUSTOM_COLOR, self.centerColor)
      lcd.drawFilledRectangle(cx, by, self._centerW, bh, CUSTOM_COLOR)

      -- Value fill (from center)
      local halfW = self._halfW
      local norm  = self._norm

      lcd.setColor(CUSTOM_COLOR, self.barColor)
      if norm > 0 then
        local fw = math.min(math.floor(norm * halfW), self._maxFillW)
        if fw > 0 then
          lcd.drawFilledRectangle(cx + self._centerW, by, fw, bh, CUSTOM_COLOR)
        end
      elseif norm < 0 then
        local fw = math.floor(-norm * halfW)
        if fw > 0 then
          lcd.drawFilledRectangle(cx - fw, by, fw, bh, CUSTOM_COLOR)
        end
      end
    else
      -- B&W fallback: just text
      lcd.drawText(self.x, ty, self.label, F_small)
      lcd.drawText(self._pctX, ty, self._pctStr, F_small)
    end
  end

  return ChannelBar
end
