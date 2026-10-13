#include "common/two_hand.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace ss2vr;
static bool near(Vec3 a, Vec3 b, float e = 1e-4f) {
    return std::abs(a.x - b.x) < e && std::abs(a.y - b.y) < e && std::abs(a.z - b.z) < e;
}
int main() {
    const Pose held{{0, 0, 0, 1}, {.2f, 1.2f, -.3f}};
    // Free hand 40 cm ahead and 8 cm below the barrel: grabbable; behind, beside or above: not.
    const Vec3 grab = held.p + Vec3{0, -.08f, -.4f};
    const Vec3 local = twoHandLocal(held, grab);
    assert(near(local, {0, -.08f, -.4f}));
    assert(twoHandGrabbable(local, 1.5f));
    assert(!twoHandGrabbable({0, -.08f, .1f}, 1.5f));
    assert(!twoHandGrabbable({.3f, -.08f, -.4f}, 1.5f));
    assert(!twoHandGrabbable({0, .3f, -.4f}, 1.5f));
    assert(!twoHandGrabbable({0, -.08f, -2.f}, 1.5f));
    assert(!twoHandGrabbable({NAN, 0, -.4f}, 1.5f));
    // At the grab the weapon does not move.
    const Pose same = twoHandAim(held, local, grab);
    assert(near(rotate(same.q, {0, 0, -1}), {0, 0, -1}) && near(same.p, held.p));
    // Free hand moves to the left: the barrel swings left, the grabbed point follows it.
    const Vec3 left = held.p + Vec3{-.4f, -.08f, -.4f};
    const Pose swung = twoHandAim(held, local, left);
    const Vec3 to = left - held.p;
    const float n = std::sqrt(dot(to, to)), m = std::sqrt(dot(local, local));
    assert(near(rotate(swung.q, local * (1 / m)), to * (1 / n)));
    assert(rotate(swung.q, {0, 0, -1}).x < -.5f);
    assert(near(swung.p, held.p));
    // Free hand on the holding hand keeps the one-hand pose.
    const Pose kept = twoHandAim(held, local, held.p);
    assert(near(rotate(kept.q, {0, 0, -1}), {0, 0, -1}));
    // Opposite vectors do not flip the weapon.
    const Quat flip = rotationBetween({0, 0, -1}, {0, 0, 1});
    assert(flip.w == 1);
    std::puts("Two-handed hold checks passed");
}
