-- components/page.lua
-- Page container component.
-- Fills the screen background, renders Header / PageTitle / Footer
-- in the right order, and exposes contentY + contentH so children
-- can position themselves inside the available area.
--
-- Props (all optional):
--   hasHeader    bool     Header bar at top                  (default false)
--   title        string   header title text                  (default "")
--   hasPageTitle bool     PageTitle band below header        (default false)
--   pageTitle    string   page-title text                    (default "")
--   hasFooter    bool     Footer bar at bottom               (default false)
--   children     array    components with :render()          (default {})
--   x, y         number   top-left origin                    (default 0, 0)
--   w, h         number   dimensions                         (default LCD_W, LCD_H)
--   bgColor      rgb      fill color                         (default theme.C.bg)
--
-- Read-only after construction:
--   page.header        → Header instance  (nil if hasHeader=false)
--   page.pageTitleBar  → PageTitle instance (nil if hasPageTitle=false)
--   page.footer        → Footer instance  (nil if hasFooter=false)
--   page.contentY      → first Y pixel available for children
--   page.contentH      → pixel height of the children area

return function(ctx)
  local theme     = ctx.theme
  local scale     = ctx.scale
  local Header    = ctx.Header
  local PageTitle = ctx.PageTitle
  local Footer    = ctx.Footer

  local Page = {}
  Page.__index = Page

  function Page.new(props)
    local self    = setmetatable({}, Page)
    self.x        = props.x       or 0
    self.y        = props.y       or 0
    self.w        = props.w       or scale.W
    self.h        = props.h       or scale.H
    self.bgColor  = props.bgColor or theme.C.bg
    self.children = props.children or {}

    local topY    = self.y
    local bottomY = self.y + self.h   -- grows upward as footer is reserved

    -- Header
    if props.hasHeader then
      self.header = Header.new({
        title = props.title or "",
        x = self.x, y = topY, w = self.w,
      })
      topY = topY + self.header.h + self.header.accentH
    else
      self.header = nil
    end

    -- PageTitle band
    if props.hasPageTitle then
      self.pageTitleBar = PageTitle.new({
        text = props.pageTitle or "",
        x = self.x, y = topY, w = self.w,
      })
      topY = topY + self.pageTitleBar.h
    else
      self.pageTitleBar = nil
    end

    -- Footer (reserved from bottom before content)
    if props.hasFooter then
      local fh = scale.sy(38)
      self.footer = Footer.new({
        x          = self.x,
        y          = bottomY - fh,
        w          = self.w,
        h          = fh,
        indicators = props.indicators or {},
      })
      bottomY = bottomY - fh
    else
      self.footer = nil
    end

    self.contentY = topY
    self.contentH = math.max(0, bottomY - topY)
    return self
  end

  function Page:setTitle(t)
    if self.header then self.header:setTitle(t) end
    return self
  end

  function Page:setPageTitle(t)
    if self.pageTitleBar then self.pageTitleBar:setText(t) end
    return self
  end

  -- Forward pagination data to the footer
  function Page:setPagination(n, total)
    if self.footer then self.footer:setPagination(n, total) end
    return self
  end

  function Page:addChild(child)
    self.children[#self.children + 1] = child
    return self
  end

  function Page:render()
    -- Background fill
    if theme.isColor then
      lcd.setColor(CUSTOM_COLOR, self.bgColor)
      lcd.drawFilledRectangle(self.x, self.y, self.w, self.h, CUSTOM_COLOR)
    else
      lcd.clear()
    end

    if self.header       then self.header:render()       end
    if self.pageTitleBar then self.pageTitleBar:render() end

    -- Children render in the content area
    for _, child in ipairs(self.children) do
      child:render()
    end

    -- Footer on top of everything (drawn last so it's never clipped)
    if self.footer then self.footer:render() end
  end

  return Page
end
