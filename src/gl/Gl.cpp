#include "gl/Gl.hpp"

#include <EGL/egl.h>
#include <GLES2/gl2ext.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace Gl {

namespace {

constexpr const char* HEADER = "#version 320 es\n"
                               "precision highp float;\n"
                               "precision highp int;\n"
                               "precision highp sampler2D;\n";

// Attribute-less unit quad in Hyprland's order (fullVerts): position = texcoord,
// (0,0) the top-left of the drawn box. Drawn as a 4-vertex triangle strip.
constexpr const char* VERTEX = R"GLSL(
uniform mat3 proj;
out vec2 v_texcoord;

void main() {
    const vec2 QUAD[4] = vec2[4](vec2(0.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0));
    vec2 p      = QUAD[gl_VertexID];
    v_texcoord  = p;
    gl_Position = vec4(proj * vec3(p, 1.0), 1.0);
}
)GLSL";

int    s_live     = 0; // textures + framebuffers + programs + queries + buffers + fences
int    s_programs = 0; // programs alive; the shared VAO lives while there is one
GLuint s_vao      = 0; // empty: ES 3 draws need a VAO, ours has no attribute enabled

std::string infoLog(GLuint object, bool program) {
    GLint len = 0;
    if (program)
        glGetProgramiv(object, GL_INFO_LOG_LENGTH, &len);
    else
        glGetShaderiv(object, GL_INFO_LOG_LENGTH, &len);
    std::string log(static_cast<size_t>(std::max(len, 1)), '\0');
    GLsizei     got = 0;
    if (program)
        glGetProgramInfoLog(object, len, &got, log.data());
    else
        glGetShaderInfoLog(object, len, &got, log.data());
    log.resize(static_cast<size_t>(got));
    while (!log.empty() && (log.back() == '\n' || log.back() == '\0'))
        log.pop_back();
    return log.empty() ? "(no log)" : log;
}

GLuint shader(GLenum type, std::string_view body, std::string* error) {
    const std::string src  = std::string(HEADER) + std::string(body);
    const char*       text = src.c_str();
    GLuint            s    = glCreateShader(type);
    glShaderSource(s, 1, &text, nullptr);
    glCompileShader(s);
    GLint ok = GL_FALSE;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        if (error)
            *error = std::format("{} shader: {}", type == GL_VERTEX_SHADER ? "vertex" : "fragment", infoLog(s, false));
        glDeleteShader(s);
        return 0;
    }
    return s;
}

} // namespace

GLint SProgram::uniform(const char* name) const {
    return glGetUniformLocation(id, name);
}

std::optional<SProgram> compile(std::string_view fragment, std::string* error) {
    const GLuint vs = shader(GL_VERTEX_SHADER, VERTEX, error);
    if (!vs)
        return std::nullopt;
    const GLuint fs = shader(GL_FRAGMENT_SHADER, fragment, error);
    if (!fs) {
        glDeleteShader(vs);
        return std::nullopt;
    }
    const GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDetachShader(prog, vs);
    glDetachShader(prog, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        if (error)
            *error = std::format("link: {}", infoLog(prog, true));
        glDeleteProgram(prog);
        return std::nullopt;
    }
    if (!s_vao)
        glGenVertexArrays(1, &s_vao);
    s_live++;
    s_programs++;
    SProgram p;
    p.id   = prog;
    p.proj = glGetUniformLocation(prog, "proj");
    return p;
}

void destroy(SProgram& p) {
    if (!p.id)
        return;
    glDeleteProgram(p.id);
    p = {};
    s_live--;
    if (--s_programs == 0 && s_vao) {
        glDeleteVertexArrays(1, &s_vao);
        s_vao = 0;
    }
}

