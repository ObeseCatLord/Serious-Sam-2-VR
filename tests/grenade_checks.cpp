#include "common/grenade_throw.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace ss2vr;
static bool near(Vec3 a, Vec3 b, float e = 1e-3f) {
    return std::abs(a.x - b.x) < e && std::abs(a.y - b.y) < e && std::abs(a.z - b.z) < e;
}
int main() {
    const Vec3 forward{0, 0, -1};
    {
        // Quick throw: launches at once from the given hand, and times out like a throw.
        GrenadeThrow q;
        q.launchNow(1, 500);
        assert(q.state == GrenadeThrow::State::Launching && q.hand == 1 && q.holding());
        assert(!q.update(true, {0, 1, 0}, 500 + GrenadeThrow::LaunchTimeout + 1, forward) && !q.holding());
        q.launchNow(1, 900);
        q.thrown();
        assert(!q.holding());
    }
    GrenadeThrow g;
    // Idle: motion does nothing.
    assert(!g.update(true, {0, 1, 0}, 100, forward) && !g.holding());
    g.press(0);
    assert(g.holding() && g.hand == 0);
    // Holding still or moving slowly does not throw.
    uint64_t t = 1000;
    for (int i = 0; i < 20; ++i, t += 11) assert(!g.update(true, {0, 1, -.002f * i}, t, forward));
    // A fast backward swing (wind-up) does not throw.
    for (int i = 0; i < 10; ++i, t += 11) assert(!g.update(true, {0, 1.2f, .03f * i}, t, forward));
    // A fast forward swing throws, once.
    bool threw = false;
    for (int i = 0; i < 10 && !threw; ++i, t += 11) threw = g.update(true, {0, 1.3f, .3f - .04f * i}, t, forward);
    assert(threw && g.state == GrenadeThrow::State::Launching);
    assert(!g.update(true, {0, 1.3f, -.5f}, t + 11, forward));
    g.thrown();
    assert(!g.holding());
    // The game not taking the throw ends it after the timeout.
    g.press(1);
    for (int i = 0; i < 10 && g.state == GrenadeThrow::State::Held; ++i, t += 11) g.update(true, {0, 1, -.05f * i}, t, forward);
    assert(g.state == GrenadeThrow::State::Launching);
    g.update(true, {}, t + GrenadeThrow::LaunchTimeout + 1, forward);
    assert(!g.holding());
    // Pressing again puts it away; tracking loss restarts the measurement.
    g.press(0);
    g.press(0);
    assert(!g.holding());
    g.press(0);
    assert(!g.update(true, {0, 1, 0}, t + 1000, forward));
    assert(!g.update(false, {0, 1, -1}, t + 1011, forward));
    assert(!g.update(true, {0, 1, -1}, t + 1022, forward)); // One sample after the loss: no speed yet.
    // Launch pose: toward the target, level, no roll.
    const Pose p = grenadeLaunchPose({1, 1, 1}, {1, 1, -9}, true, {1, 0, 0});
    assert(near(rotate(p.q, {0, 0, -1}), {0, 0, -1}) && near(p.p, {1, 1, 1}));
    const Pose up = grenadeLaunchPose({0, 0, 0}, {3, 4, 0}, true, {0, 0, -1});
    assert(near(rotate(up.q, {0, 0, -1}), {.6f, .8f, 0}));
    assert(std::abs(rotate(up.q, {1, 0, 0}).y) < 1e-4f); // Right stays level.
    const Pose fallback = grenadeLaunchPose({0, 0, 0}, {NAN, 0, 0}, false, {-1, 0, 0});
    assert(near(rotate(fallback.q, {0, 0, -1}), {-1, 0, 0}));
    std::puts("Grenade throw checks passed");
}
