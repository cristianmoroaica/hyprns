// hyprns-bench: runs a look in a headless EGL context on one GPU, without Hyprland.
//
//   hyprns-bench --look NAME --size WxH [--device /dev/dri/renderD12N] [--steps N]
//                [--fps 30] [--quality Q] [--budget MS] [--png FILE] [--time] [--transform 0|1]
//                [--switch-to NAME --switch-at STEP] [--resize WxH --resize-at STEP]
//                [--set KEY=VALUE ...] [--force-sketch-path] [--seed NAME]
//                [--change-at STEP quality=Q|fps=N ...] [--png-at STEP FILE ...]
//   hyprns-bench --field-check [--device /dev/dri/renderD12N]
//   hyprns-bench --r16f-probe [--device /dev/dri/renderD12N]
//
// --force-sketch-path: a look with speed-ups the user accepted (core/Verdicts.hpp) runs
// the sketch's own path instead, to measure against (marble: the stone per pixel per
// frame and the displacement per step, instead of the bake and once per frame; embers:
// the heat per step, the 32-tap ring and the coals per pixel per frame, instead of once
// per frame, the blur and the bake; jupiter: the cloud after every step in R32F, instead of
// once per frame in R16F). The plugin never does.
// --seed NAME: the look's seed is seedFor(NAME) (default "bench"), as a monitor of that
// connector name gets in the plugin; --seed 0 gives seed 0 (jupiter: the sketch's own events).
//
// --look bake-test (bench only): a CBake at the picture's size, 30 frames per key, one
// step per frame at 30 fps, baking vec4(fract(uv * 8), key index / 255, 1) into RGBA8 and
// showing mix(previous, next, blend). Its JSON line has a "bake" object: the rows of
// previous, next and the picture whose every pixel has alpha 255 (cleared texels have 0),
// out of the height, and the two shown keys' indices; exit 1 unless every row is whole
// and previous is the older key.
// --switch-to prints the live GL objects just before the old look goes and just after
// the new one is built.
// --look fluid-test (bench only, not a plugin look): the shared CFluid on a 400x225
// lattice with walls, omega 1.88 and jupiter's jets and beta term, carrying a cloud
// field (CField, 1600x900) started at band(y); the picture is that field in grey. Its
// JSON line has a "fluid" object: the lattice's speed and density at the end.
// --field-check: CField alone, no look: (a) zero velocity for 1000 steps leaves a field
// bit-identical (same-size and resampled velocity); (b) a uniform (0.25, 0) cells/step
// for 4 w steps on a periodic field brings it back to its start, never leaving its
// initial range; (c) a 100x57 velocity of (0.25, 0) moves the 400x225 field exactly one
// cell per step, so 400 steps return it. One JSON line {"field_check": ...}; exit 0 when all hold.
//
// --r16f-probe: how this GPU converts a float to half on an R16F store (what jupiter's
// sround16 makes irrelevant): values 1 + f ulp for f in [0, 1) (1 ulp = 2^-10) are drawn
// into R16F and read back; truncation keeps every one at 1, round-to-nearest takes those
// over a half up. One JSON line {"r16f_probe": ...}.
//
// --set changes one plugin setting (the keys of `hyprctl hyprns set`; time_scale is the
// one the bench uses).
//
// --size is the monitor in screen orientation. Each bench frame runs the look's
// steps per second / fps simulation steps (time_scale 1) and draws the picture onto an
// RGBA8 "monitor" framebuffer, until --steps steps have run. --transform 1 makes that
// framebuffer HxW and draws through a 90-degree projection, as the plugin does for a
// rotated output; the PNG is written back in screen orientation, so it matches
// --transform 0. --time measures the GPU time of each frame with
// GL_EXT_disjoint_timer_query, as the plugin does: one query around each frame's
// advance and draw (and the stretch below quality 1), polled, never waited for (the
// results still in flight are read after a glFinish at the end); the mean skips the
// first tenth of the measured frames.
//
// --quality takes the plugin's levels: 1, 0.75, 0.5, 0.35. Below 1 the look draws into
// an RGBA8 target of round(q x monitor) pixels, stretched over the "monitor" with linear
// filtering, as the plugin does. --budget MS runs the plugin's budget controller
// (CBudget, the quality as its start and ceiling) on the measured GPU time, its clock
// advancing 1/fps s per frame: a quality change rebuilds the look, its pace multiplies
// the steps per frame. Without --budget the controller is off (the budget_ms setting is
// not used by the bench).
// --switch-to: once STEP steps have run, the picture fades out over FADE_SECONDS (0.4 s)
// of frames, the look is replaced by a new one of that name and fades back in over the
// same, as in the plugin.
// --resize: once STEP steps have run, the monitor becomes WxH and the look is rebuilt.
// Every 2 s of frames the look is asked healthy() (checkHealth(), then takeHealth
// waiting); false resets it and counts as bad. --async-health does it as the plugin
// does: takeHealth without waiting every frame, checkHealth every 2 s of frames.
// Either way the JSON gets "health_cpu_ms_max", the longest the health calls of one
// frame held the CPU (wall time), and "health_verdicts", the verdicts that arrived.
// --in-flight N: at most N frames queued on the GPU (a fence per frame, the oldest waited
// for), as a compositor's swap chain paces the plugin; without it the bench queues as
// many frames as the driver takes, and a non-waiting check's answer comes frames late.
// --reduce-check: the health reduction (CReduction) against the CPU on a 517x203
// texture with NaN and Inf cells, then exit 0 when they agree.
// --change-at STEP quality=Q|fps=N (repeatable): once STEP steps have run, the quality
// (the look rebuilt, as a budget level change in the plugin) or the frame rate (the
// look told, the steps per frame follow) changes; stderr prints the live GL objects
// before and after, and the JSON gets "changes": per change the largest frame in the
// 2 s of frames from it. With --change-at every frame is waited for (glFinish), so each
// measured time belongs to its frame; the times are still the GPU's.
// --png-at STEP FILE (repeatable, transform 0): the picture of the first frame drawn
// once STEP steps have run, as a PNG.
//
// Prints one JSON line: {"look","lattice":[w,h],"steps","gpu_ms_per_frame","gpu_ms_max",
// "q","time_scale","over_budget","live_objects","bad"} (gpu_ms_max: the largest measured
// frame past the first, whose time includes the programs' first use; q, time_scale,
// over_budget: the levels at the end)
// (fluid-test adds "fluid":{"umax","umean","rho_min","rho_max"}).
// live_objects is counted after teardown; stderr shows it before the first frame too.
// Exit 0 when the picture is not uniform, the look is not blown up and live_objects at
// teardown equals the count before the first frame; 1 otherwise; 2 when it cannot run:
// a shader error (message on stderr), no device, no ES 3.2 context.

#include "core/Bake.hpp"
#include "core/Budget.hpp"
#include "core/Fields.hpp"
#include "core/Fluid.hpp"
#include "core/FramePace.hpp"
#include "core/Look.hpp"
#include "core/Reduce.hpp"
#include "gl/Gl.hpp"
#include "core/Settings.hpp"
#include "looks/Embers.hpp"
#include "looks/Jupiter.hpp"
#include "looks/Marble.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>
#include <png.h>

#include <algorithm>
#include <charconv>
#include <deque>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct SArgs {
    std::string look;
    int         w = 0, h = 0;
    std::string device;
    SSettings   settings; // defaults, changed by --set
    long        steps     = 2400;
    float       fps       = 30.0f;
    float       quality   = 1.0f;
    bool        budget    = false; // --budget given: settings.budgetMs is the budget
    std::string png;
    bool        time      = false;
    int         transform = 0;
    std::string switchTo;
    long        switchAt = -1;
    int         resizeW = 0, resizeH = 0;
    long        resizeAt = -1;
    bool        fieldCheck = false;
    bool        r16fProbe  = false;
    int         inFlight   = 0;
    bool        reduceCheck = false;
    bool        asyncHealth = false;
    bool        forceSketch = false;
    std::string seedName    = "bench"; // --seed: the monitor name the look's seed comes from
    struct SChange {
        long        at;
        std::string key; // quality or fps
        float       value;
    };
    std::vector<SChange> changes; // --change-at
    struct SGrab {
        long        at;
        std::string file;
    };
    std::vector<SGrab> grabs; // --png-at
    long               statusEvery = 0; // --status-every: a line per this many steps (jupiter's span, other looks' mean density)
    bool               churn       = false; // --churn: 200 switches, 200 quality changes, 50 resizes
};

// Bench-only looks: never in lookNames(), never in the plugin.
constexpr std::string_view FLUID_TEST = "fluid-test";
constexpr std::string_view BAKE_TEST  = "bake-test";

bool isBenchLook(std::string_view name) {
    return isLookName(name) || name == FLUID_TEST || name == BAKE_TEST;
}

