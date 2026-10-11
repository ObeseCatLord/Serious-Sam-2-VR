#include "common/wear_calibration.hpp"
#include <cassert>
#ifdef NDEBUG
#error Wear calibration checks require active assertions
#endif
using namespace ss2vr;

static Pose at(float x, float y, float z) { return {{0, 0, 0, 1}, {x, y, z}}; }

int main() {
    // Desk: motionless capture, then the user lifts and wears the headset.
    {
        WearCalibration w;
        const Pose desk = at(0, .75f, 0);
        w.captureOrigin(desk, 1000);
        uint64_t t = 1000;
        for (; t <= 3100; t += 11) assert(!w.sample(at(0, .75f + .0004f, 0), t, desk.p)); // Sub-mm jitter.
        assert(w.state == WearCalibration::State::Provisional);
        bool recentred = false;
        for (unsigned i = 0; i < 40; ++i, t += 11) assert(!w.sample(at(0, .75f + i * .025f, -.1f), t, desk.p));
        for (unsigned i = 0; i < 200 && !recentred; ++i, t += 11)
            recentred = w.sample(at(.01f * (i % 3), 1.72f, -.1f), t, desk.p); // Worn: small motion.
        assert(recentred && w.state == WearCalibration::State::Idle);
        assert(!w.sample(at(0, 1.72f, -.1f), t + 20, desk.p)); // Exactly once.
    }
    // Worn at capture: natural head motion means the origin is trusted.
    {
        WearCalibration w;
        const Pose head = at(0, 1.7f, 0);
        w.captureOrigin(head, 0);
        assert(!w.sample(at(.004f, 1.7f, 0), 30, head.p));
        assert(w.state == WearCalibration::State::Idle);
        for (uint64_t t = 40; t < 5000; t += 11) assert(!w.sample(at(.5f, 1.2f, 0), t, head.p)); // Crouch/lean.
    }
    // Rotation alone also proves the headset is worn.
    {
        WearCalibration w;
        w.captureOrigin(at(0, .75f, 0), 0);
        Pose turned = at(0, .75f, 0);
        turned.q = {0, .01f, 0, .99995f};
        w.sample(turned, 20, {0, .75f, 0});
        assert(w.state == WearCalibration::State::Idle);
    }
    // A provisional origin that never settles away does not recentre; reset cancels.
    {
        WearCalibration w;
        w.captureOrigin(at(0, .75f, 0), 0);
        for (uint64_t t = 0; t <= 2100; t += 50) w.sample(at(0, .75f, 0), t, {0, .75f, 0});
        assert(w.state == WearCalibration::State::Provisional);
        for (uint64_t t = 2200; t < 6000; t += 11) // Moving continuously: never settled.
            assert(!w.sample(at(.3f + .06f * float(t % 2), 1.2f, 0), t, {0, .75f, 0}));
        w.reset();
        assert(!w.sample(at(0, 1.7f, 0), 7000, {0, .75f, 0}) && w.state == WearCalibration::State::Idle);
    }
    return 0;
}
