#include "core/Fields.hpp"

#include <format>
#include <string_view>

namespace {

// Channels a field format stores, and how load() hands them to glTexSubImage2D.
struct SFormat {
    int    channels = 0;
    GLenum format   = 0;
};

SFormat formatOf(GLenum internal) {
    switch (internal) {
        case GL_R32F:
        case GL_R16F: return {1, GL_RED};
        case GL_RG32F:
        case GL_RG16F: return {2, GL_RG};
        case GL_RGBA32F:
        case GL_RGBA16F: return {4, GL_RGBA};
        default: return {};
    }
}

// What the advection and the MacCormack pass share: the field size, the field as it was
// before this step (fieldSrc, which a post may read too), the velocity and its sampling,
// the GLSL helpers.
std::string prelude(bool wrapY) {
    return std::format(R"GLSL(
const bool WRAPY = {};
uniform ivec2 size;
uniform sampler2D fieldSrc;
uniform sampler2D fieldVel;
uniform ivec2 velSize;
uniform float fieldDt;
layout(location = 0) out vec4 o;
)GLSL",
                       wrapY ? "true" : "false") +
        GLSL_PDELTA + GLSL_BILINEAR + GLSL_BICUBIC + R"GLSL(
// field cells the content of cell c moves per step() (dt included): the velocity at the
// cell's centre, by bilinear between lattice cell centres when the sizes differ
vec2 displacement(ivec2 c) {
    if (velSize == size)
        return texelFetch(fieldVel, c, 0).rg * fieldDt;
    vec2 scale = vec2(size) / vec2(velSize);
    return bilinear(fieldVel, (vec2(c) + 0.5) / scale, WRAPY).rg * scale * fieldDt;
}
)GLSL";
}

// One semi-Lagrangian trace: the field at the departure point, fieldDir = 1 back along
// the velocity (forward step), -1 forward along it (backward step).
constexpr const char* ADVECT = R"GLSL(
uniform float fieldDir;
void main() {
    ivec2 c = ivec2(gl_FragCoord.xy);
    o = bilinear(fieldSrc, vec2(c) + 0.5 - fieldDir * displacement(c), WRAPY);
}
)GLSL";

// MacCormack: forward + half the error of the round trip, clamped to the four cells
// around the departure point (no new extrema), then the field's own post().
constexpr const char* MACCORMACK = R"GLSL(
uniform sampler2D fieldHat;
uniform sampler2D fieldBar;
void main() {
    ivec2 c = ivec2(gl_FragCoord.xy);
    vec2  p = vec2(c) + 0.5 - displacement(c);
    ivec4 k = cellPair(ivec2(floor(p - 0.5)), size, WRAPY);
    vec4 a0 = texelFetch(fieldSrc, k.xz, 0);
    vec4 a1 = texelFetch(fieldSrc, k.yz, 0);
    vec4 a2 = texelFetch(fieldSrc, k.xw, 0);
    vec4 a3 = texelFetch(fieldSrc, k.yw, 0);
    vec4 v  = texelFetch(fieldHat, c, 0) + 0.5 * (texelFetch(fieldSrc, c, 0) - texelFetch(fieldBar, c, 0));
    v       = clamp(v, min(min(a0, a1), min(a2, a3)), max(max(a0, a1), max(a2, a3)));
    o       = post(c, v);
}
)GLSL";

constexpr const char* IDENTITY_POST = "vec4 post(ivec2 c, vec4 v) { return v; }\n";

} // namespace

bool floatTargetsAvailable(std::string* error) {
    if (Gl::hasExtension("GL_EXT_color_buffer_float"))
        return true;
    if (error)
        *error = "GL_EXT_color_buffer_float is missing: this GPU cannot render to float textures (R32F, RG32F, RGBA32F)";
    return false;
}

CField::~CField() {
    release();
}

void CField::release() {
    for (auto& fb : m_fb)
        Gl::destroyFramebuffer(fb);
    Gl::destroyFramebuffer(m_fbHat);
    Gl::destroyFramebuffer(m_fbBar);
    for (auto& tex : m_tex)
        Gl::destroyTexture(tex);
    Gl::destroyTexture(m_hat);
    Gl::destroyTexture(m_bar);
    Gl::destroy(m_advect);
    Gl::destroy(m_mac);
    m_cur = 0;
}

