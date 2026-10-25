#pragma once
#include "math.hpp"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// Water reflections from the surroundings (env_probe.cpp). The engine's water
// looks a painted image up by screen position, so in VR the reflection lay on
// the water and turned or slid with the head. Instead each frame's left-eye
// world image (before HUD) is projected into a world-aligned cube map: only
// the upper half of the view and directions above the horizon, where the guns
// and the water itself are not. The cube is anchored at a fixed world point
// (moved only when the player walks away from it), its contents taken to lie
// on a sphere around that point; the patched water programs look up where
// each eye's reflected ray meets that sphere, so reflections stay fixed in the
// world as the head turns and moves. Matrices are row-major: view is world ->
// view 3x4, projection 4x4 with clip = P * view (w = -z).
namespace ss2vr::env_probe {
constexpr unsigned FaceSize = 256;
// D3D cube face order +X, -X, +Y, -Y, +Z, -Z; s, t in [-1, 1] across the face
// (t grows down the face image). The direction the hardware samples there.
inline void faceDirection(unsigned face, float s, float t, float (&out)[3]) {
    switch (face) {
    case 0: out[0] = 1; out[1] = -t; out[2] = -s; break;
    case 1: out[0] = -1; out[1] = -t; out[2] = s; break;
    case 2: out[0] = s; out[1] = 1; out[2] = t; break;
    case 3: out[0] = s; out[1] = -1; out[2] = -t; break;
    case 4: out[0] = s; out[1] = -t; out[2] = 1; break;
    default: out[0] = -s; out[1] = -t; out[2] = -1; break;
    }
}
// Rendered faces: the world is rendered from the cube's anchor, one face per
// frame, with a square 90-degree field of view, and copied into that face.
// The camera (-Z forward, +Y up, +X right) looks along the face's centre
// direction with up = -t; D3D's faces are left-handed, so its right is -s and
// the image goes into the face mirrored left to right.
inline constexpr unsigned RenderedFaces = 5; // All but -Y (straight down): water reflects upward.
inline unsigned renderedFace(unsigned k) { return k < 3 ? k : k + 1; } // +X, -X, +Y, +Z, -Z.
inline Quat basisQuat(Vec3 right, Vec3 up, Vec3 back) {
    // Rotation matrix columns right, up, back (a right-handed orthonormal basis).
    const float m00 = right.x, m11 = up.y, m22 = back.z, trace = m00 + m11 + m22;
    Quat q;
    if (trace > 0) {
        const float s = std::sqrt(trace + 1) * 2;
        q = {(up.z - back.y) / s, (back.x - right.z) / s, (right.y - up.x) / s, .25f * s};
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1 + m00 - m11 - m22) * 2;
        q = {.25f * s, (up.x + right.y) / s, (back.x + right.z) / s, (up.z - back.y) / s};
    } else if (m11 > m22) {
        const float s = std::sqrt(1 + m11 - m00 - m22) * 2;
        q = {(up.x + right.y) / s, .25f * s, (back.y + up.z) / s, (back.x - right.z) / s};
    } else {
        const float s = std::sqrt(1 + m22 - m00 - m11) * 2;
        q = {(back.x + right.z) / s, (back.y + up.z) / s, .25f * s, (right.y - up.x) / s};
    }
    return normalize(q);
}
inline Pose faceCamera(unsigned face, const float (&anchor)[3]) {
    float f[3], s[3], t[3];
    faceDirection(face, 0, 0, f);
    faceDirection(face, 1, 0, s);
    faceDirection(face, 0, 1, t);
    const Vec3 forward{f[0], f[1], f[2]}, sAxis = Vec3{s[0], s[1], s[2]} - forward, tAxis = Vec3{t[0], t[1], t[2]} - forward;
    return {basisQuat(sAxis * -1, tAxis * -1, forward * -1), Vec3{anchor[0], anchor[1], anchor[2]}};
}
inline Fov faceFov() { return {-Pi / 4, Pi / 4, Pi / 4, -Pi / 4}; }
// Capture constants c0..c3: clip x, y, (unused), w, as a dot product with
// (n, 1), of the point anchor + radius * n (n a unit direction from the
// anchor, world) seen by the camera.
inline void captureRows(const float *view, const float *projection, const float (&anchor)[3], float radius, float (&out)[16]) {
    for (unsigned r = 0; r < 4; ++r) {
        double translation = projection[4 * r + 3];
        for (unsigned k = 0; k < 3; ++k) {
            const double viewed = double(view[4 * k]) * anchor[0] + double(view[4 * k + 1]) * anchor[1] +
                                  double(view[4 * k + 2]) * anchor[2] + view[4 * k + 3];
            translation += double(projection[4 * r + k]) * viewed;
        }
        for (unsigned j = 0; j < 3; ++j) {
            double v = 0;
            for (unsigned k = 0; k < 3; ++k) v += double(projection[4 * r + k]) * view[4 * k + j];
            out[4 * r + j] = float(v * radius);
        }
        out[4 * r + 3] = float(translation);
    }
}
// Model -> world rotation (rows) of a draw from its model -> clip matrix
// (`wvp`, rows) and the camera's view and projection: the x, y and w rows of
// the projection are inverted (its last column is 0 for a perspective), the
// view's rotation transposed. False for a degenerate projection.
inline bool modelRotation(const float *wvp, const float *view, const float *projection, float (&out)[12]) {
    const unsigned rows[3]{0, 1, 3};
    double p[3][3], a[3][4];
    for (unsigned i = 0; i < 3; ++i) {
        for (unsigned k = 0; k < 3; ++k) p[i][k] = projection[4 * rows[i] + k];
        for (unsigned k = 0; k < 4; ++k) a[i][k] = wvp[4 * rows[i] + k];
        if (projection[4 * rows[i] + 3] != 0) return false;
    }
    const double det = p[0][0] * (p[1][1] * p[2][2] - p[1][2] * p[2][1]) - p[0][1] * (p[1][0] * p[2][2] - p[1][2] * p[2][0]) +
                       p[0][2] * (p[1][0] * p[2][1] - p[1][1] * p[2][0]);
    if (!std::isfinite(det) || std::abs(det) < 1e-12) return false;
    double inv[3][3];
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 3; ++j) {
            const unsigned i1 = (j + 1) % 3, i2 = (j + 2) % 3, j1 = (i + 1) % 3, j2 = (i + 2) % 3;
            inv[i][j] = (p[i1][j1] * p[i2][j2] - p[i1][j2] * p[i2][j1]) / det;
        }
    double c[3][3]; // Model -> view rotation (rows).
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 3; ++j) {
            c[i][j] = 0;
            for (unsigned k = 0; k < 3; ++k) c[i][j] += inv[i][k] * a[k][j];
        }
    for (unsigned i = 0; i < 3; ++i) {
        for (unsigned j = 0; j < 3; ++j) {
            double w = 0;
            for (unsigned k = 0; k < 3; ++k) w += double(view[4 * k + i]) * c[k][j]; // V^T C
            if (!std::isfinite(w)) return false;
            out[4 * i + j] = float(w);
        }
        out[4 * i + 3] = 0;
    }
    return true;
}
// Below the horizon (unnormalized direction y) and below this clip height the
// view is not captured: the guns, hands and the water itself are there.
constexpr float HorizonY = -.08f, ViewFloor = .05f;
// Only the middle of the view is captured (its edges carry the most view-
// dependent shading), blended in a little each frame: the cube settles on
// what the surroundings look like instead of whatever the head just faced.
constexpr float ViewEdge = .8f, CaptureBlend = .25f;
// ps_2_0 capture program: t0.xyz = the texel's direction, s0 = the eye image,
// c0..c3 = captureRows. Texels the eye does not see (behind, outside, too low)
// are discarded, so the cube keeps what earlier views put there.
inline std::vector<uint32_t> captureProgram() {
    const auto f = [](float v) { uint32_t u; std::memcpy(&u, &v, 4); return u; };
    const auto reg = [](unsigned type, unsigned num) { return 0x80000000u | ((type & 7u) << 28) | ((type & 0x18u) << 8) | num; };
    constexpr unsigned R = 0, C = 2, T = 3, Out = 8, S = 10;
    const auto d = [&](unsigned type, unsigned num, unsigned mask) { return reg(type, num) | (mask << 16); };
    const auto s = [&](unsigned type, unsigned num, unsigned swizzle = 0xe4, bool negate = false) {
        return reg(type, num) | (swizzle << 16) | (negate ? 1u << 24 : 0u);
    };
    return {
        0xffff0200u,
        0x05000051u, d(C, 4, 0xf), f(.5f), f(-.5f), f(1), f(0),
        0x05000051u, d(C, 5, 0xf), f(ViewFloor), f(HorizonY), f(ViewEdge), f(CaptureBlend),
        0x0200001fu, 0x80000000u, d(T, 0, 0x7),
        0x0200001fu, 0x90000000u, d(S, 0, 0xf),
        0x02000024u, d(R, 6, 0x7), s(T, 0),                                   // nrm r6.xyz, t0 (n)
        0x02000001u, d(R, 6, 0x8), s(C, 4, 0xaa),                             // mov r6.w, 1
        0x03000009u, d(R, 0, 0x1), s(R, 6), s(C, 0),                          // dp4 r0.x, (n,1), c0
        0x03000009u, d(R, 0, 0x2), s(R, 6), s(C, 1),                          // dp4 r0.y, (n,1), c1
        0x03000009u, d(R, 0, 0x8), s(R, 6), s(C, 3),                          // dp4 r0.w, (n,1), c3
        0x02000006u, d(R, 1, 0x8), s(R, 0, 0xff),                             // rcp r1.w, r0.w
        0x03000005u, d(R, 1, 0x3), s(R, 0), s(R, 1, 0xff),                    // mul r1.xy, r0, r1.w (ndc)
        0x02000023u, d(R, 2, 0x3), s(R, 1),                                   // abs r2.xy, r1
        0x03000002u, d(R, 2, 0x3), s(C, 5, 0xaa), s(R, 2, 0xe4, true),        // add r2.xy, edge, -|ndc|
        0x03000002u, d(R, 2, 0x4), s(R, 1, 0x55), s(C, 5, 0x00, true),        // add r2.z, ndc.y, -floor
        0x02000001u, d(R, 2, 0x8), s(C, 4, 0xaa),                             // mov r2.w, 1
        0x01000041u, d(R, 2, 0xf),                                            // texkill r2
        0x03000002u, d(R, 3, 0x1), s(T, 0, 0x55), s(C, 5, 0x55, true),        // add r3.x, dir.y, -horizon
        0x02000001u, d(R, 3, 0x2), s(R, 0, 0xff),                             // mov r3.y, w (in front)
        0x02000001u, d(R, 3, 0xc), s(C, 4, 0xaa),                             // mov r3.zw, 1
        0x01000041u, d(R, 3, 0xf),                                            // texkill r3
        0x04000004u, d(R, 4, 0x3), s(R, 1), s(C, 4), s(C, 4, 0x00),           // mad r4.xy, ndc, (.5,-.5), .5
        0x03000042u, d(R, 5, 0xf), s(R, 4), s(S, 0),                          // texld r5, r4, s0
        0x02000001u, d(R, 5, 0x8), s(C, 5, 0xff),                             // mov r5.w, blend
        0x02000001u, d(Out, 0, 0xf), s(R, 5),                                 // mov oC0, r5
        0x0000ffffu};
}
} // namespace ss2vr::env_probe
