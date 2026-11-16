#include "plugin/PassElement.hpp"
#include "plugin/Globals.hpp"
#include "plugin/PluginConfig.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/Pass.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <stdexcept>
#include <string>
#include <unordered_set>

// Asks Hyprland for a frame of the monitor; refreshBlur also marks its blur cache stale.
//
// With decoration:blur:new_optimizations on (Hyprland's default), tiled windows (and any
// window or layer with xray) take their blur from a per-monitor cache: a blurred copy of
// the wallpaper and the background and bottom layers. Hyprland rebuilds that cache only
// in a frame where m_blurFBDirty is set. The plugin's picture is a pass element, which
// sets nothing, so the flag is set here.
//
// What it costs (Hyprland 0.56.2, CHyprOpenGLImpl::preRender): the flag stays pending,
// at no cost, until Hyprland judges that something may read the cache. That is the case
// when the monitor's active workspace has a visible tiled window (any window with xray)
// without noblur whose blur is on: opacity below 1, or an opaque region that does not
// cover the whole surface (rounded corners, CSD shadows, a client that sets none); or
// when any layer surface with an xray rule exists, on any monitor. Then the frame blurs
// the whole monitor once (decoration:blur passes), whether or not anything samples it.
// So with blur_fps -1 that is one full blur of the monitor per wallpaper frame, fps
// times a second. That GPU time is Hyprland's: the plugin's timer, budget_ms and
// `status` do not see it. blur_fps (blurRefreshDue) caps how often the timer sets it.
static void requestFrame(const PHLMONITOR& monitor, bool refreshBlur) {
    if (refreshBlur)
        monitor->m_blurFBDirty = true;
    g_pHyprRenderer->damageMonitor(monitor);
}

// On load, unload, `set`, `reset` and a config reload: every monitor, including those
// the plugin does not draw on, is redrawn and its blur cache refreshed once, whatever
// blur_fps says. On unload and `enabled 0` this is what takes the wallpaper out of the
// cache; elsewhere it is at most one extra blur per change.
static void damageAllMonitors() {
    for (const auto& monitor : State::monitorState()->monitors())
        requestFrame(monitor, true);
}

static double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// in microseconds: 1000 / fps in whole milliseconds would run 60 fps at 62.5 and
// 144 at 166.7
static std::chrono::microseconds frameInterval() {
    return std::chrono::microseconds(1000000 / std::max(1, g_pGlobalState->settings.fps));
}

// Asks for a new frame on every monitor that shows the wallpaper, and drops the
// lattices of monitors that are gone or no longer show it.
static void onFrameTimer(SP<CEventLoopTimer> self, void*) {
    if (!g_pGlobalState)
        return;
    const auto&                 settings = g_pGlobalState->settings;
    const double                now      = nowSeconds();
    std::unordered_set<int64_t> shown;
    for (const auto& monitor : State::monitorState()->monitors()) {
        if (!settings.enabled || !settings.wantsMonitor(monitor->m_name))
            continue;
        shown.insert(monitor->m_id);
        // decideFrame (FramePace) says what this monitor gets:
        // - Nothing while the screen is off or a fullscreen window covers it; the flow
        //   waits there (it only steps when drawn). Damage under a fullscreen window would
        //   also cost it direct scanout. Nothing either where no look could be built: a
        //   change (which damages the monitor) rebuilds.
        // - A frozen picture (time_scale 0, a whole picture) gets one last frame with a
        //   blur refresh, then none, except a look that moves with wall time (jupiter's
        //   grain), which gets frames without refresh. A look still baking its picture is
        //   not frozen: it needs frames to finish it.
        const auto   sim = g_pGlobalState->sims.find(monitor->m_id);
        SFrameInputs in;
        in.visible = monitor->m_enabled && monitor->m_dpmsStatus && !Fullscreen::controller()->hasFullscreen(monitor, true);
        in.blurFps = settings.blurFps;
        in.fps     = settings.fps;
        in.now     = now;
        if (sim != g_pGlobalState->sims.end()) {
            const auto& m      = sim->second;
            in.failed          = m.failed;
            in.hasLook         = m.look != nullptr;
            in.wallClock       = m.look && m.look->info().wallClock;
            in.settled         = !m.look || m.look->settled();
            in.wasFrozen       = m.frozen;
            in.switching       = !m.pending.empty();
            in.resetWaiting    = m.resetRequested;
            in.timeScale       = settings.timeScale;
            in.pace            = m.levels.timeScale;
            in.fade            = m.fade;
            in.lastBlurRefresh = m.lastBlurRefresh;
        }
        const SFrameDecision d = decideFrame(in);
        if (sim != g_pGlobalState->sims.end()) {
            auto& m  = sim->second;
            m.frozen = d.frozen;
            if (d.dark)
                m.dark.mark(); // the clock stands still: the next advance catches up little
            if (d.blur) {
                m.lastBlurRefresh = now;
                m.blurRefreshes++;
            }
            if (d.frame)
                m.gate.request();
        }
        if (d.frame)
            requestFrame(monitor, d.blur);
    }
    // a dropped lattice deletes GL objects: Hyprland's context, made current for it
    if (std::ranges::any_of(g_pGlobalState->sims, [&](const auto& kv) { return !shown.contains(kv.first); })) {
        Render::GL::g_pHyprOpenGL->makeEGLCurrent();
        std::erase_if(g_pGlobalState->sims, [&](const auto& kv) { return !shown.contains(kv.first); });
    }
    self->updateTimeout(frameInterval());
}

