-- components/loading.lua
-- Animated loading spinner using rotating line segments.
-- EdgeTX Lua has no real animation timer, so we advance a frame
-- counter each time render() is called (≈ once per run cycle).
--
-- Props:
--   cx, cy      number   center of spinner               (required)
--   r           number   radius                          (default scale.s(16))
--   color       rgb      line color                      (default theme.C.text)
--   segments    number   total segments                  (default 8)
--   tailLen     number   number of lit segments          (default 3)
--   lineW       number   line weight                     (default 2)

return function(ctx)
  local theme = ctx.theme
  local scale = ctx.scale

  -- Cache theme values
  local isColor = theme.isColor
  local C_panel = theme.C.panel

  local Loading = {}
  Loading.__index = Loading

  function Loading.new(props)
    local self     = setmetatable({}, Loading)
    self.cx        = props.cx       or math.floor(LCD_W / 2)
    self.cy        = props.cy       or math.floor(LCD_H / 2)
    self.r         = props.r        or scale.s(16)
    self.color     = props.color    or theme.C.text
    self.segments  = props.segments or 8
    self.tailLen   = props.tailLen  or 3
    self.lineW     = props.lineW    or 2
    self._frame    = 0
    self._speed    = 3   -- advance every N render calls
    self._tick     = 0

    -- Pre-compute segment endpoints (avoids cos/sin/floor every frame)
    local seg  = self.segments
    local step = 2 * math.pi / seg
    local rIn  = math.floor(self.r * 0.4)
    local rOut = self.r
    local geo  = {}
    for i = 0, seg - 1 do
      local angle = i * step - math.pi / 2
      geo[i] = {
        math.floor(self.cx + math.cos(angle) * rIn),
        math.floor(self.cy + math.sin(angle) * rIn),
        math.floor(self.cx + math.cos(angle) * rOut),
        math.floor(self.cy + math.sin(angle) * rOut),
      }
    end
    self._geo = geo

    -- Pre-compute tail alpha colors
    if theme.isColor then
      local tailColors = {}
      for d = 0, self.tailLen - 1 do
        local alpha = math.floor(255 * (self.tailLen - d) / self.tailLen)
        tailColors[d] = lcd.RGB(alpha, alpha, alpha)
      end
      self._tailColors = tailColors
    end

    return self
  end

  function Loading:render()
    -- Advance frame counter
    self._tick = self._tick + 1
    if self._tick >= self._speed then
      self._tick = 0
      self._frame = (self._frame + 1) % self.segments
    end

    local seg   = self.segments
    local geo   = self._geo
    local frame = self._frame
    local tLen  = self.tailLen

    for i = 0, seg - 1 do
      local g = geo[i]
      local dist = (frame - i) % seg
      local lit  = dist < tLen

      if isColor then
        if lit then
          lcd.setColor(CUSTOM_COLOR, self._tailColors[dist])
        else
          lcd.setColor(CUSTOM_COLOR, C_panel)
        end
        lcd.drawLine(g[1], g[2], g[3], g[4], SOLID, CUSTOM_COLOR)
      else
        lcd.drawLine(g[1], g[2], g[3], g[4], SOLID, lit and FORCE or 0)
      end
    end
  end

  return Loading
end
