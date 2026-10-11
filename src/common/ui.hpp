#pragma once
#include "math.hpp"
namespace ss2vr {
inline float signedAngle(float angle) {
    return std::remainder(angle, 2 * Pi);
}
inline float horizontalHeading(Quat orientation, float fallback = 0) {
    const Vec3 forward = rotate(orientation, {0, 0, -1});
    return forward.x * forward.x + forward.z * forward.z > .01f ? std::atan2(-forward.x, -forward.z)
                                                                : fallback;
}
inline float angularSize(float length, float distance) {
    return 2 * std::atan2(length * .5f, distance);
}
inline float sizeAtAngle(float angle, float distance) {
    return 2 * distance * std::tan(angle * .5f);
}
struct ComfortConfig {
    float enterAngle = 35 * Pi / 180, stopAngle = 8 * Pi / 180;
    float yawRate = 90 * Pi / 180, moveEnter = .35f, moveStop = .10f, moveTime = .35f;
};
// Geometry only. Native menus, inventory, and control policy remain their owners.
struct ComfortAnchor {
    Vec3 origin{};
    float heading = 0;
    uint64_t lastTick = 0;
    bool initialized = false, followingYaw = false, followingPosition = false;
    void reset() {
        *this = {};
    }
    bool update(Pose head, bool visible, uint64_t now, bool recenter = false, ComfortConfig config = {}) {
        if (!visible || !finite(head)) {
            reset();
            return false;
        }
        const float target = horizontalHeading(head.q, heading);
        if (!initialized || recenter || now < lastTick || now - lastTick > 500) {
            origin = head.p;
            heading = target;
            followingYaw = followingPosition = false;
            initialized = true;
            lastTick = now;
            return true;
        }
        const float dt = std::min(float(now - lastTick) * .001f, .05f);
        lastTick = now;
        float error = signedAngle(target - heading);
        if (!followingYaw && std::abs(error) > config.enterAngle)
            followingYaw = true;
        if (followingYaw) {
            float remaining = std::abs(error) - config.stopAngle;
            if (remaining <= 0)
                followingYaw = false;
            else {
                heading =
                    signedAngle(heading + std::copysign(std::min(remaining, config.yawRate * dt), error));
                if (remaining <= config.yawRate * dt)
                    followingYaw = false;
            }
        }
        Vec3 delta = head.p - origin;
        float distance = std::sqrt(dot(delta, delta));
        if (!followingPosition && distance > config.moveEnter)
            followingPosition = true;
        if (followingPosition) {
            if (distance <= config.moveStop)
                followingPosition = false;
            else
                origin = origin + delta * (1 - std::exp(-dt / config.moveTime));
        }
        return true;
    }
    Pose panel(Vec3 offset) const {
        Quat orientation = yaw(heading);
        return {orientation, origin + rotate(orientation, offset)};
    }
};
// A LOCAL quad faces +Z. Hide/reseed rather than let a panel cross the user's face.
inline bool comfortablePanel(Pose panel, Pose head, float minimumDistance = 1.f) {
    const Vec3 localHead = compose(inverse(panel), head).p;
    return finite(panel) && finite(head) && localHead.z >= minimumDistance;
}
struct PanelHit {
    bool valid = false;
    float u = 0, v = 0;
};
inline PanelHit pointAtPanel(Pose aim, Pose panel, float width, float height) {
    if (!finite(aim) || !finite(panel) || !std::isfinite(width) || !std::isfinite(height) || width <= 0 ||
        height <= 0)
        return {};
    Pose ray = compose(inverse(panel), aim);
    Vec3 direction = rotate(ray.q, {0, 0, -1});
    if (ray.p.z <= 0 || direction.z >= -.001f)
        return {};
    float distance = -ray.p.z / direction.z;
    if (!std::isfinite(distance) || distance > 10.f)
        return {};
    Vec3 hit = ray.p + direction * distance;
    float u = .5f + hit.x / width, v = .5f - hit.y / height;
    if (u < 0 || u > 1 || v < 0 || v > 1)
        return {};
    return {true, u, v};
}
} // namespace ss2vr