static void onRenderStage(eRenderStage stage) {
    if (stage != RENDER_POST_WALLPAPER || !g_pGlobalState)
        return;
    const auto& settings = g_pGlobalState->settings;
    const auto  monitor  = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!settings.enabled || !monitor || !settings.wantsMonitor(monitor->m_name))
        return;
    // Every render of the monitor gets the wallpaper, screenshots and workspace
    // snapshots included: they would otherwise show the background layer instead.
    g_pHyprRenderer->m_renderPass.add(makeUnique<CNsPassElement>());
}

static void onConfigReloaded() {
    if (!g_pGlobalState)
        return;
    // config values replace anything `hyprctl hyprns set` changed
    g_pGlobalState->settings = readConfig();
    damageAllMonitors();
}

// ── hyprctl hyprns status | set KEY VALUE | reset ──────────────────────────────

static std::string jsonEscape(std::string_view s) {
    std::string out;
    for (const char c : s) {
        if (c == '"' || c == '\\')
            out += '\\';
        if (static_cast<unsigned char>(c) < 0x20)
            out += std::format("\\u{:04x}", static_cast<unsigned>(c));
        else
            out += c;
    }
    return out;
}

// `q 0.75, GPU 1.412 ms/frame` or `q 1 (unmeasured)`, then the pace when the budget
// lowered it and `over budget: raise budget_ms or lower fps` when even the lowest levels
// are over (the controller has no lever left; those two are the user's).
static std::string budgetLine(const SMonitorLook& m) {
    const float gpu = m.mean.last();
    std::string out = gpu < 0 ? std::format("q {} (unmeasured)", m.levels.quality) : std::format("q {}, GPU {:.3f} ms/frame", m.levels.quality, gpu);
    if (m.levels.timeScale != 1.0f)
        out += std::format(", pace {}", m.levels.timeScale);
    if (m.levels.overBudget)
        out += ", over budget: raise budget_ms or lower fps";
    return out;
}

static std::string ctlStatus(eHyprCtlOutputFormat format) {
    const auto& state = *g_pGlobalState;
    const bool  json  = format == eHyprCtlOutputFormat::FORMAT_JSON;
    std::string out   = json ? "{\"monitors\": [" : "";
    bool        first = true;
    for (const auto& monitor : State::monitorState()->monitors()) {
        const auto it = state.sims.find(monitor->m_id);
        if (it == state.sims.end())
            continue;
        const auto&       m    = it->second;
        const std::string look = m.look ? m.look->status() : "nothing shown";
        // a failed look says why, and what is shown instead
        const std::string line = m.error.empty() ? look : m.look ? std::format("{}; showing {}: {}", m.error, m.shown, look) : std::format("{}; {}", m.error, look);
        const float       gpu  = m.mean.last();
        if (json)
            out += std::format("{}{{\"name\": \"{}\", \"look\": \"{}\", \"shown\": \"{}\", \"error\": \"{}\", \"pending\": \"{}\", \"fade\": {}, \"resets\": {}, "
                               "\"cells\": [{}, {}], \"q\": {}, \"time_scale\": {}, \"over_budget\": {}, \"blur_refreshes\": {}, \"gpu_ms_per_frame\": {}, \"status\": \"{}\"}}",
                               first ? "" : ", ", jsonEscape(monitor->m_name), jsonEscape(m.name), jsonEscape(m.shown), jsonEscape(m.error), jsonEscape(m.pending), m.fade,
                               m.resets, m.geometry.cellsW, m.geometry.cellsH, m.levels.quality, m.levels.timeScale, m.levels.overBudget,
                               m.blurRefreshes, gpu < 0 ? std::string{"null"} : std::format("{:.3f}", gpu), jsonEscape(look));
        else
            out += std::format("{}: {}; {}{}\n", monitor->m_name, line, budgetLine(m),
                               m.pending.empty() ? (m.fade < 1.0f ? std::format(" (fading in, {:.2f})", m.fade) : std::string{})
                                                 : std::format(" (switching to {}, fade {:.2f})", m.pending, m.fade));
        first = false;
    }
    if (json) {
        out += "], \"settings\": {";
        bool              firstKey = true;
        const std::string described = describeSettings(state.settings);
        for (std::string_view rest = described; !rest.empty();) {
            const auto nl   = rest.find('\n');
            const auto line = rest.substr(0, nl);
            rest            = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
            const auto sp   = line.find(' ');
            out += std::format("{}\"{}\": \"{}\"", firstKey ? "" : ", ", line.substr(0, sp), jsonEscape(sp == std::string_view::npos ? "" : line.substr(sp + 1)));
            firstKey = false;
        }
        out += "}}\n";
    } else {
        if (first)
            out += "no monitor shows the wallpaper\n";
        out += "\n" + describeSettings(state.settings);
    }
    return out;
}