[[noreturn]] void die(int code, const std::string& msg) {
    std::fprintf(stderr, "hyprns-bench: %s\n", msg.c_str());
    std::exit(code);
}

template <typename T>
bool parseNumber(std::string_view s, T& out) {
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && p == s.data() + s.size();
}

SArgs parseArgs(int argc, char** argv) {
    SArgs      a;
    const auto value = [&](int& i) -> std::string_view {
        if (i + 1 >= argc)
            die(2, std::format("{} needs a value", argv[i]));
        return argv[++i];
    };
    const auto size = [&](int& i, int& w, int& h) {
        const auto name = argv[i];
        const auto v    = value(i);
        const auto x    = v.find('x');
        if (x == std::string_view::npos || !parseNumber(v.substr(0, x), w) || !parseNumber(v.substr(x + 1), h) || w < 16 || h < 16 || w > 16384 || h > 16384)
            die(2, std::format("{} wants WxH, got {}", name, v));
    };
    for (int i = 1; i < argc; i++) {
        const std::string_view arg = argv[i];
        if (arg == "--look")
            a.look = value(i);
        else if (arg == "--size")
            size(i, a.w, a.h);
        else if (arg == "--switch-to")
            a.switchTo = value(i);
        else if (arg == "--switch-at") {
            if (!parseNumber(value(i), a.switchAt) || a.switchAt < 0)
                die(2, "--switch-at wants a step count >= 0");
        } else if (arg == "--resize")
            size(i, a.resizeW, a.resizeH);
        else if (arg == "--resize-at") {
            if (!parseNumber(value(i), a.resizeAt) || a.resizeAt < 0)
                die(2, "--resize-at wants a step count >= 0");
        } else if (arg == "--set") {
            const auto v  = value(i);
            const auto eq = v.find('=');
            if (eq == std::string_view::npos)
                die(2, std::format("--set wants KEY=VALUE, got {}", v));
            if (const auto err = applySetting(a.settings, v.substr(0, eq), v.substr(eq + 1)); !err.empty())
                die(2, std::format("--set: {}", err));
        } else if (arg == "--device")
            a.device = value(i);
        else if (arg == "--steps") {
            if (!parseNumber(value(i), a.steps) || a.steps < 0)
                die(2, "--steps wants a count >= 0");
        } else if (arg == "--fps") {
            if (!parseNumber(value(i), a.fps) || !(a.fps > 0))
                die(2, "--fps wants a number > 0");
        } else if (arg == "--quality") {
            // the plugin's own check: exactly one of the four levels
            if (const auto err = applySetting(a.settings, "quality", value(i)); !err.empty())
                die(2, std::format("--quality: {}", err));
            a.quality = a.settings.quality;
        } else if (arg == "--budget") {
            if (const auto err = applySetting(a.settings, "budget_ms", value(i)); !err.empty())
                die(2, std::format("--budget: {}", err));
            a.budget = true;
        } else if (arg == "--png")
            a.png = value(i);
        else if (arg == "--time")
            a.time = true;
        else if (arg == "--field-check")
            a.fieldCheck = true;
        else if (arg == "--r16f-probe")
            a.r16fProbe = true;
        else if (arg == "--in-flight" && i + 1 < argc)
            a.inFlight = std::max(0, std::atoi(argv[++i]));
        else if (arg == "--reduce-check")
            a.reduceCheck = true;
        else if (arg == "--async-health")
            a.asyncHealth = true;
        else if (arg == "--force-sketch-path")
            a.forceSketch = true;
        else if (arg == "--seed")
            a.seedName = value(i);
        else if (arg == "--change-at") {
            long at = -1;
            if (!parseNumber(value(i), at) || at < 0)
                die(2, "--change-at wants a step count >= 0, then KEY=VALUE");
            const auto v  = value(i);
            const auto eq = v.find('=');
            const auto key = std::string(v.substr(0, eq == std::string_view::npos ? 0 : eq));
            float      x  = 0;
            if (eq == std::string_view::npos || (key != "quality" && key != "fps") || !parseNumber(v.substr(eq + 1), x) || !(x > 0))
                die(2, std::format("--change-at STEP wants quality=Q or fps=N, got {}", v));
            if (key == "quality") {
                SSettings s;
                if (const auto err = applySetting(s, "quality", v.substr(eq + 1)); !err.empty())
                    die(2, std::format("--change-at: {}", err));
            }
            a.changes.push_back({at, key, x});
        } else if (arg == "--png-at") {
            long at = -1;
            if (!parseNumber(value(i), at) || at < 0)
                die(2, "--png-at wants a step count >= 0, then FILE");
            a.grabs.push_back({at, std::string(value(i))});
        }
        else if (arg == "--churn")
            a.churn = true;
        else if (arg == "--status-every") {
            if (!parseNumber(value(i), a.statusEvery) || a.statusEvery <= 0)
                die(2, "--status-every wants a step count > 0");
        } else if (arg == "--transform") {
            if (!parseNumber(value(i), a.transform) || (a.transform != 0 && a.transform != 1))
                die(2, "--transform wants 0 or 1");
        } else
            die(2, std::format("unknown argument {}", arg));
    }
    if (a.fieldCheck || a.r16fProbe || a.reduceCheck)
        return a;
    if (a.look.empty() || !a.w)
        die(2, "usage: hyprns-bench --look NAME --size WxH [--device /dev/dri/renderD12N] [--steps N] [--fps 30] [--quality Q] [--budget MS] [--png FILE] [--time] [--transform 0|1] "
               "[--switch-to NAME --switch-at STEP] [--resize WxH --resize-at STEP] [--set KEY=VALUE ...] [--force-sketch-path] [--seed NAME]\n"
               "       [--change-at STEP quality=Q|fps=N ...] [--png-at STEP FILE ...] [--status-every STEPS] [--churn]\n"
               "       hyprns-bench --field-check [--device /dev/dri/renderD12N]\n"
               "       hyprns-bench --r16f-probe [--device /dev/dri/renderD12N]\n"
               "       hyprns-bench --reduce-check [--device /dev/dri/renderD12N]\n"
               "  --force-sketch-path: the sketch's own path where a speed-up was accepted (marble: stone per pixel, displacement per step; embers: heat per step, ring, coals per pixel; jupiter: cloud per step in R32F)");
    for (const auto& name : {a.look, a.switchTo})
        if (!name.empty() && !isBenchLook(name))
            die(2, std::format("unknown look {} (one of: {}; bench only: {}, {})", name, lookNameList(), FLUID_TEST, BAKE_TEST));
    if (a.switchTo.empty() != (a.switchAt < 0))
        die(2, "--switch-to and --switch-at go together");
    if (!a.resizeW != (a.resizeAt < 0))
        die(2, "--resize and --resize-at go together");
    if (!a.grabs.empty() && a.transform != 0)
        die(2, "--png-at only with --transform 0");
    std::stable_sort(a.changes.begin(), a.changes.end(), [](const auto& x, const auto& y) { return x.at < y.at; });
    std::stable_sort(a.grabs.begin(), a.grabs.end(), [](const auto& x, const auto& y) { return x.at < y.at; });
    return a;
}

bool hasExtension(const char* list, std::string_view name) {
    if (!list)
        return false;
    for (std::string_view rest = list; !rest.empty();) {
        const auto sp  = rest.find(' ');
        const auto ext = rest.substr(0, sp);
        if (ext == name)
            return true;
        rest = sp == std::string_view::npos ? std::string_view{} : rest.substr(sp + 1);
    }
    return false;
}

template <typename T>
T eglProc(const char* name) {
    auto p = reinterpret_cast<T>(eglGetProcAddress(name));
    if (!p)
        die(2, std::format("EGL has no {}", name));
    return p;
}

struct SEgl {
    EGLDisplay  display = EGL_NO_DISPLAY;
    EGLContext  context = EGL_NO_CONTEXT;
    EGLSurface  surface = EGL_NO_SURFACE;
    std::string node, mode;
};

