#pragma once
// Throw velocity for a grip-carried prop.
//
// Hand samples are kept in the runtime's reference space, so player locomotion,
// snap turns and the throw boost never mix: at the release the hand's own
// velocity is rotated into the world and boosted, and the body's velocity
// (measured separately) is added unboosted. The hand's velocity comes from the
// runtime (XrSpaceVelocity) when it reports one; otherwise poses are
// differenced over at least MinWindow of their own predicted display times.
#include "math.hpp"
#include "protocol.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace ss2vr {
struct ThrowHandSample {
    double tick = 0;     // Game clock when the sample was taken (fine seconds).
    double poseTime = 0; // The pose's own time (predicted display time, seconds).
    Pose hand;           // Grip, reference space.
    Vec3 linear{}, angular{};
    uint32_t valid = 0;  // HandVelocityValid bits.
};
struct ThrowMotion {
    Vec3 linear{}, angular{}; // Reference space.
    double age = 0;           // Seconds from the peak to `now`.
    bool runtime = false, valid = false;
};
class ThrowTracker {
public:
    static constexpr unsigned Capacity = 64;
    static constexpr double Keep = .5, MinWindow = .035, MaxWindow = .1;
    void reset() { hands = bodies = 0; }
    void addHand(const ThrowHandSample &s) { hand[hands++ % Capacity] = s; }
    void addBody(double tick, Vec3 p) { body[bodies++ % Capacity] = {tick, p}; }
    unsigned handCount() const { return std::min(hands, Capacity); }
    const ThrowHandSample *newestHand() const { return hands ? &hand[(hands - 1) % Capacity] : nullptr; }
    // The fastest hand motion among the samples taken in the last `window`
    // seconds before `now` (window 0: the newest only).
    ThrowMotion peak(double now, double window) const {
        // The runtime's velocity unless it reports far less than the poses moved:
        // some runtimes flag velocities valid but leave them zero (the lab's).
        const ThrowMotion runtime = peakFrom(now, window, false), poses = peakFrom(now, window, true);
        const float r = dot(runtime.linear, runtime.linear), p = dot(poses.linear, poses.linear);
        if (runtime.valid && (!poses.valid || r >= .36f * p || p < .25f)) return runtime;
        return poses.valid ? poses : runtime;
    }
private:
    ThrowMotion peakFrom(double now, double window, bool posesOnly) const {
        ThrowMotion best;
        float bestSpeed = -1;
        const unsigned n = handCount();
        for (unsigned back = 0; back < n; ++back) {
            const auto &s = at(back);
            if (now - s.tick > window && back) break;
            ThrowMotion m;
            if (!motionAt(back, m, posesOnly)) continue;
            const float speed = dot(m.linear, m.linear);
            if (speed > bestSpeed) {
                bestSpeed = speed;
                best = m;
                best.age = now - s.tick;
                if (m.runtime) smooth(back, best);
            }
            if (!window) break;
        }
        return best;
    }
public:
    // The body's (player's) world velocity over the last ~MaxWindow of ticks.
    Vec3 bodyVelocity() const {
        const unsigned n = std::min(bodies, Capacity);
        if (n < 2) return {};
        const auto &last = body[(bodies - 1) % Capacity];
        if (hands && hand[(hands - 1) % Capacity].tick - last.tick > .15) return {}; // Older than the hand's samples.
        for (unsigned back = 1; back < n; ++back) {
            const auto &first = body[(bodies - 1 - back) % Capacity];
            const double dt = last.tick - first.tick;
            if (dt < .05 && back + 1 < n) continue;
            if (!(dt >= MinWindow) || dt > Keep) return {};
            const Vec3 v = (last.p - first.p) * float(1 / dt);
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) ? v : Vec3{};
        }
        return {};
    }
private:
    struct BodySample { double tick = 0; Vec3 p{}; };
    ThrowHandSample hand[Capacity]{};
    BodySample body[Capacity]{};
    unsigned hands = 0, bodies = 0;
    const ThrowHandSample &at(unsigned back) const { return hand[(hands - 1 - back) % Capacity]; }
    static bool finiteVec(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
    // A sample's hand motion: the runtime's velocity, else poses differenced
    // over MinWindow..MaxWindow of pose time ending at it.
    bool motionAt(unsigned back, ThrowMotion &out, bool posesOnly = false) const {
        const auto &last = at(back);
        if (!posesOnly && (last.valid & LinearVelocityValid) && finiteVec(last.linear)) {
            out.linear = last.linear;
            out.angular = (last.valid & AngularVelocityValid) && finiteVec(last.angular) ? last.angular : Vec3{};
            out.runtime = out.valid = true;
            return true;
        }
        const unsigned n = handCount();
        for (unsigned older = back + 1; older < n; ++older) {
            const auto &first = at(older);
            const double dt = last.poseTime - first.poseTime;
            if (dt < MinWindow) continue;
            if (dt > MaxWindow) return false;
            out.linear = (last.hand.p - first.hand.p) * float(1 / dt);
            Quat d = normalize(multiply(last.hand.q, inverse(first.hand.q)));
            if (d.w < 0) d = {-d.x, -d.y, -d.z, -d.w};
            const float s = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            const float angle = 2 * std::atan2(s, d.w);
            out.angular = s > 1e-6f ? Vec3{d.x / s, d.y / s, d.z / s} * float(angle / dt) : Vec3{};
            out.valid = finiteVec(out.linear) && finiteVec(out.angular);
            return out.valid;
        }
        return false;
    }
    // The runtime's per-frame velocity is noisy: average the peak with its
    // neighbours (one frame either side) that also carry a runtime velocity.
    void smooth(unsigned back, ThrowMotion &m) const {
        Vec3 linear = m.linear, angular = m.angular;
        float count = 1;
        for (int d : {-1, 1}) {
            const int i = int(back) + d;
            if (i < 0 || unsigned(i) >= handCount()) continue;
            ThrowMotion n;
            if (!motionAt(unsigned(i), n) || !n.runtime) continue;
            linear = linear + n.linear;
            angular = angular + n.angular;
            ++count;
        }
        m.linear = linear * (1 / count);
        m.angular = angular * (1 / count);
    }
};
// The world rotation of reference-space vectors in the hand's world pose
// (worldHandTracking: anchor * yaw(turn) * inverse(origin) * tracked).
inline Quat referenceToWorld(Pose anchor, Pose origin, float turn) {
    return normalize(multiply(multiply(anchor.q, yaw(turn)), inverse(origin.q)));
}
// The velocity to give a released prop whose centre is at `centre` while the
// hand (grip) is at `hand` (both world): the body's velocity plus the boosted
// hand motion including its swing about the grip.
inline Vec3 throwVelocity(const ThrowMotion &m, Quat toWorld, Vec3 hand, Vec3 centre, Vec3 body, float boost) {
    const Vec3 linear = rotate(toWorld, m.linear), angular = rotate(toWorld, m.angular);
    return body + (linear + cross(angular, centre - hand)) * boost;
}
} // namespace ss2vr
