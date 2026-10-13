#pragma once
#include "math.hpp"
#include <cmath>

// Two-handed hold: a weapon held alone (the other hand empty) can be steadied by
// the free hand. Its grip, pressed near the weapon (ahead of the holding hand, at
// or below the barrel line), records where that hand is on the weapon; while it
// stays pressed the weapon turns so that point stays on the line from the holding
// hand to the free hand. Roll still follows the holding hand. Poses are in the
// weapon's frame: -Z forward along the barrel, +Y up.
namespace ss2vr {
// Shortest rotation taking unit vector a to unit vector b (identity when opposite
// or degenerate).
inline Quat rotationBetween(Vec3 a, Vec3 b) {
    const float c = dot(a, b);
    if (!std::isfinite(c) || c < -.999f) return {0, 0, 0, 1};
    const Vec3 axis = cross(a, b);
    return normalize(Quat{axis.x, axis.y, axis.z, 1 + c});
}
// The free hand in the weapon's frame.
inline Vec3 twoHandLocal(Pose weapon, Vec3 freeHand) {
    return rotate(inverse(normalize(weapon.q)), freeHand - weapon.p);
}
// Whether the free hand, at `local` in the weapon's frame, is on the weapon:
// 5 cm to `reach` ahead of the holding hand, within 18 cm sideways, from 35 cm
// below to 15 cm above the barrel line.
inline bool twoHandGrabbable(Vec3 local, float reach) {
    return std::isfinite(local.x) && std::isfinite(local.y) && std::isfinite(local.z) && -local.z >= .05f &&
           -local.z <= reach && std::abs(local.x) <= .18f && local.y >= -.35f && local.y <= .15f;
}
// The weapon turned so the grabbed point (`grabbed`, from twoHandLocal at the
// grab) lies toward the free hand. Too close together keeps the one-hand pose.
inline Pose twoHandAim(Pose weapon, Vec3 grabbed, Vec3 freeHand) {
    const Vec3 to = freeHand - weapon.p;
    const float toLength = std::sqrt(dot(to, to)), grabbedLength = std::sqrt(dot(grabbed, grabbed));
    if (!std::isfinite(toLength) || toLength < .05f || !std::isfinite(grabbedLength) || grabbedLength < .05f)
        return weapon;
    const Vec3 now = rotate(normalize(weapon.q), grabbed * (1 / grabbedLength));
    return {normalize(multiply(rotationBetween(now, to * (1 / toLength)), normalize(weapon.q))), weapon.p};
}
} // namespace ss2vr
