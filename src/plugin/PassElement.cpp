#include "plugin/PassElement.hpp"
#include "core/Look.hpp"
#include "gl/Gl.hpp"
#include "plugin/Globals.hpp"

#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// wall time for what moves with real time whatever time_scale says (ILook::setWallClock)
uint64_t nowNanoseconds() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void notify(const std::string& text) {
    HyprlandAPI::addNotification(PHANDLE, std::format("[{}] {}", PLUGIN_NAME, text.substr(0, 400)), CHyprColor{1.0, 0.3, 0.3, 1.0}, 8000);
}

bool sameGeometry(const SGeometry& a, const SGeometry& b) {
    return a.monW == b.monW && a.monH == b.monH && a.cellsW == b.cellsW && a.cellsH == b.cellsH;
}

// The looks tried, in order, when the one asked for fails to build: the two cheapest.
constexpr std::array<std::string_view, 2> FALLBACKS = {"pattern", "marble"};

// Makes and builds `wanted` for the monitor; a look that fails falls back to the first
// of FALLBACKS that builds, with a notification naming the look and the error (kept in
// m.error for `status`). False when nothing could be built: the plugin draws nothing
// there and Hyprland's own background shows, until a change or `hyprctl hyprns reset`.
bool install(SMonitorLook& m, const std::string& wanted, const std::string& monitorName, const SGeometry& g, float quality) {
    const auto& settings = g_pGlobalState->settings;
    m.look.reset(); // the old look's GL objects go before the new one's are made
    m.name     = wanted;
    m.shown.clear();
    m.error.clear();
    m.geometry = g;
    m.quality  = quality;
    m.failed   = false;
    m.resetRequested = false; // a new look starts from rest anyway

    const auto attempt = [&](const std::string& name, std::string* error) -> std::unique_ptr<ILook> {
        auto look = makeLook(name, seedFor(monitorName));
        if (!look) {
            *error = std::format("no such look (one of: {})", lookNameList());
            return nullptr;
        }
        // the frame rate before the first build: a bake sized by it is made once
        look->setFrameRate(static_cast<float>(settings.fps));
        if (!look->build(g, quality, error))
            return nullptr;
        return look;
    };

    std::string error;
    if (auto look = attempt(wanted, &error)) {
        m.look  = std::move(look);
        m.shown = wanted;
        return true;
    }
    error = std::format("{} failed: {}", wanted, error);
    for (const auto fallbackName : FALLBACKS) {
        const std::string fallback{fallbackName};
        if (fallback == wanted)
            continue;
        std::string fallbackError;
        if (auto look = attempt(fallback, &fallbackError)) {
            notify(std::format("{}: the look {}; showing {}", monitorName, error, fallback));
            m.look  = std::move(look);
            m.shown = fallback;
            m.error = std::move(error);
            return true;
        }
        error += std::format("; {} failed too: {}", fallback, fallbackError);
    }
    notify(std::format("{}: the look {}; wallpaper off", monitorName, error));
    m.error  = std::move(error);
    m.failed = true;
    return false;
}

// Rebuilds the look in place for a new size, transform or quality; a failure goes
// through install's fallback.
bool rebuild(SMonitorLook& m, const std::string& monitorName, const SGeometry& g, float quality) {
    std::string error;
    if (m.look && m.look->build(g, quality, &error)) {
        m.geometry = g;
        m.quality  = quality;
        return true;
    }
    return install(m, m.name, monitorName, g, quality);
}

// The mean starts again: what follows runs at other levels, another size or another look.
void restartMean(SMonitorLook& m) {
    m.mean.restart();
    m.timer.discard();
}

} // namespace

