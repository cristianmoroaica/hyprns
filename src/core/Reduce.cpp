#include "core/Reduce.hpp"

#include <cmath>
#include <format>

namespace {

constexpr const char* FIRST_MAIN = R"GLSL(
uniform ivec2 size; // the lattice
layout(location = 0) out vec4 oMax;
layout(location = 1) out vec4 oSum; // the three sums, and the cells left out
// NaN or Inf from the bits (all exponent bits set): isnan/isinf may be folded to false
// by a driver that assumes finite arithmetic
bool notFinite(vec4 v) {
    uvec4 e = floatBitsToUint(v) & uvec4(0x7f800000u);
    return any(equal(e, uvec4(0x7f800000u)));
}
void main() {
    ivec2 t  = ivec2(gl_FragCoord.xy) * 8;
    vec4  mx = vec4(-3.0e38);
    vec3  sm = vec3(0.0);
    float nb = 0.0;
    for (int j = 0; j < 8; j++)
        for (int i = 0; i < 8; i++) {
            ivec2 c = t + ivec2(i, j);
            if (c.x >= size.x || c.y >= size.y)
                continue;
            vec4 a;
            vec3 b;
            vec4 k;
            cell(c, a, b, k);
            if (notFinite(a) || notFinite(vec4(b, 0.0)) || notFinite(k)) {
                nb += 1.0;
                continue;
            }
            mx = max(mx, a);
            sm += b;
        }
    oMax = mx;
    oSum = vec4(sm, nb);
}
)GLSL";

constexpr const char* COMBINE = R"GLSL(
uniform sampler2D inMax;
uniform sampler2D inSum;
uniform ivec2 size; // the level read
layout(location = 0) out vec4 oMax;
layout(location = 1) out vec4 oSum;
void main() {
    ivec2 t  = ivec2(gl_FragCoord.xy) * 8;
    vec4  mx = vec4(-3.0e38);
    vec4  sm = vec4(0.0);
    for (int j = 0; j < 8; j++)
        for (int i = 0; i < 8; i++) {
            ivec2 c = t + ivec2(i, j);
            if (c.x >= size.x || c.y >= size.y)
                continue;
            mx = max(mx, texelFetch(inMax, c, 0));
            sm += texelFetch(inSum, c, 0);
        }
    oMax = mx;
    oSum = sm;
}
)GLSL";

int tiles(int n) {
    return (n + CReduction::TILE - 1) / CReduction::TILE;
}

} // namespace

bool SReduced::finite() const {
    if (nonFinite != 0.0f || !std::isfinite(nonFinite))
        return false;
    for (float v : max)
        if (!std::isfinite(v))
            return false;
    for (float v : sum)
        if (!std::isfinite(v))
            return false;
    return true;
}

CReduction::~CReduction() {
    release();
}

bool CReduction::build(std::string_view cellGlsl, std::initializer_list<const char*> samplers, std::string* error) {
    if (built())
        return true;
    std::string log;
    auto        first = Gl::compile(std::string(cellGlsl) + FIRST_MAIN, &log);
    if (!first) {
        if (error)
            *error = std::format("the health check's reduction failed to compile: {}", log);
        return false;
    }
    auto combine = Gl::compile(COMBINE, &log);
    if (!combine) {
        Gl::destroy(*first);
        if (error)
            *error = std::format("the health check's reduction failed to compile: {}", log);
        return false;
    }
    m_first   = *first;
    m_combine = *combine;
    m_samplerLocs.clear();
    for (const char* name : samplers)
        m_samplerLocs.push_back(m_first.uniform(name));
    m_firstSize   = m_first.uniform("size");
    m_combineSize = m_combine.uniform("size");
    m_combineMax  = m_combine.uniform("inMax");
    m_combineSum  = m_combine.uniform("inSum");
    return true;
}

bool CReduction::ensureLevels(int w, int h) {
    if (!m_levels.empty() && w == m_w && h == m_h)
        return true;
    releaseLevels();
    int lw = w, lh = h;
    do {
        lw = tiles(lw);
        lh = tiles(lh);
        SLevel l;
        l.w   = lw;
        l.h   = lh;
        l.max = Gl::texture(GL_RGBA32F, lw, lh, GL_NEAREST);
        l.sum = Gl::texture(GL_RGBA32F, lw, lh, GL_NEAREST);
        l.fb  = Gl::framebuffer({l.max, l.sum});
        m_levels.push_back(l);
        if (!l.fb) {
            releaseLevels();
            return false;
        }
    } while (lw > 1 || lh > 1);
    m_w = w;
    m_h = h;
    return true;
}

void CReduction::releaseLevels() {
    for (auto& l : m_levels) {
        Gl::destroyFramebuffer(l.fb);
        Gl::destroyTexture(l.max);
        Gl::destroyTexture(l.sum);
    }
    m_levels.clear();
    m_w = m_h = 0;
}

bool CReduction::start(std::initializer_list<GLuint> inputs, int w, int h) {
    if (!built() || m_read.pending() || w < 1 || h < 1 || !ensureLevels(w, h))
        return false;
    Gl::use(m_first);
    int unit = 0;
    for (GLuint tex : inputs) {
        Gl::bind(unit, tex);
        if (static_cast<size_t>(unit) < m_samplerLocs.size())
            glUniform1i(m_samplerLocs[static_cast<size_t>(unit)], unit);
        unit++;
    }
    glUniform2i(m_firstSize, w, h);
    Gl::pass(m_first, m_levels[0].fb, 0, 0, m_levels[0].w, m_levels[0].h);
    for (size_t i = 1; i < m_levels.size(); i++) {
        const SLevel& in = m_levels[i - 1];
        Gl::use(m_combine);
        Gl::bind(0, in.max);
        Gl::bind(1, in.sum);
        glUniform1i(m_combineMax, 0);
        glUniform1i(m_combineSum, 1);
        glUniform2i(m_combineSize, in.w, in.h);
        Gl::pass(m_combine, m_levels[i].fb, 0, 0, m_levels[i].w, m_levels[i].h);
    }
    const GLuint fb = m_levels.back().fb;
    return m_read.start({{fb, 0, 0, 0, 1, 1}, {fb, 1, 0, 0, 1, 1}});
}

bool CReduction::take(SReduced& out, bool wait) {
    if (!m_read.take(m_px, wait) || m_px.size() < 8)
        return false;
    for (int k = 0; k < 4; k++)
        out.max[static_cast<size_t>(k)] = m_px[static_cast<size_t>(k)];
    for (int k = 0; k < 3; k++)
        out.sum[static_cast<size_t>(k)] = m_px[static_cast<size_t>(4 + k)];
    out.nonFinite = m_px[7];
    return true;
}

void CReduction::release() {
    m_read.release();
    releaseLevels();
    Gl::destroy(m_first);
    Gl::destroy(m_combine);
    m_samplerLocs.clear();
}
