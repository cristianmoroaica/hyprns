#include "plugin/PluginConfig.hpp"
#include "plugin/Globals.hpp"

#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/config/values/ConfigValues.hpp>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <format>

namespace {

std::string optionPath(const SSettingKey& key) {
    return std::format("plugin:{}:{}", PLUGIN_NAME, key.name);
}

// hl.plugin.hyprns.config{ look = "marble" } is hl.config{ plugin = { hyprns = { look = "marble" } } }.
int handleLuaConfig(lua_State* L) {
    if (lua_gettop(L) < 1 || !lua_istable(L, 1))
        return luaL_error(L, "hyprns.config: expected a table");
    lua_getglobal(L, "hl");
    if (!lua_istable(L, -1))
        return luaL_error(L, "hyprns.config: hl.config is not available");
    lua_getfield(L, -1, "config");
    if (!lua_isfunction(L, -1))
        return luaL_error(L, "hyprns.config: hl.config is not available");
    lua_newtable(L);         // { plugin = { hyprns = T } }
    lua_newtable(L);         // { hyprns = T }
    lua_pushvalue(L, 1);
    lua_setfield(L, -2, PLUGIN_NAME);
    lua_setfield(L, -2, "plugin");
    lua_call(L, 1, 0);       // hl.config reports unknown keys and bad values itself
    lua_pop(L, 1);           // hl
    return 0;
}

} // namespace

void registerConfig(HANDLE handle) {
    const SSettings defaults;
    for (const auto& key : settingKeys()) {
        const auto path = optionPath(key);
        const auto def  = key.get(defaults);
        switch (key.kind) {
            case ESettingKind::INT:
                HyprlandAPI::addConfigValueV2(handle, Config::Values::makeConfigValue<Config::Values::Int>(path.c_str(), "", Config::INTEGER{std::stoll(def)},
                                                                                                         Config::Values::valueOptions_t<Config::Values::Int>{}));
                break;
            case ESettingKind::FLOAT:
                HyprlandAPI::addConfigValueV2(handle, Config::Values::makeConfigValue<Config::Values::Float>(path.c_str(), "", Config::FLOAT{std::stof(def)},
                                                                                                           Config::Values::valueOptions_t<Config::Values::Float>{}));
                break;
            case ESettingKind::STRING:
                HyprlandAPI::addConfigValueV2(handle, Config::Values::makeConfigValue<Config::Values::String>(path.c_str(), "", Config::STRING{def},
                                                                                                            Config::Values::valueOptions_t<Config::Values::String>{}));
                break;
        }
    }
    HyprlandAPI::addLuaFunction(handle, PLUGIN_NAME, "config", handleLuaConfig);
}

SSettings readConfig() {
    SSettings settings;
    for (const auto& key : settingKeys()) {
        const auto value = Config::mgr()->getConfigValue(optionPath(key));
        if (!value.dataptr || !value.type)
            continue;
        std::string text;
        if (*value.type == typeid(Config::INTEGER))
            text = std::to_string(**reinterpret_cast<Config::INTEGER* const*>(value.dataptr));
        else if (*value.type == typeid(Config::FLOAT))
            text = std::format("{}", **reinterpret_cast<Config::FLOAT* const*>(value.dataptr));
        else if (*value.type == typeid(Config::STRING))
            text = **reinterpret_cast<Config::STRING* const*>(value.dataptr);
        else if (*value.type == typeid(const char*)) // a hyprlang config's string (Hyprlang::STRING): the data is the C string
            text = *value.dataptr ? static_cast<const char*>(*value.dataptr) : "";
        else
            continue;
        // a value out of range keeps the default; the error is in the log
        if (const auto err = key.set(settings, text); !err.empty())
            Log::logger->log(Log::ERR, "[{}] config: {}", PLUGIN_NAME, err);
    }
    return settings;
}