std::vector<UP<IPassElement>> CNsPassElement::draw() {
    if (!g_pGlobalState)
        return {};

    const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!monitor)
        return {};

    // Everything our passes change comes back when this ends. Hyprland caches the bound
    // program (and the viewport and blend/scissor caps): the guard puts the raw values
    // back, the program with glUseProgram, so its caches stay true.
    Gl::CStateGuard guard;

    const auto&  settings = g_pGlobalState->settings;
    auto&        m        = g_pGlobalState->sims[monitor->m_id];
    const double now      = nowSeconds();
    // Back after a dark spell (screen off, fullscreen window): a fresh mean, and the
    // results still in flight dropped, so the first decision sees neither the window
    // from before nor the slow first frames (the warm-up takes those).
    // The latch keeps it for the next frame that moves the picture on, which may be a
    // later one: Hyprland often redraws first for its own reasons.
    if (m.dark.see(m.lastFrame, now))
        restartMean(m);
    m.lastFrame = now;

    // The caller's target, for the final draw.
    GLint target = 0, viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &target);
    glGetIntegerv(GL_VIEWPORT, viewport);

    // Each pass overwrites its whole target: no blending; offscreen ones no scissor.
    glDisable(GL_BLEND);
    bool timed = false, scaled = false, advancing = false, pictureNew = false;
    {
        // the caller's framebuffers, viewport and scissor come back for the final draw
        Gl::CStateGuard offscreen;
        glDisable(GL_SCISSOR_TEST);

        // The monitor in its own orientation (e.g. 1440x2560 on a portrait monitor).
        const SGeometry   g      = geometryFor(static_cast<int>(monitor->m_transformedSize.x), static_cast<int>(monitor->m_transformedSize.y));
        const std::string wanted = settings.lookFor(monitor->m_name);

        // The budget: the GPU time of the frames measured so far decides the quality and
        // the pace. Made again when budget_ms or quality changes.
        if (!m.budget || m.budget->budgetMs() != settings.budgetMs || m.budget->fixedQuality() != settings.quality) {
            m.budget.emplace(settings.budgetMs, settings.quality);
            restartMean(m);
        }
        static std::vector<float> samples; // the render thread only
        samples.clear();
        m.timer.poll(samples);
        for (const float ms : samples)
            m.mean.add(ms);
        const SBudgetState levels = m.budget->update(m.mean.mean(), now);
        if (levels.quality != m.levels.quality || levels.timeScale != m.levels.timeScale)
            restartMean(m);
        m.levels      = levels;
        const float q = levels.quality;

        // a look built, rebuilt or reset in this frame takes its first step now
        bool fresh = false;
        if (!m.look) {
            // first frame, or nothing could be built: try again only after a change
            if (m.failed && wanted == m.name && sameGeometry(g, m.geometry) && q == m.quality)
                return {};
            m.pending.clear();
            m.fade = 1.0f;
            restartMean(m);
            if (!install(m, wanted, monitor->m_name, g, q))
                return {};
            fresh = true;
        } else {
            if (!sameGeometry(g, m.geometry) || q != m.quality) {
                restartMean(m);
                if (!rebuild(m, monitor->m_name, g, q))
                    return {};
                fresh = true;
            }
            // a look change: fade out, switch, fade in. A second change while fading out
            // only changes where it goes; going back to the shown look fades back in.
            m.pending = wanted == m.name ? std::string{} : wanted;
        }

        // Only frames the timer asked for (and a fresh look's first) move the picture on:
        // those are the frames that can refresh Hyprland's blur cache (main.cpp,
        // requestFrame). Any other redraw shows the picture as it is.
        advancing = m.gate.take(fresh || m.resetRequested);
        if (advancing) {
            const double dt = fadeDt(m.lastAdvance, now, settings.fps);
            m.lastAdvance   = now;
            if (!m.pending.empty()) {
                m.fade = fadeAfter(m.fade, dt, true);
                if (m.fade <= 0.0f) {
                    const std::string next = std::exchange(m.pending, {});
                    restartMean(m);
                    if (!install(m, next, monitor->m_name, g, q))
                        return {};
                }
            } else
                m.fade = fadeAfter(m.fade, dt, false);

            // `hyprctl hyprns reset`, asked outside the renderer, runs here under the guards
            if (std::exchange(m.resetRequested, false))
                m.look->reset();
        }

        // Below quality 1 the look draws into a smaller target, stretched over the
        // monitor below; at 1 it draws straight into the monitor's framebuffer, as before.
        scaled = q < 1.0f;
        if (scaled) {
            std::string error;
            const GLuint oldFbo = m.picture.fbo();
            const int    oldW = m.picture.width(), oldH = m.picture.height();
            const bool   made = m.picture.ensure(Gl::scaledSide(g.monW, q), Gl::scaledSide(g.monH, q), &error);
            // a target made in this frame holds nothing yet: the look draws into it
            pictureNew = made && (m.picture.fbo() != oldFbo || m.picture.width() != oldW || m.picture.height() != oldH);
            if (!made) {
                if (!m.pictureFailed)
                    notify(std::format("{}: quality {}: {}; drawing at full size", monitor->m_name, q, error));
                m.pictureFailed = true;
                scaled          = false;
            } else
                m.pictureFailed = false;
        }
        if (!scaled)
            m.picture.release();

        if (advancing) {
            // Timed: only frames that step the look, so the budget's mean is the cost of
            // a real frame. The budget's pace applies to every look: it is the second lever.
            timed = m.timer.begin();
            m.look->setFrameRate(static_cast<float>(settings.fps));
            m.look->setWallClock(nowNanoseconds());
            // the first frame after a dark spell (or a frozen spell, when the clock did
            // not run) catches up two frames' steps, not 64; every other frame counts its
            // whole time (up to MAX_DT), so a redraw rate below fps keeps the pace
            // (CStepClock::maxDtFor)
            const double maxDt = CStepClock::maxDtFor(settings.fps, m.dark.take());
            m.look->advance(m.clock.tick(now, clockRate(*m.look, settings.timeScale) * levels.timeScale, maxDt));
        }
        if (scaled && (advancing || pictureNew))
            m.look->draw(m.picture.fbo(), 0, 0, m.picture.width(), m.picture.height(), Gl::FULL, m.fade);
    }

    // The box in screen orientation; the projection rotates it into the framebuffer,
    // so the picture's top row is the top of what is seen. (Checked on a nested
    // output with transform 1: an inverse-monitor texture transform, as for
    // sampling the framebuffer, turns the picture sideways.)
    const CBox box{0, 0, monitor->m_transformedSize.x, monitor->m_transformedSize.y};
    Mat3x3     matrix = g_pHyprRenderer->projectBoxToTarget(box);
    matrix.transpose();
    // Opaque: the wallpaper replaces whatever the background layer drew.
    if (scaled)
        m.picture.stretch(static_cast<GLuint>(target), viewport[0], viewport[1], viewport[2], viewport[3], matrix.getMatrix());
    else
        m.look->draw(static_cast<GLuint>(target), viewport[0], viewport[1], viewport[2], viewport[3], matrix.getMatrix(), m.fade);
    if (timed)
        m.timer.end();

    // The blown-up check, never waiting for the GPU: every frame, a verdict that has
    // arrived is taken (BLOWN: reset; a look defers a reset's GL work to its next
    // advance); every 2 s a new check is queued (a reduction of the lattice and a read
    // of a few bytes behind a fence; nothing while one is in flight). After the frame's
    // steps (a look built or reset this frame has run its first initialised step by
    // now), and after the timer, so it is not counted as the look's GPU time. Its own
    // guard: no scissor, no blending, and the caller's state back afterwards.
    {
        Gl::CStateGuard health;
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        if (m.look->takeHealth(false) == EHealth::BLOWN) {
            m.look->reset(); // its first step comes with the timer's next frame
            m.resets++;
        }
        if (m.lastHealth < 0)
            m.lastHealth = now;
        else if (now - m.lastHealth >= HEALTH_SECONDS) {
            m.lastHealth = now;
            m.look->checkHealth();
        }
    }
    return {};
}
