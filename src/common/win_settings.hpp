#pragma once
#include "settings.hpp"
#include <cwchar>
#include <string>
#include <windows.h>
namespace ss2vr {
// Values chosen in the VR options panel live in SS2VR.user.ini beside SS2VR.ini
// and override it (the packaged SS2VR.ini stays unmodified for the installer).
inline std::wstring userSettingsFile(const std::wstring &filename) {
    const size_t slash = filename.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? std::wstring() : filename.substr(0, slash + 1)) + L"SS2VR.user.ini";
}
inline VrSettings loadSettings(const std::wstring &filename) {
    VrSettings settings;
    const std::wstring user = userSettingsFile(filename);
    auto number = [&](const wchar_t *section, const wchar_t *key, float fallback, float minimum,
                      float maximum) {
        wchar_t buffer[64]{}, defaultValue[64]{};
        std::swprintf(defaultValue, 64, L"%g", double(fallback));
        DWORD length = GetPrivateProfileStringW(section, key, L"", buffer, 64, user.c_str());
        if (!length) length = GetPrivateProfileStringW(section, key, defaultValue, buffer, 64, filename.c_str());
        wchar_t *end = nullptr;
        float value = std::wcstof(buffer, &end);
        if (!length || length >= 63 || end == buffer || *end || !std::isfinite(value))
            return fallback;
        return std::clamp(value, minimum, maximum);
    };
    auto &c = settings.comfort;
    c.enterAngle = number(L"Comfort", L"FollowStartDegrees", 35, 10, 90) * Pi / 180;
    c.stopAngle = number(L"Comfort", L"FollowStopDegrees", 8, 0, c.enterAngle * 180 / Pi - 1) * Pi / 180;
    c.yawRate = number(L"Comfort", L"FollowDegreesPerSecond", 90, 20, 180) * Pi / 180;
    c.moveEnter = number(L"Comfort", L"PositionStartMeters", .35f, .2f, .6f);
    c.moveStop = number(L"Comfort", L"PositionStopMeters", .1f, .05f, c.moveEnter - .05f);
    c.moveTime = number(L"Comfort", L"PositionEaseSeconds", .35f, .15f, 1.f);
    settings.menuDistance = number(L"Comfort", L"MenuDistanceMeters", 2.2f, 1.5f, 3.f);
    settings.menuWidth = number(L"Comfort", L"MenuWidthMeters", 2.2f, 1.4f, 2.8f);
    settings.hudDistance = number(L"Comfort", L"HudDistanceMeters", 1.5f, 1.25f, 2.5f);
    settings.hudWidth = number(L"Comfort", L"HudWidthMeters", 1.05f, .8f, 1.5f);
    settings.fireHaptic = number(L"Feedback", L"FireStrength", .35f, 0, 1);
    settings.damageHaptic = number(L"Feedback", L"DamageStrength", .55f, 0, 1);
    settings.lasers = number(L"Laser", L"Enabled", 1, 0, 1) >= .5f;
    settings.laserDistance = number(L"Laser", L"MaxDistanceMeters", 100, 5, 200);
    settings.scopeEyeRelief = number(L"Scope", L"EyeReliefMeters", .1f, .02f, .3f);
    settings.scopeAlwaysLive = number(L"Scope", L"AlwaysLive", 1, 0, 1) >= .5f;
    settings.scopeIdleMagnification = number(L"Scope", L"IdleMagnification", 4, 1.5f, 10);
    settings.renderScale = number(L"Render", L"Scale", 1, .25f, 4);
    settings.foveation.mode = int(number(L"Render", L"Foveation", 1, 0, 2) + .5f);
    settings.desktopWorld = number(L"Render", L"DesktopWorld", 0, 0, 1) >= .5f;
    settings.shadowReuse = number(L"Render", L"ShadowReuse", 1, 0, 1) >= .5f;
    settings.waterMirror = number(L"Render", L"WaterMirror", 1, 0, 1) >= .5f;
    settings.waterMirrorScale = number(L"Render", L"WaterMirrorScale", .5f, .25f, 1);
    settings.eyeReplay = number(L"Render", L"EyeReplay", 0, 0, 1) >= .5f;
    settings.singlePass = number(L"Render", L"SinglePass", 1, 0, 1) >= .5f;
    settings.instantRides = number(L"Rides", L"InstantTransitions", 1, 0, 1) >= .5f;
    settings.creatureHandAim = number(L"Rides", L"HandAimCreatures", 1, 0, 1) >= .5f;
    settings.rideHeadPitch = number(L"Rides", L"HeadPitchAim", 1, 0, 1) >= .5f;
    settings.spatialAudio = number(L"Audio", L"Spatial", 1, 0, 1) >= .5f;
    settings.audioOcclusion = number(L"Audio", L"Occlusion", 1, 0, 1) >= .5f;
    settings.hudMode = int(number(L"HUD", L"Mode", 1, 0, 1) + .5f);
    settings.hudWristScale = number(L"HUD", L"WristScale", .31f, .1f, 1.f);
    {
        auto &v = settings.voice;
        v.receive = number(L"Voice", L"Receive", 1, 0, 1) >= .5f;
        v.microphone = int(number(L"Voice", L"Microphone", 1, 0, 2) + .5f);
        v.sensitivity = number(L"Voice", L"Sensitivity", 55, 0, 100);
        v.inputGain = number(L"Voice", L"InputGain", 1, 0, 4);
        v.volume = number(L"Voice", L"Volume", 1, 0, 2);
        v.radioVolume = number(L"Voice", L"RadioVolume", .45f, 0, 1);
        v.spatialDistance = number(L"Voice", L"SpatialDistanceMeters", 24, 2, 200);
        v.indicators = number(L"Voice", L"Indicators", 1, 0, 1) >= .5f;
        wchar_t device[64]{};
        if (!GetPrivateProfileStringW(L"Voice", L"InputDevice", L"", device, 64, user.c_str()))
            GetPrivateProfileStringW(L"Voice", L"InputDevice", L"", device, 64, filename.c_str());
        std::wcsncpy(v.device, device, 63);
    }
    settings.singlePassInstancing = number(L"Render", L"SinglePassInstancing", 0, 0, 1) >= .5f;
    settings.uiAspect = number(L"Render", L"UiAspect", 16.f / 9, 1.f, 2.5f);
    settings.snapTurn = number(L"Comfort", L"SnapTurn", 0, 0, 1) >= .5f;
    settings.smoothTurnRate = number(L"Comfort", L"SmoothTurnDegreesPerSecond", 150, 30, 360) * Pi / 180;
    settings.labWeaponAudit = number(L"Lab", L"WeaponAudit", 0, 0, 1) >= .5f;
    settings.labWeaponAuditHand = number(L"Lab", L"WeaponAuditHand", 1, 0, 1) >= .5f ? 1 : 0;
    settings.labHoldWeapon = int(number(L"Lab", L"HoldWeapon", -1, -1, 16));
    settings.labAuditSeconds = int(number(L"Lab", L"AuditSeconds", 5, 2, 60));
    settings.labFixedPose = number(L"Lab", L"FixedPose", 0, 0, 1) >= .5f;
    settings.labAutoContinue = number(L"Lab", L"AutoContinue", 0, 0, 1) >= .5f;
    settings.foveation.inset = number(L"Render", L"FoveationInset", .4f, .25f, .7f);
    settings.foveation.periphery = number(L"Render", L"FoveationPeriphery", .5f, .25f, .8f);
    settings.weaponScale = number(L"Weapons", L"Scale", 0, 0, 4);
    settings.roomscale = number(L"Roomscale", L"Enabled", 0, 0, 1) >= .5f;
    settings.physicalMelee = number(L"Melee", L"Enabled", 0, 0, 1) >= .5f;
    settings.immersiveSwimming = number(L"Swimming", L"Immersive", 0, 0, 1) >= .5f;
    settings.immersiveVehicleGrips = number(L"Vehicles", L"ImmersiveGrips", 1, 0, 1) >= .5f;
    settings.gripTurnRateDegrees = number(L"Vehicles", L"GripTurnRateDegrees", 120, 0, 360);
    {
        auto &w = settings.steeringWheel;
        w.enabled = number(L"SteeringWheel", L"Enabled", 1, 0, 1) >= .5f;
        w.previewOnFoot = number(L"SteeringWheel", L"PreviewOnFoot", 0, 0, 1) >= .5f;
        w.offset = {number(L"SteeringWheel", L"OffsetX", 0, -.5f, .5f),
                    number(L"SteeringWheel", L"OffsetY", -.38f, -.9f, .2f),
                    number(L"SteeringWheel", L"OffsetZ", -.42f, -.9f, -.15f)};
        w.tiltDegrees = number(L"SteeringWheel", L"TiltDegrees", 25, -10, 70);
        w.scale = number(L"SteeringWheel", L"Scale", 1, .6f, 1.6f);
        w.maxAngleDegrees = number(L"SteeringWheel", L"MaxAngleDegrees", 135, 45, 540);
        w.fullLockDegrees = number(L"SteeringWheel", L"FullLockDegrees", 90, 15, 540);
        w.turnRateDegrees = number(L"SteeringWheel", L"TurnRateDegrees", 120, 20, 360);
    }
    settings.remoteHeadTracking = number(L"Multiplayer", L"RemoteHeadTracking", 1, 0, 1) >= .5f;
    settings.headFade = number(L"HeadComfort", L"Enabled", 0, 0, 1) >= .5f;
    settings.headRadius = number(L"HeadComfort", L"RadiusMeters", .12f, .05f, .2f);
    settings.headClearanceMargin = number(L"HeadComfort", L"ClearanceMarginMeters",
        number(L"HeadComfort", L"FadeDepthMeters", .05f, .02f, .15f), .02f, .15f);
    for (unsigned weapon = 0; weapon < WeaponCount; ++weapon) {
        wchar_t section[32];
        std::swprintf(section, 32, L"Weapon%d", int(weapon));
        Vec3 p{number(section, L"OffsetX", 0, -.3f, .3f), number(section, L"OffsetY", 0, -.3f, .3f),
               number(section, L"OffsetZ", 0, -.3f, .3f)};
        float pitch = number(section, L"PitchDegrees", 0, -90, 90) * Pi / 180;
        float heading = number(section, L"YawDegrees", 0, -90, 90) * Pi / 180;
        float roll = number(section, L"RollDegrees", 0, -90, 90) * Pi / 180;
        settings.weaponScaleById[weapon] = number(section, L"Scale", 1, .25f, 4);
        settings.gripOffset[weapon] = {
            multiply(multiply(yaw(heading), {std::sin(pitch / 2), 0, 0, std::cos(pitch / 2)}),
                     {0, 0, std::sin(roll / 2), std::cos(roll / 2)}),
            p};
    }
    return settings;
}
} // namespace ss2vr
