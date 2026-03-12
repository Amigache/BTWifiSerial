-- pages/dashboard.lua
-- Dashboard page — data-driven from store.
-- System section: prefs with PF_DASHBOARD flag + info items (firmware-defined).
-- Channels section: live channel bars polled from store.channels.

return function(ctx)
  local Page       = ctx.Page
  local Section    = ctx.Section
  local List       = ctx.List
  local Grid       = ctx.Grid
  local ChannelBar = ctx.ChannelBar
  local scale      = ctx.scale
  local proto      = ctx.proto
  local store      = ctx.store

  local Dashboard = {}
  Dashboard.__index = Dashboard

  -- Build system list rows from PF_DASHBOARD prefs + info items.
  local function buildSysRows()
    local rows = {}
    -- Prefs flagged for dashboard display
    for _, id in ipairs(store.prefsOrder) do
      local p = store.prefs[id]
      if p and bit32.band(p.flags, proto.PF_DASHBOARD) ~= 0 then
        local val = "--"
        if     p.type == proto.FT_ENUM   then val = p.options and p.options[p.curIdx + 1] or "?"
        elseif p.type == proto.FT_STRING then val = p.value or ""
        elseif p.type == proto.FT_INT    then val = tostring(p.value or 0)
        elseif p.type == proto.FT_BOOL   then val = p.value and "On" or "Off"
        end
        rows[#rows + 1] = { _prefId = id, label = p.label, value = val }
      end
    end
    -- Info items (firmware version, addresses, …)
    for _, id in ipairs(store.infoOrder) do
      local inf = store.info[id]
      if inf then
        local val = (inf.value ~= nil) and tostring(inf.value) or "--"
        rows[#rows + 1] = { _infoId = id, label = inf.label, value = val }
      end
    end
    return rows
  end

  -- Update only the row that corresponds to a changed pref or info item.
  local function refreshRow(list, keyField, id, newVal)
    for _, row in ipairs(list.rows) do
      if row[keyField] == id then
        row.value = newVal
        return
      end
    end
  end

  function Dashboard.new()
    local self = setmetatable({}, Dashboard)
    local theme = ctx.theme

    self._page = Page.new({
      hasHeader    = true,
      title        = "BTWifiSerial",
      hasPageTitle = true,
      pageTitle    = "Dashboard",
      hasFooter    = true,
      indicators   = ctx.indicators,
    })

    -- ── Proportional layout ───────────────────────────────────────
    local contentY = self._page.contentY
    local contentH = self._page.contentH
    local gap      = scale.sy(8)                                       -- top & inter-section gap
    local bottomGap = scale.sy(10)                                     -- keep space above footer
    local secHdrH  = theme.FH.body + scale.sy(14) + 1 + scale.sy(12)   -- matches Section defaults
    local PAD      = scale.sx(17)                                      -- section title x

    -- Channels grid has a fixed required height (4 rows)
    local chCellH    = scale.sy(25)
    local chGapY     = scale.sy(1)
    local chContentH = 4 * chCellH + 3 * chGapY

    -- System list gets the remaining vertical space
    local sysContentH = contentH - gap - secHdrH - gap - secHdrH - chContentH - bottomGap

    -- ── System section ────────────────────────────────────────────
    local sysSecY  = contentY + gap
    self._sysSection = Section.new({ title = "System", y = sysSecY })

    local sysListY = sysSecY + secHdrH
    self._list = List.new({
      y          = sysListY,
      h          = sysContentH,
      selectable = false,
      showScroll = true,
      cols = {
        { key = "label" },
        { key = "value", xFrac = 0.54 },
      },
      rows = buildSysRows(),  -- may be empty until prefs/info arrive
    })

    -- ── Channels section ──────────────────────────────────────────
    local chSecY   = sysListY + sysContentH + gap
    self._chSection = Section.new({ title = "Channels", y = chSecY })

    local chGridY  = chSecY + secHdrH

    self._grid = Grid.new({
      x     = PAD,
      y     = chGridY,
      w     = LCD_W - 2 * PAD,
      cols  = 2,
      cellH = chCellH,
      gapX  = scale.sx(16),
      gapY  = chGapY,
    })

    -- Create 8 channel bars in row-major order:
    --   col 0: CH1-CH4    col 1: CH5-CH8
    self._bars = {}
    for i = 1, 8 do
      local col = (i <= 4) and 0 or 1
      local row = (i <= 4) and (i - 1) or (i - 5)
      local cx, cy, cw = self._grid:cellPos(col, row)
      self._bars[i] = ChannelBar.new({
        x     = cx,
        y     = cy,
        w     = cw,
        h     = chCellH,
        label = "CH" .. i,
      })
    end
    self._grid:setChildren(self._bars)

    self._page:addChild(self._sysSection)
    self._page:addChild(self._list)
    self._page:addChild(self._chSection)
    self._page:addChild(self._grid)

    -- ── React to store events ──────────────────────────────────────
    -- Rebuild system rows whenever the full lists arrive.
    store.on("prefs_ready", function() self._list:setRows(buildSysRows()) end)
    store.on("info_ready",  function() self._list:setRows(buildSysRows()) end)

    -- Patch individual rows on incremental updates.
    store.on("pref_changed", function(pref)
      if bit32.band(pref.flags, proto.PF_DASHBOARD) == 0 then return end
      local val
      if     pref.type == proto.FT_ENUM   then val = pref.options and pref.options[pref.curIdx + 1] or "?"
      elseif pref.type == proto.FT_STRING then val = pref.value or ""
      elseif pref.type == proto.FT_INT    then val = tostring(pref.value or 0)
      elseif pref.type == proto.FT_BOOL   then val = pref.value and "On" or "Off"
      end
      refreshRow(self._list, "_prefId", pref.id, val)
    end)

    store.on("info_changed", function(inf)
      refreshRow(self._list, "_infoId", inf.id, tostring(inf.value or "--"))
    end)

    return self
  end

  function Dashboard:handleEvent(event)
    self._list:handleEvent(event)
  end

  function Dashboard:setPagination(n, total)
    self._page:setPagination(n, total)
  end

  function Dashboard:render()
    -- Refresh channel bars from store (polled — channels arrive ~100 Hz)
    for i = 1, 8 do
      if self._bars[i] then
        self._bars[i]:setValue(store.channels[i] or 0)
      end
    end
    self._page:render()
  end

  function Dashboard:contentY()
    return self._page.contentY
  end

  return Dashboard
end
