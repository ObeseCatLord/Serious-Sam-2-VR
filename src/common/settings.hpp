#pragma once
#include "ui.hpp"
#include "steering_wheel.hpp"
namespace ss2vr {
struct VrSettings {
    ComfortConfig comfort;
    float menuDistance = 2.2f, menuWidth = 2.2f;
    float hudDistance = 1.5f, hudWidth = 1.05f;
    float fireHaptic = .35f, damageHaptic = .55f;
    bool lasers = true;
    float laserDistance = 100.f;
    float renderScale = 1.f; // Multiplier on the runtime's recommended per-eye size.
    float scopeEyeRelief = .1f; // Optical presentation parameter, not native zoom timing.
    bool immersiveSwimming = false;
    bool immersiveVehicleGrips = true;
    SteeringWheelSettings steeringWheel;
    bool roomscale = false; // Development gate until controller/replication completion.
    bool physicalMelee = false; // Unique-saw single-player integration gate.
    bool headFade = false;
    bool remoteHeadTracking = true;
    float headRadius = .12f, headClearanceMargin = .05f;
    Pose gripOffset[WeaponCount];
    // Hand-held weapon model scale: the global [Weapons] Scale times [WeaponN] Scale.
    float weaponScale = 1.f, weaponScaleById[WeaponCount]{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
};
inline float weaponModelScale(const VrSettings &settings, int weapon) {
    const float base = settings.weaponScale;
    return weapon >= 0 && weapon < int(WeaponCount) ? base * settings.weaponScaleById[weapon] : base;
}
inline Pose calibratedGrip(const Input &input, unsigned hand, int weapon, const VrSettings &settings) {
    Pose tracked = weaponTracking(input, hand);
    return weapon >= 0 && weapon < int(WeaponCount) ? compose(tracked, settings.gripOffset[weapon]) : tracked;
}
} // namespace ss2vr
