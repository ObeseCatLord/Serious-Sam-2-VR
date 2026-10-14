#pragma once
#include "replay_math.hpp"
#include <cmath>

// Water reflections in single pass. The engine has no planar mirror: water
// samples an artist-made reflection image by screen position (the clip-space
// position its vertex program writes). With one centre camera for both eyes
// that image sat on the water surface and turned with the head, like a screen
// projected onto it. Each eye instead looks it up from a reference camera at
// that eye, aimed along a smoothed, level copy of the view direction: the
// image lies at infinity (the eyes agree on it), stays put in the world through
// quick head motion and slowly follows where you look.
namespace ss2vr::water {
using replay::M4;
struct Reference {
    double yaw = 0, pitch = 0;
    bool seeded = false;
};
// Heading and pitch of a world -> view matrix (row-major 3x4, -Z forward, +Y up).
inline bool viewAngles(const float *view, double &yaw, double &pitch) noexcept {
    const double fx = -view[8], fy = -view[9], fz = -view[10]; // Forward = -(third row).
    const double n = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (!std::isfinite(n) || n < 1e-6) return false;
    yaw = std::atan2(-fx, -fz);
    pitch = std::asin(std::fmax(-1., std::fmin(1., fy / n)));
    return std::isfinite(yaw) && std::isfinite(pitch);
}
// Follows the view with time constant `tau` seconds; a large jump (snap turn,
// teleport, cut) is taken at once. Pitch stays within +-1 rad.
inline void follow(Reference &r, const float *view, double seconds, double tau = 1.) noexcept {
    double yaw, pitch;
    if (!viewAngles(view, yaw, pitch)) return;
    pitch = std::fmax(-1., std::fmin(1., pitch));
    if (!r.seeded) { r = {yaw, pitch, true}; return; }
    constexpr double Pi = 3.14159265358979323846;
    double dy = std::remainder(yaw - r.yaw, 2 * Pi), dp = pitch - r.pitch;
    if (std::abs(dy) > 1.2 || std::abs(dp) > 1.2 || !(tau > 0)) { r = {yaw, pitch, true}; return; }
    const double a = 1 - std::exp(-std::fmax(0., seconds) / tau);
    r.yaw = std::remainder(r.yaw + a * dy, 2 * Pi);
    r.pitch += a * dp;
}
// The clip-space map K = AP_c * Eref_i * AP_c^-1 for eye i: centre clip
// position -> the eye's reference camera clip position. Both eyes' reference
// cameras share the centre projection AP_c, so a direction maps to the same
// texel in both (the image at infinity); `change` is the eye's EyeChange.
inline bool eyeClip(const replay::ViewState &s, const replay::EyeChange &change, const Reference &ref, M4 &out) noexcept {
    using namespace replay;
    if (!change.valid || !ref.seeded) return false;
    const M4 ap = m4(s.adjusted.data(), 4), p = m4(s.projection.data(), 4);
    M4 apInv;
    if (!perspective(p) || !invert(ap, apInv)) return false;
    // Rc: world -> centre view rotation; Q: world -> reference rotation (Ry(yaw) Rx(pitch) transposed).
    const float *v = s.view.data();
    const double Rc[9]{v[0], v[1], v[2], v[4], v[5], v[6], v[8], v[9], v[10]};
    const double cy = std::cos(ref.yaw), sy = std::sin(ref.yaw), cp = std::cos(ref.pitch), sp = std::sin(ref.pitch);
    const double Rv[9]{cy, sy * sp, sy * cp, 0, cp, -sp, -sy, cy * sp, cy * cp}; // view -> world
    // S = Q Rc^T = (Rc Rv)^T: centre view -> reference view.
    double S[9]{};
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 3; ++j) {
            double m = 0;
            for (unsigned k = 0; k < 3; ++k) m += Rc[3 * j + k] * Rv[3 * k + i]; // (Rc Rv)[j][i]
            S[3 * i + j] = m;
        }
    // The eye's position in centre view space: c = -Ri^T ti.
    const M4 &E = change.view;
    double c[3]{};
    for (unsigned k = 0; k < 3; ++k) c[k] = -(E[k] * E[3] + E[4 + k] * E[7] + E[8 + k] * E[11]);
    M4 eref{};
    for (unsigned i = 0; i < 3; ++i) {
        for (unsigned j = 0; j < 3; ++j) eref[4 * i + j] = S[3 * i + j];
        eref[4 * i + 3] = -(S[3 * i] * c[0] + S[3 * i + 1] * c[1] + S[3 * i + 2] * c[2]);
    }
    eref[15] = 1;
    out = multiply(multiply(ap, eref), apInv);
    for (double x : out) if (!std::isfinite(x)) return false;
    return true;
}
} // namespace ss2vr::water
