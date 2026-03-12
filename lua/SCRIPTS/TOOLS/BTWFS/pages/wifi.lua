-- pages/wifi.lua
-- WiFi info page — mode-aware read-only display of WiFi status and config.
--
-- Rows shown depend on the active WiFi Mode:
--   Off  → Status, WiFi Mode
--   AP   → Status, IP Address, WiFi Mode, AP SSID, AP Password, UDP Port
--   STA  → Status, IP Address, WiFi Mode, STA SSID, STA Password, UDP Port

return function(ctx)
  local Page    = ctx.Page
  local List    = ctx.List
  local scale   = ctx.scale
  local theme   = ctx.theme
  local proto   = ctx.proto
  local store   = ctx.store

  -- ── Pref IDs shown per WiFi mode (0=Off, 1=AP, 2=STA) ────────────
  local PREF_IDS = {
    [0] = { 0x01 },                    -- Off:  WiFi Mode
    [1] = { 0x01, 0x07, 0x09, 0x08 }, -- AP:   WiFi Mode, AP SSID, AP Password, UDP Port
    [2] = { 0x01, 0x0A, 0x0B, 0x08 }, -- STA:  WiFi Mode, STA SSID, STA Password, UDP Port
  }

  -- ── Helpers ────────────────────────────────────────────────────────
  local function getWifiModeIdx()
    local wmp = store.prefs[0x01]
    return wmp and wmp.curIdx or 0
  end

  local function getStaSsid()
    local p = store.prefs[0x0A]
    return p and p.value or ""
  end

  local function prefValue(p)
    if     p.type == proto.FT_ENUM   then
      return p.options and p.options[p.curIdx + 1] or "?"
    elseif p.type == proto.FT_STRING then
      return (p.value and p.value ~= "") and p.value or "(not set)"
    elseif p.type == proto.FT_INT    then
      return tostring(p.value or 0)
    elseif p.type == proto.FT_BOOL   then
      return p.value and "On" or "Off"
    end
    return "--"
  end

  -- ── Status row (mutated in place) ─────────────────────────────────
  local ROW_STATUS = { label = "Status", value = "--" }
  local _wifiActive = false

  local function computeStatus()
    local idx = getWifiModeIdx()
    if idx == 0 then
      return "Off"
    elseif idx == 1 then
      return _wifiActive and "Running" or "Pending restart"
    else  -- STA
      if _wifiActive then
        return "Connected"
      elseif getStaSsid() == "" then
        return "No SSID configured"
      else
        return "Disconnected"
      end
    end
  end

  local function updateStatus()
    ROW_STATUS.value = computeStatus()
  end

  -- ── IP row (mutated in place) ──────────────────────────────────────
  local ROW_IP = { label = "IP Address", value = "192.168.4.1" }

  local function updateIpRow()
    ROW_IP.value = (getWifiModeIdx() == 2) and "(DHCP)" or "192.168.4.1"
  end

  -- ── Build row list ─────────────────────────────────────────────────
  local function buildRows()
    local idx = getWifiModeIdx()
    updateStatus()
    updateIpRow()

    local rows = { ROW_STATUS }
    if idx ~= 0 then
      rows[#rows + 1] = ROW_IP
    end

    local ids = PREF_IDS[idx] or PREF_IDS[0]
    for _, id in ipairs(ids) do
      local p = store.prefs[id]
      if p then
        rows[#rows + 1] = { _prefId = id, label = p.label, value = prefValue(p) }
      end
    end
    return rows
  end

  -- ── Patch a single pref row value ─────────────────────────────────
  local function refreshPrefRow(list, pref)
    for _, row in ipairs(list.rows) do
      if row._prefId == pref.id then
        row.value = prefValue(pref)
        return
      end
    end
  end

  -- ── Page class ─────────────────────────────────────────────────────
  local Wifi = {}
  Wifi.__index = Wifi

  function Wifi.new()
    local self = setmetatable({}, Wifi)

    self._page = Page.new({
      hasHeader    = true,
      title        = "BTWifiSerial",
      hasPageTitle = true,
      pageTitle    = "WiFi",
      hasFooter    = true,
      indicators   = ctx.indicators,
    })

    local gap = scale.sy(8)

    self._list = List.new({
      y          = self._page.contentY + gap,
      h          = self._page.contentH - gap,
      selectable = false,
      showScroll = true,
      cols = {
        { key = "label" },
        { key = "value", xFrac = 0.54 },
      },
      rows = buildRows(),
    })

    self._page:addChild(self._list)

    -- Full rebuild when prefs arrive (mode may have changed after restart)
    store.on("prefs_ready", function()
      self._list:setRows(buildRows())
    end)

    store.on("pref_changed", function(pref)
      if pref.id == 0x01 then
        -- WiFi Mode changed → different row set, full rebuild
        self._list:setRows(buildRows())
      else
        -- Credential or port changed → patch value in place + refresh status
        updateStatus()
        refreshPrefRow(self._list, pref)
      end
    end)

    store.on("status", function(s)
      _wifiActive = s.wifiClients
      updateStatus()
    end)

    return self
  end

  function Wifi:handleEvent(event)
    self._list:handleEvent(event)
  end

  function Wifi:setPagination(n, total)
    self._page:setPagination(n, total)
  end

  function Wifi:render()
    self._page:render()
  end

  return Wifi
end
