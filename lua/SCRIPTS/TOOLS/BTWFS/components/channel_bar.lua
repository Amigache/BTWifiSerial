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

    return self
  end

  function ChannelBar:setValue(v)
    self.value = v
  end

  function ChannelBar:render()
    local v   = self.value
    local by  = self.y + self._barOff
    local ty  = self.y + self._txtOff
    local bw  = self._barW
    local bx  = self._barX
    local bh  = self.barH

    -- Percentage: map value from [min..max] to [-100..+100]
    local range = self.max - self.min
    local pct = 0
    if range > 0 then
      pct = math.floor((v - self.min) / range * 200 - 100 + 0.5)
    end

    if theme.isColor then
      -- Left label
      lcd.setColor(CUSTOM_COLOR, self.textColor)
      lcd.drawText(self.x, ty, self.label, theme.F.small + CUSTOM_COLOR)

      -- Bar background
      lcd.setColor(CUSTOM_COLOR, self.bgColor)
      lcd.drawFilledRectangle(bx, by, bw, bh, CUSTOM_COLOR)

      -- Center line
      local cx = bx + math.floor(bw / 2)
      lcd.setColor(CUSTOM_COLOR, self.centerColor)
      lcd.drawFilledRectangle(cx, by, self._centerW, bh, CUSTOM_COLOR)

      -- Value fill (from center)
      local halfW = math.floor(bw / 2)
      -- Normalise v into -1..+1 range
      local norm = 0
      if range > 0 then
        norm = (v - self.min) / range * 2 - 1  -- -1 to +1
      end

      lcd.setColor(CUSTOM_COLOR, self.barColor)
      if norm > 0 then
        local maxFw = halfW - self._centerW
        local fw = math.min(math.floor(norm * halfW), maxFw)
        if fw > 0 then
          lcd.drawFilledRectangle(cx + self._centerW, by, fw, bh, CUSTOM_COLOR)
        end
      elseif norm < 0 then
        local fw = math.floor(-norm * halfW)
        if fw > 0 then
          lcd.drawFilledRectangle(cx - fw, by, fw, bh, CUSTOM_COLOR)
        end
      end

      -- Percentage text
      lcd.setColor(CUSTOM_COLOR, self.textColor)
      lcd.drawText(self._pctX, ty, pct .. "%", theme.F.small + CUSTOM_COLOR)
    else
      -- B&W fallback: just text
      lcd.drawText(self.x, ty, self.label, theme.F.small)
      lcd.drawText(self._pctX, ty, pct .. "%", theme.F.small)
    end
  end

  return ChannelBar
end
