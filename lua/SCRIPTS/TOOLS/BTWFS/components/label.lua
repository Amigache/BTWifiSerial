-- components/label.lua
-- Factory: returns the Label class bound to the shared context.
--
-- Usage:
--   local Label = loadScript(BASE .. "/components/label.lua")(ctx)
--   local lbl   = Label.new({ text="Hello", x=10, y=10 })
--   lbl:render()
--
-- Props:
--   text    string   (default "")
--   x, y    number   position in pixels
--   font    flag     EdgeTX font flag  (default theme.F.body)
--   color   rgb      lcd.RGB() value   (default theme.C.text)
--   flags   number   extra EdgeTX draw flags ORed in

return function(ctx)
  local theme = ctx.theme

  local Label = {}
  Label.__index = Label

  function Label.new(props)
    local self  = setmetatable({}, Label)
    self.text   = props.text  or ""
    self.x      = props.x     or 0
    self.y      = props.y     or 0
    self.font   = props.font  or theme.F.body
    self.color  = props.color or theme.C.text
    self.flags  = props.flags or 0
    return self
  end

  -- Setters (chainable)
  function Label:setText(t)  self.text  = t ; return self end
  function Label:setPos(x,y) self.x = x ; self.y = y ; return self end
  function Label:setColor(c) self.color = c ; return self end

  function Label:render()
    local flags = self.font + self.flags
    if theme.isColor then
      lcd.setColor(CUSTOM_COLOR, self.color)
      lcd.drawText(self.x, self.y, self.text, flags + CUSTOM_COLOR)
    else
      lcd.drawText(self.x, self.y, self.text, flags)
    end
  end

  return Label
end
