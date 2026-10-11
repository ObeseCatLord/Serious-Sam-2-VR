#pragma once
#include "ui.hpp"
#include "steering_wheel.hpp"
#include "foveation.hpp"
namespace ss2vr {
struct VrSettings {
    ComfortConfig comfort;
    float menuDistance = 2.2f, menuWidth = 2.2f;
    float hudDistance = 1.5f, hudWidth = 1.05f;
    float fireHaptic = .35f, damageHaptic = .55f;
    bool lasers = true;
    float laserDistance = 100.f;
    float renderScale = 1.f; // Multiplier on the runtime's recommended per-eye size.
    FoveationSettings foveation;
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
    // Hand-held weapon model scale: [Weapons] Scale (0 = automatic, matching the
    // flat game's apparent weapon size) times the per-weapon [WeaponN] Scale.
    float weaponScale = 0.f, weaponScaleById[WeaponCount]{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
};
inline Pose calibratedGrip(const Input &input, unsigned hand, int weapon, const VrSettings &settings) {
    Pose tracked = weaponTracking(input, hand);
    return weapon >= 0 && weapon < int(WeaponCount) ? compose(tracked, settings.gripOffset[weapon]) : tracked;
}
} // namespace ss2vr