// An ES 3.2 context on the EGL device whose DRM render node is `wanted` (default: the
// first render node by name), surfaceless when the display offers it, else a pbuffer.
SEgl openContext(const std::string& wanted) {
    if (!hasExtension(eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS), "EGL_EXT_platform_device"))
        die(2, "EGL client extension EGL_EXT_platform_device missing");
    const auto queryDevices = eglProc<PFNEGLQUERYDEVICESEXTPROC>("eglQueryDevicesEXT");
    const auto queryString  = eglProc<PFNEGLQUERYDEVICESTRINGEXTPROC>("eglQueryDeviceStringEXT");
    const auto getDisplay   = eglProc<PFNEGLGETPLATFORMDISPLAYEXTPROC>("eglGetPlatformDisplayEXT");

    EGLint count = 0;
    if (!queryDevices(0, nullptr, &count) || count <= 0)
        die(2, "eglQueryDevicesEXT found no device");
    std::vector<EGLDeviceEXT> devices(static_cast<size_t>(count));
    queryDevices(count, devices.data(), &count);

    EGLDeviceEXT chosen = nullptr;
    std::string  chosenNode, seen;
    for (EGLint i = 0; i < count; i++) {
        if (!hasExtension(queryString(devices[i], EGL_EXTENSIONS), "EGL_EXT_device_drm_render_node"))
            continue;
        const char* node = queryString(devices[i], EGL_DRM_RENDER_NODE_FILE_EXT);
        if (!node)
            continue;
        seen += std::format(" {}", node);
        const bool take = wanted.empty() ? (!chosen || std::string_view(node) < chosenNode) : (!chosen && wanted == node);
        if (take) {
            chosen     = devices[i];
            chosenNode = node;
        }
    }
    if (!chosen)
        die(2, std::format("no EGL device with render node {} (render nodes seen:{})", wanted.empty() ? "(any)" : wanted, seen.empty() ? " none" : seen));

    SEgl egl;
    egl.node    = chosenNode;
    egl.display = getDisplay(EGL_PLATFORM_DEVICE_EXT, chosen, nullptr);
    EGLint major = 0, minor = 0;
    if (egl.display == EGL_NO_DISPLAY || !eglInitialize(egl.display, &major, &minor))
        die(2, std::format("{}: eglInitialize failed (0x{:x})", chosenNode, eglGetError()));
    if (!eglBindAPI(EGL_OPENGL_ES_API))
        die(2, std::format("{}: eglBindAPI(EGL_OPENGL_ES_API) failed (0x{:x})", chosenNode, eglGetError()));

    const char* exts        = eglQueryString(egl.display, EGL_EXTENSIONS);
    const bool  surfaceless = hasExtension(exts, "EGL_KHR_surfaceless_context");
    EGLConfig   config      = EGL_NO_CONFIG_KHR;
    if (!surfaceless || !hasExtension(exts, "EGL_KHR_no_config_context")) {
        const EGLint attribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
        EGLint       n         = 0;
        if (!eglChooseConfig(egl.display, attribs, &config, 1, &n) || n < 1)
            die(2, std::format("{}: no ES3 pbuffer config (0x{:x})", chosenNode, eglGetError()));
    }
    const EGLint ctxAttribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
    egl.context               = eglCreateContext(egl.display, config, EGL_NO_CONTEXT, ctxAttribs);
    if (egl.context == EGL_NO_CONTEXT)
        die(2, std::format("{}: eglCreateContext for OpenGL ES 3.2 failed (0x{:x})", chosenNode, eglGetError()));
    if (surfaceless) {
        egl.mode = "surfaceless";
    } else {
        const EGLint pb[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        egl.surface       = eglCreatePbufferSurface(egl.display, config, pb);
        if (egl.surface == EGL_NO_SURFACE)
            die(2, std::format("{}: no surfaceless context and eglCreatePbufferSurface failed (0x{:x})", chosenNode, eglGetError()));
        egl.mode = "pbuffer";
    }
    if (!eglMakeCurrent(egl.display, egl.surface, egl.surface, egl.context))
        die(2, std::format("{}: eglMakeCurrent failed (0x{:x})", chosenNode, eglGetError()));

    const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    if (!version || !std::string_view(version).starts_with("OpenGL ES 3.2"))
        die(2, std::format("{}: context is \"{}\", not OpenGL ES 3.2", chosenNode, version ? version : "(none)"));
    std::fprintf(stderr, "hyprns-bench: %s, EGL %d.%d %s, %s, %s\n", chosenNode.c_str(), major, minor, egl.mode.c_str(), reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                 version);
    return egl;
}

void closeContext(SEgl& egl) {
    eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (egl.surface != EGL_NO_SURFACE)
        eglDestroySurface(egl.display, egl.surface);
    eglDestroyContext(egl.display, egl.context);
    eglTerminate(egl.display);
}

bool writePng(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    png_structp png  = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop   info = png ? png_create_info_struct(png) : nullptr;
    if (!png || !info || setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        std::fclose(f);
        return false;
    }
    png_init_io(png, f);
    png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (int y = 0; y < h; y++)
        png_write_row(png, rgba.data() + static_cast<size_t>(y) * w * 4);
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    return std::fclose(f) == 0;
}

// ── fluid-test: CFluid + CField as jupiter will use them (bench only) ───────────────

// The jets and the beta term of sketchJove's force (reference/b7-sketch.html), without
// the storms and the eye; `size` is the lattice.
constexpr const char* JOVE_FORCE = R"GLSL(
uniform float u0;
uniform float relax;
uniform float beta;
vec2 force(ivec2 c, vec2 u, float rho) {
    vec2  p   = vec2(c) + 0.5;
    float y   = p.y / float(size.y);
    float jet = u0 * sin(3.14159265 * 7.0 * y);
    vec2  F   = relax * vec2(jet - u.x, -0.3 * u.y);
    float f   = beta * (p.y - 0.5 * float(size.y));
    vec2  dv  = u - vec2(jet, 0.0);
    F += f * vec2(dv.y, -dv.x) - 0.5 * f * f * dv;
    return F;
}
)GLSL";

// The cloud's pull back to its bands (sketchJove's pMac, without storms and eye); `size`
// is the field, so y is the same fraction of the height as in the lattice.
constexpr const char* BAND_POST = R"GLSL(
uniform float back;
float band(float y) { return 0.5 + 0.36 * sin(3.14159265 * 7.0 * y + 1.5708) + 0.14 * sin(3.14159265 * 17.0 * y + 2.0); }
vec4 post(ivec2 c, vec4 v) {
    float y = (float(c.y) + 0.5) / float(size.y);
    return vec4(clamp(mix(v.r, band(y), back), 0.0, 1.0), 0.0, 0.0, 1.0);
}
)GLSL";

// The field in grey, filtered by hand (R32F is not filterable).
constexpr const char* GREY_DISPLAY = R"GLSL(
uniform sampler2D fieldTex;
uniform float fade;
in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;
)GLSL";
constexpr const char* GREY_DISPLAY_MAIN = R"GLSL(
void main() {
    float v   = bilinear(fieldTex, v_texcoord * vec2(textureSize(fieldTex, 0)), false).r;
    fragColor = vec4(vec3(clamp(v, 0.0, 1.0)) * fade, 1.0);
}
)GLSL";

class CFluidTestLook final : public ILook {
  public:
    static constexpr int LW = 400, LH = 225, K = 4;

    ~CFluidTestLook() override {
        Gl::destroy(m_display);
    }

    // The lattice is the sketch's, whatever the monitor: built once.
    bool build(const SGeometry&, float, std::string* error) override {
        if (m_display.id)
            return true;
        if (!m_fluid.build({LW, LH, true, 1.88f, JOVE_FORCE}, error))
            return false;
        const auto& fp = m_fluid.program();
        Gl::use(fp);
        glUniform1f(fp.uniform("u0"), 0.04f);
        glUniform1f(fp.uniform("relax"), 1.5e-3f);
        glUniform1f(fp.uniform("beta"), 4.5e-4f);
        if (!m_cloud.build({LW * K, LH * K, GL_R32F, false, BAND_POST}, error))
            return false;
        Gl::use(m_cloud.program());
        glUniform1f(m_cloud.program().uniform("back"), 2.5e-4f);
        std::string log;
        auto        display = Gl::compile(std::string(GREY_DISPLAY) + GLSL_BILINEAR + GREY_DISPLAY_MAIN, &log);
        if (!display) {
            if (error)
                *error = std::format("the fluid-test display shader failed to compile: {}", log);
            return false;
        }
        m_display = *display;
        seed();
        return true;
    }
    float stepsPerSecond() const override {
        return 360.0f; // the sketch: 6 steps per frame at 60 frames per second
    }
    void advance(int steps) override {
        for (int i = 0; i < steps; i++) {
            m_fluid.step();
            m_cloud.step(m_fluid.velocity(), LW, LH, 1.0f);
            m_steps++;
        }
    }
    void draw(GLuint fbo, int x, int y, int w, int h, const std::array<GLfloat, 9>& proj, float fade) override {
        Gl::use(m_display);
        Gl::bind(0, m_cloud.texture());
        glUniform1i(m_display.uniform("fieldTex"), 0);
        glUniform1f(m_display.uniform("fade"), std::clamp(fade, 0.0f, 1.0f));
        Gl::pass(m_display, fbo, x, y, w, h, proj);
    }
    // the bench's own look reads synchronously (CFluid::stats()), at the take
    void checkHealth() override { m_checkDue = true; }
    EHealth takeHealth(bool wait) override {
        if (!std::exchange(m_checkDue, false))
            return wait ? (m_last.bad ? EHealth::BLOWN : EHealth::HEALTHY) : EHealth::NONE;
        m_last = m_fluid.stats();
        return m_last.bad ? EHealth::BLOWN : EHealth::HEALTHY;
    }
    void reset() override {
        m_fluid.init();
        seed();
        m_steps = 0;
    }
    std::string status() const override {
        return std::format("fluid-test: lattice {}x{}, cloud {}x{}, {} steps, |u| max {:.4f} mean {:.5f}, rho {:.4f}..{:.4f}", LW, LH, LW * K, LH * K, m_steps, m_last.umax,
                           m_last.umean, m_last.rhoMin, m_last.rhoMax);
    }
    SLookInfo info() const override {
        return {LW, LH};
    }
    const CFluid::SStats& lastStats() const {
        return m_last;
    }

