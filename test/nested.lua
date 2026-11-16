-- Minimal config for the nested Hyprland that hyprns is tested in (test/nested.sh).
-- An empty desktop on one output, no clients, no logo: whatever the plugin draws
-- is the only thing on screen.

hl.monitor({ output = "", mode = "preferred", position = "auto", scale = 1, transform = tonumber(os.getenv("HYPRNS_TEST_TRANSFORM") or "0") })

hl.config({
  misc = {
    disable_hyprland_logo = true,
    disable_splash_rendering = true,
    background_color = 0xff404040,  -- ARGB: without the ff the background is transparent
  },
  animations = { enabled = false },
  -- keep writing hyprland.log after the config loads (Hyprland stops by default), so a
  -- crash's "Hyprland received signal" line and hyprns's own lines reach it (soak.sh)
  debug = { disable_logs = false },
})

-- HYPRNS_TEST_EXTRA=FILE: more config for one test, run on every (re)load
local extra = os.getenv("HYPRNS_TEST_EXTRA")
if extra and extra ~= "" then dofile(extra) end