namespace {

// format and type for a zero upload, for a format a framebuffer cannot clear
bool zeroUploadFormat(GLenum internal, GLenum& format, GLenum& type, int& bytes) {
    switch (internal) {
        case GL_R32F: format = GL_RED; type = GL_FLOAT; bytes = 4; return true;
        case GL_RG32F: format = GL_RG; type = GL_FLOAT; bytes = 8; return true;
        case GL_RGBA32F: format = GL_RGBA; type = GL_FLOAT; bytes = 16; return true;
        case GL_R16F: format = GL_RED; type = GL_HALF_FLOAT; bytes = 2; return true;
        case GL_RG16F: format = GL_RG; type = GL_HALF_FLOAT; bytes = 4; return true;
        case GL_RGBA16F: format = GL_RGBA; type = GL_HALF_FLOAT; bytes = 8; return true;
        case GL_R8: format = GL_RED; type = GL_UNSIGNED_BYTE; bytes = 1; return true;
        case GL_RG8: format = GL_RG; type = GL_UNSIGNED_BYTE; bytes = 2; return true;
        case GL_RGBA8: format = GL_RGBA; type = GL_UNSIGNED_BYTE; bytes = 4; return true;
        default: return false;
    }
}

// Zeroes a new texture: a clear through a framebuffer of its own (the scissor and the
// colour mask, which a clear obeys, off for it and restored), or a zero upload when
// the format cannot be a colour target.
void clearTexture(GLuint t, GLenum internal, int w, int h) {
    GLint     drawFb = 0, readFb = 0;
    GLboolean mask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFb);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFb);
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLuint          fb      = 0;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    const bool cleared = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (cleared) {
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        const GLfloat zero[4] = {0, 0, 0, 0};
        glClearBufferfv(GL_COLOR, 0, zero);
        glColorMask(mask[0], mask[1], mask[2], mask[3]);
        if (scissor)
            glEnable(GL_SCISSOR_TEST);
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFb));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFb));
    glDeleteFramebuffers(1, &fb);
    GLenum format = 0, type = 0;
    int    bytes  = 0;
    if (!cleared && zeroUploadFormat(internal, format, type, bytes)) {
        const std::vector<uint8_t> zeros(static_cast<size_t>(w) * h * bytes, 0);
        upload(t, w, h, format, type, zeros.data());
    }
}

} // namespace

GLuint texture(GLenum internal, int w, int h, GLenum filter, GLenum wrapS, GLenum wrapT) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexStorage2D(GL_TEXTURE_2D, 1, internal, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapS);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapT);
    glBindTexture(GL_TEXTURE_2D, 0);
    s_live++;
    clearTexture(t, internal, w, h);
    return t;
}

void destroyTexture(GLuint& tex) {
    if (!tex)
        return;
    glDeleteTextures(1, &tex);
    tex = 0;
    s_live--;
}

void upload(GLuint tex, int w, int h, GLenum format, GLenum type, const void* pixels) {
    CUnpackState unpack;
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, format, type, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
}

GLuint framebuffer(std::initializer_list<GLuint> attachments) {
    GLuint fb = 0;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    std::vector<GLenum> bufs;
    for (GLuint tex : attachments) {
        const GLenum at = GL_COLOR_ATTACHMENT0 + bufs.size();
        glFramebufferTexture2D(GL_FRAMEBUFFER, at, GL_TEXTURE_2D, tex, 0);
        bufs.push_back(at);
    }
    glDrawBuffers(bufs.size(), bufs.data());
    const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!ok) {
        glDeleteFramebuffers(1, &fb);
        return 0;
    }
    s_live++;
    return fb;
}

void destroyFramebuffer(GLuint& fb) {
    if (!fb)
        return;
    glDeleteFramebuffers(1, &fb);
    fb = 0;
    s_live--;
}

void use(const SProgram& p) {
    glUseProgram(p.id);
}

void bind(int unit, GLuint tex) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, tex);
}

void pass(const SProgram& p, GLuint fbo, int x, int y, int w, int h, const std::array<GLfloat, 9>& proj) {
    glUseProgram(p.id);
    glUniformMatrix3fv(p.proj, 1, GL_FALSE, proj.data());
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(x, y, w, h);
    glBindVertexArray(s_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

int liveObjects() {
    return s_live;
}

bool hasExtension(std::string_view name) {
    const auto* list = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (!list)
        return false;
    for (std::string_view rest = list; !rest.empty();) {
        const auto sp = rest.find(' ');
        if (rest.substr(0, sp) == name)
            return true;
        rest = sp == std::string_view::npos ? std::string_view{} : rest.substr(sp + 1);
    }
    return false;
}

// ── pixel-store state ───────────────────────────────────────────────────────────────

CPackState::CPackState(GLuint buffer) {
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &m_buffer);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &m_rowLength);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &m_skipPixels);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &m_skipRows);
    glGetIntegerv(GL_PACK_ALIGNMENT, &m_alignment);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
}

