-- components/footer.lua
-- Footer bar: pinned to the bottom of the page.
-- Black background + a single accent/white line at the top edge.
--
-- Props (all optional):
--   x, y        number       position (y = bottom of content area)  (default computed by Page)
--   w           number       width                                   (default LCD_W)
--   h           number       height                                  (default scale.sy(38))
--   bgColor     rgb          background color                        (default theme.C.header)
--   lineColor   rgb          top border line color                   (default theme.C.text)
--   lineH       number       top border thickness                    (default max(1,scale.sy(2)))
--   indicators  array        array of StatusDot instances drawn left-aligned (default {})

return function(ctx)
  local theme = ctx.theme
  local scale = ctx.scale

  -- Cache theme values
  local isColor    = theme.isColor
  local C_subtext  = theme.C.subtext
  local F_small    = theme.F.small
  local F_SMALL_CC = F_small + CUSTOM_COLOR

  local PAD_L = scale.sx(15)  -- left padding for indicators
  local PAD_R = scale.sx(15)  -- right padding for pagination text
  local IND_GAP = scale.sx(18) -- horizontal gap between indicators
  local BW_PAG_X = scale.sx(30) -- B&W pagination X offset

  local Footer = {}
  Footer.__index = Footer

  function Footer.new(props)
    local self       = setmetatable({}, Footer)
    self.x           = props.x          or 0
    self.y           = props.y          or (scale.H - (props.h or scale.sy(38)))
    self.w           = props.w          or scale.W
    self.h           = props.h          or scale.sy(38)
    self.bgColor     = props.bgColor    or theme.C.header
    self.lineColor   = props.lineColor  or theme.C.text
    self.lineH       = props.lineH      or math.max(1, scale.sy(2))
    self._indicators = props.indicators or {}
    self._pageN      = 0
    self._pageT      = 0
    self._pagText    = nil
    self._pagTx      = 0  -- cached X position for pagination text
    self._ty         = self.y + math.floor((self.h - theme.FH.small) / 2) - scale.sy(3)
    self._bwPagX     = self.x + self.w - BW_PAG_X  -- B&W pagination X
    return self
  end

  -- Called by the navigator on page change
  function Footer:setPagination(n, total)
    self._pageN = n
    self._pageT = total
    if total > 0 then
      self._pagText = n .. " / " .. total
      if lcd.sizeText then
        self._pagTw = lcd.sizeText(self._pagText, F_small)
      else
        self._pagTw = BW_PAG_X
      end
      self._pagTx = self.x + self.w - self._pagTw - PAD_R
    else
      self._pagText = nil
    end
  end

  function Footer:render()
    if isColor then
      -- Background
      lcd.setColor(CUSTOM_COLOR, self.bgColor)
      lcd.drawFilledRectangle(self.x, self.y, self.w, self.h, CUSTOM_COLOR)
      -- Top border line
      lcd.setColor(CUSTOM_COLOR, self.lineColor)
      lcd.drawFilledRectangle(self.x, self.y, self.w, self.lineH, CUSTOM_COLOR)

      -- Status indicators: same vertical position as pagination text
      local ty = self._ty
      local ix = self.x + PAD_L
      for _, dot in ipairs(self._indicators) do
        dot._x = ix
        dot._y = ty
        dot:render()
        ix = ix + dot:width() + IND_GAP
      end

      -- Pagination text: X pre-computed in setPagination()
      if self._pagText then
        lcd.setColor(CUSTOM_COLOR, C_subtext)
        lcd.drawText(self._pagTx, ty, self._pagText, F_SMALL_CC)
      end
    else
      lcd.drawLine(self.x, self.y, self.x + self.w - 1, self.y, SOLID, 0)
      if self._pagText then
        lcd.drawText(self._bwPagX, self.y + 2, self._pagText, SMLSIZE)
      end
    end
  end

  return Footer
end

