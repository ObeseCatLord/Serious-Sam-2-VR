#pragma once
#include "math.hpp"
#include <algorithm>
#include <cmath>

// Rides with handles (the hoverbike and the UFO): the hands steer as on a
// steering wheel, without grabbing anything, the left hand on its left side and
// the right hand on its right. The wheel's turn is the tilt of the line from the
// left hand to the right one about the forward axis: the right hand lower turns
// right. Returned as a stick-like deflection -1..1 (positive right) with a dead
// zone around level and full lock at `full` radians.
namespace ss2vr {
inline bool handWheelDeflection(Vec3 left, Vec3 right, float &out, float dead = .1f, float full = .6f) noexcept {
    out = 0;
    const float dx = right.x - left.x, dz = right.z - left.z, dy = right.y - left.y;
    const float across = std::sqrt(dx * dx + dz * dz);
    if (!std::isfinite(across + dy) || across < .15f) return false; // Hands too close (or crossed above each other).
    const float tilt = std::atan2(-dy, across);                     // Right hand lower: positive.
    const float amount = std::clamp((std::abs(tilt) - dead) / (full - dead), 0.f, 1.f);
    out = std::copysign(amount, tilt);
    return true;
}
} // namespace ss2vr
