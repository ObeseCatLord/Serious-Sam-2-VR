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
// The environment cube's surroundings are taken to lie on a sphere this far
// (metres) around the viewer: reflections keep parallax for anything nearer.
constexpr float ProxyRadius = 40.f;
// What a water draw shows as its reflection in VR. The engine's own image is
// looked up by screen position (and on some levels is a copy of the screen), so
// it follows the head; a projection of the eye's view into the cube did too.
// Neither is ever used: the reflection is world-fixed (the cube rendered from
// its anchor, or the planar mirror), or else it is off: a neutral sky colour
// that does not depend on the head at all, through the cube variants when the
// programs have them, else as the reflection image under the unchanged programs.
// `Engine` remains only for the lab switch and for a pixel program in which the
// reflection lookup cannot be found (logged; nothing can be replaced).
enum class Reflection { World, NeutralCube, NeutralImage, Engine };
namespace reason {
constexpr unsigned NoCube = 1, NoRotation = 2, Lab = 4, NoVariant = 8, NoCamera = 16, NoAnchor = 32, NoPixelVariant = 64,
                   NoSampler = 128, NotStereo = 256;
// Any of these and the cube cannot be looked up world-fixed.
constexpr unsigned NotWorld = NoCube | NoRotation | NoVariant | NoCamera | NoAnchor | NoPixelVariant | NotStereo;
}
inline Reflection reflectionFor(unsigned why) noexcept {
    if (why & reason::Lab) return Reflection::Engine;
    if (!(why & reason::NotWorld)) return Reflection::World;
    // The neutral cube needs both cube variants and nothing else (no anchor, rotation or camera).
    if (!(why & (reason::NoVariant | reason::NoPixelVariant | reason::NotStereo))) return Reflection::NeutralCube;
    return why & reason::NoSampler ? Reflection::Engine : Reflection::NeutralImage;
}
// The neutral reflection: a clear sky's colour (the cube's texel; the image's is
// this times the cube variants' gain, so both look the same on the water).
constexpr unsigned char NeutralSky[3]{150, 175, 200};
inline unsigned char neutralImageLevel(unsigned char sky, float gain) noexcept {
    const float v = float(sky) * gain + .5f;
    return static_cast<unsigned char>(!(v > 0) ? 0 : v > 255 ? 255 : v);
}
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
// Inverse of a 3x3 (rows of a 3x4: `m`, stride 4), into `out` (3x3, stride 3).
inline bool invert3(const float *m, float (&out)[9]) noexcept {
    const double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(det) || std::abs(det) < 1e-12) return false;
    const double r[9]{e * i - f * h, c * h - b * i, b * f - c * e, f * g - d * i, a * i - c * g,
                      c * d - a * f, d * h - e * g, b * g - a * h, a * e - b * d};
    for (unsigned k = 0; k < 9; ++k) out[k] = float(r[k] / det);
    return true;
}
} // namespace ss2vr::water
