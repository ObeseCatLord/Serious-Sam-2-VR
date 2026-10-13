#pragma once
#include "rider.hpp"
#include <array>
namespace ss2vr {
// Preserve the native origin and roll while orienting presentation to the
// original shoot direction. This never changes native aim, physics or fire.
inline bool nativeLaserPose(const Pose &origin, Vec3 direction, Pose &out, Vec3 *unitDirection = nullptr) {
    if (!validNativeBodyPose(origin))
        return false;
    const float length2 = dot(direction, direction);
    if (!std::isfinite(length2) || length2 < 1e-12f)
        return false;
    direction = direction * (1 / std::sqrt(length2));
    const Quat rotation = normalize(origin.q);
    const Vec3 forward = rotate(rotation, {0, 0, -1});
    const float cosine = std::clamp(dot(forward, direction), -1.f, 1.f);
    Quat correction;
    const Vec3 axis = cross(forward, direction);
    const float sine = std::sqrt(dot(axis, axis));
    if (sine <= 1e-6f && cosine < 0) {
        const Vec3 axis = rotate(rotation, {0, 1, 0});
        correction = {axis.x, axis.y, axis.z, 0};
    } else if (sine > 1e-6f) {
        const float halfAngle = std::atan2(sine, cosine) * .5f;
        const Vec3 vector = axis * (std::sin(halfAngle) / sine);
        correction = {vector.x, vector.y, vector.z, std::cos(halfAngle)};
    }
    const Pose result{normalize(multiply(correction, rotation)), origin.p};
    if (!validNativeBodyPose(result))
        return false;
    out = result;
    if (unitDirection)
        *unitDirection = direction;
    return true;
}
enum class LaserSourceKind : uint8_t { Handheld, Vehicle };
// Native identities only, local to the sample bank; never serialized or used
// to retain ownership of a native object across callbacks.
struct VehicleLaserSource {
    RiderIdentity rider;
    uint32_t model = 0, instance = 0, resource = 0, config = 0;
    uint32_t action = 0, blast = 0, attachment = 0, mechanism = 0;
    bool operator==(const VehicleLaserSource &) const = default;
    bool usable() const {
        return rider.seated() && model && instance && resource && config && mechanism;
    }
};
struct NativeModelScratch {
    std::array<uint32_t, 9> counts{};
    uint32_t evaluated = 0;
    bool idle() const {
        return !evaluated && std::all_of(counts.begin(), counts.end(), [](auto n) { return n == 0; });
    }
};
struct LaserAim {
    Pose muzzle, body;
    Vec3 end;
    uint32_t owner = 0, weapon = 0, generation = 0;
    uint64_t sequence = 0, tickMs = 0;
    bool valid = false, hit = false;
    LaserSourceKind kind = LaserSourceKind::Handheld;
    VehicleLaserSource vehicle;
    uint64_t requestSequence = 0;
    // Handheld: the muzzle relative to the weapon's grip pose it was computed
    // from, and the hit distance, so each frame rebuilds the beam from the same
    // grip pose that places the weapon model (never a stale world pose).
    Pose local{};
    float distance = 0;
    bool relative = false;
};
struct LaserFrame {
    Pose body;
    uint32_t owner = 0, generation = 0, weapon[2]{};
    uint64_t sequence = 0, now = 0;
    bool handValid[2]{}, wheel[2]{}, selecting[2]{};
    LaserSourceKind kind = LaserSourceKind::Handheld;
    VehicleLaserSource vehicle;
    uint64_t requestSequence = 0;
};
inline bool laserEligible(const LaserAim &aim, const LaserFrame &frame, unsigned hand) {
    // A grip-relative handheld beam is rebuilt from the frame's own grip pose, so
    // it does not need the sample's exact request (late-latched poses made those
    // flicker); world-space (vehicle) beams still do.
    const bool rebuilt = aim.relative && aim.kind == LaserSourceKind::Handheld;
    if (hand >= 2 || !aim.valid || !aim.owner || !aim.weapon || aim.owner != frame.owner ||
        aim.weapon != frame.weapon[hand] || aim.generation != frame.generation ||
        (!rebuilt && aim.sequence != frame.sequence) || frame.now < aim.tickMs || frame.now - aim.tickMs > 100 ||
        aim.kind != frame.kind || (!rebuilt && aim.requestSequence != frame.requestSequence) ||
        !frame.handValid[hand] || !finite(aim.muzzle) ||
        !finite(aim.body) || !finite(frame.body) || !std::isfinite(aim.end.x) || !std::isfinite(aim.end.y) ||
        !std::isfinite(aim.end.z))
        return false;
    if (aim.kind == LaserSourceKind::Vehicle) {
        if (hand != 1 || !aim.vehicle.usable() || aim.vehicle != frame.vehicle ||
            aim.owner != aim.vehicle.rider.player || aim.weapon != aim.vehicle.rider.ride)
            return false;
    } else if (frame.wheel[hand] || frame.selecting[hand])
        return false;
    if (rebuilt) return finite(aim.local) && std::isfinite(aim.distance) && aim.distance >= 0;
    const Vec3 drift = aim.body.p - frame.body.p;
    const float alignment = aim.body.q.x * frame.body.q.x + aim.body.q.y * frame.body.q.y +
                            aim.body.q.z * frame.body.q.z + aim.body.q.w * frame.body.q.w;
    return dot(drift, drift) <= .0009f && std::abs(alignment) >= .99995f;
}
// Evaluate age and ownership once; both eyes consume this exact immutable pair.
inline std::array<LaserAim, 2> freezeLaserPair(const LaserAim (&samples)[2], const LaserFrame &frame) {
    std::array<LaserAim, 2> pair{samples[0], samples[1]};
    for (unsigned hand = 0; hand != 2; ++hand)
        pair[hand].valid = laserEligible(pair[hand], frame, hand);
    return pair;
}
} // namespace ss2vr
