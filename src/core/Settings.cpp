#include "core/Settings.hpp"
#include "core/Look.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <optional>
#include <sstream>

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n'))
        s.remove_suffix(1);
    return s;
}

std::optional<double> parseNumber(std::string_view s) {
    s          = trim(s);
    double v   = 0;
    const auto [end, err] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (s.empty() || err != std::errc{} || end != s.data() + s.size() || !std::isfinite(v))
        return std::nullopt;
    return v;
}

// Every refusal of `look` or `monitor_looks` goes through this: it ends with the looks.
std::string lookRefusal(std::string_view what) {
    return std::format("{}; the looks are: {}", what, lookNameList());
}

// "DP-4:jupiter, DP-5:embers" -> pairs; empty -> no pairs; anything else -> an error
std::string parseMonitorLooks(std::string_view text, std::vector<std::pair<std::string, std::string>>& out) {
    out.clear();
    text = trim(text);
    if (text.empty())
        return {};
    const auto usage = [](std::string_view item) {
        return lookRefusal(std::format("monitor_looks takes CONNECTOR:look pairs separated by commas, e.g. DP-4:marble,HDMI-A-1:jupiter (got '{}')", item));
    };
    for (std::string_view rest = text;;) {
        const auto comma = rest.find(',');
        const auto item  = trim(rest.substr(0, comma));
        const auto colon = item.find(':');
        if (item.empty() || colon == std::string_view::npos || item.find(':', colon + 1) != std::string_view::npos)
            return usage(item);
        const auto connector = trim(item.substr(0, colon));
        const auto look      = trim(item.substr(colon + 1));
        if (connector.empty() || connector.find_first_of(" \t") != std::string_view::npos)
            return usage(item);
        if (!isLookName(look))
            return lookRefusal(std::format("monitor_looks: unknown look '{}' for {}", look, connector));
        if (std::ranges::any_of(out, [&](const auto& p) { return p.first == connector; }))
            return lookRefusal(std::format("monitor_looks names {} twice", connector));
        out.emplace_back(connector, look);
        if (comma == std::string_view::npos)
            break;
        rest = rest.substr(comma + 1);
    }
    return {};
}

SSettingKey number(const char* name, double lo, double hi, std::function<float&(SSettings&)> field) {
    return {name, ESettingKind::FLOAT,
            [=](SSettings& s, std::string_view v) -> std::string {
                const auto n = parseNumber(v);
                if (!n || *n < lo || *n > hi)
                    return std::format("{} takes a number from {} to {}", name, lo, hi);
                field(s) = static_cast<float>(*n);
                return {};
            },
            [=](const SSettings& s) { return std::format("{}", field(const_cast<SSettings&>(s))); }};
}

SSettingKey integer(const char* name, int lo, int hi, std::function<int&(SSettings&)> field) {
    return {name, ESettingKind::INT,
            [=](SSettings& s, std::string_view v) -> std::string {
                const auto n = parseNumber(v);
                if (!n || *n != std::floor(*n) || *n < lo || *n > hi)
                    return std::format("{} takes a whole number from {} to {}", name, lo, hi);
                field(s) = static_cast<int>(*n);
                return {};
            },
            [=](const SSettings& s) { return std::to_string(field(const_cast<SSettings&>(s))); }};
}

constexpr std::array<float, 4> QUALITIES = {1.0f, 0.75f, 0.5f, 0.35f};

} // namespace

bool SSettings::wantsMonitor(std::string_view name) const {
    const auto list = trim(monitors);
    if (list.empty())
        return true;
    std::stringstream ss{std::string{list}};
    for (std::string entry; std::getline(ss, entry, ',');) {
        if (trim(entry) == name)
            return true;
    }
    return false;
}

std::string SSettings::lookFor(std::string_view monitor) const {
    for (const auto& [connector, name] : monitorLooks)
        if (connector == monitor)
            return name;
    return look;
}

const std::vector<SSettingKey>& settingKeys() {
    static const std::vector<SSettingKey> k = {
        {"enabled", ESettingKind::INT,
         [](SSettings& s, std::string_view v) -> std::string {
             const auto n = parseNumber(v);
             if (!n || (*n != 0 && *n != 1))
                 return "enabled takes 0 or 1";
             s.enabled = *n == 1;
             return {};
         },
         [](const SSettings& s) { return std::string{s.enabled ? "1" : "0"}; }},
        {"monitors", ESettingKind::STRING,
         [](SSettings& s, std::string_view v) -> std::string {
             s.monitors = std::string{trim(v)};
             return {};
         },
         [](const SSettings& s) { return s.monitors; }},
        integer("fps", 1, 240, [](SSettings& s) -> int& { return s.fps; }),
        {"look", ESettingKind::STRING,
         [](SSettings& s, std::string_view v) -> std::string {
             const auto name = trim(v);
             if (!isLookName(name))
                 return lookRefusal(std::format("look: unknown look '{}'", name));
             s.look = std::string{name};
             return {};
         },
         [](const SSettings& s) { return s.look; }},
        {"monitor_looks", ESettingKind::STRING,
         [](SSettings& s, std::string_view v) -> std::string {
             std::vector<std::pair<std::string, std::string>> parsed;
             if (auto err = parseMonitorLooks(v, parsed); !err.empty())
                 return err;
             s.monitorLooks = std::move(parsed);
             return {};
         },
         [](const SSettings& s) {
             std::string out;
             for (const auto& [connector, name] : s.monitorLooks)
                 out += std::format("{}{}:{}", out.empty() ? "" : ",", connector, name);
             return out;
         }},
        number("time_scale", 0, 4, [](SSettings& s) -> float& { return s.timeScale; }),
        number("budget_ms", 0, 50, [](SSettings& s) -> float& { return s.budgetMs; }),
        {"quality", ESettingKind::FLOAT,
         [](SSettings& s, std::string_view v) -> std::string {
             const auto n = parseNumber(v);
             const auto it = n ? std::ranges::find_if(QUALITIES, [&](float q) { return std::fabs(*n - q) < 1e-6; }) : QUALITIES.end();
             if (it == QUALITIES.end())
                 return "quality takes 1, 0.75, 0.5 or 0.35";
             s.quality = *it;
             return {};
         },
         [](const SSettings& s) { return std::format("{}", s.quality); }},
        integer("blur_fps", -1, 240, [](SSettings& s) -> int& { return s.blurFps; }),
    };
    return k;
}

std::string applySetting(SSettings& settings, std::string_view key, std::string_view value) {
    std::string name{trim(key)};
    std::replace(name.begin(), name.end(), '-', '_');
    for (const auto& k : settingKeys()) {
        if (name == k.name)
            return k.set(settings, value);
    }
    std::string known;
    for (const auto& k : settingKeys())
        known += std::string{known.empty() ? "" : ", "} + k.name;
    return std::format("unknown setting '{}' (one of: {})", name, known);
}

std::string describeSettings(const SSettings& settings) {
    std::string out;
    for (const auto& key : settingKeys())
        out += std::format("{} {}\n", key.name, key.get(settings));
    return out;
}
