#pragma once

// A whole-lattice reduction on the GPU, for the health checks: per cell the look's GLSL
// gives four values to take the largest of, three to sum, and four that must be finite.
// Passes over 8x8 tiles bring them down to one texel, which Gl::CReadback reads into a
// pixel-pack buffer behind a fence: 32 bytes, and the render thread never waits for the
// GPU (take(false) polls on later frames). Min is the largest of the negated value.
//
// The look's snippet declares its samplers (bound to units 0, 1, ... in the order the
// names are given) and
//     void cell(ivec2 c, out vec4 mx, out vec3 sm, out vec4 chk);
// A cell whose mx, sm or chk holds a NaN or an infinity is counted as non-finite and
// left out of the max and the sums.
//
// start() binds its own programs, textures, framebuffers and viewport; the caller saves
// and restores them (Gl::CStateGuard) and has the scissor test and blending off.

#include "gl/Gl.hpp"

#include <array>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

struct SReduced {
    std::array<float, 4> max{};
    std::array<float, 3> sum{};
    float                nonFinite = 0; // cells left out for a NaN or an infinity
    bool                 finite() const;  // no such cell, and every max and sum finite
};

class CReduction {
  public:
    CReduction() = default;
    ~CReduction();
    CReduction(const CReduction&)            = delete;
    CReduction& operator=(const CReduction&) = delete;

    // Compiles the passes (once; again only after release()). False + *error when the
    // driver rejects them.
    bool build(std::string_view cellGlsl, std::initializer_list<const char*> samplers, std::string* error);
    bool built() const { return m_first.id != 0; }
    // Runs the passes over a w x h lattice with these textures on the samplers and
    // queues the read. False (nothing done) while a read is in flight or unbuilt.
    bool start(std::initializer_list<GLuint> inputs, int w, int h);
    // The result once it is there; wait: block until it is.
    bool take(SReduced& out, bool wait);
    bool pending() const { return m_read.pending(); }
    void cancel() { m_read.cancel(); }
    void release();

    static constexpr int TILE = 8;

  private:
    struct SLevel {
        int    w = 0, h = 0;
        GLuint max = 0, sum = 0, fb = 0;
    };
    bool ensureLevels(int w, int h);
    void releaseLevels();

    Gl::SProgram       m_first, m_combine;
    std::vector<GLint> m_samplerLocs;
    GLint              m_firstSize = -1, m_combineSize = -1, m_combineMax = -1, m_combineSum = -1;
    std::vector<SLevel> m_levels;
    int                m_w = 0, m_h = 0;
    Gl::CReadback      m_read;
    std::vector<float> m_px;
};
