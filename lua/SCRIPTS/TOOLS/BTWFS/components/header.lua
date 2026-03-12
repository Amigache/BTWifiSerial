-- components/header.lua
-- Header bar component.
-- Renders a solid background bar + accent line at the bottom + a title Label.
--
-- Usage:
--   local Header = loadScript(BASE .. "/components/header.lua")(ctx)
--   local hdr    = Header.new({ title = "BTWifiSerial" })
--   hdr:render()
--
-- Props (all optional):
--   title   string   text shown inside the bar        (default "")
--   x       number   left edge                        (default 0)
--   y       number   top edge                         (default 0)
--   w       number   width                            (default LCD_W)
--   h       number   bar height in pixels             (default scale.sy(50))
--   bgColor rgb      background color                 (default theme.C.header)
--   accentColor rgb  accent line color                (default theme.C.accent)
--   accentH number   accent line height               (default max(1,scale.sy(2)))
--   titleFont flag   font for title label             (default theme.F.title)
--   titleColor rgb   color for title text             (default theme.C.text)
--   paddingX number  horizontal text padding          (default scale.sx(15))

return function(ctx)
  local theme = ctx.theme
  local scale = ctx.scale
  local Label = ctx.Label   -- already instantiated Label class

  local Header = {}
  Header.__index = Header

  function Header.new(props)
    local self         = setmetatable({}, Header)
    self.title         = props.title      or ""
    self.x             = props.x          or 0
    self.y             = props.y          or 0
    self.w             = props.w          or scale.W
    self.h             = props.h          or scale.sy(50)
    self.bgColor       = props.bgColor    or theme.C.header
    self.accentColor   = props.accentColor or theme.C.accent
    self.accentH       = props.accentH    or math.max(1, scale.sy(2))
    self.paddingX      = props.paddingX   or scale.sx(15)

    -- Vertical center of text inside bar.
    -- theme.FH.title is the actual rendered font height in pixels (fixed, not scaled).
    -- titleOffset allows manual per-design fine-tuning if needed (default 0).
    local font    = props.titleFont   or theme.F.title
    local color   = props.titleColor  or theme.C.text
    local textH   = theme.FH.title
    local offset  = props.titleOffset or -scale.sy(2)
    local textY   = self.y + math.floor((self.h - textH) / 2) + offset

    self._label = Label.new({
      text  = self.title,
      x     = self.x + self.paddingX,
      y     = textY,
      font  = font,
      color = color,
    })

    return self
  end

  -- Update title text at runtime (e.g. show current page name)
  function Header:setTitle(t)
    self.title = t
    self._label:setText(t)
    return self
  end

  function Header:render()
    -- Background bar
    if theme.isColor then
      lcd.setColor(CUSTOM_COLOR, self.bgColor)
      lcd.drawFilledRectangle(self.x, self.y, self.w, self.h, CUSTOM_COLOR)
    else
      lcd.drawFilledRectangle(self.x, self.y, self.w, self.h, 0)
    end

    -- Title label
    self._label:render()

    -- Accent line at the bottom of the bar
    local lineY = self.y + self.h
    if theme.isColor then
      lcd.setColor(CUSTOM_COLOR, self.accentColor)
      lcd.drawFilledRectangle(self.x, lineY, self.w, self.accentH, CUSTOM_COLOR)
    else
      lcd.drawLine(self.x, lineY, self.x + self.w - 1, lineY, SOLID, 0)
    end
  end

  return Header
end
