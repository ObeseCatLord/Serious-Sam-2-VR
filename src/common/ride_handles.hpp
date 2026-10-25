#pragma once
#include "model_tree.hpp"
#include "ride_geometry.hpp"
#include <array>
#include <cmath>
#include <span>

// The hoverbike's and UFO's handles turned with the steering, code only: the
// ride's mesh is drawn without its handle triangles, then each handle again with
// its bone's palette rows turned about the handle's pivot (in mesh space).
namespace ss2vr {
// Each handle's turn in mesh space: about `axis` (unit) through `pivot`.
struct RideHandleRig {
    std::array<Vec3, 2> pivot{}, axis{};
    bool valid = false;
};
// The surface's handle triangles: hand 0's 30, then hand 1's.
inline uint32_t rideHandleFirstTriangle(RideHandleProfile profile) {
    return profile == RideHandleProfile::Fighter ? 1788u : profile == RideHandleProfile::Saucer ? 2430u : 0u;
}
inline RideHandleProfile rideHandleLayoutProfile(int32_t vertices, int32_t triangles) {
    if (vertices == 2741 && triangles == 2806) return RideHandleProfile::Fighter;
    if (vertices == 2464 && triangles == 2626) return RideHandleProfile::Saucer;
    return RideHandleProfile::Unknown;
}
// The first row of three consecutive constant rows equal to `palette` (its 3x4
// rows), or -1. Exact apart from float noise: a palette is copied, not computed.
inline int findPaletteRows(std::span<const std::array<float, 4>> rows, const Matrix34 &palette) {
    for (size_t r = 0; r + 3 <= rows.size(); ++r) {
        bool equal = true;
        for (unsigned i = 0; i < 3 && equal; ++i)
            for (unsigned j = 0; j < 4 && equal; ++j) {
                const float a = rows[r + i][j], b = palette.m[i * 4 + j];
                equal = std::abs(a - b) <= 1e-5f + 1e-5f * std::abs(b);
            }
        if (equal) return int(r);
    }
    return -1;
}
inline Quat axisAngle(Vec3 axis, float angle) {
    const float s = std::sin(angle * .5f);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(angle * .5f)};
}
// The palette that draws a handle turned by `angle` radians about its pivot.
inline Matrix34 turnedPalette(const Matrix34 &palette, Vec3 pivot, Vec3 axis, float angle) {
    const Quat q = axisAngle(axis, angle);
    const Vec3 offset = pivot - rotate(q, pivot);
    return affineMultiply(palette, matrix(Pose{q, offset}));
}
// The handles' turn, from their meshes (model space: +Y up, -Z forward, +X
// right): both roll together like a yoke about the forward axis through the
// midpoint between them, as the hands steer (right hand lower, right handle
// lower for a right turn).
inline bool fitRideHandleRig(const RideHandleGeometry &g, RideHandleRig &out) {
    out = {};
    if (!g.copied) return false;
    std::array<Vec3, 2> centre{};
    for (unsigned hand = 0; hand < 2; ++hand) {
        const auto &h = g.handles[hand];
        if (h.vertices < 4 || h.vertices > RideHandleMesh::MaxVertices) return false;
        Vec3 c{};
        for (unsigned v = 0; v < h.vertices; ++v) c = c + h.positions[v];
        centre[hand] = c * (1.f / float(h.vertices));
    }
    const Vec3 middle = (centre[0] + centre[1]) * .5f, across = centre[0] - centre[1];
    // Side by side across the ride (apart along X), or this is not a pair of handles.
    if (!std::isfinite(middle.x + middle.y + middle.z) || std::abs(across.x) < .05f ||
        std::abs(across.x) < 2 * std::max(std::abs(across.y), std::abs(across.z)))
        return false;
    for (unsigned hand = 0; hand < 2; ++hand) {
        out.pivot[hand] = middle;
        out.axis[hand] = {0, 0, -1};
    }
    out.valid = true;
    return true;
}
} // namespace ss2vr
