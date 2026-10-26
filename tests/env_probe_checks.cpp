#include "common/env_probe.hpp"
#include "common/weapon_view.hpp"
#include <utility>
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace ss2vr::env_probe;
static bool near(float a, float b, float e = 1e-4f) { return std::abs(a - b) < e; }
// Row-major 4x4 product.
static void mul(const float *a, const float *b, float *out) {
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned j = 0; j < 4; ++j) {
            float v = 0;
            for (unsigned k = 0; k < 4; ++k) v += a[4 * i + k] * b[4 * k + j];
            out[4 * i + j] = v;
        }
}
int main() {
    // Face centres point along the axes; the +Y face's top edge (t = -1) toward -Z.
    float d[3];
    faceDirection(0, 0, 0, d); assert(d[0] == 1 && d[1] == 0 && d[2] == 0);
    faceDirection(3, 0, 0, d); assert(d[1] == -1);
    faceDirection(5, 0, 0, d); assert(d[2] == -1);
    faceDirection(2, 0, -1, d); assert(d[1] == 1 && d[2] == -1);
    // Rendered faces: each camera looks along its face's centre, and a face texel
    // (s, t) lands at clip (-s, -t) of that camera's square 90-degree image.
    const float origin[3]{1, 2, 3};
    for (unsigned k = 0; k < RenderedFaces; ++k) {
        const unsigned face = renderedFace(k);
        assert(face != 3);
        const ss2vr::Pose cam = faceCamera(face, origin);
        float centre[3], texel[3];
        faceDirection(face, 0, 0, centre);
        const ss2vr::Vec3 forward = ss2vr::rotate(cam.q, {0, 0, -1});
        assert(std::abs(forward.x - centre[0]) < 1e-5f && std::abs(forward.y - centre[1]) < 1e-5f && std::abs(forward.z - centre[2]) < 1e-5f);
        assert(cam.p.x == 1 && cam.p.y == 2 && cam.p.z == 3);
        for (const auto [fs, ft] : {std::pair{.5f, -.25f}, std::pair{-.7f, .6f}}) {
            faceDirection(face, fs, ft, texel);
            const ss2vr::Vec3 local = ss2vr::rotate(ss2vr::inverse(cam.q), {texel[0], texel[1], texel[2]});
            const ss2vr::Matrix44 proj = ss2vr::projection(faceFov());
            const float w = -local.z, x = (proj.m[0] * local.x + proj.m[2] * local.z) / w, y = (proj.m[5] * local.y + proj.m[6] * local.z) / w;
            assert(std::abs(x + fs) < 1e-4f && std::abs(y + ft) < 1e-4f);
        }
    }
    // An asymmetric projection (w = -z) and a camera turned 0.4 rad about y.
    const float P[16]{1.2f, 0, .1f, 0, 0, 1.1f, -.05f, 0, 0, 0, -1, -.1f, 0, 0, -1, 0};
    const float c = std::cos(.4f), s = std::sin(.4f);
    // Camera rotation Ry (world from camera); view = Ry^T.
    const float view[12]{c, 0, -s, 3, 0, 1, 0, -2, s, 0, c, 7};
    float rows[16];
    // Anchored at the camera, the rows act on directions as the camera's rotation does.
    const float camera[3]{-(view[0] * view[3] + view[4] * view[7] + view[8] * view[11]),
                          -(view[1] * view[3] + view[5] * view[7] + view[9] * view[11]),
                          -(view[2] * view[3] + view[6] * view[7] + view[10] * view[11])};
    captureRows(view, P, camera, 1, rows);
    // The camera's forward in world (-Z of Ry = (-s, 0, -c)) projects to the centre (less the skew).
    const float fwd[3]{-s, 0, -c};
    const auto dot3 = [&](unsigned r, const float *v) { return rows[4 * r] * v[0] + rows[4 * r + 1] * v[1] + rows[4 * r + 2] * v[2]; };
    assert(near(dot3(3, fwd), 1) && near(dot3(0, fwd) / dot3(3, fwd), -.1f) && near(dot3(1, fwd) / dot3(3, fwd), .05f));
    // The camera's right (c, 0, -s) mixed in moves the point to +x.
    const float right[3]{fwd[0] + .3f * c, 0, fwd[2] - .3f * s};
    assert(dot3(0, right) / dot3(3, right) > .2f);
    // Behind the camera: w < 0.
    const float back[3]{s, 0, c};
    assert(dot3(3, back) < 0);
    // Model rotation from model -> clip: W = yaw 0.9 scaled 2, moved; V = the view above.
    // Anchored elsewhere: (n, 1) maps to the clip position of anchor + radius * n.
    {
        const float anchor[3]{4, -1, 2}, radius = 30;
        float at[16];
        captureRows(view, P, anchor, radius, at);
        const float n[3]{.6f, .0f, -.8f};
        const float X[3]{anchor[0] + radius * n[0], anchor[1] + radius * n[1], anchor[2] + radius * n[2]};
        float v4[4], clip[4];
        for (unsigned k = 0; k < 3; ++k) v4[k] = view[4 * k] * X[0] + view[4 * k + 1] * X[1] + view[4 * k + 2] * X[2] + view[4 * k + 3];
        v4[3] = 1;
        for (unsigned r = 0; r < 4; ++r) clip[r] = P[4 * r] * v4[0] + P[4 * r + 1] * v4[1] + P[4 * r + 2] * v4[2] + P[4 * r + 3] * v4[3];
        for (unsigned r : {0u, 1u, 3u})
            assert(near(at[4 * r] * n[0] + at[4 * r + 1] * n[1] + at[4 * r + 2] * n[2] + at[4 * r + 3], clip[r], 1e-3f));
    }
    const float cw = std::cos(.9f) * 2, sw = std::sin(.9f) * 2;
    const float W[16]{cw, 0, sw, 10, 0, 2, 0, -1, -sw, 0, cw, 4, 0, 0, 0, 1};
    const float V4[16]{view[0], view[1], view[2], view[3], view[4], view[5], view[6], view[7], view[8], view[9], view[10], view[11], 0, 0, 0, 1};
    float VW[16], wvp[16], rot[12];
    mul(V4, W, VW); mul(P, VW, wvp);
    assert(modelRotation(wvp, view, P, rot));
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 3; ++j) assert(near(rot[4 * i + j], W[4 * i + j], 1e-3f));
    const float zero[16]{};
    assert(!modelRotation(wvp, view, zero, rot));
    // The capture program is a closed ps_2_0 program.
    const auto program = captureProgram();
    assert(program.front() == 0xffff0200u && program.back() == 0x0000ffffu && program.size() < 160);
    {
        // Moving a face's near plane from 0.1 m to 15 m, finite far (1000 m) and infinite far.
        const auto depthAt = [](const ss2vr::Matrix44 &p, float distance) {
            return (p.m[10] * -distance + p.m[11]) / distance; // z / w at that view distance.
        };
        const float n = .1f, f = 1000;
        ss2vr::Matrix44 p{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, f / (n - f), f * n / (n - f), 0, 0, -1, 0}};
        float nearWas = 0, farWas = 0;
        assert(ss2vr::withNearPlane(p, 15, &nearWas, &farWas) && std::abs(nearWas - n) < 1e-3f && std::abs(farWas - f) < .5f);
        assert(std::abs(depthAt(p, 15)) < 1e-4f && std::abs(depthAt(p, f) - 1) < 1e-3f && depthAt(p, 10) < 0);
        ss2vr::Matrix44 q{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, -n, 0, 0, -1, 0}};
        assert(ss2vr::withNearPlane(q, 15, &nearWas, &farWas) && std::isinf(farWas));
        assert(std::abs(depthAt(q, 15)) < 1e-4f && depthAt(q, 1e6f) < 1 && depthAt(q, 10) < 0);
        // Not that form: left alone.
        ss2vr::Matrix44 r = p; r.m[14] = 1;
        assert(!ss2vr::withNearPlane(r, 20));
    }
    std::puts("Environment probe checks passed");
}
