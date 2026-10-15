#include "common/turret_look.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace ss2vr;
int main() {
    const Vec3 start{.3f, .1f, .2f};
    // Stick right turns right (heading down), left turns left.
    assert(advanceTurretLook(start, 1, 0, 2, .1f).x < start.x);
    assert(advanceTurretLook(start, -1, 0, 2, .1f).x > start.x);
    // Stick up looks up (pitch up), down looks down.
    assert(advanceTurretLook(start, 0, 1, 2, .1f).y > start.y);
    assert(advanceTurretLook(start, 0, -1, 2, .1f).y < start.y);
    // Full deflection moves at the rate; the dead zone holds still; bank is zero.
    assert(std::abs(advanceTurretLook(start, 1, 0, 2, .1f).x - (start.x - .2f)) < 1e-6f);
    // Half way past the dead zone moves at a quarter of the rate (squared response).
    assert(std::abs(advanceTurretLook(start, .575f, 0, 2, .1f).x - (start.x - .05f)) < 1e-5f);
    const Vec3 held = advanceTurretLook(start, .1f, -.1f, 2, .1f);
    assert(held.x == start.x && held.y == start.y && held.z == 0);
    // Pitch stays within +-1.4 rad; bad input does nothing.
    assert(advanceTurretLook({0, 1.39f, 0}, 0, 1, 10, 1).y == 1.4f);
    assert(advanceTurretLook({0, -1.39f, 0}, 0, -1, 10, 1).y == -1.4f);
    assert(advanceTurretLook(start, NAN, NAN, 2, .1f).x == start.x);
    std::puts("Turret stick look checks passed");
}
