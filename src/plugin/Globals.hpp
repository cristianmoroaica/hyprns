#pragma once

#include "core/Budget.hpp"
#include "core/FramePace.hpp"
#include "core/Look.hpp"
#include "core/StepClock.hpp"
#include "gl/Gl.hpp"
#include "plugin/PluginConfig.hpp"

#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class CEventLoopTimer;

inline constexpr const char* PLUGIN_NAME    = "hyprns";
inline constexpr const char* PLUGIN_AUTHOR  = "Cristian Moroaica";
inline constexpr const char* PLUGIN_VERSION = "0.1.0";

inline HANDLE PHANDLE = nullptr;

// What one monitor shows. A look change sets `pending`: fade runs to 0 over
// FADE_SECONDS, the look is replaced, fade runs back to 1 over FADE_SECONDS.
struct SMonitorLook {
    std::string            name;  // the look asked for
    std::string            shown; // the look built and drawn: name, or a fallback when name failed; empty = none
    std::string            error; // why `name` (and the fallbacks) failed to build; empty when name is shown
    std::unique_ptr<ILook> look;
    CStepClock             clock;
    SGeometry              geometry{};
    float                  fade = 1.0f;
    std::string            pending; // the look to switch to; empty = none

    float    quality    = 0.0f; // the quality `look` was built with

    // The GPU time budget. The timer wraps the look's advance and draw (and the
    // stretch); its results feed `mean`, the mean feeds `budget`. Any change of level,
    // look or size restarts the mean, so no decision mixes frames of two levels.
    Gl::CGpuTimer          timer;
    CGpuMean               mean;
    std::optional<CBudget> budget; // made again when budget_ms or quality changes
    SBudgetState           levels;
    Gl::CPictureTarget     picture;        // the look's target while quality < 1
    bool                   pictureFailed  = false; // the target could not be made (notified once)
    bool                   resetRequested = false; // `hyprctl hyprns reset`: done in the next draw
    uint64_t resets     = 0;    // a health check said BLOWN, reset() called
    double   lastHealth = -1.0; // seconds, when the last health check was queued
    double   lastFrame  = -1.0; // seconds, the last drawn frame (dark spells)
    double   lastAdvance = -1.0; // seconds, the last frame that moved the picture on (fade timing)
    double   lastBlurRefresh = -1.0; // seconds, the last timer frame that marked the blur cache stale
    uint64_t blurRefreshes   = 0;    // timer frames that marked it stale (status JSON, for checks)
    CAdvanceGate gate;               // the timer asks for a frame that moves the picture on
    CDarkLatch   dark;               // a dark spell ended: the next advance catches up little
    bool     frozen     = false; // the timer's last tick found the picture frozen
    bool     failed     = false; // nothing could be built for name/geometry/quality: wait for a change
};

inline constexpr double HEALTH_SECONDS = 2.0;

struct SGlobalState {
    std::vector<Hyprutils::Signal::CHyprSignalListener> listeners;
    SP<CEventLoopTimer>                                 frameTimer;
    SP<SHyprCtlCommand>                                 ctlCommand;
    SSettings                                           settings;
    // one look per monitor, by monitor id; made on the monitor's first frame
    std::unordered_map<int64_t, SMonitorLook> sims;
};

inline std::unique_ptr<SGlobalState> g_pGlobalState;
