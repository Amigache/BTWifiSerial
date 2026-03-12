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

  local PageTitle = {}
  PageTitle.__index = PageTitle

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
    return self
  end

  function PageTitle:setText(t)
    self.text = string.upper(t)
    return self
  end

  function PageTitle:render()
    -- Background band
    if theme.isColor then
      lcd.setColor(CUSTOM_COLOR, self.bgColor)
      lcd.drawFilledRectangle(self.x, self.y, self.w, self.h, CUSTOM_COLOR)
    else
      -- B&W: just draw the text, no fill
      lcd.drawText(self.x + scale.sx(4), self.y + scale.sy(4), self.text, self.font + BOLD)
      return
    end

    -- Centered text.
    -- The -scale.sy(3) is an empirical correction: EdgeTX body glyphs
    -- have internal leading that shifts them visually below the true bbox top.
    local tw = (lcd.sizeText and lcd.sizeText(self.text, self.font)) or 0
    local tx = (tw > 0) and math.floor((self.w - tw) / 2) or scale.sx(15)
    local textH = theme.FH.body
    local ty    = self.y + math.floor((self.h - textH) / 2) - scale.sy(4)

    lcd.setColor(CUSTOM_COLOR, self.color)
    lcd.drawText(self.x + tx, ty, self.text, self.font + CUSTOM_COLOR)
  end

  return PageTitle
end
