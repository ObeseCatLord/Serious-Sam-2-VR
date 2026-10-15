#include "common/env_probe.hpp"
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
    // An asymmetric projection (w = -z) and a camera turned 0.4 rad about y.
    const float P[16]{1.2f, 0, .1f, 0, 0, 1.1f, -.05f, 0, 0, 0, -1, -.1f, 0, 0, -1, 0};
    const float c = std::cos(.4f), s = std::sin(.4f);
    // Camera rotation Ry (world from camera); view = Ry^T.
    const float view[12]{c, 0, -s, 3, 0, 1, 0, -2, s, 0, c, 7};
    float rows[16];
    captureRows(view, P, rows);
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
    std::puts("Environment probe checks passed");
}
