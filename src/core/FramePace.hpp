#pragma once

// When a plugin frame moves the picture on, how a look switch fades, and when the frame
// also refreshes Hyprland's blur cache. No Hyprland or GL types: the unit tests build this.

#include <utility>

// A look switch fades the picture to black over FADE_SECONDS, replaces the look, and
// fades back in over FADE_SECONDS: 0.4 s each way. The owner asked for 0.4 s (goal.md);
// the spec's wording ("fades ... and back over 0.4 s") can also mean 0.4 s in total.
inline constexpr double FADE_SECONDS = 0.4;

// The fade after dt more seconds of a switch: toward 0 while fading out, toward 1
// otherwise. Within a ten-thousandth of either end it is that end, so a fade takes a
// whole number of frames.
float fadeAfter(float fade, double dt, bool out);

// The time a fade moves on in this frame: the time since the last frame that moved the
// picture on (0 on the first), at most two frames of fps and at least 0.1 s, so a fade
// takes 0.4 s also at low fps but does not jump after a dark spell.
double fadeDt(double lastAdvance, double now, int fps);

// blur_fps: whether this wallpaper frame also marks the monitor's blur cache stale.
//   blurFps < 0: every wallpaper frame.
//   blurFps 0:   never (the cache follows only Hyprland's own reasons to refresh it).
//   blurFps N:   at most min(N, fps) times a second: the first frame, then every k-th
//                frame of fps, k = ceil(fps / N), and never less than 1/N apart. The
//                timer fires on time or late, never early, so this is never above N.
// lastRefresh < 0: no refresh yet.
bool blurRefreshDue(int blurFps, int fps, double now, double lastRefresh);

// Whether the picture stays the same from one plugin frame to the next: the pace is 0
// (time_scale 0, or the budget's pace), no switch or fade-in runs, no reset waits, and
// the look's picture is whole (settled: marble and embers bake theirs over a second of
// frames and show black until then). A look that moves with wall time
// (SLookInfo::wallClock, jupiter's grain) still changes.
bool pictureFrozen(float timeScale, float pace, float fade, bool switching, bool resetWaiting, bool settled);

// Whether a draw moves the picture on (steps the look, moves the fade). Only frames the
// plugin's timer asked for do, so the picture changes only in frames that can also
// refresh the blur cache; a redraw Hyprland makes for its own reasons (an animation, a
// video in a window) shows the picture as it is. A request waits for the next draw.
class CAdvanceGate {
  public:
    void request() { m_requested = true; }
    // fresh: the look was just built, rebuilt or reset, and takes its first step now
    bool take(bool fresh) { return std::exchange(m_requested, false) || fresh; }

  private:
    bool m_requested = false;
};

// A dark spell (screen off, fullscreen window, or a frozen picture whose clock did not
// run) ends with the next frame that moves the picture on: that frame catches up at
// most two frames of steps (CStepClock::maxDtFor). Any draw can see the spell end, often
// a redraw of Hyprland's own that does not move the picture; the latch keeps it until
// the frame that does.
class CDarkLatch {
  public:
    // a draw: whether it comes after a dark spell (more than DARK_SECONDS since the last
    // draw; the caller restarts its GPU mean); the latch keeps it
    bool see(double lastFrame, double now);
    // the timer skipped the monitor (hidden, or frozen): the clock did not run
    void mark() { m_pending = true; }
    // a frame that moves the picture on: whether it is the first after a dark spell
    bool take() { return std::exchange(m_pending, false); }

  private:
    bool m_pending = false;
};

// What the frame timer knows about one monitor.
struct SFrameInputs {
    bool   hasLook   = false; // a look is built (false: the first frame builds it)
    bool   failed    = false; // nothing could be built: wait for a change
    bool   visible   = true;  // enabled, screen on, no fullscreen window
    bool   wallClock = false; // the look moves with wall time (SLookInfo::wallClock)
    bool   settled   = true;  // the look's picture is whole (ILook::settled)
    bool   wasFrozen = false; // the last tick found the picture frozen
    bool   switching = false; // a look switch waits or runs
    bool   resetWaiting = false;
    float  timeScale = 1.0f;  // the setting
    float  pace      = 1.0f;  // the budget's pace
    float  fade      = 1.0f;
    int    blurFps   = -1;
    int    fps       = 30;
    double now       = 0.0;
    double lastBlurRefresh = -1.0;
};

// What the frame timer does with that monitor.
struct SFrameDecision {
    bool frame  = false; // ask for a frame that moves the picture on
    bool blur   = false; // and mark the blur cache stale with it
    bool frozen = false; // the picture is frozen (kept as wasFrozen for the next tick)
    bool dark   = false; // the clock does not run now: mark the dark latch
};

// The frame timer's decision for one monitor:
// - failed: nothing. Hidden: nothing, and the next frame that moves the picture is the
//   end of a dark spell.
// - no look yet: a frame with a blur refresh (the first frame builds the look).
// - frozen: one more frame, with a blur refresh, on the first frozen tick (the cache gets
//   the final picture); then nothing, except frames without refresh for a wall-clock
//   look. The clock stands still, so the frame after the freeze ends counts as after a
//   dark spell (no catch-up burst).
// - otherwise: a frame, with a refresh as blur_fps says.
SFrameDecision decideFrame(const SFrameInputs& in);
