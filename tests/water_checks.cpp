#include "common/water_reflection.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace ss2vr;
static bool near(float a, float b, float e = 1e-5f) { return std::abs(a - b) < e; }
int main() {
    // Centre camera turned 0.3 rad left (world -> view = Ry(0.3)^T); the left eye
    // sits 3.2 cm to the camera's left: in world space that is along -camera x.
    const float yaw = .3f, c = std::cos(yaw), s = std::sin(yaw);
    const float view[12]{c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0};
    const Matrix34 toLeft{{1, 0, 0, .032f, 0, 1, 0, 0, 0, 0, 1, 0}}; // x_eye = x_centre + 0.032
    const auto change = replay::eyeChange(toLeft, Fov{-.9f, .9f, .8f, -.8f}, Fov{-.9f, .9f, .8f, -.8f});
    float offset[3];
    assert(water::eyeOffset(view, change, offset));
    // Eye position in centre view space is (-0.032, 0, 0); camera x in world is (c, 0, -s).
    assert(near(offset[0], -.032f * c) && near(offset[1], 0) && near(offset[2], .032f * s));
    replay::EyeChange invalid;
    assert(!water::eyeOffset(view, invalid, offset));
    // The inverse of a turned, scaled model rotation.
    const float rows[12]{2 * c, 0, 2 * s, 9, 0, 2, 0, 9, -2 * s, 0, 2 * c, 9};
    float inv[9];
    assert(water::invert3(rows, inv));
    for (unsigned r = 0; r < 3; ++r)
        for (unsigned k = 0; k < 3; ++k) {
            float v = 0;
            for (unsigned j = 0; j < 3; ++j) v += rows[4 * r + j] * inv[3 * j + k];
            assert(near(v, r == k ? 1.f : 0.f));
        }
    const float flat[12]{};
    assert(!water::invert3(flat, inv));
    std::puts("Water reflection checks passed");
}
