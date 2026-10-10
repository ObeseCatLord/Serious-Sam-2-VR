#pragma once
#include "controls.hpp"
#include "math.hpp"
#include "rider.hpp"
#include <algorithm>
#include <cmath>

namespace ss2vr {
// Virtual steering wheel for rides without authored driver controls. The wheel
// is placed relative to the seated rig anchor (the native seated eye with body
// orientation); hands use the same bodyHandTracking mapping, so vehicle motion
// is never mistaken for turning. Its frame: X right, Y up, Z toward the driver.
struct SteeringWheelSettings {
    bool enabled = true, previewOnFoot = false;
    Vec3 offset{0, -.38f, -.42f};
    float tiltDegrees = 25, scale = 1;
    float maxAngleDegrees = 135, fullLockDegrees = 90, turnRateDegrees = 120;
};
inline constexpr float SteeringWheelRim = .175f;
inline Pose steeringWheelPose(const SteeringWheelSettings &s) {
    // Tilt the wheel's driver-facing normal upward, like a car's column.
    const float tilt = -s.tiltDegrees * Pi / 180;
    return {{std::sin(tilt * .5f), 0, 0, std::cos(tilt * .5f)}, s.offset};
}
// Rotation of the rim about its axis; positive is counterclockwise as seen by
// the driver (a left turn).
inline Pose steeringWheelSpin(const SteeringWheelSettings &s, float angle) {
    const auto base = steeringWheelPose(s);
    return compose(base, Pose{{0, 0, std::sin(angle * .5f), std::cos(angle * .5f)}, {}});
}

struct SteeringWheelGrasp {
    struct Hand {
        InputSampleBoundary blocked;
        uint32_t squeeze = 0, pose = 0;
        float phase = 0; // Hand angle around the axis at the last sample.
        bool armed = false, held = false;
    };
    Hand hands[2];
    uint64_t sequence = 0, session = 0, reference = 0;
    uint32_t producer = 0;
    float angle = 0; // Current rim rotation (radians).
    bool keyed = false;

    void cancel(const Input &input, uint32_t currentProducer) {
        for (auto &hand : hands) {
            hand.armed = hand.held = false;
            hand.blocked = {input.session, input.reference, input.sequence, input.tickMs, currentProducer};
        }
    }
    // Ride/seat/rig changes interrupt without guessing continuity.
    void reset() {
        for (auto &hand : hands) hand = {};
        keyed = false; angle = 0; sequence = 0;
    }
    bool held() const { return hands[0].held || hands[1].held; }
    // Steering continues while held and while the released rim recentres.
    bool steering() const { return held() || angle != 0; }
    // Steering command in [-1, 1]; positive steers right.
    float steer(const SteeringWheelSettings &s) const {
        const float lock = std::max(1.f, s.fullLockDegrees) * Pi / 180;
        return std::clamp(-angle / lock, -1.f, 1.f);
    }
    // Hand position in the unrotated wheel frame, from rig-relative grip pose.
    static Vec3 local(const SteeringWheelSettings &s, const Input &input, const Pose &origin, float turn, unsigned h) {
        const auto grip = bodyHandTracking(origin, turn, input.head, input.grip[h]);
        const auto wheel = steeringWheelPose(s);
        return rotate(inverse(wheel.q), grip.p - wheel.p) * (1 / std::max(.25f, s.scale));
    }
    static bool onRim(Vec3 p) {
        const float radial = std::sqrt(p.x * p.x + p.y * p.y);
        return std::isfinite(radial) && std::abs(radial - SteeringWheelRim) <= .07f && std::abs(p.z) <= .09f &&
            radial > .05f;
    }
    // Returns true while at least one hand holds the rim. dt is seconds since
    // the previous call and drives self-centering after release.
    bool update(const SteeringWheelSettings &s, const Input &input, const uint32_t poseGeneration[2],
                const Pose &origin, float turn, uint32_t currentProducer, bool eligible, float dt) {
        dt = std::isfinite(dt) ? std::clamp(dt, 0.f, .1f) : 0.f;
        const bool sameStream = keyed && session == input.session && reference == input.reference &&
            producer == currentProducer && input.sequence >= sequence;
        if (!eligible || !finite(origin) || !std::isfinite(turn)) {
            cancel(input, currentProducer);
            keyed = false;
        } else if (!sameStream) {
            cancel(input, currentProducer);
            keyed = true; session = input.session; reference = input.reference;
            producer = currentProducer; sequence = input.sequence;
        }
        const bool fresh = keyed && input.sequence != sequence;
        float turned = 0;
        unsigned turning = 0;
        if (keyed)
            for (unsigned h = 0; h < 2; ++h) {
                auto &hand = hands[h];
                const bool changed = hand.squeeze != input.wheelInputEpoch[h] || hand.pose != poseGeneration[h];
                const bool available = poseGeneration[h] && rideGripPoseEligible(input, h) &&
                    (input.wheelAdmissionMask & (1u << h)) && input.wheelInputEpoch[h];
                if (changed || !available) {
                    // Availability loss or a new action epoch needs a fresh release then press.
                    hand.armed = hand.held = false;
                    hand.blocked = {input.session, input.reference, input.sequence, input.tickMs, currentProducer};
                    hand.squeeze = input.wheelInputEpoch[h]; hand.pose = poseGeneration[h];
                    continue;
                }
                if (!fresh) continue;
                const bool down = (input.buttons[h] & Wheel) != 0;
                const auto p = local(s, input, origin, turn, h);
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) { hand.held = hand.armed = false; continue; }
                const float phase = std::atan2(p.y, p.x);
                if (!down) {
                    hand.held = false;
                    if (hand.blocked.permits(input, input.tickMs, currentProducer)) hand.armed = true;
                } else if (hand.held && p.x * p.x + p.y * p.y < .05f * .05f) {
                    // Near the hub the phase is singular: drop the grasp; a regrab rebaselines.
                    hand.held = hand.armed = false;
                } else if (hand.held) {
                    // Keep holding even if the hand drifts off the rim; only phase drives rotation.
                    const float difference = phase - hand.phase;
                    turned += std::atan2(std::sin(difference), std::cos(difference));
                    ++turning;
                    hand.phase = phase;
                } else if (hand.armed && onRim(p)) {
                    // A new grip only sets its own baseline: joining never jumps the rim.
                    hand.armed = false; hand.held = true; hand.phase = phase;
                } else if (hand.armed) {
                    hand.armed = false; // A press away from the rim must be released first.
                }
            }
        const float limit = std::clamp(s.maxAngleDegrees, 30.f, 900.f) * Pi / 180;
        if (turning) {
            angle = std::clamp(angle + turned / float(turning), -limit, limit);
        } else if (!held()) {
            const float back = 6.f * dt; // Self-centering, about 340 degrees per second.
            angle = std::abs(angle) <= back ? 0.f : angle - std::copysign(back, angle);
        }
        if (keyed) sequence = input.sequence;
        return held();
    }
};
} // namespace ss2vr
