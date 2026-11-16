#pragma once

// The thin GL layer every look draws through. No Hyprland types: the same code runs
// inside the plugin (Hyprland's EGL context) and inside hyprns-bench (a headless one).
// Every call expects an OpenGL ES 3.2 context to be current.

#include <GLES3/gl32.h>

#include <array>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Gl {

struct SProgram {
    GLuint id   = 0;
    GLint  proj = -1; // the vertex shader's `uniform mat3 proj`, looked up by compile()
    GLint  uniform(const char* name) const;
};

// Fragment source without #version/precision lines; pairs with our own vertex shader
// (attribute-less, v_texcoord (0,0) top-left, uniform mat3 proj). nullopt + *error on failure.
// A shader the driver rejects never aborts: the driver's log comes back in *error.
std::optional<SProgram> compile(std::string_view fragment, std::string* error);
void                    destroy(SProgram&);

// One-level texture with immutable storage (glTexStorage2D), the filter on both min
// and mag, cleared to zero (glTexStorage2D leaves it undefined: whatever the memory held,
// another program's pictures included). Note: R32F/RGBA32F are not filterable in core
// GLES; use GL_NEAREST for them.
GLuint texture(GLenum internal, int w, int h, GLenum filter, GLenum wrapS = GL_CLAMP_TO_EDGE, GLenum wrapT = GL_CLAMP_TO_EDGE);
void   destroyTexture(GLuint&); // deletes and zeroes; 0 is ignored
// Replaces the whole level 0 of a texture from tightly packed rows (row 0 first), under
// CUnpackState: the context's unpack state (shared with Hyprland) does not apply.
void   upload(GLuint tex, int w, int h, GLenum format, GLenum type, const void* pixels);

GLuint framebuffer(std::initializer_list<GLuint> attachments); // 0 when incomplete
void   destroyFramebuffer(GLuint&);                            // deletes and zeroes; 0 is ignored

void   use(const SProgram&);
void   bind(int unit, GLuint tex);

// Maps the unit quad onto the whole target (column-major): v_texcoord (0,0) lands on
// the target's first row, so offscreen passes keep row 0 of their input at row 0.
inline constexpr std::array<GLfloat, 9> FULL = {2, 0, 0, 0, 2, 0, -1, -1, 1};
// Draws the unit quad with the program (made current) into fbo, viewport x,y,w,h.
// Uniforms other than proj are the caller's: Gl::use, set them, then pass.
void pass(const SProgram&, GLuint fbo, int x, int y, int w, int h, const std::array<GLfloat, 9>& proj = FULL);

// Textures + framebuffers + programs + queries + buffers + fences made and not yet
// deleted through Gl.
int liveObjects();

// ── pixel-store state ───────────────────────────────────────────────────────────────

// The context is Hyprland's (and every other plugin's): a bound pixel buffer makes a
// pointer an offset into it, a row length or skip makes the driver read or write past
// our memory. These save the pack (read-back) or unpack (upload) state, set tight rows
// (alignment 1, row length and skips 0) with `buffer` bound (0: client memory), and put
// everything back on destruction.
class CPackState {
  public:
    explicit CPackState(GLuint buffer = 0);
    ~CPackState();
    CPackState(const CPackState&)            = delete;
    CPackState& operator=(const CPackState&) = delete;

  private:
    GLint m_buffer = 0, m_rowLength = 0, m_skipPixels = 0, m_skipRows = 0, m_alignment = 4;
};

class CUnpackState {
  public:
    explicit CUnpackState(GLuint buffer = 0);
    ~CUnpackState();
    CUnpackState(const CUnpackState&)            = delete;
    CUnpackState& operator=(const CUnpackState&) = delete;

  private:
    GLint m_buffer = 0, m_rowLength = 0, m_imageHeight = 0, m_skipPixels = 0, m_skipRows = 0, m_skipImages = 0, m_alignment = 4;
};

// ── read-back without waiting ───────────────────────────────────────────────────────

// Reads RGBA float texels into a pixel-pack buffer and fences them; take() hands them
// over once the GPU has written them. take(false) never blocks (the render thread
// polls it on later frames), take(true) waits (the bench, tests). One read in flight at
// a time.
class CReadback {
  public:
    CReadback() = default;
    ~CReadback();
    CReadback(const CReadback&)            = delete;
    CReadback& operator=(const CReadback&) = delete;

    struct SRegion {
        GLuint fb;
        int    attachment; // GL_COLOR_ATTACHMENT0 + attachment
        int    x, y, w, h;
    };
    // Queues the reads (RGBA, GL_FLOAT, one region after another in the buffer). False,
    // nothing queued, while a read is in flight.
    bool start(std::initializer_list<SRegion> regions);
    // The floats once they are there (true); false while not ready or none in flight.
    bool take(std::vector<float>& out, bool wait);
    bool pending() const { return m_fence != nullptr; }
    void cancel();  // drops a read in flight
    void release(); // and the buffer