CPackState::~CPackState() {
    glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(m_buffer));
    glPixelStorei(GL_PACK_ROW_LENGTH, m_rowLength);
    glPixelStorei(GL_PACK_SKIP_PIXELS, m_skipPixels);
    glPixelStorei(GL_PACK_SKIP_ROWS, m_skipRows);
    glPixelStorei(GL_PACK_ALIGNMENT, m_alignment);
}

CUnpackState::CUnpackState(GLuint buffer) {
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &m_buffer);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &m_rowLength);
    glGetIntegerv(GL_UNPACK_IMAGE_HEIGHT, &m_imageHeight);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &m_skipPixels);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &m_skipRows);
    glGetIntegerv(GL_UNPACK_SKIP_IMAGES, &m_skipImages);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &m_alignment);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
}

CUnpackState::~CUnpackState() {
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(m_buffer));
    glPixelStorei(GL_UNPACK_ROW_LENGTH, m_rowLength);
    glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, m_imageHeight);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, m_skipPixels);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, m_skipRows);
    glPixelStorei(GL_UNPACK_SKIP_IMAGES, m_skipImages);
    glPixelStorei(GL_UNPACK_ALIGNMENT, m_alignment);
}

// ── read-back without waiting ───────────────────────────────────────────────────────

CReadback::~CReadback() {
    release();
}

bool CReadback::start(std::initializer_list<SRegion> regions) {
    if (m_fence)
        return false;
    size_t bytes = 0;
    for (const auto& r : regions)
        bytes += static_cast<size_t>(r.w) * r.h * 4 * sizeof(float);
    if (!bytes)
        return false;
    if (!m_buffer) {
        glGenBuffers(1, &m_buffer);
        if (!m_buffer)
            return false;
        s_live++;
        m_capacity = 0;
    }
    CPackState pack(m_buffer);
    if (bytes > m_capacity) {
        glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(bytes), nullptr, GL_STREAM_READ);
        m_capacity = bytes;
    }
    GLint readFb = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFb);
    size_t offset = 0;
    for (const auto& r : regions) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, r.fb);
        glReadBuffer(GL_COLOR_ATTACHMENT0 + r.attachment);
        glReadPixels(r.x, r.y, r.w, r.h, GL_RGBA, GL_FLOAT, reinterpret_cast<void*>(offset));
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        offset += static_cast<size_t>(r.w) * r.h * 4 * sizeof(float);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFb));
    m_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (!m_fence)
        return false;
    s_live++;
    m_bytes   = bytes;
    m_flushed = false;
    return true;
}

bool CReadback::take(std::vector<float>& out, bool wait) {
    if (!m_fence)
        return false;
    // the first poll flushes (the commands may still sit in the driver's queue); a wait
    // gives up after 5 s (a hung GPU) and drops the read
    const GLbitfield flags  = wait || !m_flushed ? GL_SYNC_FLUSH_COMMANDS_BIT : 0;
    m_flushed               = true;
    const GLenum     status = glClientWaitSync(m_fence, flags, wait ? 5'000'000'000ull : 0);
    if (status == GL_TIMEOUT_EXPIRED && !wait)
        return false;
    const bool signalled = status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED;
    cancel();
    if (!signalled)
        return false;
    CPackState  pack(m_buffer);
    const void* p = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(m_bytes), GL_MAP_READ_BIT);
    if (!p)
        return false;
    out.resize(m_bytes / sizeof(float));
    std::memcpy(out.data(), p, m_bytes);
    // GL_FALSE: the store was corrupted while mapped (a VT switch, a GPU reset); what was
    // copied is not the read, so there is no result (the next check reads again)
    if (glUnmapBuffer(GL_PIXEL_PACK_BUFFER) != GL_TRUE)
        return false;
    return true;
}

