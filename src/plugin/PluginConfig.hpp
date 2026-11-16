#pragma once

#include "core/Settings.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>

// Registers plugin:hyprns:* (every key in settingKeys()) and hl.plugin.hyprns.config{...}.
void registerConfig(HANDLE handle);

// The values in the config now.
SSettings readConfig();
