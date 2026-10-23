#pragma once
#include "math.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

// Eye-1 replay of a recorded eye-0 render: the engine's view-dependent matrix
// uploads are rewritten from the eye-0 view/projection to the eye-1 ones.
// Engine conventions: views are row-major 3x4 (world -> view, -Z forward),
// projections OpenGL-style 4x4 with near/far only in the z row; the adjusted
// projection AP = A * P adds the API's depth/drawport adjustment.
namespace ss2vr::replay {
using M4 = std::array<double, 16>;

inline M4 m4(const float *rows, unsigned count) noexcept {
    M4 out{};
    for (unsigned i = 0; i < count * 4 && i < 16; ++i) out[i] = rows[i];
    if (count == 3) out[15] = 1;
    return out;
}
inline M4 multiply(const M4 &a, const M4 &b) noexcept {
    M4 out{};
    for (unsigned r = 0; r < 4; ++r)
        for (unsigned c = 0; c < 4; ++c)
            for (unsigned k = 0; k < 4; ++k) out[4*r+c] += a[4*r+k] * b[4*k+c];
    return out;
}
// General 4x4 inverse (cofactor expansion); false when singular.
inline bool invert(const M4 &m, M4 &out) noexcept {
    const double *a = m.data();
    double inv[16];
    inv[0] = a[5]*a[10]*a[15]-a[5]*a[11]*a[14]-a[9]*a[6]*a[15]+a[9]*a[7]*a[14]+a[13]*a[6]*a[11]-a[13]*a[7]*a[10];
    inv[4] = -a[4]*a[10]*a[15]+a[4]*a[11]*a[14]+a[8]*a[6]*a[15]-a[8]*a[7]*a[14]-a[12]*a[6]*a[11]+a[12]*a[7]*a[10];
    inv[8] = a[4]*a[9]*a[15]-a[4]*a[11]*a[13]-a[8]*a[5]*a[15]+a[8]*a[7]*a[13]+a[12]*a[5]*a[11]-a[12]*a[7]*a[9];
    inv[12] = -a[4]*a[9]*a[14]+a[4]*a[10]*a[13]+a[8]*a[5]*a[14]-a[8]*a[6]*a[13]-a[12]*a[5]*a[10]+a[12]*a[6]*a[9];
    inv[1] = -a[1]*a[10]*a[15]+a[1]*a[11]*a[14]+a[9]*a[2]*a[15]-a[9]*a[3]*a[14]-a[13]*a[2]*a[11]+a[13]*a[3]*a[10];
    inv[5] = a[0]*a[10]*a[15]-a[0]*a[11]*a[14]-a[8]*a[2]*a[15]+a[8]*a[3]*a[14]+a[12]*a[2]*a[11]-a[12]*a[3]*a[10];
    inv[9] = -a[0]*a[9]*a[15]+a[0]*a[11]*a[13]+a[8]*a[1]*a[15]-a[8]*a[3]*a[13]-a[12]*a[1]*a[11]+a[12]*a[3]*a[9];
    inv[13] = a[0]*a[9]*a[14]-a[0]*a[10]*a[13]-a[8]*a[1]*a[14]+a[8]*a[2]*a[13]+a[12]*a[1]*a[10]-a[12]*a[2]*a[9];
    inv[2] = a[1]*a[6]*a[15]-a[1]*a[7]*a[14]-a[5]*a[2]*a[15]+a[5]*a[3]*a[14]+a[13]*a[2]*a[7]-a[13]*a[3]*a[6];
    inv[6] = -a[0]*a[6]*a[15]+a[0]*a[7]*a[14]+a[4]*a[2]*a[15]-a[4]*a[3]*a[14]-a[12]*a[2]*a[7]+a[12]*a[3]*a[6];
    inv[10] = a[0]*a[5]*a[15]-a[0]*a[7]*a[13]-a[4]*a[1]*a[15]+a[4]*a[3]*a[13]+a[12]*a[1]*a[7]-a[12]*a[3]*a[5];
    inv[14] = -a[0]*a[5]*a[14]+a[0]*a[6]*a[13]+a[4]*a[1]*a[14]-a[4]*a[2]*a[13]-a[12]*a[1]*a[6]+a[12]*a[2]*a[5];
    inv[3] = -a[1]*a[6]*a[11]+a[1]*a[7]*a[10]+a[5]*a[2]*a[11]-a[5]*a[3]*a[10]-a[9]*a[2]*a[7]+a[9]*a[3]*a[6];
    inv[7] = a[0]*a[6]*a[11]-a[0]*a[7]*a[10]-a[4]*a[2]*a[11]+a[4]*a[3]*a[10]+a[8]*a[2]*a[7]-a[8]*a[3]*a[6];
    inv[11] = -a[0]*a[5]*a[11]+a[0]*a[7]*a[9]+a[4]*a[1]*a[11]-a[4]*a[3]*a[9]-a[8]*a[1]*a[7]+a[8]*a[3]*a[5];
    inv[15] = a[0]*a[5]*a[10]-a[0]*a[6]*a[9]-a[4]*a[1]*a[10]+a[4]*a[2]*a[9]+a[8]*a[1]*a[6]-a[8]*a[2]*a[5];
    const double det = a[0]*inv[0] + a[1]*inv[4] + a[2]*inv[8] + a[3]*inv[12];
    if (!std::isfinite(det) || std::abs(det) < 1e-30) return false;
    for (unsigned i = 0; i < 16; ++i) out[i] = inv[i] / det;
    return true;
}
// Perspective projections carry w = -z_view (bottom row 0,0,-1,0).
inline bool perspective(const M4 &p) noexcept {
    return p[12] == 0 && p[13] == 0 && p[14] < 0 && p[15] == 0;
}
// Clip-space map from the eye-0 frustum to the eye-1 frustum for the same view
// space: x1 = s*x0 + (s*b0 - b1)*w with a = 2/(r-l), b = (r+l)/(r-l) (y alike).
// Applied to any perspective projection, it keeps that projection's own FOV
// scale while moving it by the difference between the two eyes' frusta.
inline M4 frustumShift(Fov eye0, Fov eye1) noexcept {
    const auto axis = [](double lo, double hi, double &a, double &b) {
        const double l = std::tan(lo), r = std::tan(hi);
        a = 2 / (r - l); b = (r + l) / (r - l);
    };
    double ax0, bx0, ax1, bx1, ay0, by0, ay1, by1;
    axis(eye0.left, eye0.right, ax0, bx0); axis(eye1.left, eye1.right, ax1, bx1);
    axis(eye0.down, eye0.up, ay0, by0); axis(eye1.down, eye1.up, ay1, by1);
    const double sx = ax1 / ax0, sy = ay1 / ay0;
    return {sx, 0, 0, sx * bx0 - bx1,
            0, sy, 0, sy * by0 - by1,
            0, 0, 1, 0,
            0, 0, 0, 1};
}
// What one recorded upload was computed from (engine globals at bind time).
struct ViewState {
    std::array<float, 16> adjusted{}, projection{};
    std::array<float, 12> view{};
    bool operator==(const ViewState &o) const noexcept {
        return !std::memcmp(adjusted.data(), o.adjusted.data(), sizeof(adjusted)) &&
               !std::memcmp(projection.data(), o.projection.data(), sizeof(projection)) &&
               !std::memcmp(view.data(), o.view.data(), sizeof(view));
    }
};
// Per-frame eye geometry: eye-0 view space -> eye-1 view space (rigid) and both FOVs.
struct EyeChange {
    M4 view{};           // E, 4x4 rigid
    M4 rotation{};       // E without translation (background/sky views at infinity)
    M4 frustum{};        // T
    bool valid = false;
};
inline EyeChange eyeChange(const Matrix34 &eye0ToEye1, Fov eye0, Fov eye1) noexcept {
    EyeChange out;
    out.view = m4(eye0ToEye1.m, 3);
    out.rotation = out.view;
    out.rotation[3] = out.rotation[7] = out.rotation[11] = 0;
    out.frustum = frustumShift(eye0, eye1);
    bool finiteAll = true;
    for (double v : out.view) finiteAll = finiteAll && std::isfinite(v);
    for (double v : out.frustum) finiteAll = finiteAll && std::isfinite(v);
    out.valid = finiteAll;
    return out;
}
// The eye-1 counterparts of one recorded view state and the clip-space map
// R = AP1 * E * AP0^-1 that turns any eye-0 MVP into the eye-1 MVP.
struct Rewrite {
    M4 adjusted{}, projection{}, view{}, clip{}, viewChange{};
    M4 adjusted0{};      // AP0, the recorded adjusted projection
    M4 toModel{};        // (AP0 * V0)^-1: recovers the model matrix from an MVP
    bool modelValid = false;
    bool identity = true, valid = false;
};
inline Rewrite rewrite(const ViewState &s, const EyeChange &e) noexcept {
    Rewrite out;
    if (!e.valid) return out;
    const M4 ap0 = m4(s.adjusted.data(), 4), p0 = m4(s.projection.data(), 4), v0 = m4(s.view.data(), 3);
    if (!perspective(p0)) { // Orthographic passes (fills, light-space images) do not depend on the eye.
        out.adjusted = ap0; out.projection = p0; out.view = v0;
        out.clip = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        out.viewChange = out.clip;
        out.valid = true;
        return out;
    }
    // A view whose camera sits at the origin is the native background (sky)
    // child view: it only rotates.
    const bool background = s.view[3] == 0 && s.view[7] == 0 && s.view[11] == 0;
    out.viewChange = background ? e.rotation : e.view;
    M4 p0inv, ap0inv;
    if (!invert(p0, p0inv) || !invert(ap0, ap0inv)) return out;
    const M4 adjust = multiply(ap0, p0inv);                       // A
    out.projection = multiply(e.frustum, p0);                      // T * P0
    out.adjusted = multiply(adjust, out.projection);               // A * T * P0
    // T only moves x and y: the z and w rows stay bit-exact (consumers test
    // the perspective row for exact values), as do A*T*P0's when A keeps them.
    for (unsigned i = 8; i < 16; ++i) out.projection[i] = p0[i];
    if (std::abs(adjust[8]) < 1e-9 && std::abs(adjust[9]) < 1e-9 && std::abs(adjust[12]) < 1e-9 && std::abs(adjust[13]) < 1e-9)
        for (unsigned i = 8; i < 16; ++i) out.adjusted[i] = ap0[i];
    out.view = multiply(out.viewChange, v0);                       // E * V0
    out.clip = multiply(multiply(out.adjusted, out.viewChange), ap0inv);
    for (double v : out.clip) if (!std::isfinite(v)) return out;
    out.adjusted0 = ap0;
    out.modelValid = invert(multiply(ap0, v0), out.toModel);
    out.identity = false;
    out.valid = true;
    return out;
}
// Rewrite `rows` constant rows (4 floats each) of one matrix upload by type.
// Engine GfxMatrixType: 0x46 MV (3 rows), 0x48 model, 0x49 view, 0x4a adjusted
// projection, anything else MVP (4 rows).
inline void rewriteUpload(unsigned type, const Rewrite &r, const float *in, unsigned rows, float *out) noexcept {
    std::memcpy(out, in, size_t(rows) * 4 * sizeof(float));
    if (!r.valid || r.identity || type == 0x48) return;
    const M4 *left = nullptr;
    M4 replaced{};
    if (type == 0x4a) { for (unsigned i = 0; i < rows * 4 && i < 16; ++i) out[i] = float(r.adjusted[i]); return; }
    if (type == 0x46 || type == 0x49) left = &r.viewChange;   // MV1 = E*MV0, V1 = E*V0
    else left = &r.clip;                                      // MVP1 = R*MVP0
    const M4 m = m4(in, rows == 3 ? 3 : 4);
    replaced = multiply(*left, m);
    for (unsigned i = 0; i < rows * 4 && i < 16; ++i) out[i] = float(replaced[i]);
}
// Four constant rows that the engine's shader blocks put at c1..c4: rewrite
// them as an MVP only if they are one for this view state, i.e. undoing the
// recorded projection and view leaves an affine model matrix.
inline bool rewriteIfMvp(const Rewrite &r, const float *in, float *out) noexcept {
    std::memcpy(out, in, 16 * sizeof(float));
    if (!r.valid || r.identity || !r.modelValid) return false;
    const M4 m = multiply(r.toModel, m4(in, 4));
    double scale = 0;
    for (unsigned i = 0; i < 12; ++i) scale = std::max(scale, std::abs(m[i]));
    const double eps = 1e-3 * std::max(1., scale);
    if (!(std::abs(m[12]) < eps && std::abs(m[13]) < eps && std::abs(m[14]) < eps && std::abs(m[15] - 1) < 1e-3))
        return false;
    const M4 mvp = multiply(r.clip, m4(in, 4));
    for (unsigned i = 0; i < 16; ++i) out[i] = float(mvp[i]);
    return true;
}
// An MVP drawn through this view's frustum but with its own depth row (another
// near/far, an oblique clip plane): its x, y and w rows are AP0's rows times an
// affine X (view * model). Solve X from those rows, express the depth row as a
// view-space functional g (z = g . X), and rebuild the eye-1 MVP as
// [AP1.x; AP1.y; g; AP1.w] * E * X: the same depth function, moved with the eye.
inline bool rewriteFrustumMvp(const Rewrite &r, const float *in, float *out) noexcept {
    std::memcpy(out, in, 16 * sizeof(float));
    if (!r.valid || r.identity) return false;
    const M4 &p = r.adjusted0;
    const M4 m = m4(in, 4);
    // Q: AP0's x, y, w rows restricted to the first three columns.
    const double q[3][3] = {{p[0], p[1], p[2]}, {p[4], p[5], p[6]}, {p[12], p[13], p[14]}};
    const double det = q[0][0] * (q[1][1] * q[2][2] - q[1][2] * q[2][1]) - q[0][1] * (q[1][0] * q[2][2] - q[1][2] * q[2][0]) +
                       q[0][2] * (q[1][0] * q[2][1] - q[1][1] * q[2][0]);
    if (!std::isfinite(det) || std::abs(det) < 1e-12) return false;
    if (std::abs(m[12]) + std::abs(m[13]) + std::abs(m[14]) < 1e-9) return false; // Not perspective.
    double qi[3][3];
    qi[0][0] = (q[1][1] * q[2][2] - q[1][2] * q[2][1]) / det; qi[0][1] = (q[0][2] * q[2][1] - q[0][1] * q[2][2]) / det;
    qi[0][2] = (q[0][1] * q[1][2] - q[0][2] * q[1][1]) / det; qi[1][0] = (q[1][2] * q[2][0] - q[1][0] * q[2][2]) / det;
    qi[1][1] = (q[0][0] * q[2][2] - q[0][2] * q[2][0]) / det; qi[1][2] = (q[0][2] * q[1][0] - q[0][0] * q[1][2]) / det;
    qi[2][0] = (q[1][0] * q[2][1] - q[1][1] * q[2][0]) / det; qi[2][1] = (q[0][1] * q[2][0] - q[0][0] * q[2][1]) / det;
    qi[2][2] = (q[0][0] * q[1][1] - q[0][1] * q[1][0]) / det;
    M4 x{};
    x[15] = 1;
    for (unsigned j = 0; j < 4; ++j) {
        const double t = j == 3 ? 1 : 0; // X's fourth row is (0,0,0,1).
        const double rhs[3] = {m[j] - p[3] * t, m[4 + j] - p[7] * t, m[12 + j] - p[15] * t};
        for (unsigned k = 0; k < 3; ++k) x[4 * k + j] = qi[k][0] * rhs[0] + qi[k][1] * rhs[1] + qi[k][2] * rhs[2];
    }
    M4 xi;
    if (!invert(x, xi)) return false;
    M4 g{}; // Row 2: the depth row as a functional of X's space.
    for (unsigned c = 0; c < 4; ++c)
        for (unsigned k = 0; k < 4; ++k) g[8 + c] += m[8 + k] * xi[4 * k + c];
    M4 p1 = r.adjusted;
    for (unsigned c = 0; c < 4; ++c) p1[8 + c] = g[8 + c];
    const M4 result = multiply(multiply(p1, r.viewChange), x);
    for (double v : result) if (!std::isfinite(v)) return false;
    for (unsigned i = 0; i < 16; ++i) out[i] = float(result[i]);
    return true;
}
// Clip-space user plane (row vector, plane . clip >= 0) for the eye-1 clip space.
inline void rewritePlane(const Rewrite &r, const float *in, float *out) noexcept {
    std::memcpy(out, in, 4 * sizeof(float));
    if (!r.valid || r.identity) return;
    M4 inv;
    if (!invert(r.clip, inv)) return;
    for (unsigned c = 0; c < 4; ++c) {
        double v = 0;
        for (unsigned k = 0; k < 4; ++k) v += double(in[k]) * inv[4*k+c];
        out[c] = float(v);
    }
}
// The far plane of an eye whose view is turned against the centre camera's
// (canted displays): a point off to one side lies deeper along the eye turned
// toward it than along the centre's axis, so what the engine kept inside its
// far plane was clipped in that eye only (distant trees missing in one eye).
// Given the eye's clip map rows (`clip`, centre clip -> eye clip) and the
// centre's adjusted projection, rewrites the depth row so the eye's far plane
// lies beyond the centre frustum's deepest point along the eye axis; the near
// plane stays exactly where it was. False (unchanged) for a non-standard
// projection or when no corner of the frustum lies deeper.
inline bool extendFar(M4 &clip, const M4 &adjusted, const M4 &eyeView) noexcept {
    const M4 &a = adjusted;
    if (std::abs(a[8]) > 1e-9 || std::abs(a[9]) > 1e-9 || std::abs(a[12]) > 1e-9 || std::abs(a[13]) > 1e-9 ||
        std::abs(a[15]) > 1e-9 || !(a[14] < 0) || std::abs(a[11]) < 1e-12)
        return false;
    // z_c = a22 z + a23, w = a32 z (z = -depth): near where z_c = 0, far where z_c = w.
    const double nearPlane = a[11] / a[10], farPlane = a[11] / (a[10] - a[14]);
    if (!(nearPlane > 0) || !(farPlane > nearPlane) || !std::isfinite(farPlane)) return false;
    M4 inv;
    if (!invert(a, inv)) return false;
    double ratio = 1;
    for (unsigned corner = 0; corner < 4; ++corner) {
        const double n[4] = {corner & 1 ? 1. : -1., corner & 2 ? 1. : -1., .5, 1};
        double p[4]{};
        for (unsigned k = 0; k < 4; ++k) for (unsigned c = 0; c < 4; ++c) p[k] += inv[4 * k + c] * n[c];
        if (!(std::abs(p[3]) > 1e-12)) return false;
        const double d[3] = {p[0] / p[3], p[1] / p[3], p[2] / p[3]};
        if (!(d[2] < 0)) return false;
        const double eyeDepth = -(eyeView[8] * d[0] + eyeView[9] * d[1] + eyeView[10] * d[2]);
        ratio = std::max(ratio, eyeDepth / -d[2]);
    }
    if (!(ratio > 1 + 1e-6) || !std::isfinite(ratio)) return false;
    // A point at depth D (w = -a32 D): new z_c' = c' z + d' with the near plane
    // kept and the far plane at ratio * far (1% more), as A z_c + B w.
    const double far1 = farPlane * ratio * 1.01;
    const double c1 = a[14] * far1 / (far1 - nearPlane), d1 = c1 * nearPlane;
    const double A = d1 / a[11], B = (c1 - A * a[10]) / a[14];
    for (unsigned c = 0; c < 4; ++c) clip[8 + c] = A * clip[8 + c] + B * clip[12 + c];
    return true;
}
// A scissor rectangle (pixels l, t, r, b) of the camera whose adjusted
// projection is `adjusted` (rows, clip = AP * view, view looks down -z), for
// viewport x, y, w, h, as seen through `clip` (that camera's clip -> another
// camera's clip, rows): the other camera's rectangle covering everything inside
// it from `nearest` metres to 100 km away (the corner rays' bounding box, a
// pixel of slack). The engine's scissors (portals) are the centre camera's; each
// eye has its own frustum. False when a corner falls behind either camera.
inline bool mapScissor(const M4 &clip, const M4 &adjusted, double x, double y, double w, double h,
                       double (&rect)[4], double nearest = 1) noexcept {
    const M4 &a = adjusted;
    if (std::abs(a[12]) > 1e-9 || std::abs(a[13]) > 1e-9 || !(w > 0) || !(h > 0)) return false;
    const double det = a[0] * a[5] - a[1] * a[4];
    if (!std::isfinite(det) || std::abs(det) < 1e-12) return false;
    double out[4] = {1e300, 1e300, -1e300, -1e300}; // left, top, right, bottom
    for (double depth : {nearest, 1e5})
        for (unsigned corner = 0; corner < 4; ++corner) {
            const double px = corner & 1 ? rect[2] : rect[0], py = corner & 2 ? rect[3] : rect[1];
            const double nx = 2 * (px - x) / w - 1, ny = 1 - 2 * (py - y) / h, z = -depth;
            const double cw = a[14] * z + a[15];
            if (!(cw > 0)) return false;
            // The view point on this pixel's ray at that depth: solve the x, y rows.
            const double bx = nx * cw - a[2] * z - a[3], by = ny * cw - a[6] * z - a[7];
            const double vx = (bx * a[5] - by * a[1]) / det, vy = (a[0] * by - a[4] * bx) / det;
            const double c[4] = {nx * cw, ny * cw, a[8] * vx + a[9] * vy + a[10] * z + a[11], cw};
            double e[4];
            for (unsigned r = 0; r < 4; ++r) e[r] = clip[4 * r] * c[0] + clip[4 * r + 1] * c[1] + clip[4 * r + 2] * c[2] + clip[4 * r + 3] * c[3];
            if (!(e[3] > 1e-9) || !std::isfinite(e[0] + e[1])) return false;
            const double ex = x + (e[0] / e[3] + 1) * .5 * w, ey = y + (1 - e[1] / e[3]) * .5 * h;
            out[0] = std::min(out[0], ex); out[1] = std::min(out[1], ey);
            out[2] = std::max(out[2], ex); out[3] = std::max(out[3], ey);
        }
    rect[0] = std::floor(out[0]) - 1; rect[1] = std::floor(out[1]) - 1;
    rect[2] = std::ceil(out[2]) + 1; rect[3] = std::ceil(out[3]) + 1;
    return true;
}
} // namespace ss2vr::replay
