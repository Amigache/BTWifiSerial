-- components/grid.lua
-- Simple grid layout helper: computes cell positions for a rows×cols grid.
--
-- Props:
--   x, y        number   top-left origin                 (required)
--   w           number   total width                     (required)
--   cols        number   number of columns               (default 2)
--   cellH       number   height per cell                 (required)
--   gapX        number   horizontal gap between columns  (default scale.sx(12))
--   gapY        number   vertical gap between rows       (default scale.sy(4))
--
-- Methods:
--   grid:cellPos(col, row)  → x, y, w (0-indexed col/row)
--   grid:totalH(rows)       → total height for given row count
--   grid:setChildren(list)  → list of {render=fn} placed into cells, row-major
--   grid:render()           → render all children

return function(ctx)
  local scale = ctx.scale

  local Grid = {}
  Grid.__index = Grid

  function Grid.new(props)
    local self       = setmetatable({}, Grid)
    self.x           = props.x     or 0
    self.y           = props.y     or 0
    self.w           = props.w     or scale.sx(600)
    self.cols        = props.cols  or 2
    self.cellH       = props.cellH or scale.sy(25)
    self.gapX        = props.gapX  or scale.sx(12)
    self.gapY        = props.gapY  or scale.sy(4)

    self._cellW = math.floor((self.w - (self.cols - 1) * self.gapX) / self.cols)
    self._children = {}

    return self
  end

  function Grid:cellPos(col, row)
    local cx = self.x + col * (self._cellW + self.gapX)
    local cy = self.y + row * (self.cellH + self.gapY)
    return cx, cy, self._cellW
  end

  function Grid:totalH(rows)
    return rows * self.cellH + (rows - 1) * self.gapY
  end

  function Grid:setChildren(children)
    self._children = children
  end

  function Grid:render()
    for _, c in ipairs(self._children) do
      if c.render then c:render() end
    end
  end

  return Grid
end
