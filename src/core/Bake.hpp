#pragma once

// A keyframe bake cache: a picture too costly to compute per pixel per frame (marble's
// stone, embers' coals) is baked into textures a strip per frame, and the look shows a
// blend of the two newest complete keys.
//
// Three textures: previous and next are complete and shown, mix(previous, next,
// blend()); back is being baked, one strip of rows per advance(). A key is framesPerKey
// strips of (nearly) equal height, so after framesPerKey advances back is complete and
// the keys rotate: previous <- next, next <- back, back <- the old previous. blend()
// rises by 1 / framesPerKey per advance, from 0 right after a rotation, so the picture
// moves at an even pace from key to key and a key is shown only once it is whole: no
// strip boundary is ever on screen.
//
// Priming: after build() (or restart()) previous and next hold nothing yet. The next
// advance() bakes both whole, `primeRows` rows per call (default: all of them in that
// one call); ready() is false until both are, and the caller shows black meanwhile.
// While the callback runs, bakingKey() says which key it bakes (0 previous, 1 next while
// priming, 2 back), so it can set that key's time.
//
// Rebuild (a new size or frame rate, the look's state kept): rebuild() keeps the shown
// pair (previous and next, and the blend at that moment) and goes on showing it while
// the new keys prime, then frees it on the advance that makes them whole. The held pair
// is at its old size: the look maps into it with shownMap() (what it set with setMap()
// when that pair was built). A second rebuild while one primes drops the half-primed
// keys and keeps the held pair. Without a whole pair to keep (first build, a restart)
// nothing is shown until the keys are whole: showing() is false and the look shows black.
//
// The textures are cleared to (0, 0, 0, 0) when made, filtered by the hardware
// (GL_LINEAR, CLAMP_TO_EDGE): a bake is an 8-bit colour target (RGBA8, SRGB8_ALPHA8),
// unlike the float fields, which are only read with texelFetch.

#include "gl/Gl.hpp"

#include <array>
#include <functional>
#include <string>

// The strip schedule alone, no GL (unit tested).
class CBakeSchedule {
  public:
    struct SStrip {
        int y0, y1; // rows [y0, y1)
    };
    // strip k (0..framesPerKey-1) of a texture of `rows` rows: floor(k rows / F) up to
    // floor((k + 1) rows / F); empty when there are fewer rows than strips
    static SStrip strip(int k, int rows, int framesPerKey);

    void  reset(int rows, int framesPerKey); // a new key begins
    bool  starting() const { return m_frame == 0; }
    float blend() const { return static_cast<float>(m_frame) / static_cast<float>(m_frames); }
    // this frame's strip in *out, then on to the next; true when that strip completed
    // the key (the caller rotates), and the schedule starts the next key
    bool advance(SStrip* out);

  private:
    int m_rows = 0, m_frames = 1, m_frame = 0;
};

class CBake {
  public:
    using FBakeStrip = std::function<void(GLuint fbo, int y0, int y1)>;

    CBake() = default;
    ~CBake();
    CBake(const CBake&)            = delete;
    CBake& operator=(const CBake&) = delete;

    // (Re)makes the three w x h textures and their framebuffers; primes on the next advance.
    bool build(int w, int h, GLenum internal, int framesPerKey, std::string* error);
    // As build(), keeping the shown pair on screen until the new keys are whole (above).
    // On failure the held pair (if any) stays shown and nothing else is left.
    bool rebuild(int w, int h, GLenum internal, int framesPerKey, std::string* error);
    void release();
    void restart();                // previous and next count as empty again: the next advance primes (a held pair goes)
    void setPrimeRows(int rows);   // rows primed per advance (<= 0: all in one advance)
    // The look's own mapping into the keys being built (kept with them when held).
    void setMap(const std::array<float, 4>& map) { m_map = map; }

    // Priming: rows of previous, then next, until both are whole. After that: this
    // frame's strip of back, rotating the keys when it completes the key.
    void advance(const FBakeStrip& bakeStrip);

    bool   ready() const { return m_primed >= 2 * m_h && m_h > 0; }
    bool   starting() const { return ready() && m_schedule.starting(); } // back begins on this advance: latch its time
    bool   rotated() const { return m_rotated; }                        // the last advance rotated the keys
    bool   holding() const { return !ready() && m_held[0] != 0; }        // the old pair is shown while the new primes
    bool   showing() const { return ready() || holding(); }              // something whole to show
    int    bakingKey() const { return m_baking; }
    GLuint previous() const { return holding() ? m_held[0] : m_tex[m_prev]; }
    GLuint next() const { return holding() ? m_held[1] : m_tex[m_next]; }
    float  blend() const { return ready() ? m_schedule.blend() : holding() ? m_heldBlend : 0.0f; }
    const std::array<float, 4>& shownMap() const { return holding() ? m_heldMap : m_map; }
    int    width() const { return m_w; }
    int    height() const { return m_h; }
    int    framesPerKey() const { return m_frames; }
    GLenum internal() const { return m_internal; }

  private:
    bool makeKeys(int w, int h, GLenum internal, int framesPerKey, std::string* error);
    void releaseKeys();
    void releaseHeld();
    void restartKeys();

    GLuint               m_tex[3] = {}, m_fb[3] = {};
    int                  m_prev = 0, m_next = 1, m_back = 2;
    int                  m_w = 0, m_h = 0, m_frames = 1, m_primed = 0, m_primeRows = 0, m_baking = -1;
    GLenum               m_internal = 0;
    bool                 m_rotated  = false;
    CBakeSchedule        m_schedule;
    std::array<float, 4> m_map{};
    GLuint               m_held[2] = {}; // the old previous and next while the new keys prime
    float                m_heldBlend = 0.0f;
    std::array<float, 4> m_heldMap{};
};

// The fluid-step times of a bake's keys, for a look whose keys are stills of a clock
// (marble's stone, embers' coals); no GL (unit tested). Per bake advance, before it:
// frame(); in the strip callback keyTime(bake.bakingKey()); after it, rotated() when the
// bake rotated. Priming bakes previous at the prime time and next one nominal key period
// later (nominal = steps per second x frames per key / fps). Each back key is latched on
// the frame it starts: next's time plus the steps the last key period ran (nominal for the
// first), so the shown time runs at the clock's pace at any time_scale or frame rate, a
// key period or two behind it.
class CKeyClock {
  public:
    // The next frame() primes from `from` (< 0: that frame's step).
    void requestPrime(double from = -1.0);
    // A rebuild: prime from the time shown now (whole: the bake is ready and shows blend;
    // otherwise its held or priming pair starts at previous's time). A request already
    // pending is kept.
    void requestPrimeFromShown(bool whole, float blend);
    // ready, starting: the bake's, before its advance; steps: this frame's fluid steps
    void   frame(double step, double nominal, bool ready, bool starting, int steps);
    void   rotated(); // previous <- next <- back
    double keyTime(int bakingKey) const { return bakingKey == 0 ? m_prev : bakingKey == 1 ? m_next : m_back; }
    double shown(bool whole, float blend) const { return whole ? m_prev + static_cast<double>(blend) * (m_next - m_prev) : m_prev; }

  private:
    double m_prev = 0, m_next = 0, m_back = 0, m_period = 0, m_from = -1.0;
    bool   m_havePeriod = false, m_pending = false;
};
