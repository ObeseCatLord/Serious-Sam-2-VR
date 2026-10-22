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
    bool snapTurn = false;                 // Smooth stick turning unless enabled.
    bool sprintDefault = true; // [Comfort] SprintByDefault: sprint unless the left stick click toggled to walk.
    float smoothTurnRate = 150 * Pi / 180; // Radians per second at full deflection.
    bool eyeReplay = false; // Right eye replayed from the left eye's recorded D3D9 calls.
    bool singlePass = true; // Both eyes from one world traversal (double-wide target).
    bool singlePassInstancing = false; // One instanced draw per world draw (lab: artifacts).
    bool shadowReuse = true; // Dynamic shadow images rendered once per frame and reused for the second eye.
    bool desktopWorld = false; // Also draw the world into the desktop window on VR frames (costs a third render).
    float uiAspect = 16.f / 9; // Aspect the game lays out its 2D view (menus, HUD, movies) at in VR.
    bool labWeaponAudit = false; // Private lab: cycle every weapon and log its hand references.
    int labWeaponAuditHand = 1;  // Hand whose selection the audit simulates.
    int labAuditSeconds = 5;     // Seconds each audited weapon is held.
    int labHoldWeapon = -1;      // With WeaponAudit: select this native weapon once and keep it.
    bool labFixedPose = false;   // Measurement only: untracked headset uses a fixed head pose.
    bool labAutoContinue = false; // Measurement only: confirm loading prompts/menus until gameplay.
    float scopeEyeRelief = .1f; // Optical presentation parameter, not native zoom timing.
    // [Scope] AlwaysLive: a held sniper's scope always shows the magnified view
    // (IdleMagnification while not zoomed; zooming uses the game's own zoom).
    bool scopeAlwaysLive = true;
    float scopeIdleMagnification = 4;
    bool immersiveSwimming = false;
    bool immersiveVehicleGrips = true;
    bool instantRides = true; // [Rides] InstantTransitions: mount and dismount without the animation.
    bool creatureHandAim = true; // [Rides] HandAimCreatures: dinosaurs aim where the right hand points.
    // [Audio] Spatial: the game's sound on OpenAL Soft with HRTF and the tracked
    // head as the listener; Occlusion: sounds behind walls are muffled.
    bool spatialAudio = true, audioOcclusion = true;
    // [HUD] Mode: 1 the game's HUD as watch faces on the wrists (health, armor,
    // lives, Netricsa left; ammo, grenades right; the rest head-mounted), 0 all
    // head-mounted. WristScale: metres the HUD's full width spans on a wrist.
    int hudMode = 1;
    float hudWristScale = .31f;
    bool rideHeadPitch = true; // [Rides] HeadPitchAim: a ride's guns pitch with the head.
    // [Vehicles] GripTurnRateDegrees: holding a ride's own controls turned turns
    // it at up to this rate (a control stick); 0 turns it with the hands 1:1.
    float gripTurnRateDegrees = 120;
    // [Voice] multiplayer voice chat with other SS2VR players (common/voice.hpp).
    struct Voice {
        bool receive = true;           // Receive: hear other players.
        int microphone = 1;            // Microphone: 0 off, 1 open mic (sends while you speak), 2 hand to mouth (push to talk).
        float sensitivity = 55;        // Sensitivity 0..100: how readily the voice gate opens.
        float inputGain = 1;           // InputGain 0..4
        float volume = 1;              // Volume 0..2
        float radioVolume = .45f;      // RadioVolume 0..1: distant (or unseen) players, centred.
        float spatialDistance = 24;    // SpatialDistanceMeters: beyond it a voice is all radio.
        bool indicators = true;        // Indicators: rings over talking players, a dot at your hand while sending.
        wchar_t device[64]{};          // InputDevice: capture device name; empty is the system default.
    } voice;
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
