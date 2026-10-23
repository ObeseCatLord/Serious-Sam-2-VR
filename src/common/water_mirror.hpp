#pragma once
#include "math.hpp"
#include "replay_math.hpp"
#include <cmath>

// Planar water reflections ([Render] WaterMirror). The engine's water samples a
// painted image by screen position; the mod renders the world once more from
// the centre camera mirrored about the water plane and the patched water
// program looks that image up by each water point's position in the mirror
// camera, so reflections of things close by (walkways, trunks) sit where they
// belong for both eyes. Planes are n.x + d = 0 (world), n the unit normal
// pointing out of the water.
namespace ss2vr::water_mirror {
// A water draw's plane: the engine's water meshes are flat at model y = 0 (the
// model matrix places, turns and scales them). `model` maps model -> world (rows).
inline bool modelPlane(const replay::M4 &model, double (&plane)[4]) noexcept {
    replay::M4 inv;
    if (!replay::invert(model, inv)) return false;
    // Normal: the model y row of the inverse (covector of y = 0), through the origin's image.
    double n[3] = {inv[4], inv[5], inv[6]};
    const double length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (!(length > 1e-9) || !std::isfinite(length)) return false;
    for (double &v : n) v /= length;
    if (n[1] < 0) for (double &v : n) v = -v; // Out of the water: up.
    const double o[3] = {model[3], model[7], model[11]};
    plane[0] = n[0]; plane[1] = n[1]; plane[2] = n[2];
    plane[3] = -(n[0] * o[0] + n[1] * o[1] + n[2] * o[2]);
    return std::isfinite(plane[3]);
}
// The same plane (within `distance` metres and `angle` radians of normal).
inline bool samePlane(const double (&a)[4], const double (&b)[4], double distance = .05, double angle = .02) noexcept {
    const double dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    return dot > std::cos(angle) && std::abs(a[3] - b[3]) < distance;
}
inline double signedDistance(const double (&plane)[4], const Vec3 &p) noexcept {
    return plane[0] * p.x + plane[1] * p.y + plane[2] * p.z + plane[3];
}
// The mirror camera: `camera` (pose: x right, y up, looking down -z) reflected
// about the plane, its x axis flipped so the pose stays a rotation. What it
// sees is the true mirror image turned left for right, which a lookup by its
// own projection undoes: a point's image in it is where the reflection shows it.
inline Pose mirrorCamera(const Pose &camera, const double (&plane)[4]) noexcept {
    const Vec3 n{float(plane[0]), float(plane[1]), float(plane[2])};
    const auto reflect = [&](Vec3 v) { const float k = 2 * (n.x * v.x + n.y * v.y + n.z * v.z); return Vec3{v.x - k * n.x, v.y - k * n.y, v.z - k * n.z}; };
    const float distance = float(signedDistance(plane, camera.p));
    const Vec3 p{camera.p.x - 2 * distance * n.x, camera.p.y - 2 * distance * n.y, camera.p.z - 2 * distance * n.z};
    const Vec3 x = reflect(rotate(camera.q, Vec3{1, 0, 0})), y = reflect(rotate(camera.q, Vec3{0, 1, 0})),
               z = reflect(rotate(camera.q, Vec3{0, 0, 1}));
    // Columns -x, y, z: a proper rotation (det +1).
    const float m[3][3] = {{-x.x, y.x, z.x}, {-x.y, y.y, z.y}, {-x.z, y.z, z.z}};
    Quat q;
    const float trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0) {
        const float s = std::sqrt(trace + 1) * 2;
        q = {(m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s, .25f * s};
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const float s = std::sqrt(1 + m[0][0] - m[1][1] - m[2][2]) * 2;
        q = {.25f * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s, (m[2][1] - m[1][2]) / s};
    } else if (m[1][1] > m[2][2]) {
        const float s = std::sqrt(1 + m[1][1] - m[0][0] - m[2][2]) * 2;
        q = {(m[0][1] + m[1][0]) / s, .25f * s, (m[1][2] + m[2][1]) / s, (m[0][2] - m[2][0]) / s};
    } else {
        const float s = std::sqrt(1 + m[2][2] - m[0][0] - m[1][1]) * 2;
        q = {(m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, .25f * s, (m[1][0] - m[0][1]) / s};
    }
    return {normalize(q), p};
}
// Its field of view: the camera's with left and right exchanged (its x is flipped),
// widened by `margin` (tangent) for the water's ripple.
inline Fov mirrorFov(const Fov &f, float margin = .1f) noexcept {
    return {std::atan(std::tan(-f.right) - margin), std::atan(std::tan(-f.left) + margin), std::atan(std::tan(f.up) + margin),
            std::atan(std::tan(f.down) - margin)};
}
// The plane in a camera's clip space (for a programmable pipeline's user clip
// plane: kept where plane . clip >= 0), lowered by `below` metres so nothing at
// the water line is cut; `viewProjection` maps world -> clip (rows).
inline bool clipPlane(const replay::M4 &viewProjection, const double (&plane)[4], float (&out)[4], double below = .05) noexcept {
    replay::M4 inv;
    if (!replay::invert(viewProjection, inv)) return false;
    const double p[4] = {plane[0], plane[1], plane[2], plane[3] + below};
    for (unsigned c = 0; c < 4; ++c) {
        const double v = p[0] * inv[c] + p[1] * inv[4 + c] + p[2] * inv[8 + c] + p[3] * inv[12 + c];
        if (!std::isfinite(v)) return false;
        out[c] = float(v);
    }
    return true;
}
// The plane in a camera's own space (x right, y up, looking down -z).
inline void planeInCamera(const double (&plane)[4], const Pose &camera, double (&out)[4]) noexcept {
    const Matrix34 m = matrix(camera); // Camera -> world.
    for (unsigned c = 0; c < 3; ++c) out[c] = plane[0] * m.m[c] + plane[1] * m.m[4 + c] + plane[2] * m.m[8 + c];
    out[3] = plane[0] * m.m[3] + plane[1] * m.m[7] + plane[2] * m.m[11] + plane[3];
}
// Oblique near plane (Lengyel): `projection` (rows, OpenGL depth -1..1, w = -z)
// with its depth row replaced so the near plane is `plane` (camera space, kept
// where plane . v >= 0) and the farthest corner of the frustum's far end maps
// to depth 1. Everything under the water then falls in front of the near plane
// and is clipped without user clip planes; x, y and w are unchanged.
inline bool obliqueNear(Matrix44 &projection, const double (&plane)[4]) noexcept {
    replay::M4 p = replay::m4(projection.m, 4), inv;
    if (!replay::invert(p, inv)) return false;
    double scale = 0;
    for (unsigned corner = 0; corner < 4; ++corner) { // The far end's corners, in camera space.
        const double n[4] = {corner & 1 ? 1. : -1., corner & 2 ? 1. : -1., 1, 1};
        double q[4]{};
        for (unsigned r = 0; r < 4; ++r) for (unsigned k = 0; k < 4; ++k) q[r] += inv[4 * r + k] * n[k];
        if (!(std::abs(q[3]) > 1e-12)) return false;
        const double w = -(q[2] / q[3]), side = plane[0] * q[0] / q[3] + plane[1] * q[1] / q[3] + plane[2] * q[2] / q[3] + plane[3];
        if (side > 0 && w > 0) scale = std::max(scale, side / w);
    }
    if (!(scale > 0) || !std::isfinite(scale)) return false;
    // z_c = a (C . v) - w: depth -1 on the plane, 1 at the farthest corner.
    const double a = 2 / scale;
    for (unsigned c = 0; c < 4; ++c) projection.m[8 + c] = float(a * plane[c] - p[12 + c]);
    return true;
}
} // namespace ss2vr::water_mirror