  private:
    // sketchJove's seed(): band((y + 0.5) / DH) on every row
    void seed() {
        const int          w = LW * K, h = LH * K;
        std::vector<float> d(static_cast<size_t>(w) * h * 4, 0.0f);
        for (int y = 0; y < h; y++) {
            const double yy = (y + 0.5) / h;
            const float  v  = static_cast<float>(0.5 + 0.36 * std::sin(3.14159265 * 7 * yy + 1.5708) + 0.14 * std::sin(3.14159265 * 17 * yy + 2.0));
            for (int x = 0; x < w; x++)
                d[(static_cast<size_t>(y) * w + x) * 4] = v;
        }
        m_cloud.load(d);
    }

    CFluid         m_fluid;
    CField         m_cloud;
    Gl::SProgram   m_display;
    CFluid::SStats m_last{0, 0, 0, 0, false};
    bool           m_checkDue = false;
    long           m_steps = 0;
};

// ── bake-test: CBake alone (bench only) ─────────────────────────────────────────────

constexpr const char* BAKE_TEST_STRIP = R"GLSL(
uniform vec2 size;
uniform float keyTime;
layout(location = 0) out vec4 o;
void main() {
    vec2 uv = gl_FragCoord.xy / size;
    o = vec4(fract(uv * 8.0), keyTime, 1.0);
}
)GLSL";

constexpr const char* BAKE_TEST_SHOW = R"GLSL(
uniform sampler2D keyA;
uniform sampler2D keyB;
uniform float blend;
uniform float fade;
in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;
void main() {
    vec4 c = mix(texture(keyA, v_texcoord), texture(keyB, v_texcoord), blend);
    fragColor = vec4(c.rgb * fade, c.a);
}
)GLSL";

class CBakeTestLook final : public ILook {
  public:
    static constexpr int FRAMES = 30;

    ~CBakeTestLook() override {
        m_bake.release();
        Gl::destroy(m_strip);
        Gl::destroy(m_show);
    }
    bool build(const SGeometry& g, float quality, std::string* error) override {
        const int w = Gl::scaledSide(g.monW, quality), h = Gl::scaledSide(g.monH, quality);
        if (m_strip.id && w == m_bake.width() && h == m_bake.height())
            return true;
        std::string log;
        if (!m_strip.id) {
            auto strip = Gl::compile(BAKE_TEST_STRIP, &log);
            auto show  = Gl::compile(BAKE_TEST_SHOW, &log);
            if (!strip || !show) {
                if (error)
                    *error = std::format("the bake-test shaders failed to compile: {}", log);
                return false;
            }
            m_strip = *strip;
            m_show  = *show;
        }
        m_keys = 0;
        return m_bake.build(w, h, GL_RGBA8, FRAMES, error);
    }
    float stepsPerSecond() const override {
        return 30.0f; // one step per frame at the bench's 30 fps
    }
    void advance(int steps) override {
        if (steps <= 0 && m_bake.ready())
            return;
        const bool wasReady = m_bake.ready();
        if (m_bake.starting())
            m_backKey = m_keys + 2;
        Gl::use(m_strip);
        glUniform2f(m_strip.uniform("size"), static_cast<float>(m_bake.width()), static_cast<float>(m_bake.height()));
        m_bake.advance([&](GLuint fbo, int y0, int y1) {
            const int key = m_bake.bakingKey();
            glUniform1f(m_strip.uniform("keyTime"), static_cast<float>(key == 2 ? m_backKey : key) / 255.0f);
            Gl::pass(m_strip, fbo, 0, y0, m_bake.width(), y1 - y0);
        });
        if (wasReady && m_bake.starting())
            m_keys++;
    }
    void draw(GLuint fbo, int x, int y, int w, int h, const std::array<GLfloat, 9>& proj, float fade) override {
        Gl::use(m_show);
        Gl::bind(0, m_bake.previous());
        Gl::bind(1, m_bake.next());
        glUniform1i(m_show.uniform("keyA"), 0);
        glUniform1i(m_show.uniform("keyB"), 1);
        glUniform1f(m_show.uniform("blend"), m_bake.blend());
        glUniform1f(m_show.uniform("fade"), std::clamp(fade, 0.0f, 1.0f));
        Gl::pass(m_show, fbo, x, y, w, h, proj);
    }
    void checkHealth() override {}
    EHealth takeHealth(bool wait) override {
        return wait ? EHealth::HEALTHY : EHealth::NONE;
    }
    void reset() override {
        m_bake.restart();
        m_keys = 0;
    }
    std::string status() const override {
        return std::format("bake-test: {}x{}, {} frames per key, {} rotations, blend {:.4f}", m_bake.width(), m_bake.height(), m_bake.framesPerKey(), m_keys, m_bake.blend());
    }
    SLookInfo info() const override {
        return {1, 1};
    }

    // Rows of a key whose every pixel has alpha 255, and the key index of its first pixel.
    struct SKeyCheck {
        int wholeRows, key;
    };
    SKeyCheck checkKey(bool previous) const {
        GLuint             tex = previous ? m_bake.previous() : m_bake.next();
        GLuint             fb  = Gl::framebuffer({tex});
        const int          w = m_bake.width(), h = m_bake.height();
        std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, fb);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        Gl::destroyFramebuffer(fb);
        return {wholeRows(px, w, h), px.empty() ? -1 : px[2]};
    }
    static int wholeRows(const std::vector<uint8_t>& px, int w, int h) {
        int rows = 0;
        for (int y = 0; y < h; y++) {
            bool whole = true;
            for (int x = 0; x < w && whole; x++)
                whole = px[(static_cast<size_t>(y) * w + x) * 4 + 3] == 255;
            rows += whole;
        }
        return rows;
    }
    int height() const {
        return m_bake.height();
    }

  private:
    CBake        m_bake;
    Gl::SProgram m_strip, m_show;
    int          m_keys = 0, m_backKey = 0;
};

// ── --field-check: CField on its own ─────────────────────────────────────────────────

std::vector<float> readRgba(GLuint tex, int w, int h) {
    std::vector<float> px(static_cast<size_t>(w) * h * 4);
    GLuint             fb = Gl::framebuffer({tex});
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, px.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    Gl::destroyFramebuffer(fb);
    return px;
}

GLuint velocityTexture(int w, int h, float ux, float uy) {
    std::vector<float> v(static_cast<size_t>(w) * h * 2);
    for (size_t i = 0; i < v.size(); i += 2) {
        v[i]     = ux;
        v[i + 1] = uy;
    }
    const GLuint tex = Gl::texture(GL_RG32F, w, h, GL_NEAREST);
    Gl::upload(tex, w, h, GL_RG, GL_FLOAT, v.data());
    return tex;
}

// --r16f-probe: 64 values 1 + (i / 64) ulp (i = 0..63) stored into R16F and read back
int r16fProbe() {
    std::string log;
    auto        prog = Gl::compile(R"GLSL(
layout(location = 0) out vec4 o;
void main() { o = vec4(1.0 + floor(gl_FragCoord.x) / 64.0 * exp2(-10.0), 0.0, 0.0, 1.0); }
)GLSL",
                                   &log);
    if (!prog) {
        std::fprintf(stderr, "hyprns-bench: the probe shader failed: %s\n", log.c_str());
        return 2;
    }
    GLuint tex = Gl::texture(GL_R16F, 64, 1, GL_NEAREST), fb = Gl::framebuffer({tex});
    Gl::use(*prog);
    Gl::pass(*prog, fb, 0, 0, 64, 1);
    const auto px = readRgba(tex, 64, 1);
    int        upBelowHalf = 0, upAboveHalf = 0, upAtHalf = 0;
    for (int i = 0; i < 64; i++) {
        const bool up = px[static_cast<size_t>(i) * 4] > 1.0f;
        if (i < 32)
            upBelowHalf += up;
        else if (i == 32)
            upAtHalf += up;
        else
            upAboveHalf += up;
    }
    const char* mode = upBelowHalf == 0 && upAboveHalf == 0 && upAtHalf == 0 ? "truncate"
        : upBelowHalf == 0 && upAboveHalf == 31                         ? "round to nearest"
                                                                        : "other";
    std::printf("{\"r16f_probe\": {\"renderer\": \"%s\", \"rounded_up_below_half\": %d, \"rounded_up_at_half\": %d, \"rounded_up_above_half\": %d, \"mode\": \"%s\"}}\n",
                reinterpret_cast<const char*>(glGetString(GL_RENDERER)), upBelowHalf, upAtHalf, upAboveHalf, mode);
    Gl::destroyFramebuffer(fb);
    Gl::destroyTexture(tex);
    Gl::destroy(*prog);
    return 0;
}

