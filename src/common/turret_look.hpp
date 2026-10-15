#pragma once
#include "math.hpp"
#include <algorithm>
#include <cmath>

// Turret aim from the right stick, like the mouse on the desktop. `look` is the
// native control look (heading, pitch, bank) in the engine's convention:
// heading grows turning left and pitch grows looking up (mthQuaternionToEuler
// of an OpenXR yaw +0.5 gives heading +0.5, of a pitch +0.3 gives pitch +0.3).
// Stick right therefore lowers the heading, stick up raises the pitch.
namespace ss2vr {
// Squared response past the dead zone: fine aiming near the centre, full rate
// at full deflection (a linear response felt twitchy on a turret).
inline float turretStickRate(float axis) {
    if (!std::isfinite(axis) || std::abs(axis) < .15f) return 0.f;
    const float t = (std::min(1.f, std::abs(axis)) - .15f) / .85f;
    return std::copysign(t * t, axis);
}
inline Vec3 advanceTurretLook(Vec3 look, float stickX, float stickY, float rate, float seconds) {
    look.x -= turretStickRate(stickX) * rate * seconds;
    look.y = std::clamp(look.y + turretStickRate(stickY) * rate * seconds, -1.4f, 1.4f);
    look.z = 0;
    return look;
}
} // namespace ss2vr
