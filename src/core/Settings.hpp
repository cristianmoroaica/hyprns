#pragma once

// Everything the config (plugin:hyprns:*) and `hyprctl hyprns set` control, and the
// key table both go through. No Hyprland types: the unit tests build this; the
// Hyprland registration (registerConfig, readConfig) is in plugin/PluginConfig.

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct SSettings {
    bool        enabled  = true;
    std::string monitors;      // comma-separated connector names; empty = every monitor
    int         fps      = 30; // how often the picture is redrawn
    std::string look     = "pattern";
    // CONNECTOR:look pairs that override `look`, in the order given
    std::vector<std::pair<std::string, std::string>> monitorLooks;
    float       timeScale = 1.0f; // every look's pace (0 freezes)
    float       budgetMs  = 2.0f; // GPU time a frame may take; 0 turns the controller off
    float       quality   = 1.0f; // 1, 0.75, 0.5 or 0.35: passed to ILook::build
    int         blurFps   = -1;   // Hyprland's blur cache refreshes: -1 every frame, 0 never, N at most N/s (blurRefreshDue)

    bool        wantsMonitor(std::string_view name) const;
    // The look this monitor shows: its monitor_looks entry, else `look`.
    std::string lookFor(std::string_view monitor) const;
};

enum class ESettingKind { INT, FLOAT, STRING };

struct SSettingKey {
    const char*                                              name;
    ESettingKind                                             kind;
    std::function<std::string(SSettings&, std::string_view)> set; // error, or empty when applied
    std::function<std::string(const SSettings&)>             get;
};

// Every key, in config order.
const std::vector<SSettingKey>& settingKeys();

// `hyprctl hyprns set KEY VALUE`: changes one setting until the next config reload.
// Returns an error message (the setting keeps its value), or empty when it was applied.
std::string applySetting(SSettings& settings, std::string_view key, std::string_view value);

// One line per setting, `key value`, in config order.
std::string describeSettings(const SSettings& settings);
