#include "common/water_reflection.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace ss2vr;
using replay::M4;
static bool near(double a, double b, double e = 1e-5) { return std::abs(a - b) < e; }
static M4 apply(const M4 &m, const double (&p)[4], double (&out)[4]) {
    for (unsigned r = 0; r < 4; ++r) out[r] = m[4 * r] * p[0] + m[4 * r + 1] * p[1] + m[4 * r + 2] * p[2] + m[4 * r + 3] * p[3];
    return m;
}
int main() {
    // A centre camera looking along world -Z, turned 0.3 rad left (yaw) and level.
    const float yaw = .3f;
    replay::ViewState s;
    const Matrix44 p = projection(Fov{-.9f, .9f, .8f, -.8f}, .1f, 1000.f);
    std::memcpy(s.projection.data(), p.m, sizeof(p.m));
    std::memcpy(s.adjusted.data(), p.m, sizeof(p.m));
    const float c = std::cos(yaw), sn = std::sin(yaw);
    // world -> view rotation = Ry(yaw)^T, camera at (1, 2, 3).
    const float R[9]{c, 0, -sn, 0, 1, 0, sn, 0, c};
    const float eye[3]{1, 2, 3};
    for (unsigned r = 0; r < 3; ++r) {
        for (unsigned k = 0; k < 3; ++k) s.view[4 * r + k] = R[3 * r + k];
        s.view[4 * r + 3] = -(R[3 * r] * eye[0] + R[3 * r + 1] * eye[1] + R[3 * r + 2] * eye[2]);
    }
    double vy = 0, vp = 0;
    assert(water::viewAngles(s.view.data(), vy, vp) && near(vy, yaw) && near(vp, 0));
    // Left eye 3.2 cm left of the centre, parallel.
    const Matrix34 toLeft{{1, 0, 0, .032f, 0, 1, 0, 0, 0, 0, 1, 0}};
    const auto change = replay::eyeChange(toLeft, Fov{-.9f, .9f, .8f, -.8f}, Fov{-.95f, .85f, .8f, -.8f});
    water::Reference ref;
    water::follow(ref, s.view.data(), 0);
    assert(ref.seeded && near(ref.yaw, yaw));
    // Reference aimed like the view: K is the centre projection moved to the eye
    // (AP_c * E * AP_c^-1 for a parallel eye).
    M4 k;
    assert(water::eyeClip(s, change, ref, k));
    const M4 apc = replay::m4(s.adjusted.data(), 4);
    M4 apcInv;
    assert(replay::invert(apc, apcInv));
    const M4 expected = replay::multiply(replay::multiply(apc, change.view), apcInv);
    for (unsigned i = 0; i < 16; ++i) assert(near(k[i], expected[i], 1e-6));
    // Reference turned away: a far point keeps its place in the world. Two eyes
    // agree on a point at (near) infinity: same reference texcoord up to parallax.
    water::Reference turned = ref;
    turned.yaw += .2;
    const Matrix34 toRight{{1, 0, 0, -.032f, 0, 1, 0, 0, 0, 0, 1, 0}};
    const auto right = replay::eyeChange(toRight, Fov{-.85f, .95f, .8f, -.8f}, Fov{-.9f, .9f, .8f, -.8f});
    M4 kl, kr;
    assert(water::eyeClip(s, change, turned, kl) && water::eyeClip(s, right, turned, kr));
    // A far centre-view point straight ahead, as centre clip coordinates.
    const double far[4]{0, -50, -900, 1};
    double clip[4], l[4], r[4];
    M4 ap = replay::m4(s.adjusted.data(), 4);
    apply(ap, far, clip);
    apply(kl, clip, l);
    apply(kr, clip, r);
    assert(std::abs(l[0] / l[3] - r[0] / r[3]) < 2e-4 && std::abs(l[1] / l[3] - r[1] / r[3]) < 2e-4);
    // Turned 0.2 rad: that point now sits off the reference axis (it moved in the image).
    double straight[4];
    apply(k, clip, straight);
    assert(std::abs(l[0] / l[3] - straight[0] / straight[3]) > .1);
    // Following: 1 s at tau 1 s covers 63% of a small turn; a large jump snaps.
    water::Reference f = ref;
    f.yaw = yaw - .2;
    water::follow(f, s.view.data(), 1., 1.);
    assert(near(f.yaw, yaw - .2 * std::exp(-1.), 1e-6));
    f.yaw = yaw - 2;
    water::follow(f, s.view.data(), .01, 1.);
    assert(near(f.yaw, yaw));
    std::puts("Water reflection checks passed");
}