void CReadback::cancel() {
    if (!m_fence)
        return;
    glDeleteSync(m_fence);
    m_fence = nullptr;
    s_live--;
}

void CReadback::release() {
    cancel();
    if (m_buffer) {
        glDeleteBuffers(1, &m_buffer);
        m_buffer = 0;
        s_live--;
    }
    m_capacity = m_bytes = 0;
}

// ── GPU timing ──────────────────────────────────────────────────────────────────────

namespace {

PFNGLGETQUERYOBJECTUI64VEXTPROC s_queryResult64 = nullptr;
bool                            s_timerChecked  = false;
// Bumped by whichever timer reads GL_GPU_DISJOINT_EXT set (the read clears it): every
// query begun before the bump is dropped by every timer, not only by that one.
unsigned s_disjointEpoch = 0;

} // namespace

bool timerQueries() {
    if (!s_timerChecked) {
        s_timerChecked  = true;
        const auto* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        if (ext && std::string_view(ext).find("GL_EXT_disjoint_timer_query") != std::string_view::npos)
            s_queryResult64 = reinterpret_cast<PFNGLGETQUERYOBJECTUI64VEXTPROC>(eglGetProcAddress("glGetQueryObjectui64vEXT"));
    }
    return s_queryResult64 != nullptr;
}

GLuint query() {
    GLuint q = 0;
    glGenQueries(1, &q);
    if (q)
        s_live++;
    return q;
}

void destroyQuery(GLuint& q) {
    if (!q)
        return;
    glDeleteQueries(1, &q);
    q = 0;
    s_live--;
}

CGpuTimer::CGpuTimer(int slots) : m_slots(static_cast<size_t>(std::max(1, slots))) {}

CGpuTimer::~CGpuTimer() {
    release();
}

bool CGpuTimer::begin() {
    m_open = -1;
    if (!timerQueries())
        return false;
    SSlot& slot = m_slots[m_next];
    if (slot.pending)
        return false;
    // only one time-elapsed query may be open in the context
    GLint current = 0;
    glGetQueryiv(GL_TIME_ELAPSED_EXT, GL_CURRENT_QUERY, &current);
    if (current != 0)
        return false;
    if (!slot.id && !(slot.id = query()))
        return false;
    glBeginQuery(GL_TIME_ELAPSED_EXT, slot.id);
    slot.epoch = s_disjointEpoch;
    m_open     = m_next;
    return true;
}

void CGpuTimer::end() {
    if (m_open < 0)
        return;
    glEndQuery(GL_TIME_ELAPSED_EXT);
    m_slots[m_open].pending = true;
    m_slots[m_open].gen     = m_gen;
    m_next                  = (m_open + 1) % static_cast<int>(m_slots.size());
    m_open                  = -1;
}

void CGpuTimer::poll(std::vector<float>& msOut) {
    if (!s_queryResult64)
        return;
    // a disjoint event (clock change, power state) makes every result in flight
    // meaningless, in every timer: the read clears the flag, so it moves the shared epoch
    GLint disjoint = 0;
    glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
    if (disjoint)
        s_disjointEpoch++;
    // oldest first: the ring from the next slot to be used
    const int n = static_cast<int>(m_slots.size());
    for (int k = 0; k < n; k++) {
        SSlot& slot = m_slots[(m_next + k) % n];
        if (!slot.pending)
            continue;
        GLuint available = GL_FALSE;
        glGetQueryObjectuiv(slot.id, GL_QUERY_RESULT_AVAILABLE, &available);
        if (!available)
            continue;
        slot.pending = false;
        GLuint64 ns  = 0;
        s_queryResult64(slot.id, GL_QUERY_RESULT, &ns);
        if (keepResult(slot.gen, m_gen, slot.epoch, s_disjointEpoch))
            msOut.push_back(static_cast<float>(static_cast<double>(ns) / 1e6));
    }
}

void CGpuTimer::discard() {
    m_gen++;
}

