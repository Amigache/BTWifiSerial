-- components/button.lua
-- Simple rectangular button with centered label.
--
-- Props:
--   x, y        number   top-left                       (required)
--   w, h        number   dimensions                     (required)
--   label       string   button text                    (default "")
--   bgColor     rgb      background fill                (default theme.C.panel)
--   textColor   rgb      label color                    (default theme.C.text)
--   font        flag     font flag                      (default theme.F.small)
--   fontH       number   font pixel height              (default theme.FH.small)
--   focused     bool     draw accent border when true   (default false)
--   focusColor  rgb      border color when focused      (default theme.C.accent)

return function(ctx)
  local theme = ctx.theme
  local scale = ctx.scale

  local Button = {}
  Button.__index = Button

  function Button.new(props)
    local self       = setmetatable({}, Button)
    self.x           = props.x          or 0
    self.y           = props.y          or 0
    self.w           = props.w          or scale.sx(100)
    self.h           = props.h          or scale.sy(32)
    self.label       = props.label      or ""
    self.bgColor     = props.bgColor    or theme.C.panel
    self.textColor   = props.textColor  or theme.C.text
    self.font        = props.font       or theme.F.small
    self.fontH       = props.fontH      or theme.FH.small
    self.focused     = props.focused    or false
    self.focusColor  = props.focusColor or theme.C.accent
    return self
  end

  function Button:setFocused(f) self.focused = f end
  function Button:setLabel(l)   self.label = l end

  function Button:render()
    if not theme.isColor then
      local fl = self.focused and INVERS or 0
      lcd.drawText(self.x + 2, self.y + 2, self.label, self.font + fl)
      return
    end

    -- Background: focused → fill with focusColor, else normal bgColor
    if self.focused then
      lcd.setColor(CUSTOM_COLOR, self.focusColor)
    else
      lcd.setColor(CUSTOM_COLOR, self.bgColor)
    end
    lcd.drawFilledRectangle(self.x, self.y, self.w, self.h, CUSTOM_COLOR)

    -- Centered label
    local tw = lcd.sizeText and lcd.sizeText(self.label, self.font) or 0
    local tx = self.x + math.floor((self.w - tw) / 2)
    local ty = self.y + math.floor((self.h - self.fontH) / 2) - scale.sy(3)
    lcd.setColor(CUSTOM_COLOR, self.textColor)
    lcd.drawText(tx, ty, self.label, self.font + CUSTOM_COLOR)
  end

  return Button
end
