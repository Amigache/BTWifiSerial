-- components/page_title.lua
-- Page title band: full-width grayish bar with a centered, uppercase label.
-- Sits immediately below the Header (or at y=0 if no header).
-- Mirrors the drawPageHeader() style from the original BTWifiSerial script.
--
-- Props (all optional):
--   text     string   title text (auto-uppercased)   (default "")
--   x, y     number   position                       (default 0, contentY)
--   w        number   width                          (default LCD_W)
--   h        number   bar height                     (default scale.sy(38))
--   bgColor  rgb      background                     (default theme.C.panel)
--   color    rgb      text color                     (default theme.C.text)
--   font     flag     text font                      (default theme.F.body)

return function(ctx)
  local theme = ctx.theme
  local scale = ctx.scale

  -- Cache theme values
  local isColor = theme.isColor

  -- Pre-compute B&W padding (avoid per-frame scale calls)
  local BW_PAD_X = scale.sx(4)
  local BW_PAD_Y = scale.sy(4)

  local PageTitle = {}
  PageTitle.__index = PageTitle

  local function computeTextPos(self)
    if lcd.sizeText then
      local tw = lcd.sizeText(self.text, self.font)
      self._tx = self.x + ((tw > 0) and math.floor((self.w - tw) / 2) or scale.sx(15))
    else
      self._tx = self.x + scale.sx(15)
    end
    self._ty = self.y + math.floor((self.h - theme.FH.body) / 2) - scale.sy(4)
  end

  function PageTitle.new(props)
    local self    = setmetatable({}, PageTitle)
    self.text     = string.upper(props.text or "")
    self.x        = props.x       or 0
    self.y        = props.y       or 0
    self.w        = props.w       or scale.W
    self.h        = props.h       or scale.sy(38)
    self.bgColor  = props.bgColor or theme.C.panel
    self.color    = props.color   or theme.C.text
    self.font     = props.font    or theme.F.body
    self._fontCC  = self.font + CUSTOM_COLOR
    computeTextPos(self)
    return self
  end

  function PageTitle:setText(t)
    self.text = string.upper(t)
    computeTextPos(self)
    return self
  end

  function PageTitle:render()
    -- Background band
    if isColor then
      lcd.setColor(CUSTOM_COLOR, self.bgColor)
      lcd.drawFilledRectangle(self.x, self.y, self.w, self.h, CUSTOM_COLOR)
    else
      -- B&W: just draw the text, no fill (padding pre-computed)
      lcd.drawText(self.x + BW_PAD_X, self.y + BW_PAD_Y, self.text, self.font + BOLD)
      return
    end

    lcd.setColor(CUSTOM_COLOR, self.color)
    lcd.drawText(self._tx, self._ty, self.text, self._fontCC)
  end

  return PageTitle
end
