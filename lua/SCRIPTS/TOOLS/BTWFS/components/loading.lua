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
    return self
  end

  function Loading:render()
    -- Advance frame counter
    self._tick = self._tick + 1
    if self._tick >= self._speed then
      self._tick = 0
      self._frame = (self._frame + 1) % self.segments
    end

    local seg  = self.segments
    local step = 2 * math.pi / seg
    local rIn  = math.floor(self.r * 0.4)
    local rOut = self.r

    for i = 0, seg - 1 do
      local angle = i * step - math.pi / 2   -- start from top
      local x1 = self.cx + math.floor(math.cos(angle) * rIn)
      local y1 = self.cy + math.floor(math.sin(angle) * rIn)
      local x2 = self.cx + math.floor(math.cos(angle) * rOut)
      local y2 = self.cy + math.floor(math.sin(angle) * rOut)

      -- Determine brightness: segments in the tail are lit
      local dist = (self._frame - i) % seg
      local lit  = dist < self.tailLen

      if theme.isColor then
        if lit then
          -- Fade: closer to head = brighter
          local alpha = math.floor(255 * (self.tailLen - dist) / self.tailLen)
          lcd.setColor(CUSTOM_COLOR, lcd.RGB(alpha, alpha, alpha))
        else
          lcd.setColor(CUSTOM_COLOR, theme.C.panel)
        end
        lcd.drawLine(x1, y1, x2, y2, SOLID, CUSTOM_COLOR)
      else
        local fl = lit and FORCE or 0
        lcd.drawLine(x1, y1, x2, y2, SOLID, fl)
      end
    end
  end

  return Loading
end