static std::string onHyprCtl(eHyprCtlOutputFormat format, std::string request) {
    if (!g_pGlobalState)
        return "hyprns: not loaded\n";
    std::string_view rest{request};
    if (rest.starts_with(PLUGIN_NAME))
        rest.remove_prefix(std::string_view{PLUGIN_NAME}.size());
    while (!rest.empty() && rest.front() == ' ')
        rest.remove_prefix(1);

    if (rest.empty() || rest == "status")
        return ctlStatus(format);

    if (rest == "reset") {
        // carried out in each monitor's next draw, under the renderer's state guard: no
        // look GL call runs out here. A monitor where no look could be built tries again.
        for (auto& [id, m] : g_pGlobalState->sims) {
            m.resetRequested = true;
            m.failed         = false;
        }
        damageAllMonitors();
        return "hyprns: flows restarted\n";
    }

    if (rest.starts_with("set ")) {
        rest.remove_prefix(4);
        while (!rest.empty() && rest.front() == ' ')
            rest.remove_prefix(1);
        const auto sp = rest.find(' ');
        if (sp == std::string_view::npos)
            return "hyprns: usage: hyprctl hyprns set KEY VALUE\n";
        if (const auto err = applySetting(g_pGlobalState->settings, rest.substr(0, sp), rest.substr(sp + 1)); !err.empty())
            return std::format("hyprns: {}\n", err);
        damageAllMonitors();
        return "ok\n";
    }

    return "hyprns: usage: hyprctl hyprns <status|set KEY VALUE|reset>  (-j for JSON status)\n";
}

// ── plugin entry points ─────────────────────────────────────────────────────────

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    // The plugin passes C++ objects to Hyprland: built against other headers (another
    // Hyprland commit or library versions) it would crash the compositor. Refuse to load,
    // before anything is registered, and say why.
    const std::string server = __hyprland_api_get_hash(), client = __hyprland_api_get_client_hash();
    if (server != client) {
        HyprlandAPI::addNotification(handle,
                                     std::format("[{}] not loaded: built for another Hyprland ({}), this one is {}; rebuild it (hyprpm update, or make) "
                                                 "against the running Hyprland's headers",
                                                 PLUGIN_NAME, client, server),
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 15000);
        throw std::runtime_error(std::format("[{}] version mismatch: built for {}, running {}", PLUGIN_NAME, client, server));
    }

    g_pGlobalState = std::make_unique<SGlobalState>();

    registerConfig(handle);
    g_pGlobalState->settings = readConfig();

    auto& listeners = g_pGlobalState->listeners;
    listeners.push_back(Event::bus()->m_events.render.stage.listen([](eRenderStage stage) { onRenderStage(stage); }));
    listeners.push_back(Event::bus()->m_events.config.reloaded.listen([] { onConfigReloaded(); }));

    g_pGlobalState->ctlCommand = HyprlandAPI::registerHyprCtlCommand(handle, SHyprCtlCommand{.name = PLUGIN_NAME, .exact = false, .fn = onHyprCtl});

    g_pGlobalState->frameTimer = makeShared<CEventLoopTimer>(frameInterval(), onFrameTimer, nullptr);
    g_pEventLoopManager->addTimer(g_pGlobalState->frameTimer);

    // Redraw every monitor now so the wallpaper appears without waiting for damage.
    damageAllMonitors();

    return {PLUGIN_NAME, "Live Navier–Stokes wallpaper", PLUGIN_AUTHOR, PLUGIN_VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
    if (!g_pGlobalState)
        return;
    g_pGlobalState->listeners.clear();
    if (g_pGlobalState->ctlCommand) {
        HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, g_pGlobalState->ctlCommand);
        g_pGlobalState->ctlCommand.reset();
    }
    // the timer's callback is code in this library
    if (g_pGlobalState->frameTimer) {
        g_pGlobalState->frameTimer->cancel();
        g_pEventLoopManager->removeTimer(g_pGlobalState->frameTimer);
        g_pGlobalState->frameTimer.reset();
    }
    // The render pass keeps the last frame's elements until the next frame: one left
    // there is destroyed through this library's vtable after dlclose (seen: SEGV on
    // the first frame after `hyprctl plugin unload`, 2026-11-15, nested test).
    g_pHyprRenderer->m_renderPass.removeAllOfType("CNsPassElement");
    // The GL objects are deleted here, in Hyprland's one EGL context (made current, not
    // assumed to be).
    if (Render::GL::g_pHyprOpenGL)
        Render::GL::g_pHyprOpenGL->makeEGLCurrent();
    g_pGlobalState->sims.clear();
    g_pGlobalState.reset();

    damageAllMonitors();
}
