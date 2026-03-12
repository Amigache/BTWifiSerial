-- lib/scale.lua
-- Resolution-independent scaling helpers.
-- All layout values are defined on a 800×480 reference canvas
-- and scaled to the actual LCD at runtime.

local M = {}

M.W     = LCD_W
M.H     = LCD_H
M.REF_W = 800
M.REF_H = 480

-- Scale a horizontal value from the 800-px reference
function M.sx(v) return math.floor(v * M.W / M.REF_W) end

-- Scale a vertical value from the 480-px reference
function M.sy(v) return math.floor(v * M.H / M.REF_H) end

-- Scale using the smaller ratio (good for icon sizes / line weights)
function M.s(v)
  local rx = M.W / M.REF_W
  local ry = M.H / M.REF_H
  return math.floor(v * math.min(rx, ry))
end

return M
