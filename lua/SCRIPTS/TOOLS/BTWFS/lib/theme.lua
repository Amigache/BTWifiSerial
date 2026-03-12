-- lib/theme.lua
-- Central palette and font definitions.
-- Adapt automatically to color vs B&W displays and screen size.

local M = {}

M.isColor = (lcd.setColor ~= nil)
local big  = (LCD_W >= 600)   -- true on 800×480, false on smaller radios

-- ── Palette ──────────────────────────────────────────────────────────
if M.isColor then
  M.C = {
    bg      = lcd.RGB(30,  34,  42),   -- #1E222A  main background
    header  = lcd.RGB(18,  26,  42),   -- #121A2A  header bar
    accent  = lcd.RGB(0,  140, 255),   -- #008CFF  accent line / highlights
    panel   = lcd.RGB(49,  60,  81),   -- #313C51  panel / divider
    text    = lcd.RGB(255, 255, 255),  -- #FFFFFF  primary text
    subtext = lcd.RGB(130, 130, 130),  -- #828282  secondary / hint text
    red     = lcd.RGB(234,  33,   0),  -- #EA2100  error / warning
    green   = lcd.RGB(46,  204, 113),  -- #2ECC71  ok / connected
    orange  = lcd.RGB(255, 140,   0),  -- #FF8C00  caution
    black   = lcd.RGB(0,     0,   0),  -- #000000  pure black (footer bg, etc.)
    editBg  = lcd.RGB(0,    85, 170),  -- #0055AA  dimmer blue for edit mode
    -- Modal severity colors
    modalInfo    = lcd.RGB(52, 152, 219),  -- #3498DB  softer blue (distinct from accent)
    modalSuccess = lcd.RGB(46, 204, 113),  -- #2ECC71  green
    modalError   = lcd.RGB(231, 76,  60),  -- #E74C3C  red
    modalWarning = lcd.RGB(241, 196,  15), -- #F1C40F  egg-yellow
    overlay = lcd.RGB(0, 0, 0),            -- #000000  semi-transparent overlay base
  }
else
  -- Minimal B&W mapping – most fields unused on b&w screens
  M.C = {
    bg      = BLACK,
    header  = BLACK,
    accent  = WHITE,
    panel   = 0,
    text    = WHITE,
    subtext = 0,
    red     = WHITE,
    green   = WHITE,
    orange  = WHITE,
  }
end

-- ── Fonts ─────────────────────────────────────────────────────────────
-- Use standard EdgeTX font flags; adjust per screen size.
M.F = {
  title  = big and MIDSIZE or BOLD,
  body   = big and 0       or SMLSIZE,
  small  = SMLSIZE,
}

-- Actual bitmap font pixel heights for vertical centering.
-- These are FIXED pixel values (EdgeTX fonts do not scale with resolution).
-- MIDSIZE on 800x480 ≈ 36px; BOLD on smaller screens ≈ 18px.
-- On non-big color displays F.body and F.small are both SMLSIZE (~12px);
-- on B&W displays SMLSIZE is 8px.
M.FH = {
  title = big and 36 or 18,                        -- MIDSIZE (big) / BOLD (small)
  body  = big and 16 or 12,                        -- normal 0-flag (big) / SMLSIZE (small)
  small = M.isColor and (big and 14 or 12) or 8,  -- SMLSIZE: color-big≈14, color-small≈12, B&W=8
}

return M
