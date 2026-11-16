#pragma once

// The shared D2Q9 lattice-Boltzmann fluid of the looks (pattern, embers, marble,
// jupiter): BGK collision, forcing by the velocity-shift method, ported from mkLBM in
// reference/b7-sketch.html. Periodic in x; in y either periodic or closed by
// bounce-back walls above the first and below the last row.
//
// Lattice layout, one ping-pong pair of four colour attachments written by one draw:
//   0  RGBA32F  (f0, f1, f2, f3)
//   1  RGBA32F  (f4, f5, f6, f7)
//   2  RGBA32F  (f8, ux, uy, rho)    as the sketch's F2: stats() reads only this one
//   3  RG32F    (ux, uy)             velocity(): what the fields and the looks sample
// Directions: CX = (0, 1, 0, -1, 0, 1, -1, -1, 1), CY = (0, 0, 1, 0, -1, 1, 1, -1, -1),
// y grows with the row index (row 0 the top, as in Fields.hpp). The velocity written
// out is the half-force-corrected u + F / (2 rho), lattice cells per step.
//
// Texture units: step() binds units 0..2 (the lattice); a force that samples its own
// textures takes unit 3 and up, bound by the caller before step(). Uniform names the
// step owns (a force must not declare them): F0, F1, F2, size, omega. The force snippet
// can use pdelta, bilinear, bicubic (Fields.hpp) and CX, CY, W, feq.

#include "gl/Gl.hpp"

#include <string>

// forceGlsl: `vec2 force(ivec2 c, vec2 u, float rho)` + its uniforms: the body force on
// cell c given its velocity and density, in lattice units. Empty = no force.
struct SFluidSpec {
    int         w, h;
    bool        wallsY;
    float       omega;
    std::string forceGlsl;
};

class CFluid {
  public:
    CFluid() = default;
    ~CFluid();
    CFluid(const CFluid&)            = delete;
    CFluid& operator=(const CFluid&) = delete;

    bool                build(const SFluidSpec&, std::string* error); // (re)makes everything, then init()
    void                init();                                       // rest state, rho 1
    const Gl::SProgram& program() const;                              // the step program: set force uniforms on it before step()
    void                step();                                       // one LBM step
    GLuint              velocity() const;                             // RG32F (ux, uy), same size as the lattice, valid after step()
    GLuint              state() const { return m_tex[m_cur][2]; }     // RGBA32F (f8, ux, uy, rho): for a health check's reduction
    struct SStats {
        float umax, umean, rhoMin, rhoMax;
        bool  bad;         // any NaN/Inf, or umax > 0.4, or rhoMin <= 0
        float rhoMean = 0; // the mean density (mass per cell: 1 at rest)
    };
    // A synchronous glReadPixels of the whole velocity/density, summed in doubles: for
    // the bench and tests (exact means), never on the plugin's render path.
    SStats stats();
    int    width() const { return m_spec.w; }
    int    height() const { return m_spec.h; }

  private:
    void release();

    SFluidSpec   m_spec{};
    GLuint       m_tex[2][4] = {};
    GLuint       m_fb[2]     = {};
    int          m_cur       = 0;
    Gl::SProgram m_step;
    GLint        m_uF[3] = {-1, -1, -1}, m_uSize = -1, m_uOmega = -1;
};
