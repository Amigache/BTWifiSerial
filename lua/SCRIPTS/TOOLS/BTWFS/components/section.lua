-- components/section.lua
-- Section header: accent-colored title + 1px divider line below.
-- Mirrors drawSubHeader() from the reference script.
--
-- Props:
--   title      string   section label text           (required)
--   x          number   left edge                    (default scale.sx(17))
--   y          number   top of the title text        (required / default 0)
--   w          number   line width                   (default LCD_W - 2*paddingX)
--   titleColor rgb      title text color             (default theme.C.accent)
--   lineColor  rgb      divider line color           (default theme.C.panel)
--   font       flag     title font                   (default theme.F.body)
--   gapTL      number   gap from title-bottom to line (default scale.sy(14))
--   gapLC      number   gap from line to content      (default scale.sy(12))
--   marginTop  number   extra space above the title    (default 0)
--
-- Read-only after construction:
--   section.contentY  → first Y pixel below the divider (ready for content)
--   section.h         → total height consumed (title + gapTL + line + gapLC)

return function(ctx)
  local theme = ctx.theme
  local scale = ctx.scale

  local isColor = theme.isColor
  local PAD = scale.sx(17)

  local Section = {}
  Section.__index = Section

  function Section.new(props)
    local self       = setmetatable({}, Section)
    self.title       = props.title      or ""
    self.x           = props.x          or PAD
    self.y           = props.y          or 0
    self.w           = props.w          or (scale.W - 2 * PAD)
    self.titleColor  = props.titleColor or theme.C.accent
    self.lineColor   = props.lineColor  or theme.C.panel
    self.font        = props.font       or theme.F.body
    self.gapTL       = props.gapTL      or scale.sy(14)  -- title-bottom → line
    self.gapLC       = props.gapLC      or scale.sy(12)  -- line         → content
    self.y           = self.y + (props.marginTop or 0)

    self._lineY      = self.y + theme.FH.body + self.gapTL
    self.contentY    = self._lineY + 1 + self.gapLC
    self.h           = self.contentY - self.y
    self._fontCC     = self.font + CUSTOM_COLOR
    return self
  end

  function Section:render()
    if isColor then
      -- Title text in accent color
      lcd.setColor(CUSTOM_COLOR, self.titleColor)
      lcd.drawText(self.x, self.y, self.title, self._fontCC)

      -- 1px divider line
      lcd.setColor(CUSTOM_COLOR, self.lineColor)
      lcd.drawFilledRectangle(self.x, self._lineY, self.w, 1, CUSTOM_COLOR)
    else
      lcd.drawText(self.x, self.y, self.title, self.font + BOLD)
      lcd.drawLine(self.x, self._lineY, self.x + self.w - 1, self._lineY, SOLID, 0)
    end
  end

  return Section
end
