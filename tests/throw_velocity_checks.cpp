#include "common/throw_velocity.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace ss2vr;
static bool near(float a, float b, float e = 1e-3f) { return std::fabs(a - b) < e; }
int main() {
    // Runtime velocities: the peak in the window wins, averaged with its neighbours.
    ThrowTracker t;
    const float speeds[] = {1, 2, 6, 7, 6, 3, 1};
    for (int i = 0; i < 7; ++i) {
        ThrowHandSample s;
        s.tick = s.poseTime = i * .011;
        s.linear = {0, 0, -speeds[i]};
        s.angular = {0, 1, 0};
        s.valid = LinearVelocityValid | AngularVelocityValid;
        t.addHand(s);
    }
    ThrowMotion m = t.peak(6 * .011, .2);
    assert(m.valid && m.runtime && near(m.linear.z, -(6 + 7 + 6) / 3.f) && near(m.angular.y, 1));
    // Out of the window: only the newest.
    m = t.peak(6 * .011 + 1, .2);
    assert(m.valid && near(m.linear.z, -2)); // The newest, averaged with its neighbour.
    // Poses only: differenced over >= 35 ms of pose time, never one noisy frame.
    ThrowTracker p;
    for (int i = 0; i < 20; ++i) {
        ThrowHandSample s;
        s.tick = i * .011;
        s.poseTime = i * .011;
        s.hand.p = {0, 0, -3.f * float(s.poseTime)}; // 3 m/s forward.
        if (i == 19) s.hand.p.z -= .02f;              // A 2 cm jitter on the newest pose.
        p.addHand(s);
    }
    m = p.peak(19 * .011, .2);
    assert(m.valid && !m.runtime && std::fabs(m.linear.z) < 3.6f && std::fabs(m.linear.z) > 2.9f);
    // Body motion adds unboosted; the boost scales the hand part only.
    ThrowTracker b;
    for (int i = 0; i < 10; ++i) b.addBody(i * .016, {-6.f * i * .016f, 0, 0}); // Backpedal 6 m/s along -x.
    const Vec3 body = b.bodyVelocity();
    assert(near(body.x, -6, .01f));
    ThrowMotion hand;
    hand.valid = true;
    hand.linear = {3, 0, 0};
    const Vec3 v = throwVelocity(hand, Quat{}, {}, {}, body, 4);
    assert(near(v.x, -6 + 12, .02f));
    // The reference-to-world rotation follows the turn: a hand moving -z in the
    // reference space with a 90 degree turn moves along -x in the world.
    const Quat r = referenceToWorld(Pose{}, Pose{}, float(M_PI / 2));
    const Vec3 w = rotate(r, Vec3{0, 0, -1});
    assert(near(w.x, -1) && near(w.z, 0));
    // Swing about the grip: a prop 0.3 m beyond the hand gains omega x r.
    ThrowMotion spin;
    spin.valid = true;
    spin.angular = {0, 0, 10};
    const Vec3 lever = throwVelocity(spin, Quat{}, {}, {.3f, 0, 0}, {}, 1);
    assert(near(lever.y, 3));
    // Runtime velocities flagged valid but zero (the lab runtime): the poses win.
    ThrowTracker z;
    for (int i = 0; i < 20; ++i) {
        ThrowHandSample s;
        s.tick = s.poseTime = i * .011;
        s.hand.p = {0, 0, -4.f * float(s.poseTime)};
        s.valid = LinearVelocityValid | AngularVelocityValid;
        z.addHand(s);
    }
    m = z.peak(19 * .011, .2);
    assert(m.valid && !m.runtime && std::fabs(m.linear.z + 4) < .05f);
    std::puts("throw velocity checks passed");
}