void CGpuTimer::release() {
    if (m_open >= 0) {
        glEndQuery(GL_TIME_ELAPSED_EXT);
        m_open = -1;
    }
    for (auto& slot : m_slots) {
        destroyQuery(slot.id);
        slot.pending = false;
    }
    m_next = 0;
}

// ── the picture at a lower resolution ──────────────────────────────────────────────

namespace {

constexpr const char* STRETCH = R"GLSL(
uniform sampler2D img;
in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

void main() {
    fragColor = vec4(texture(img, v_texcoord).rgb, 1.0);
}
)GLSL";

} // namespace

int scaledSide(int side, float quality) {
    return std::max(1, static_cast<int>(std::lround(static_cast<double>(side) * quality)));
}

CPictureTarget::~CPictureTarget() {
    release();
}

bool CPictureTarget::ensure(int w, int h, std::string* error) {
    w = std::max(1, w);
    h = std::max(1, h);
    if (m_fbo && w == m_w && h == m_h)
        return true;
    // a size that failed is not made again every frame: only another size tries again
    if (!m_fbo && w == m_failedW && h == m_failedH) {
        if (error)
            *error = m_failure;
        return false;
    }
    m_failedW = w;
    m_failedH = h;
    if (!m_program.id) {
        auto program = compile(STRETCH, &m_failure);
        if (!program) {
            if (error)
                *error = m_failure;
            return false;
        }
        m_program = *program;
        m_imgLoc  = m_program.uniform("img");
    }
    destroyFramebuffer(m_fbo);
    destroyTexture(m_tex);
    m_w = m_h = 0;
    m_tex     = texture(GL_RGBA8, w, h, GL_LINEAR);
    m_fbo     = framebuffer({m_tex});
    if (!m_fbo) {
        destroyTexture(m_tex);
        m_failure = std::format("the {}x{} picture target is incomplete", w, h);
        if (error)
            *error = m_failure;
        return false;
    }
    m_w       = w;
    m_h       = h;
    m_failedW = m_failedH = 0;
    m_failure.clear();
    return true;
}

void CPictureTarget::stretch(GLuint fbo, int x, int y, int w, int h, const std::array<GLfloat, 9>& proj) {
    if (!m_fbo)
        return;
    use(m_program);
    bind(0, m_tex);
    glUniform1i(m_imgLoc, 0);
    pass(m_program, fbo, x, y, w, h, proj);
}

void CPictureTarget::release() {
    destroyFramebuffer(m_fbo);
    destroyTexture(m_tex);
    destroy(m_program);
    m_imgLoc = -1;
    m_w = m_h = 0;
    // the failed size is kept: a caller releases right after a failed ensure() (drawing
    // at full size), and the next frame must not try that size again
}

CStateGuard::CStateGuard() {
    glGetIntegerv(GL_CURRENT_PROGRAM, &m_program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &m_vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &m_active);
    for (int u = 0; u < UNITS; u++) {
        glActiveTexture(GL_TEXTURE0 + u);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &m_tex[u]);
    }
    glActiveTexture(m_active);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &m_drawFb);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &m_readFb);
    glGetIntegerv(GL_VIEWPORT, m_viewport);
    glGetIntegerv(GL_SCISSOR_BOX, m_scissorBox);
    m_scissor = glIsEnabled(GL_SCISSOR_TEST);
    m_blend   = glIsEnabled(GL_BLEND);
}

CStateGuard::~CStateGuard() {
    glUseProgram(m_program);
    glBindVertexArray(m_vao);
    for (int u = 0; u < UNITS; u++) {
        glActiveTexture(GL_TEXTURE0 + u);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_tex[u]));
    }
    glActiveTexture(m_active);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_drawFb);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_readFb);
    glViewport(m_viewport[0], m_viewport[1], m_viewport[2], m_viewport[3]);
    glScissor(m_scissorBox[0], m_scissorBox[1], m_scissorBox[2], m_scissorBox[3]);
    if (m_scissor)
        glEnable(GL_SCISSOR_TEST);
    else
        glDisable(GL_SCISSOR_TEST);
    if (m_blend)
        glEnable(GL_BLEND);
    else
        glDisable(GL_BLEND);
}

} // namespace Gl