  private:
    GLuint m_buffer = 0;
    size_t m_capacity = 0, m_bytes = 0;
    GLsync m_fence   = nullptr;
    bool   m_flushed = false;
};

// The current context lists GL extension `name` (a whole word of GL_EXTENSIONS).
bool hasExtension(std::string_view name);

// ── GPU timing (GL_EXT_disjoint_timer_query, optional) ──────────────────────────────

bool   timerQueries();       // the extension is there (looked up once)
GLuint query();              // a query object (glGenQueries)
void   destroyQuery(GLuint&); // deletes and zeroes; 0 is ignored

// Times what runs between begin() and end() on the GPU, never waiting for a result: a
// small ring of queries, each read once the GPU reports it done (poll, frames later).
// begin() is false (nothing timed this frame) without the extension, while every slot
// still waits for its result, or while another time-elapsed query is open in the
// context (another plugin is measuring: only one may be open). A disjoint event drops
// the results it covers, in every timer: reading GL_GPU_DISJOINT_EXT clears it, so the
// timer that reads it set bumps one disjoint epoch shared by all timers, and each query
// keeps the epoch it began in. discard() drops every result still in flight (they
// measured something that no longer runs).
class CGpuTimer {
  public:
    explicit CGpuTimer(int slots = 4);
    ~CGpuTimer();
    CGpuTimer(const CGpuTimer&)            = delete;
    CGpuTimer& operator=(const CGpuTimer&) = delete;

    bool begin();
    void end(); // only after a begin() that returned true
    // Appends the GPU ms of every query that finished since the last poll, oldest first.
    void poll(std::vector<float>& msOut);
    void discard();
    void release(); // deletes the queries (they are made again on demand)

  private:
    struct SSlot {
        GLuint   id      = 0;
        bool     pending = false;
        unsigned gen     = 0; // this timer's discard() count at begin
        unsigned epoch   = 0; // the shared disjoint epoch at begin
    };
    std::vector<SSlot> m_slots;
    int                m_next = 0, m_open = -1;
    unsigned           m_gen  = 0;
};

// A finished query's result counts only when neither its timer's discard() nor a
// disjoint event (seen by any timer) came after its begin.
inline bool keepResult(unsigned genAtBegin, unsigned genNow, unsigned epochAtBegin, unsigned epochNow) {
    return genAtBegin == genNow && epochAtBegin == epochNow;
}

// ── the picture at a lower resolution ──────────────────────────────────────────────

// An RGBA8 target the look draws into (with FULL) when the quality is below 1, and the
// pass that stretches it over the monitor with linear filtering through the monitor's
// projection. Opaque; the stretch copies the target as it is (the fade is the look's).
class CPictureTarget {
  public:
    CPictureTarget() = default;
    ~CPictureTarget();
    CPictureTarget(const CPictureTarget&)            = delete;
    CPictureTarget& operator=(const CPictureTarget&) = delete;

    // (Re)makes the target at w x h (each at least 1) when that changed. False when
    // the GL objects could not be made (*error says which).
    bool   ensure(int w, int h, std::string* error = nullptr);
    GLuint fbo() const { return m_fbo; }
    int    width() const { return m_w; }
    int    height() const { return m_h; }
    void   stretch(GLuint fbo, int x, int y, int w, int h, const std::array<GLfloat, 9>& proj);
    void   release(); // the GL objects; a failed size stays remembered

  private:
    SProgram m_program;
    GLint    m_imgLoc = -1;
    GLuint   m_tex = 0, m_fbo = 0;
    int      m_w = 0, m_h = 0;
    int      m_failedW = 0, m_failedH = 0; // the size that could not be made: not tried again every frame
    std::string m_failure;
};

// round(q x side), at least 1: the target's side for a monitor side at quality q.
int scaledSide(int side, float quality);

// Saves what our passes change and puts it back on destruction: program, VAO,
// active texture, the bindings of units 0 to 4 (the most any look binds), draw/read
// framebuffers, viewport, scissor box and test, blend. Pixel-store state is
// CPackState's and CUnpackState's, around each read-back and upload. Restoring the raw values keeps Hyprland's caches true (it caches the
// bound program, the viewport and the blend/scissor caps).
class CStateGuard {
  public:
    CStateGuard();
    ~CStateGuard();
    CStateGuard(const CStateGuard&)            = delete;
    CStateGuard& operator=(const CStateGuard&) = delete;

  private:
    static constexpr int UNITS = 5;
    GLint     m_program = 0, m_vao = 0, m_active = GL_TEXTURE0, m_tex[UNITS] = {}, m_drawFb = 0, m_readFb = 0;
    GLint     m_viewport[4] = {}, m_scissorBox[4] = {};
    GLboolean m_scissor = GL_FALSE, m_blend = GL_FALSE;
};

} // namespace Gl
