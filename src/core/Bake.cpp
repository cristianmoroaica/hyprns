#include "core/Bake.hpp"

#include <algorithm>
#include <format>
#include <utility>

CBakeSchedule::SStrip CBakeSchedule::strip(int k, int rows, int framesPerKey) {
    const long n = std::max(1, framesPerKey), r = std::max(0, rows), kk = std::clamp<long>(k, 0, n - 1);
    return {static_cast<int>(kk * r / n), static_cast<int>((kk + 1) * r / n)};
}

void CBakeSchedule::reset(int rows, int framesPerKey) {
    m_rows   = std::max(0, rows);
    m_frames = std::max(1, framesPerKey);
    m_frame  = 0;
}

bool CBakeSchedule::advance(SStrip* out) {
    if (out)
        *out = strip(m_frame, m_rows, m_frames);
    if (++m_frame < m_frames)
        return false;
    m_frame = 0;
    return true;
}

CBake::~CBake() {
    release();
}

void CBake::releaseKeys() {
    for (auto& fb : m_fb)
        Gl::destroyFramebuffer(fb);
    for (auto& tex : m_tex)
        Gl::destroyTexture(tex);
    m_w = m_h = 0;
    m_primed  = 0;
    m_baking  = -1;
    m_rotated = false;
}

void CBake::releaseHeld() {
    for (auto& tex : m_held)
        Gl::destroyTexture(tex);
}

void CBake::release() {
    releaseKeys();
    releaseHeld();
}

bool CBake::makeKeys(int w, int h, GLenum internal, int framesPerKey, std::string* error) {
    if (w < 1 || h < 1) {
        if (error)
            *error = std::format("a bake of {}x{} pixels", w, h);
        return false;
    }
    for (int i = 0; i < 3; i++) {
        m_tex[i] = Gl::texture(internal, w, h, GL_LINEAR);
        m_fb[i]  = Gl::framebuffer({m_tex[i]});
        if (!m_fb[i]) {
            releaseKeys();
            if (error)
                *error = std::format("the {}x{} bake framebuffers (format 0x{:x}) are incomplete", w, h, internal);
            return false;
        }
        const GLfloat zero[4] = {0, 0, 0, 0};
        glBindFramebuffer(GL_FRAMEBUFFER, m_fb[i]);
        glClearBufferfv(GL_COLOR, 0, zero);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    m_w        = w;
    m_h        = h;
    m_internal = internal;
    m_frames   = std::max(1, framesPerKey);
    restartKeys();
    return true;
}

bool CBake::build(int w, int h, GLenum internal, int framesPerKey, std::string* error) {
    release();
    return makeKeys(w, h, internal, framesPerKey, error);
}

bool CBake::rebuild(int w, int h, GLenum internal, int framesPerKey, std::string* error) {
    if (ready()) {
        // the shown pair stays: its textures move out of the set (their framebuffers and
        // back go with the rest of the old keys); an older held pair cannot be shown now
        releaseHeld();
        m_held[0]     = std::exchange(m_tex[m_prev], 0);
        m_held[1]     = std::exchange(m_tex[m_next], 0);
        m_heldBlend   = m_schedule.blend();
        m_heldMap     = m_map;
    }
    // otherwise: half-primed keys (or none) go, a held pair stays shown
    releaseKeys();
    return makeKeys(w, h, internal, framesPerKey, error);
}

void CBake::restartKeys() {
    m_prev    = 0;
    m_next    = 1;
    m_back    = 2;
    m_primed  = 0;
    m_rotated = false;
    m_schedule.reset(m_h, m_frames);
}

void CBake::restart() {
    releaseHeld();
    restartKeys();
}

void CBake::setPrimeRows(int rows) {
    m_primeRows = rows;
}

void CBake::advance(const FBakeStrip& bakeStrip) {
    m_rotated = false;
    if (!m_h)
        return;
    if (!ready()) {
        // previous is rows [0, h) of the prime, next rows [h, 2h)
        const int total = 2 * m_h, end = m_primeRows > 0 ? std::min(total, m_primed + m_primeRows) : total;
        while (m_primed < end) {
            const int key = m_primed < m_h ? 0 : 1, base = key * m_h;
            const int y0 = m_primed - base, y1 = std::min(end - base, m_h);
            m_baking     = key;
            bakeStrip(m_fb[key == 0 ? m_prev : m_next], y0, y1);
            m_primed = base + y1;
        }
        m_baking = -1;
        if (ready()) {
            m_schedule.reset(m_h, m_frames);
            releaseHeld(); // the new pair is whole: shown from this frame's draw
        }
        return;
    }
    CBakeSchedule::SStrip s{};
    const bool            done = m_schedule.advance(&s);
    if (s.y1 > s.y0) {
        m_baking = 2;
        bakeStrip(m_fb[m_back], s.y0, s.y1);
        m_baking = -1;
    }
    if (done) {
        const int old = m_prev;
        m_prev        = m_next;
        m_next        = m_back;
        m_back        = old;
        m_rotated     = true;
    }
}

void CKeyClock::requestPrime(double from) {
    m_pending = true;
    m_from    = from;
}

void CKeyClock::requestPrimeFromShown(bool whole, float blend) {
    if (!m_pending)
        requestPrime(shown(whole, blend));
}

void CKeyClock::frame(double step, double nominal, bool ready, bool starting, int steps) {
    if (m_pending) {
        m_prev       = m_from >= 0.0 ? m_from : step;
        m_next       = m_prev + nominal;
        m_back       = m_next;
        m_period     = 0;
        m_havePeriod = false;
        m_pending    = false;
    }
    if (starting) {
        m_back       = m_next + (m_havePeriod ? m_period : nominal);
        m_period     = 0;
        m_havePeriod = true;
    }
    if (ready)
        m_period += steps;
}

void CKeyClock::rotated() {
    m_prev = m_next;
    m_next = m_back;
}
