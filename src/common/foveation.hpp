#pragma once
#include "protocol.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ss2vr {
// Foveated readback. The game still renders each eye at the runtime's
// recommended size, but returns only a downsampled full view plus a
// full-resolution crop around the gaze; the host submits them as a base and an
// inset projection layer. Projection is linear in tangent space, so a pixel
// crop of the full image covers an exactly known FOV.
struct FoveationSettings {
    int mode = 1;            // 0 off, 1 when the runtime provides eye gaze, 2 always (fixed centre without gaze).
    float inset = .4f;       // Inset size as a fraction of the full image per axis.
    float periphery = .5f;   // Base (full-FOV) resolution as a fraction of the full image per axis.
};
struct PixelRect {
    int32_t x = 0, y = 0, w = 0, h = 0;
};
// Even pixel count for a scaled dimension, at least 2.
inline uint32_t foveationExtent(uint32_t full, float fraction) {
    const float f = std::isfinite(fraction) ? std::clamp(fraction, .1f, 1.f) : 1.f;
    return std::max<uint32_t>(2, uint32_t(std::lround(full * f * .5f)) * 2);
}
// The base and inset images must share one eye pixel buffer (full width x height).
inline bool foveationFits(uint32_t width, uint32_t height, uint32_t lowW, uint32_t lowH, uint32_t insetW,
                          uint32_t insetH) {
    return lowW && lowH && insetW && insetH && lowW <= width && lowH <= height && insetW <= width &&
        insetH <= height && uint64_t(lowW) * lowH + uint64_t(insetW) * insetH <= uint64_t(width) * height;
}
// Pixel rect of insetW x insetH centred at tangent (tx, ty) (x right, y up) of a
// full width x height image rendered with fov; shifted to stay inside the image.
inline PixelRect insetCrop(Fov fov, uint32_t width, uint32_t height, uint32_t insetW, uint32_t insetH, float tx,
                           float ty) {
    const float l = std::tan(fov.left), r = std::tan(fov.right), u = std::tan(fov.up), d = std::tan(fov.down);
    if (!(r > l) || !(u > d) || !std::isfinite(tx) || !std::isfinite(ty)) { tx = (l + r) * .5f; ty = (u + d) * .5f; }
    const float cx = (tx - l) / (r - l) * float(width), cy = (u - ty) / (u - d) * float(height);
    PixelRect rect;
    rect.w = int32_t(std::min(insetW, width));
    rect.h = int32_t(std::min(insetH, height));
    rect.x = std::clamp(int32_t(std::lround(cx - rect.w * .5f)), 0, int32_t(width) - rect.w);
    rect.y = std::clamp(int32_t(std::lround(cy - rect.h * .5f)), 0, int32_t(height) - rect.h);
    return rect;
}
// Exact FOV angles of a pixel rect inside the full image.
inline Fov rectFov(Fov fov, uint32_t width, uint32_t height, PixelRect rect) {
    const float l = std::tan(fov.left), r = std::tan(fov.right), u = std::tan(fov.up), d = std::tan(fov.down);
    const float x0 = l + (r - l) * float(rect.x) / float(width), x1 = l + (r - l) * float(rect.x + rect.w) / float(width);
    const float y0 = u - (u - d) * float(rect.y) / float(height), y1 = u - (u - d) * float(rect.y + rect.h) / float(height);
    return {std::atan(x0), std::atan(x1), std::atan(y0), std::atan(y1)};
}
// Gaze direction in the same space as eye orientation `eye` -> tangent-space
// point (x right, y up) clamped inside the eye FOV. False when not forward.
inline bool gazeTangent(Quat eye, Vec3 gaze, Fov fov, float &tx, float &ty) {
    const Quat inverseEye{-eye.x, -eye.y, -eye.z, eye.w};
    // Rotate the gaze into eye space: q * v * q^-1 with q = inverseEye.
    const auto rotateBy = [](Quat q, Vec3 v) {
        const Vec3 u{q.x, q.y, q.z};
        const auto cross = [](Vec3 a, Vec3 b) { return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; };
        const Vec3 t = cross(u, v), tt{2 * t.x, 2 * t.y, 2 * t.z};
        const Vec3 c = cross(u, tt);
        return Vec3{v.x + q.w * tt.x + c.x, v.y + q.w * tt.y + c.y, v.z + q.w * tt.z + c.z};
    };
    const Vec3 d = rotateBy(inverseEye, gaze);
    if (!std::isfinite(d.x) || !std::isfinite(d.y) || !std::isfinite(d.z) || d.z > -1e-3f) return false;
    tx = std::clamp(-d.x / d.z, std::tan(fov.left), std::tan(fov.right));
    ty = std::clamp(-d.y / d.z, std::tan(fov.down), std::tan(fov.up));
    return true;
}
// Feathered inset edge: alpha ramps 0..255 across `band` border pixels (BGRA).
inline void featherAlpha(uint8_t *bgra, uint32_t width, uint32_t height, uint32_t band) {
    if (!bgra || !width || !height) return;
    band = std::min({band, width / 2, height / 2});
    for (uint32_t y = 0; y < height; ++y) {
        const uint32_t dy = std::min(y, height - 1 - y);
        const bool rowBand = dy < band;
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t dx = std::min(x, width - 1 - x);
            if (!rowBand && dx >= band) { x = width - 1 - band; continue; } // Skip the opaque interior.
            const uint32_t edge = std::min(dx, dy);
            bgra[(size_t(y) * width + x) * 4 + 3] = uint8_t(edge >= band ? 255 : (edge * 255 + band / 2) / band);
        }
    }
}
} // namespace ss2vr
