#pragma once

// One look: a picture that moves, behind one monitor. Every look is a class behind
// ILook; the plugin and the bench make one per monitor through makeLook. No Hyprland
// types: the same code runs in the plugin and in hyprns-bench.
//
// Orientation: draw() gets v_texcoord (0,0) at the picture's top-left. A look ported
// from a WebGL sketch keeps the sketch's coordinates in its simulation (lattice row 0 =
// the bottom of the picture, as on the sketch's canvas) and flips once, in its picture
// pass (cell y = (1 - v_texcoord.y) * height); see looks/PatternShaders.hpp.

#include <GLES3/gl32.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

// The monitor in its own orientation (1440x2560 on a portrait output) and the lattice
// a look sizes itself by.
struct SGeometry {
    int monW, monH, cellsW, cellsH;
};
// The short side is 225 cells, the long side follows the aspect (a half rounds up).
SGeometry geometryFor(int monW, int monH);

// What a look states about itself (ILook::info()). The defaults are a look that runs
// SGeometry's lattice and changes only when it steps: the four looks keep them, except
// that jupiter sets wallClock. The bench's test looks set their own lattice.
struct SLookInfo {
    int  latticeW = 0, latticeH = 0; // the lattice it runs; 0 = SGeometry's cellsW x cellsH
    bool wallClock = false;          // the picture also moves with wall time (setWallClock), at time_scale 0 too
};

// A health check's verdict: none yet (still in flight, or nothing to check), or one.
enum class EHealth {
    NONE,
    HEALTHY,
    BLOWN,
};

// Every method runs with a GL ES 3.2 context current, except info() (no GL).
//
// Health, in two halves so that the render thread never waits for the GPU: the caller
// calls checkHealth() after advance() (every ~2 s), which reduces the lattice on the GPU
// and queues a read of a few bytes behind a fence (nothing when nothing stepped since the
// last check, or one is still in flight); takeHealth(false), polled every frame, returns
// the verdict once it has arrived (NONE until then, a frame or more later), and BLOWN
// means: call reset(). takeHealth(true) waits for a check in flight, or without one gives
// the last verdict. healthy() is both, waiting (the bench and the tests). A look is
// healthy until its first initialised step (a freshly built or reset look may hold
// textures nothing has written yet); build() and reset() drop a check in flight.
// checkHealth() binds programs, textures and framebuffers: the caller saves its state
// and has the scissor test and blending off. reset() may run where the caller's
// scissor is on: a look defers the GL work of a reset to its next advance().
class ILook {
  public:
    virtual ~ILook() = default;
    virtual bool        build(const SGeometry& g, float quality, std::string* error) = 0; // (re)allocates; called again when either changes
    virtual float       stepsPerSecond() const = 0;                                        // before time_scale
    virtual void        advance(int steps) = 0;                                            // once per drawn frame, steps may be 0
    virtual void        draw(GLuint fbo, int x, int y, int w, int h, const std::array<GLfloat, 9>& proj, float fade) = 0; // fade 0 = black, 1 = picture
    virtual void        checkHealth() = 0;
    virtual EHealth     takeHealth(bool wait) = 0;
    bool                healthy(); // checkHealth() then takeHealth(true): false = blown up, the caller resets
    virtual void        reset() = 0;
    virtual std::string status() const = 0;                                                // one line
    virtual SLookInfo   info() const { return {}; }
    // Whether the picture is whole. A look that builds it over frames (marble's and
    // embers' bake, jupiter's first cloud) is not until then, and needs advance() calls
    // (steps may be 0) to finish it: the plugin keeps drawing a frozen monitor until it is.
    virtual bool settled() const { return true; }
    // The frames per second the caller draws at (the plugin's fps setting), told before
    // every advance(); a look that spreads work over frames (a bake) sizes it by this.
    // No GL here: a look acts on a change in its next advance().
    virtual void setFrameRate(float fps) {}
    // Wall time in nanoseconds (any fixed origin), told before every advance(); for what
    // moves with real time whatever time_scale says (jupiter's sensor grain). The plugin
    // passes steady_clock, the bench a deterministic 1/fps per frame. No GL here.
    virtual void setWallClock(uint64_t ns) {}
    // The fluid's mean density (mass per cell, 1 at rest), read back from the GPU: for
    // tests and the bench, not per frame. NaN for a look without a fluid.
    virtual float meanDensity() { return NAN; }
};

// pattern, embers, marble, jupiter
std::span<const std::string_view> lookNames();
bool                              isLookName(std::string_view name);
// "pattern, embers, marble, jupiter"
std::string lookNameList();

// nullptr for a name lookNames() does not have. The seed makes two monitors with the
// same look differ; the same seed gives the same picture.
std::unique_ptr<ILook> makeLook(std::string_view name, uint64_t seed);

// The steps per second the clock runs a look at: its own, times time_scale (0 freezes).
float clockRate(const ILook& look, float timeScale);

// A stable seed for a monitor, from its connector name (FNV-1a, 64 bit).
uint64_t seedFor(std::string_view monitorName);
