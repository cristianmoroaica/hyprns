-- hyprns: live Navier–Stokes wallpapers drawn by Hyprland itself.
-- Looks: pattern, embers, marble, jupiter.
-- Load the plugin first (hl.plugin.load or `hyprctl plugin load`); this block only
-- runs while it is loaded. Every key can also be changed live, until the next config
-- reload: `hyprctl hyprns set KEY VALUE`; `hyprctl hyprns status` shows them all.
-- Every value below is the default.

if hl.plugin and hl.plugin.hyprns then
    hl.plugin.hyprns.config({
        enabled = 1,              -- 0 or 1: draw the wallpaper at all
        monitors = "",            -- connector names, comma separated; empty = every monitor
        fps = 30,                 -- 1 to 240: frames per second the wallpaper redraws at

        look = "pattern",         -- pattern, embers, marble or jupiter, on every monitor
        monitor_looks = "",       -- CONNECTOR:look pairs that override look, e.g. "DP-4:marble, HDMI-A-1:jupiter"
        time_scale = 1,           -- 0 to 4: the pace of every look (0 freezes)
        budget_ms = 2,            -- 0 to 50: GPU ms a frame may take per monitor; 0 turns the controller off
        quality = 1,              -- 1, 0.75, 0.5 or 0.35: the picture's resolution (a fraction of the monitor's); with the controller on, its starting level and ceiling
        blur_fps = -1,            -- -1 to 240: how often Hyprland's blur behind translucent windows follows the wallpaper; -1 = every frame, 0 = never, N = at most N a second
    })
end
