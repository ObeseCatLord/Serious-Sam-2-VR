#pragma once
#include "replay_math.hpp"
#include <cmath>

// Water reflections in single pass. The engine has no planar mirror: water
// samples an artist-made reflection image by screen position, which cannot
// work for two eyes (with the single-pass centre camera the image lay on the
// water and turned with the head). The patched water vertex program instead
// looks the image up by the reflected view direction from each eye (see
// shader_patch eyeClipSite), so the reflection stays fixed in the world, at
// a distance, and agrees between the eyes. This gives the eye position: the
// engine's view position constant plus the eye's offset from the centre camera.
namespace ss2vr::water {
// Scale of the dome lookup: 1 spreads the image once over the sky hemisphere.
constexpr float DomeScale = 1.f;
// The eye's offset from the centre camera in world space: `view` is the centre
// camera's world -> view matrix (row-major 3x4), `change` the eye's EyeChange
// (centre view -> eye view).
inline bool eyeOffset(const float *view, const replay::EyeChange &change, float (&out)[3]) noexcept {
    if (!change.valid) return false;
    const auto &E = change.view;
    double c[3];
    for (unsigned k = 0; k < 3; ++k) c[k] = -(E[k] * E[3] + E[4 + k] * E[7] + E[8 + k] * E[11]); // -Ri^T ti
    for (unsigned j = 0; j < 3; ++j) {
        const double w = view[j] * c[0] + view[4 + j] * c[1] + view[8 + j] * c[2]; // Rc^T c
        if (!std::isfinite(w)) return false;
        out[j] = float(w);
    }
    return true;
}
} // namespace ss2vr::water
