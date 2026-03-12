-- lib/input.lua
-- Shared EdgeTX input-event helpers used across BTWFS pages.

local M = {}

function M.evEnter(e)
  return (EVT_VIRTUAL_ENTER ~= nil and e == EVT_VIRTUAL_ENTER)
      or (EVT_ENTER_BREAK   ~= nil and e == EVT_ENTER_BREAK)
end

function M.evExit(e)
  return (EVT_VIRTUAL_EXIT ~= nil and e == EVT_VIRTUAL_EXIT)
      or (EVT_EXIT_BREAK   ~= nil and e == EVT_EXIT_BREAK)
end

function M.evNext(e)
  return (EVT_VIRTUAL_NEXT ~= nil and e == EVT_VIRTUAL_NEXT)
      or (EVT_ROT_RIGHT    ~= nil and e == EVT_ROT_RIGHT)
      or (EVT_PLUS_BREAK   ~= nil and e == EVT_PLUS_BREAK)
      or (EVT_PLUS_REPT    ~= nil and e == EVT_PLUS_REPT)
end

function M.evPrev(e)
  return (EVT_VIRTUAL_PREV  ~= nil and e == EVT_VIRTUAL_PREV)
      or (EVT_ROT_LEFT      ~= nil and e == EVT_ROT_LEFT)
      or (EVT_MINUS_BREAK   ~= nil and e == EVT_MINUS_BREAK)
      or (EVT_MINUS_REPT    ~= nil and e == EVT_MINUS_REPT)
end

function M.evPageNext(e)
  return EVT_VIRTUAL_NEXT_PAGE ~= nil and e == EVT_VIRTUAL_NEXT_PAGE
end

function M.evPagePrev(e)
  return EVT_VIRTUAL_PREV_PAGE ~= nil and e == EVT_VIRTUAL_PREV_PAGE
end

return M