// The health reduction against the CPU: a 517x203 RGBA32F texture (not a multiple of the
// tile) of values in [-0.5, 1.5) with three NaN and two Inf cells; the per-cell snippet
// takes max over the four channels, sums three of them and checks all four. The maxima
// must match exactly, the sums to 1e-5 relative, the left-out count must be 5; the
// non-waiting take must give nothing at once (or the answer) and the answer later.
int reduceCheck() {
    const int   liveBefore = Gl::liveObjects();
    constexpr int W = 517, H = 203;
    std::vector<float> v(static_cast<size_t>(W) * H * 4);
    uint32_t           state = 777;
    for (auto& x : v) {
        state = state * 1664525u + 1013904223u;
        x     = static_cast<float>(state >> 8) / 16777216.0f * 2.0f - 0.5f;
    }
    const size_t bad[5] = {7, 40000, 104950, 333, 77777};
    v[bad[0] * 4 + 1]   = NAN;
    v[bad[1] * 4 + 3]   = NAN;
    v[bad[2] * 4 + 0]   = -NAN;
    v[bad[3] * 4 + 2]   = INFINITY;
    v[bad[4] * 4 + 1]   = -INFINITY;
    GLuint tex = Gl::texture(GL_RGBA32F, W, H, GL_NEAREST);
    Gl::upload(tex, W, H, GL_RGBA, GL_FLOAT, v.data());

    float  mx[4] = {-INFINITY, -INFINITY, -INFINITY, -INFINITY};
    double sm[3] = {0, 0, 0};
    int    nonFinite = 0;
    for (size_t i = 0; i < static_cast<size_t>(W) * H; i++) {
        const float* p = &v[i * 4];
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]) || !std::isfinite(p[3])) {
            nonFinite++;
            continue;
        }
        for (int k = 0; k < 4; k++)
            mx[k] = std::max(mx[k], p[k]);
        for (int k = 0; k < 3; k++)
            sm[k] += p[k];
    }

    CReduction  r;
    std::string error;
    if (!r.build(R"GLSL(
uniform sampler2D A;
void cell(ivec2 c, out vec4 mx, out vec3 sm, out vec4 chk) {
    vec4 a = texelFetch(A, c, 0);
    mx  = a;
    sm  = a.xyz;
    chk = a;
}
)GLSL",
                 {"A"}, &error))
        die(2, error);
    bool     ok = true;
    SReduced got{};
    // the non-waiting path: nothing, or the answer, at once; the answer within 2 s
    ok           = ok && r.start({tex}, W, H) && r.pending();
    const auto t0 = std::chrono::steady_clock::now();
    int        polls = 0;
    bool       have  = false;
    while (!have && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2)) {
        have = r.take(got, false);
        polls++;
    }
    ok = ok && have && !r.pending();
    for (int k = 0; k < 4; k++)
        ok = ok && got.max[k] == mx[k];
    for (int k = 0; k < 3; k++)
        ok = ok && std::fabs(got.sum[k] - sm[k]) <= 1e-5 * std::max(1.0, std::fabs(sm[k]));
    ok = ok && got.nonFinite == static_cast<float>(nonFinite) && nonFinite == 5 && !got.finite();
    // the waiting path, and a texture with nothing bad in it
    for (const size_t i : bad)
        for (int k = 0; k < 4; k++)
            v[i * 4 + k] = 0.25f;
    Gl::upload(tex, W, H, GL_RGBA, GL_FLOAT, v.data());
    SReduced clean{};
    ok = ok && r.start({tex}, W, H) && r.take(clean, true) && clean.finite() && clean.nonFinite == 0.0f;
    r.release();
    Gl::destroyTexture(tex);
    const int liveAfter = Gl::liveObjects();
    ok                  = ok && liveAfter == liveBefore;
    std::printf("{\"reduce_check\": {\"size\": [%d, %d], \"max\": [%g, %g, %g, %g], \"max_cpu\": [%g, %g, %g, %g], \"sum\": [%.6f, %.6f, %.6f], "
                "\"sum_cpu\": [%.6f, %.6f, %.6f], \"non_finite\": %g, \"non_finite_cpu\": %d, \"polls\": %d, \"live_objects\": [%d, %d], \"ok\": %s}}\n",
                W, H, got.max[0], got.max[1], got.max[2], got.max[3], mx[0], mx[1], mx[2], mx[3], got.sum[0], got.sum[1], got.sum[2], sm[0], sm[1], sm[2],
                got.nonFinite, nonFinite, polls, liveBefore, liveAfter, ok ? "true" : "false");
    return ok ? 0 : 1;
}