bool CField::build(const SFieldSpec& spec, std::string* error) {
    release();
    m_spec = spec;
    if (spec.w < 1 || spec.h < 1) {
        if (error)
            *error = std::format("a field of {}x{} cells", spec.w, spec.h);
        return false;
    }
    if (!formatOf(spec.internal).channels) {
        if (error)
            *error = std::format("a field cannot be of internal format 0x{:x} (R/RG/RGBA, 32F or 16F)", spec.internal);
        return false;
    }
    if (!floatTargetsAvailable(error))
        return false;

    const std::string head = prelude(spec.wrapY);
    std::string       log;
    auto              advect = Gl::compile(head + ADVECT, &log);
    if (!advect) {
        if (error)
            *error = std::format("the field's advection shader failed to compile: {}", log);
        return false;
    }
    m_advect = *advect;
    auto mac = Gl::compile(head + (spec.postGlsl.empty() ? std::string(IDENTITY_POST) : spec.postGlsl) + MACCORMACK, &log);
    if (!mac) {
        if (error)
            *error = std::format("the field's MacCormack shader (with its post) failed to compile: {}", log);
        release();
        return false;
    }
    m_mac = *mac;

    for (auto* u : {&m_advU, &m_macU}) {
        const auto& p = u == &m_advU ? m_advect : m_mac;
        u->src        = p.uniform("fieldSrc");
        u->hat        = p.uniform("fieldHat");
        u->bar        = p.uniform("fieldBar");
        u->vel        = p.uniform("fieldVel");
        u->size       = p.uniform("size");
        u->velSize    = p.uniform("velSize");
        u->dt         = p.uniform("fieldDt");
        u->dir        = p.uniform("fieldDir");
    }

    for (auto* tex : {&m_tex[0], &m_tex[1], &m_hat, &m_bar})
        *tex = Gl::texture(spec.internal, spec.w, spec.h, GL_NEAREST);
    m_fb[0] = Gl::framebuffer({m_tex[0]});
    m_fb[1] = Gl::framebuffer({m_tex[1]});
    m_fbHat = Gl::framebuffer({m_hat});
    m_fbBar = Gl::framebuffer({m_bar});
    if (!m_fb[0] || !m_fb[1] || !m_fbHat || !m_fbBar) {
        if (error)
            *error = std::format("the field's {}x{} framebuffers (format 0x{:x}) are incomplete", spec.w, spec.h, spec.internal);
        release();
        return false;
    }
    const GLfloat zero[4] = {0, 0, 0, 0};
    for (GLuint fb : {m_fb[0], m_fb[1], m_fbHat, m_fbBar}) {
        glBindFramebuffer(GL_FRAMEBUFFER, fb);
        glClearBufferfv(GL_COLOR, 0, zero);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

void CField::load(const std::vector<float>& rgba) {
    const SFormat f = formatOf(m_spec.internal);
    const size_t  n = static_cast<size_t>(m_spec.w) * m_spec.h;
    if (!m_tex[m_cur] || rgba.size() != n * 4)
        return;
    if (f.channels == 4) {
        Gl::upload(m_tex[m_cur], m_spec.w, m_spec.h, f.format, GL_FLOAT, rgba.data());
        return;
    }
    std::vector<float> packed(n * f.channels);
    for (size_t i = 0; i < n; i++)
        for (int k = 0; k < f.channels; k++)
            packed[i * f.channels + k] = rgba[i * 4 + k];
    Gl::upload(m_tex[m_cur], m_spec.w, m_spec.h, f.format, GL_FLOAT, packed.data());
}

const Gl::SProgram& CField::program() const {
    return m_mac;
}

GLuint CField::texture() const {
    return m_tex[m_cur];
}

void CField::step(GLuint velocity, int velW, int velH, float dt) {
    if (!m_mac.id || !velocity || velW < 1 || velH < 1)
        return;
    const int  w = m_spec.w, h = m_spec.h, nxt = 1 - m_cur;
    const auto common = [&](const SUniforms& u, int velUnit) {
        glUniform1i(u.vel, velUnit);
        glUniform2i(u.size, w, h);
        glUniform2i(u.velSize, velW, velH);
        glUniform1f(u.dt, dt);
    };

    // forward: the field at the departure point
    Gl::use(m_advect);
    Gl::bind(0, m_tex[m_cur]);
    Gl::bind(1, velocity);
    glUniform1i(m_advU.src, 0);
    glUniform1f(m_advU.dir, 1.0f);
    common(m_advU, 1);
    Gl::pass(m_advect, m_fbHat, 0, 0, w, h);

    // backward: the forward result carried back again
    Gl::bind(0, m_hat);
    glUniform1f(m_advU.dir, -1.0f);
    Gl::pass(m_advect, m_fbBar, 0, 0, w, h);

    // corrected, clamped, post
    Gl::use(m_mac);
    Gl::bind(0, m_tex[m_cur]);
    Gl::bind(1, m_hat);
    Gl::bind(2, m_bar);
    Gl::bind(3, velocity);
    glUniform1i(m_macU.src, 0);
    glUniform1i(m_macU.hat, 1);
    glUniform1i(m_macU.bar, 2);
    common(m_macU, 3);
    Gl::pass(m_mac, m_fb[nxt], 0, 0, w, h);
    m_cur = nxt;
}
