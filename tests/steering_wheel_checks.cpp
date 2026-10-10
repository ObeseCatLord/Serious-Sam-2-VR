#include "common/steering_wheel.hpp"
#include <cassert>
#include <cmath>
#ifdef NDEBUG
#error Steering wheel checks require active assertions
#endif
using namespace ss2vr;

namespace {
SteeringWheelSettings settings;
Vec3 rimPoint(float phase, float radial = SteeringWheelRim, float z = 0) {
    const auto wheel = steeringWheelPose(settings);
    return wheel.p + rotate(wheel.q, Vec3{radial * std::cos(phase), radial * std::sin(phase), z});
}
struct Rig {
    Input input{};
    uint32_t poses[2]{1, 1};
    SteeringWheelGrasp grasp;
    Rig() {
        input.session = input.reference = 1;
        input.headValid = 1;
        input.head = {{0, 0, 0, 1}, {0, 0, 0}};
        input.gripValid[0] = input.gripValid[1] = 1;
        input.grip[0] = {{0, 0, 0, 1}, rimPoint(Pi)};
        input.grip[1] = {{0, 0, 0, 1}, rimPoint(0)};
        input.wheelAdmissionMask = 3;
        input.wheelInputEpoch[0] = input.wheelInputEpoch[1] = 1;
    }
    bool step(bool fresh = true, bool eligible = true, float dt = .011f) {
        if (fresh) { ++input.sequence; ++input.tickMs; }
        return grasp.update(settings, input, poses, Pose{{0, 0, 0, 1}, {}}, 0, 7, eligible, dt);
    }
    void place(unsigned h, float phase) { input.grip[h].p = rimPoint(phase); }
};
bool near(float a, float b, float e = 1e-4f) { return std::abs(a - b) <= e; }

void oneHandTurn() {
    Rig r;
    assert(!r.step()); assert(!r.step()); // Key the stream, then observe a release.
    r.input.buttons[1] = Wheel;
    assert(r.step() && r.grasp.angle == 0); // Grabbing sets a baseline only.
    r.place(1, -.3f);                      // Clockwise as seen by the driver.
    assert(r.step() && near(r.grasp.angle, -.3f));
    assert(r.grasp.steer(settings) > 0);   // Clockwise steers right.
    assert(r.step(false) && near(r.grasp.angle, -.3f)); // Cached input never integrates twice.
    r.place(1, -.3f + 2 * Pi - .1f);       // Phase wrap is a small turn, not a full spin.
    assert(r.step() && near(r.grasp.angle, -.4f));
}
void twoHandsAndTransfer() {
    Rig r;
    r.step(); r.step();
    r.input.buttons[0] = r.input.buttons[1] = Wheel;
    assert(r.step());
    r.place(0, Pi + .2f); r.place(1, .2f); // Both hands turn left together.
    assert(r.step() && near(r.grasp.angle, .2f));
    r.input.buttons[1] = 0;                // Release one hand: no jump.
    assert(r.step() && near(r.grasp.angle, .2f));
    r.place(0, Pi + .3f);
    assert(r.step() && near(r.grasp.angle, .3f));
    r.place(1, 1.0f); r.input.buttons[1] = Wheel; // Rejoin elsewhere on the rim: baseline only.
    assert(r.step() && near(r.grasp.angle, .3f));
}
void releaseRearmAndCentering() {
    Rig r;
    r.step(); r.step();
    r.input.buttons[1] = Wheel; r.step();
    r.place(1, .5f); r.step();
    assert(near(r.grasp.angle, .5f));
    r.input.buttons[1] = 0;
    assert(!r.step(true, true, .05f) && r.grasp.angle < .5f && r.grasp.angle > 0); // Self-centering.
    for (int i = 0; i < 10; ++i) r.step(true, true, .05f);
    assert(r.grasp.angle == 0);
    // A press away from the rim consumes arming; sliding onto the rim cannot auto-grab.
    r.input.grip[1].p = rimPoint(0, .4f);
    r.input.buttons[1] = Wheel;
    assert(!r.step());
    r.place(1, 0);
    assert(!r.step());
    r.input.buttons[1] = 0; r.step();
    r.input.buttons[1] = Wheel;
    assert(r.step());
}
void availabilityLoss() {
    Rig r;
    r.step(); r.step();
    r.input.buttons[1] = Wheel; assert(r.step());
    r.poses[1] = 2;                        // Grip-pose interruption.
    assert(!r.step());
    assert(!r.step() && !r.grasp.hands[1].armed); // Still-held button cannot regrab.
    r.input.buttons[1] = 0; r.step();
    r.input.buttons[1] = Wheel; assert(r.step());
    r.input.wheelInputEpoch[1] = 2;        // New squeeze epoch.
    assert(!r.step());
    r.input.buttons[1] = 0; r.step();
    r.input.buttons[1] = Wheel; assert(r.step());
    assert(!r.step(true, false));          // Ineligible ride/seat drops the grasp.
    r.input.buttons[1] = 0;
    assert(!r.step());
    r.input.buttons[1] = Wheel; assert(!r.step()); // Needs a release observed after rekeying.
    r.input.buttons[1] = 0; r.step(); r.input.buttons[1] = Wheel; assert(r.step());
    r.input.gripValid[1] = 0;
    assert(!r.step());
}
void limits() {
    Rig r;
    r.step(); r.step();
    r.input.buttons[1] = Wheel; r.step();
    float phase = 0;
    for (int i = 0; i < 40; ++i) { phase += .2f; r.place(1, phase); r.step(); }
    assert(near(r.grasp.angle, settings.maxAngleDegrees * Pi / 180));
    assert(r.grasp.steer(settings) == -1);
    const Pose spun = steeringWheelSpin(settings, Pi / 2);
    const Vec3 right = rotate(spun.q, Vec3{1, 0, 0}), base = rotate(steeringWheelPose(settings).q, Vec3{0, 1, 0});
    assert(near(dot(right, base), 1, 1e-4f)); // Spin turns the rim about its own axis.
}
void hubAndReversal() {
    Rig r;
    r.step(); r.step();
    r.input.buttons[1] = Wheel; r.step();
    r.input.grip[1].p = rimPoint(1.f, .02f); // Hand slides to the hub.
    assert(!r.step() && r.grasp.angle == 0);   // Grasp dropped, no spin.
    r.place(1, 0); assert(!r.step());          // Held button cannot regrab.
    r.input.buttons[1] = 0; r.step(); r.input.buttons[1] = Wheel; assert(r.step());
    float phase = 0;
    for (int i = 0; i < 30; ++i) { phase -= .2f; r.place(1, phase); r.step(); }
    const float limit = -settings.maxAngleDegrees * Pi / 180;
    assert(near(r.grasp.angle, limit));
    phase += .1f; r.place(1, phase); r.step();  // Reversal acts immediately: no wind-up.
    assert(near(r.grasp.angle, limit + .1f));
}
void oppositeHandsAndTiming() {
    Rig r;
    r.step(); r.step();
    r.input.buttons[0] = r.input.buttons[1] = Wheel; r.step();
    r.place(0, Pi + .2f); r.place(1, -.2f);    // Hands fight: averaged turn cancels.
    assert(r.step() && near(r.grasp.angle, 0));
    r.input.buttons[0] = 0; r.place(1, .3f); r.step(); // Right hand turns left alone.
    assert(near(r.grasp.angle, .5f));
    r.input.buttons[1] = 0;
    assert(!r.step(true, true, std::nanf("")) && near(r.grasp.angle, .5f)); // NaN dt holds still.
    assert(r.grasp.steering());                // Released rim still steers while recentering.
    r.step(true, true, 5.f);                   // Huge dt is clamped to 0.1 s.
    assert(near(r.grasp.angle, .5f - .6f < 0 ? 0 : .5f - .6f) && !r.grasp.steering());
}
void streamChanges() {
    Rig r;
    r.step(); r.step();
    r.input.buttons[1] = Wheel; assert(r.step());
    ++r.input.session;                         // New XR session rekeys and drops the grasp.
    assert(!r.step());
    r.input.buttons[1] = 0; r.step(); r.input.buttons[1] = Wheel; assert(r.step());
    r.input.sequence -= 5;                     // Sequence regression is a new stream.
    assert(!r.step(false));
}
} // namespace

int main() {
    hubAndReversal();
    oppositeHandsAndTiming();
    streamChanges();
    oneHandTurn();
    twoHandsAndTransfer();
    releaseRearmAndCentering();
    availabilityLoss();
    limits();
    return 0;
}