int fieldCheck() {
    const int   liveBefore = Gl::liveObjects();
    std::string error;
    constexpr int W = 400, H = 225;
    bool          ok = true;

    // (a) zero velocity, 1000 steps: bit-identical; the velocity at the field's size and
    // at another size (the resampled path)
    std::vector<float> noise(static_cast<size_t>(W) * H * 4);
    uint32_t           state = 12345;
    for (auto& v : noise) {
        state = state * 1664525u + 1013904223u;
        v     = static_cast<float>(state >> 8) / 16777216.0f * 2.0f - 0.5f; // [-0.5, 1.5)
    }
    bool identical[2] = {false, false};
    for (int pass = 0; pass < 2; pass++) {
        CField     field;
        const bool wrapY = pass == 1;
        if (!field.build({W, H, GL_RGBA32F, wrapY, ""}, &error))
            die(2, error);
        field.load(noise);
        const int    vw = pass == 0 ? W : 100, vh = pass == 0 ? H : 57;
        GLuint       vel  = velocityTexture(vw, vh, 0.0f, 0.0f);
        const auto   before = readRgba(field.texture(), W, H);
        for (int i = 0; i < 1000; i++)
            field.step(vel, vw, vh, 1.0f);
        const auto after = readRgba(field.texture(), W, H);
        identical[pass]  = before == noise && std::memcmp(before.data(), after.data(), before.size() * sizeof(float)) == 0;
        ok               = ok && identical[pass];
        Gl::destroyTexture(vel);
    }

    // (b) (0.25, 0) cells per step for 4 W steps on a periodic field: once round, back to
    // the start. Channels: 0 smooth (sin x cos), 1 a gaussian blob (sigma 18 cells),
    // 2 a 40x45-cell checkerboard (sharp edges), 3 stripes 40 cells wide in x (0.25 / 0.75)
    std::vector<float> start(static_cast<size_t>(W) * H * 4);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float*       p  = &start[(static_cast<size_t>(y) * W + x) * 4];
            const double fx = (x + 0.5) / W, fy = (y + 0.5) / H;
            const double dx = x + 0.5 - 0.5 * W, dy = y + 0.5 - 0.5 * H;
            p[0] = static_cast<float>(0.5 + 0.4 * std::sin(2 * M_PI * 3 * fx) * std::cos(2 * M_PI * 2 * fy));
            p[1] = static_cast<float>(std::exp(-(dx * dx + dy * dy) / (2 * 18.0 * 18.0)));
            p[2] = (x / 40 + y / 45) % 2 ? 1.0f : 0.0f;
            p[3] = (x / 40) % 2 ? 0.75f : 0.25f;
        }
    float lo[4] = {INFINITY, INFINITY, INFINITY, INFINITY}, hi[4] = {-INFINITY, -INFINITY, -INFINITY, -INFINITY};
    for (size_t i = 0; i < start.size(); i++) {
        lo[i % 4] = std::min(lo[i % 4], start[i]);
        hi[i % 4] = std::max(hi[i % 4], start[i]);
    }
    float  seenLo[4], seenHi[4];
    double maxErr[4] = {0, 0, 0, 0}, meanErr[4] = {0, 0, 0, 0};
    std::copy(lo, lo + 4, seenLo);
    std::copy(hi, hi + 4, seenHi);
    bool inRange = true;
    {
        CField field;
        if (!field.build({W, H, GL_RGBA32F, true, ""}, &error))
            die(2, error);
        field.load(start);
        GLuint vel = velocityTexture(W, H, 0.25f, 0.0f);
        // every step read back: the range must hold all the way, not only at the end
        for (int i = 0; i < 4 * W; i++) {
            field.step(vel, W, H, 1.0f);
            const auto now = readRgba(field.texture(), W, H);
            for (size_t j = 0; j < now.size(); j++) {
                const int k = static_cast<int>(j % 4);
                seenLo[k]   = std::min(seenLo[k], now[j]);
                seenHi[k]   = std::max(seenHi[k], now[j]);
                if (!(now[j] >= lo[k] && now[j] <= hi[k]))
                    inRange = false;
            }
        }
        const auto end = readRgba(field.texture(), W, H);
        for (size_t j = 0; j < end.size(); j++) {
            const double e = std::fabs(static_cast<double>(end[j]) - start[j]);
            maxErr[j % 4]  = std::max(maxErr[j % 4], e);
            meanErr[j % 4] += e / (static_cast<double>(W) * H);
        }
        Gl::destroyTexture(vel);
    }
    ok = ok && inRange;

    // (c) the resampled path moving: a 100x57 velocity of (0.25, 0) scales by 400/100 to
    // exactly one field cell per step, so W steps bring the field back exactly
    double resampledErr = 0;
    {
        CField field;
        if (!field.build({W, H, GL_RGBA32F, true, ""}, &error))
            die(2, error);
        field.load(start);
        GLuint vel = velocityTexture(100, 57, 0.25f, 0.0f);
        for (int i = 0; i < W; i++)
            field.step(vel, 100, 57, 1.0f);
        const auto end = readRgba(field.texture(), W, H);
        for (size_t j = 0; j < end.size(); j++)
            resampledErr = std::max(resampledErr, std::fabs(static_cast<double>(end[j]) - start[j]));
        Gl::destroyTexture(vel);
    }
    ok = ok && resampledErr <= 1e-6;

    const int liveAfter = Gl::liveObjects();
    if (liveAfter != liveBefore) {
        std::fprintf(stderr, "hyprns-bench: %d GL objects leaked by the field check\n", liveAfter - liveBefore);
        ok = false;
    }
    const auto list = [](const auto& a, int prec) {
        std::string s = "[";
        for (int k = 0; k < 4; k++)
            s += std::format("{}{:.{}g}", k ? ", " : "", static_cast<double>(a[k]), prec);
        return s + "]";
    };
    std::printf("{\"field_check\": {\"zero_velocity\": {\"steps\": 1000, \"identical_same_size\": %s, \"identical_resampled\": %s}, "
                "\"uniform_shift\": {\"velocity\": [0.25, 0], \"steps\": %d, \"max_abs_err\": %s, \"mean_abs_err\": %s, \"initial_min\": %s, \"initial_max\": %s, "
                "\"seen_min\": %s, \"seen_max\": %s, \"in_range\": %s}, \"resampled_shift\": {\"velocity_size\": [100, 57], \"steps\": %d, \"max_abs_err\": %.3g}, "
                "\"leaked\": %d}}\n",
                identical[0] ? "true" : "false", identical[1] ? "true" : "false", 4 * W, list(maxErr, 4).c_str(), list(meanErr, 4).c_str(), list(lo, 6).c_str(),
                list(hi, 6).c_str(), list(seenLo, 9).c_str(), list(seenHi, 9).c_str(), inRange ? "true" : "false", W, resampledErr, liveAfter - liveBefore);
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    const SArgs args = parseArgs(argc, argv);

    SEgl egl = openContext(args.device);
    if (args.fieldCheck || args.r16fProbe || args.reduceCheck) {
        const int code = args.fieldCheck ? fieldCheck() : args.r16fProbe ? r16fProbe() : reduceCheck();
        closeContext(egl);
        return code;
    }

    // The "monitor": RGBA8, rotated for transform 1. Framebuffer row 0 is the top of
    // what is scanned out, so FULL maps the picture's top row there.
    const bool                   rotated = args.transform == 1;
    int                          monW = args.w, monH = args.h; // screen orientation; --resize changes it
    int                          fbW = 0, fbH = 0;
    GLuint                       outTex = 0, outFb = 0;
    // transform 1: screen (u, v) -> framebuffer column (1 - v) * fbW, row u * fbH (column-major)
    const std::array<GLfloat, 9> ROTATE_90 = {0, 2, 0, -2, 0, 0, 1, -1, 1};
    const auto&                  proj      = rotated ? ROTATE_90 : Gl::FULL;
    const auto                   makeOutput = [&] {
        Gl::destroyFramebuffer(outFb);
        Gl::destroyTexture(outTex);
        fbW    = rotated ? monH : monW;
        fbH    = rotated ? monW : monH;
        outTex = Gl::texture(GL_RGBA8, fbW, fbH, GL_NEAREST);
        outFb  = Gl::framebuffer({outTex});
        if (!outFb)
            die(2, "the RGBA8 output framebuffer is incomplete");
    };
    makeOutput();

    const int liveBefore = Gl::liveObjects();

    const bool timers = (args.time || args.budget) && Gl::timerQueries();
    if ((args.time || args.budget) && !timers)
        std::fprintf(stderr, "hyprns-bench: GL_EXT_disjoint_timer_query missing, no timing%s\n", args.budget ? " (the budget stays at the quality setting)" : "");

    const float          timeScale = args.settings.timeScale;
    const uint64_t       seed = args.seedName == "0" ? uint64_t{0} : seedFor(args.seedName);
    bool                 bad = false, stalled = false;
    int                  latticeW = 0, latticeH = 0;
    uint64_t             stepsRun = 0, resets = 0;
    std::vector<float>   gpuSamples; // every measured frame, ms
    std::vector<long>    sampleFrame; // its frame (with --change-at; -1 otherwise)
    struct SChangeDone {
        long        frame;
        std::string key;
        float       value;
        long        window;         // 2 s of frames at the frame rate from then on
        int         liveAfter = -1; // live GL objects 2 s of frames later
    };
    std::vector<SChangeDone> changeAt;
    SBudgetState         levels{args.quality, 1.0f, false, false};
    std::vector<uint8_t> pixels;
    std::string          shown = args.look; // the look at the end
    std::optional<CFluid::SStats> fluid;    // fluid-test only
    double                        healthMsMax    = 0.0; // the longest one frame's health calls held the CPU
    long                          healthVerdicts = 0;
    std::deque<GLsync>            inFlightFences;
    std::string                   bakeJson; // bake-test only
    bool                          bakeBad = false;
    {
        std::string error;
        float       quality = args.quality; // what the look is built with
        const auto  install = [&](const std::string& name, const SGeometry& g) {
            shown     = name;
            std::unique_ptr<ILook> look;
            if (name == FLUID_TEST)
                look = std::make_unique<CFluidTestLook>();
            else if (name == BAKE_TEST)
                look = std::make_unique<CBakeTestLook>();
            else if (name == "marble" && args.forceSketch)
                look = std::make_unique<CMarbleLook>(seed, true);
            else if (name == "embers" && args.forceSketch)
                look = std::make_unique<CEmbersLook>(seed, true);
            else if (name == "jupiter" && args.forceSketch)
                look = std::make_unique<CJupiterLook>(seed, true);
            else
                look = makeLook(name, seed);
            look->setFrameRate(args.fps);
            if (!look->build(g, quality, &error))
                die(2, error);
            return look;
        };
        SGeometry geometry = geometryFor(monW, monH);
        auto      look     = install(args.look, geometry);

        // the plugin's measurement and budget; a large ring: nothing paces the bench's
        // frames, so the CPU runs ahead of the GPU
        Gl::CGpuTimer      timer(64);
        CGpuMean           mean;
        CBudget            budget(args.budget ? args.settings.budgetMs : 0.0f, args.quality);
        Gl::CPictureTarget picture;
        std::vector<float> polled;
        const auto         restartMean = [&] {
            mean.restart();
            timer.discard();
        };

        // the fade of a switch, as in the plugin: out over FADE_SECONDS, replace, back in
        enum class EFade { NONE, OUT, IN } phase = EFade::NONE;
        float        fade    = 1.0f;
        const double frameDt = fadeDt(0.0, 1.0 / args.fps, args.fps); // the plugin's dt for one frame
        bool        switched = false, resized = false;

        const long healthEvery = std::max(1L, std::lround(2.0 * args.fps)); // frames: every 2 s
        const long idleLimit   = std::max(1L, std::lround(10.0 * args.fps)); // frames without a step
        double     debt = 0.0, clock = 0.0;
        uint64_t   wallNs = 0;
        long       left = args.steps, frame = 0, idle = 0;
        float      fps  = args.fps; // --change-at fps=N changes it
        size_t     nextChange = 0, nextGrab = 0;
        long       nextStatus = args.statusEvery;
        const bool syncFrames = !args.changes.empty() && timers;
        for (bool first = true; first || left > 0; first = false) {
            frame++;
            const long done = args.steps - left;
            while (nextChange < args.changes.size() && done >= args.changes[nextChange].at) {
                const auto& c       = args.changes[nextChange++];
                const int   liveOld = Gl::liveObjects();
                if (c.key == "fps") {
                    fps = c.value;
                    look->setFrameRate(fps); // the look remakes what depends on it on its next advance
                } else {
                    budget = CBudget(args.budget ? args.settings.budgetMs : 0.0f, c.value);
                    levels.quality = quality = c.value;
                    if (!look->build(geometry, quality, &error))
                        die(2, error);
                    restartMean();
                }
                changeAt.push_back({frame, c.key, c.value, std::lround(2.0 * fps)});
                std::fprintf(stderr, "hyprns-bench: %s=%g at step %ld: live GL objects before %d, after the change %d\n", c.key.c_str(), static_cast<double>(c.value), done,
                             liveOld, Gl::liveObjects());
            }
            if (!resized && args.resizeAt >= 0 && done >= args.resizeAt) {
                resized = true;
                monW    = args.resizeW;
                monH    = args.resizeH;
                makeOutput();
                geometry          = geometryFor(monW, monH);
                const int liveOld = Gl::liveObjects();
                if (!look->build(geometry, quality, &error))
                    die(2, error);
                std::fprintf(stderr, "hyprns-bench: resize at step %ld: live GL objects before the rebuild %d, after %d\n", done, liveOld, Gl::liveObjects());
                restartMean();
            }
            if (!switched && args.switchAt >= 0 && done >= args.switchAt) {
                switched = true;
                phase    = EFade::OUT;
            }
            if (phase == EFade::OUT) {
                fade = fadeAfter(fade, frameDt, true);
                if (fade <= 0.0f) {
                    const int liveOld = Gl::liveObjects();
                    look.reset(); // the old look's GL objects go before the new one's are made
                    look  = install(args.switchTo, geometry);
                    std::fprintf(stderr, "hyprns-bench: switch at step %ld: live GL objects with the old look %d, with the new %d\n", args.steps - left, liveOld,
                                 Gl::liveObjects());
                    phase = EFade::IN;
                    restartMean();
                }
            } else if (phase == EFade::IN) {
                fade = fadeAfter(fade, frameDt, false);
                if (fade >= 1.0f)
                    phase = EFade::NONE;
            }

            // the budget, on the frames measured so far; a level change rebuilds the look
            // and starts the mean again, as in the plugin
            if (timers) {
                polled.clear();
                timer.poll(polled);
                for (const float ms : polled) {
                    mean.add(ms);
                    gpuSamples.push_back(ms);
                    sampleFrame.push_back(syncFrames ? frame - 1 : -1); // synced: last frame's
                }
            }
            const SBudgetState next = budget.update(mean.mean(), clock);
            clock += 1.0 / fps;
            if (next.quality != levels.quality || next.timeScale != levels.timeScale)
                restartMean();
            levels = next;
            if (levels.quality != quality) {
                quality = levels.quality;
                if (!look->build(geometry, quality, &error))
                    die(2, error);
            }

            const double perFrame = clockRate(*look, timeScale) * levels.timeScale / fps;
            debt += perFrame;
            const int steps = static_cast<int>(std::min<double>(std::floor(debt), static_cast<double>(left)));
            debt -= steps;
            left -= steps;
            // A look that runs no steps (time_scale 0) never reaches
            // --steps: stop now, or after 10 s of frames without a step.
            idle = steps == 0 ? idle + 1 : 0;
            if (left > 0 && (!(perFrame > 0) || idle > idleLimit)) {
                std::fprintf(stderr, "hyprns-bench: %.4f steps per frame, %ld frames without a step: stopping with %ld steps left\n", perFrame, idle, left);
                stalled = true;
                break;
            }

            // below quality 1: into the smaller target, stretched over the "monitor"
            const bool scaled = quality < 1.0f;
            if (scaled) {
                if (!picture.ensure(Gl::scaledSide(monW, quality), Gl::scaledSide(monH, quality), &error))
                    die(2, error);
            } else
                picture.release();
            // the wall clock, deterministic: 1/fps s per frame, in whole nanoseconds
            wallNs += static_cast<uint64_t>(std::llround(1e9 / fps));
            look->setWallClock(wallNs);
            const bool timed = timers && timer.begin();
            look->advance(steps);
            stepsRun += static_cast<uint64_t>(steps);
            if (scaled) {
                look->draw(picture.fbo(), 0, 0, picture.width(), picture.height(), Gl::FULL, fade);
                picture.stretch(outFb, 0, 0, fbW, fbH, proj);
            } else
                look->draw(outFb, 0, 0, fbW, fbH, proj, fade);
            if (timed)
                timer.end();
            if (syncFrames)
                glFinish(); // this frame's time is ready at the next poll
            if (nextGrab < args.grabs.size() && args.steps - left >= args.grabs[nextGrab].at) {
                std::vector<uint8_t> grab(static_cast<size_t>(fbW) * fbH * 4);
                glBindFramebuffer(GL_FRAMEBUFFER, outFb);
                glReadBuffer(GL_COLOR_ATTACHMENT0);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, fbW, fbH, GL_RGBA, GL_UNSIGNED_BYTE, grab.data());
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                while (nextGrab < args.grabs.size() && args.steps - left >= args.grabs[nextGrab].at) {
                    const auto& g = args.grabs[nextGrab++];
                    if (!writePng(g.file, fbW, fbH, grab))
                        std::fprintf(stderr, "hyprns-bench: could not write %s\n", g.file.c_str());
                    std::fprintf(stderr, "hyprns-bench: grab at step %ld (frame %ld): %s; %s\n", args.steps - left, frame, g.file.c_str(), look->status().c_str());
                }
            }
            for (auto& c : changeAt)
                if (c.liveAfter < 0 && frame >= c.frame + c.window)
                    c.liveAfter = Gl::liveObjects();
            // --in-flight N: at most N frames queued on the GPU, as a compositor's swap
            // chain allows; the wait for the oldest is not counted as the health calls'
            if (args.inFlight > 0) {
                inFlightFences.push_back(glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0));
                while (static_cast<int>(inFlightFences.size()) > args.inFlight) {
                    glClientWaitSync(inFlightFences.front(), GL_SYNC_FLUSH_COMMANDS_BIT, 10'000'000'000ull);
                    glDeleteSync(inFlightFences.front());
                    inFlightFences.pop_front();
                }
            }

            {
                const auto t0     = std::chrono::steady_clock::now();
                bool       blown  = false;
                if (args.asyncHealth) {
                    const EHealth h = look->takeHealth(false);
                    healthVerdicts += h != EHealth::NONE;
                    blown = h == EHealth::BLOWN;
                    if (frame % healthEvery == 0)
                        look->checkHealth();
                } else if (frame % healthEvery == 0) {
                    blown = !look->healthy();
                    healthVerdicts++;
                }
                healthMsMax = std::max(healthMsMax, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
                if (blown) {
                    look->reset();
                    resets++;
                }
            }
            // --status-every: jupiter's density and speed extremes over the last span of
            // steps, from its health checks (a long-run log); any other look with a fluid,
            // its mean density now
            if (args.statusEvery > 0 && static_cast<long>(stepsRun) >= nextStatus) {
                nextStatus += args.statusEvery;
                if (auto* jl = dynamic_cast<CJupiterLook*>(look.get())) {
                    const auto sp = jl->takeSpan();
                    std::fprintf(stderr,
                                 "{\"steps\": %lu, \"checks\": %d, \"rho_min\": %.5f, \"rho_max\": %.5f, \"rho_mean_min\": %.6f, \"rho_mean_max\": %.6f, "
                                 "\"u_max\": %.5f, \"resets\": %lu}\n",
                                 static_cast<unsigned long>(stepsRun), sp.checks, static_cast<double>(sp.rhoMin), static_cast<double>(sp.rhoMax),
                                 static_cast<double>(sp.rhoMeanMin), static_cast<double>(sp.rhoMeanMax), static_cast<double>(sp.umax),
                                 static_cast<unsigned long>(resets));
                    std::fflush(stderr);
                } else if (const float rm = look->meanDensity(); !std::isnan(rm)) {
                    std::fprintf(stderr, "{\"steps\": %lu, \"rho_mean\": %.7f, \"resets\": %lu}\n", static_cast<unsigned long>(stepsRun),
                                 static_cast<double>(rm), static_cast<unsigned long>(resets));
                    std::fflush(stderr);
                }
            }
        }
        // --churn: what the plugin does to a look over a long session, many times over:
        // 200 look switches (each a new look built, the old one destroyed), 200 quality
        // changes (rebuilds), 50 resizes (output and look rebuilt), a frame after each;
        // it ends where it started (the --look, q, size), so the live GL objects must be
        // the count before it
        if (args.churn) {
            // the count starts from a freshly built look, as it ends
            look.reset();
            look                  = install(args.look, geometry);
            const int   liveStart = Gl::liveObjects();
            const auto  names     = lookNames();
            const auto  frameOnce = [&] {
                look->advance(1);
                look->draw(outFb, 0, 0, fbW, fbH, proj, 1.0f);
                glFinish();
            };
            const int startW = monW, startH = monH;
            for (int i = 1; i <= 200; i++) {
                look.reset();
                look = install(i == 200 ? args.look : std::string(names[static_cast<size_t>(i) % names.size()]), geometry);
                frameOnce();
            }
            const float qs[4] = {0.75f, 0.5f, 0.35f, 1.0f};
            for (int i = 0; i < 200; i++) {
                quality = i == 199 ? args.quality : qs[i % 4];
                if (!look->build(geometry, quality, &error))
                    die(2, error);
                frameOnce();
            }
            for (int i = 1; i <= 50; i++) {
                const bool odd = i % 2 == 1;
                monW           = odd ? startH : startW; // the other orientation, then back
                monH           = odd ? startW : startH;
                makeOutput();
                geometry = geometryFor(monW, monH);
                if (!look->build(geometry, quality, &error))
                    die(2, error);
                frameOnce();
            }
            std::fprintf(stderr, "{\"churn\": {\"switches\": 200, \"quality_changes\": 200, \"resizes\": 50, \"live_before\": %d, \"live_after\": %d}}\n", liveStart,
                         Gl::liveObjects());
        }
        bad = stalled || resets > 0 || !look->healthy();
        // the lattice the look says it runs (0: the geometry's)
        const SLookInfo info = look->info();
        latticeW             = info.latticeW > 0 ? info.latticeW : geometry.cellsW;
        latticeH             = info.latticeH > 0 ? info.latticeH : geometry.cellsH;
        if (const auto* test = dynamic_cast<const CFluidTestLook*>(look.get()))
            fluid = test->lastStats(); // the bench's own look: from the healthy() just above
        std::fprintf(stderr, "hyprns-bench: %s\n", look->status().c_str());

        pixels.resize(static_cast<size_t>(fbW) * fbH * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, outFb);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, fbW, fbH, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (const auto* bt = dynamic_cast<const CBakeTestLook*>(look.get())) {
            const auto prev = bt->checkKey(true), next = bt->checkKey(false);
            const int  pic = CBakeTestLook::wholeRows(pixels, fbW, fbH), h = bt->height();
            bakeJson = std::format(", \"bake\": {{\"height\": {}, \"previous_rows\": {}, \"next_rows\": {}, \"picture_rows\": {}, \"picture_height\": {}, \"keys\": [{}, {}]}}", h,
                                   prev.wholeRows, next.wholeRows, pic, fbH, prev.key, next.key);
            bakeBad = prev.wholeRows != h || next.wholeRows != h || pic != fbH || prev.key >= next.key;
        }

        // the results still in flight: done after a glFinish
        if (timers) {
            glFinish();
            polled.clear();
            timer.poll(polled);
            gpuSamples.insert(gpuSamples.end(), polled.begin(), polled.end());
            sampleFrame.insert(sampleFrame.end(), polled.size(), syncFrames ? frame : -1);
        }
        std::fprintf(stderr, "hyprns-bench: %zu frames measured, q %g, time_scale %g%s\n", gpuSamples.size(), static_cast<double>(levels.quality),
                     static_cast<double>(levels.timeScale), levels.overBudget ? ", over budget" : "");
        // the timer's queries and the picture target go with this scope, before the count
    }

    // mean GPU time per frame, past the first tenth (the start: programs, first touches)
    std::optional<double> gpuMs;
    std::optional<float>  gpuMax; // past the first measured frame
    if (!gpuSamples.empty()) { // --time or --budget
        const size_t skip = gpuSamples.size() / 10;
        double       sum  = 0;
        for (size_t i = skip; i < gpuSamples.size(); i++)
            sum += gpuSamples[i];
        gpuMs = sum / static_cast<double>(gpuSamples.size() - skip);
        std::fprintf(stderr, "hyprns-bench: first measured frame %.3f ms, largest %.3f ms\n", static_cast<double>(gpuSamples.front()),
                     static_cast<double>(*std::max_element(gpuSamples.begin(), gpuSamples.end())));
        if (gpuSamples.size() > 1)
            gpuMax = *std::max_element(gpuSamples.begin() + 1, gpuSamples.end());
    }
    // per change: the largest frame in the 2 s of frames from it
    std::string changesJson;
    for (size_t i = 0; i < changeAt.size(); i++) {
        const auto& c   = changeAt[i];
        const long  end = c.frame + c.window;
        float       mx  = -1.0f;
        long        at  = -1;
        for (size_t s = 0; s < gpuSamples.size(); s++)
            if (sampleFrame[s] >= c.frame && sampleFrame[s] < end && gpuSamples[s] > mx) {
                mx = gpuSamples[s];
                at = sampleFrame[s];
            }
        changesJson += std::format("{}{{\"frame\": {}, \"{}\": {}, \"max_ms\": {}, \"max_at_frame\": {}, \"live_objects_2s_later\": {}}}", i ? ", " : "", c.frame, c.key, c.value,
                                   mx < 0 ? std::string{"null"} : std::format("{:.3f}", mx), at, c.liveAfter);
    }
    if (!changesJson.empty())
        changesJson = ", \"changes\": [" + changesJson + "]";

    const int liveAfter = Gl::liveObjects();
    std::fprintf(stderr, "hyprns-bench: live GL objects before the first frame %d, after teardown %d\n", liveBefore, liveAfter);
    Gl::destroyFramebuffer(outFb);
    Gl::destroyTexture(outTex);

    // every GL object a look made is gone again
    const bool leaked = liveAfter != liveBefore;
    if (leaked)
        std::fprintf(stderr, "hyprns-bench: %d GL objects leaked\n", liveAfter - liveBefore);

    // back to screen orientation, row 0 at the top
    std::vector<uint8_t> screen(static_cast<size_t>(monW) * monH * 4);
    for (int sy = 0; sy < monH; sy++)
        for (int sx = 0; sx < monW; sx++) {
            const int fx = rotated ? fbW - 1 - sy : sx, fy = rotated ? sx : sy;
            std::memcpy(&screen[(static_cast<size_t>(sy) * monW + sx) * 4], &pixels[(static_cast<size_t>(fy) * fbW + fx) * 4], 4);
        }

    uint8_t lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
    for (size_t i = 0; i < screen.size(); i += 4)
        for (int c = 0; c < 3; c++) {
            lo[c] = std::min(lo[c], screen[i + c]);
            hi[c] = std::max(hi[c], screen[i + c]);
        }
    const bool uniform = hi[0] - lo[0] <= 8 && hi[1] - lo[1] <= 8 && hi[2] - lo[2] <= 8;

    if (!args.png.empty() && !writePng(args.png, monW, monH, screen))
        std::fprintf(stderr, "hyprns-bench: could not write %s\n", args.png.c_str());

    closeContext(egl);

    const std::string fluidJson = fluid ? std::format(", \"fluid\": {{\"umax\": {:.4f}, \"umean\": {:.5f}, \"rho_min\": {:.4f}, \"rho_max\": {:.4f}}}", fluid->umax, fluid->umean,
                                                      fluid->rhoMin, fluid->rhoMax)
                                        : "";
    std::printf("{\"look\": \"%s\", \"lattice\": [%d, %d], \"steps\": %llu, \"gpu_ms_per_frame\": %s, \"gpu_ms_max\": %s, \"q\": %s, \"time_scale\": %s, \"over_budget\": %s, "
                "\"live_objects\": %d, \"bad\": %s, \"health_cpu_ms_max\": %.3f, \"health_verdicts\": %ld%s%s%s}\n",
                shown.c_str(), latticeW, latticeH, static_cast<unsigned long long>(stepsRun), gpuMs ? std::format("{:.4f}", *gpuMs).c_str() : "null",
                gpuMax ? std::format("{:.3f}", *gpuMax).c_str() : "null", std::format("{}", levels.quality).c_str(), std::format("{}", levels.timeScale).c_str(),
                levels.overBudget ? "true" : "false", liveAfter, bad ? "true" : "false", healthMsMax, healthVerdicts, fluidJson.c_str(), bakeJson.c_str(),
                changesJson.c_str());
    if (uniform)
        std::fprintf(stderr, "hyprns-bench: the picture is uniform\n");
    if (bakeBad)
        std::fprintf(stderr, "hyprns-bench: a bake key or the picture has rows nothing wrote, or the keys are out of order\n");
    return bad || uniform || leaked || bakeBad ? 1 : 0;
}
