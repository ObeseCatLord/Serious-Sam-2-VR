#pragma once
#include "math.hpp"
#include "protocol.hpp"
#include <algorithm>
#include <cmath>

// Single-pass stereo camera: the world is traversed once from a centre camera
// whose frustum covers both eyes; each eye's image is then an exact clip-space
// reprojection of that pass (see replay_math.hpp, rewrite()).
namespace ss2vr::single_pass {
// Midpoint of the two eyes, orientation halfway between them.
inline Pose centreEye(const Pose &a, const Pose &b) {
    Quat qb = b.q;
    if (a.q.x * qb.x + a.q.y * qb.y + a.q.z * qb.z + a.q.w * qb.w < 0) qb = {-qb.x, -qb.y, -qb.z, -qb.w};
    return {normalize(Quat{a.q.x + qb.x, a.q.y + qb.y, a.q.z + qb.z, a.q.w + qb.w}),
            Vec3{(a.p.x + b.p.x) * .5f, (a.p.y + b.p.y) * .5f, (a.p.z + b.p.z) * .5f}};
}
// Union of both eyes' frusta seen from the centre (corner directions rotated
// into the centre frame), widened by `margin` in tangent space for the eyes'
// offset from the centre (objects closer than ipd/2 / margin may still clip).
inline Fov unionFov(const Pose &centre, const Pose (&eyes)[2], const Fov (&fovs)[2], float margin = .08f) {
    float left = 1e9f, right = -1e9f, up = -1e9f, down = 1e9f;
    for (unsigned i = 0; i < 2; ++i) {
        const Quat relative = multiply(Quat{-centre.q.x, -centre.q.y, -centre.q.z, centre.q.w}, eyes[i].q);
        for (float tx : {std::tan(fovs[i].left), std::tan(fovs[i].right)})
            for (float ty : {std::tan(fovs[i].down), std::tan(fovs[i].up)}) {
                const Vec3 d = rotate(relative, Vec3{tx, ty, -1});
                if (!(d.z < 0)) continue;
                const float x = d.x / -d.z, y = d.y / -d.z;
                left = std::min(left, x); right = std::max(right, x);
                down = std::min(down, y); up = std::max(up, y);
            }
    }
    return {std::atan(left - margin), std::atan(right + margin), std::atan(up + margin), std::atan(down - margin)};
}
// The centre request: eye 0 is the centre camera with the union frustum (the
// engine renders from request eye 0 / fov 0 during the single pass).
inline Request centreRequest(const Request &r, float margin = .08f) {
    Request out = r;
    const Pose eyes[2]{r.eye[0], r.eye[1]};
    const Fov fovs[2]{r.fov[0], r.fov[1]};
    out.eye[0] = out.eye[1] = centreEye(eyes[0], eyes[1]);
    out.fov[0] = out.fov[1] = unionFov(out.eye[0], eyes, fovs, margin);
    return out;
}
} // namespace ss2vr::single_pass
