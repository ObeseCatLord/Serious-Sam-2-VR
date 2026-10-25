#pragma once
// VR options (the host's panel in VR, a window on the desktop): the settings
// each page shows, what one step does to them, and where they are saved
// (SS2VR.user.ini, which the game reloads while it runs). Windows only.
#include "math.hpp"
#include "options_layout.hpp"
#include "settings.hpp"
#include "win_settings.hpp"
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <string>
#include <vector>
#include <windows.h>
#include <mmsystem.h>

namespace ss2vr::options_model {
struct OptionsRow {
    std::wstring label, value;
    const wchar_t *help = L"";
    std::wstring note; // Shown in amber (e.g. a restart is needed).
    float fraction = -1; // A numeric value's place between its limits (slider), or -1.
    bool dim = false;    // Not in effect with the other settings (e.g. snap angle while smooth).
    bool locked = false; // Cannot be changed here (VR-only settings on the desktop).
};

enum OptionsItem : unsigned {
    // Movement
    OptMoveDirection, OptTurning, OptTurnSpeed, OptSnapAngle, OptSwimming, OptGripTurn,
    // Graphics
    OptScale, OptFoveation, OptInset, OptPeriphery, OptWaterMirror, OptMirrorScale, OptScopeLive, OptScopeZoom,
    OptDesktopWorld, OptInstancing,
    // Sound
    OptSpatial, OptOcclusion, OptFireHaptic, OptDamageHaptic,
    // Voice chat
    OptVoiceReceive, OptMicrophone, OptMicDevice, OptMicSensitivity, OptMicGain, OptVoiceVolume, OptRadioVolume,
    OptVoiceRange, OptVoiceIndicators,
    // Interface
    OptHud, OptHudSize, OptLasers, OptLaserRange, OptMenuDistance, OptMenuSize,
    OptCount
};
inline constexpr unsigned OptionsPageCount = 5;
inline constexpr unsigned OptionsPageStart[OptionsPageCount + 1] = {OptMoveDirection, OptScale, OptSpatial, OptVoiceReceive, OptHud,
                                                                    OptCount};
inline constexpr const wchar_t *OptionsPages[OptionsPageCount] = {L"Movement", L"Graphics", L"Sound", L"Voice chat", L"Interface"};
struct OptionKey { const wchar_t *section, *key; };
inline constexpr OptionKey OptionKeys[OptCount] = {
    {L"Comfort", L"MoveDirection"}, {L"Comfort", L"SnapTurn"}, {L"Comfort", L"SmoothTurnDegreesPerSecond"},
    {L"Comfort", L"SnapTurnDegrees"}, {L"Swimming", L"Immersive"}, {L"Vehicles", L"GripTurnRateDegrees"},
    {L"Render", L"Scale"}, {L"Render", L"Foveation"}, {L"Render", L"FoveationInset"}, {L"Render", L"FoveationPeriphery"},
    {L"Render", L"WaterMirror"}, {L"Render", L"WaterMirrorScale"}, {L"Scope", L"AlwaysLive"}, {L"Scope", L"IdleMagnification"},
    {L"Render", L"DesktopWorld"}, {L"Render", L"SinglePassInstancing"},
    {L"Audio", L"Spatial"}, {L"Audio", L"Occlusion"}, {L"Feedback", L"FireStrength"}, {L"Feedback", L"DamageStrength"},
    {L"Voice", L"Receive"}, {L"Voice", L"Microphone"}, {L"Voice", L"InputDevice"}, {L"Voice", L"Sensitivity"},
    {L"Voice", L"InputGain"}, {L"Voice", L"Volume"}, {L"Voice", L"RadioVolume"}, {L"Voice", L"SpatialDistanceMeters"},
    {L"Voice", L"Indicators"},
    {L"HUD", L"Mode"}, {L"HUD", L"WristScale"}, {L"Laser", L"Enabled"}, {L"Laser", L"MaxDistanceMeters"},
    {L"Comfort", L"MenuDistanceMeters"}, {L"Comfort", L"MenuWidthMeters"}};
inline unsigned pageItems(unsigned page) { return page < OptionsPageCount ? OptionsPageStart[page + 1] - OptionsPageStart[page] : 0; }
struct OptionsContext {
    bool vr = true;          // In VR; false on the desktop, where VR-only settings are greyed out.
    bool eyeTracking = true; // The headset reports eye tracking (foveation follows the gaze).
};
// The settings that matter without a headset: sound and voice chat.
inline bool desktopOption(unsigned item) {
    return item == OptSpatial || item == OptOcclusion || (item >= OptVoiceReceive && item <= OptVoiceIndicators);
}
inline bool saveOption(unsigned item, const wchar_t *text, const std::wstring &settingsFile) {
    return item < OptCount &&
           WritePrivateProfileStringW(OptionKeys[item].section, OptionKeys[item].key, text, userSettingsFile(settingsFile).c_str());
}
inline OptionsRow vrOptionRow(unsigned item, const VrSettings &settings, const VrSettings &startup, bool eyeTracking) {
    const auto onOff = [](bool v) { return std::wstring(v ? L"On" : L"Off"); };
    const auto format = [](const wchar_t *pattern, double v) { wchar_t b[32]; std::swprintf(b, 32, pattern, v); return std::wstring(b); };
    const auto percent = [&](float v) { return format(L"%.0f%%", double(std::lround(v * 100))); };
    const auto restart = [](bool changed) { return std::wstring(changed ? L"Restart the game to apply this" : L""); };
    const auto &f = settings.foveation;
    const auto &v = settings.voice;
    using options_layout::fraction;
    switch (item) {
    case OptMoveDirection:
        return {L"Walk direction", settings.moveDirection == 1 ? L"Left hand" : settings.moveDirection == 2 ? L"Right hand" : L"Head",
                L"The left stick walks where your head faces, or where a controller points (head-relative while it is untracked)."};
    case OptTurning:
        return {L"Turning", settings.snapTurn ? L"Snap" : L"Smooth", L"The right stick turns you smoothly or in snap steps."};
    case OptTurnSpeed:
        return {L"Smooth turn speed", format(L"%.0f°/s", double(std::lround(settings.smoothTurnRate * 180 / Pi))),
                L"How fast smooth turning turns at full stick.", L"",
                fraction(settings.smoothTurnRate * 180 / Pi, 30, 360), settings.snapTurn};
    case OptSnapAngle:
        return {L"Snap turn angle", format(L"%.0f°", double(settings.snapTurnDegrees)), L"How far one snap turn turns you.", L"",
                fraction(settings.snapTurnDegrees, 15, 90), !settings.snapTurn};
    case OptSwimming:
        return {L"Arm-stroke swimming", onOff(settings.immersiveSwimming),
                L"Swim by pulling both arms through the water like a breaststroke (the stick still swims too)."};
    case OptGripTurn:
        return {L"Vehicle control turn rate",
                settings.gripTurnRateDegrees > 0 ? format(L"%.0f°/s", double(settings.gripTurnRateDegrees)) : std::wstring(L"1:1 with hands"),
                L"Holding a vehicle's own controls turned steers it like a control stick at up to this rate; 1:1 turns it with your hands.",
                L"", fraction(settings.gripTurnRateDegrees, 0, 360)};
    case OptScale:
        return {L"Render resolution", format(L"%.0f%%", double(std::lround(settings.renderScale * 100))),
                L"Each eye's resolution as a share of what the headset recommends. Lower is faster, higher is sharper.", L"",
                fraction(settings.renderScale, .5f, 2)};
    case OptFoveation:
        return {L"Foveated rendering", f.mode == 0 ? L"Off" : f.mode == 1 ? L"Eye-tracked" : L"Fixed (centre)",
                L"Renders the edges of the view at lower resolution: where you look (eye tracking) or at the centre.",
                f.mode == 1 && !eyeTracking ? L"No eye tracking found: off" : L""};
    case OptInset:
        return {L"Sharp region size", percent(f.inset), L"How much of the view stays at full resolution.", L"", fraction(f.inset, .25f, .7f),
                f.mode == 0};
    case OptPeriphery:
        return {L"Edge resolution", percent(f.periphery), L"The resolution outside the sharp region.", L"",
                fraction(f.periphery, .25f, .8f), f.mode == 0};
    case OptWaterMirror:
        return {L"Water reflections", settings.waterMirror ? L"Mirror" : L"Cubemap",
                L"Mirror: the world rendered mirrored for each eye (exact, costs two extra renders). Cubemap: a captured picture of the surroundings (cheap, no close-up detail)."};
    case OptMirrorScale:
        return {L"Reflection resolution", percent(settings.waterMirrorScale),
                L"The mirrored render's size as a share of an eye's. Lower is faster.", L"",
                fraction(settings.waterMirrorScale, .25f, 1), !settings.waterMirror};
    case OptScopeLive:
        return {L"Scopes always magnify", onOff(settings.scopeAlwaysLive),
                L"A held sniper's scope shows the magnified view even when you are not zoomed."};
    case OptScopeZoom:
        return {L"Unzoomed scope magnification", format(L"%.1fx", double(settings.scopeIdleMagnification)),
                L"The scope's magnification while you are not zoomed.", L"", fraction(settings.scopeIdleMagnification, 1.5f, 10),
                !settings.scopeAlwaysLive};
    case OptDesktopWorld:
        return {L"Show the game on the monitor", onOff(settings.desktopWorld),
                L"Also draws the game in its desktop window, for others watching or streaming. Costs a third render."};
    case OptInstancing:
        return {L"Instanced single pass", onOff(settings.singlePassInstancing),
                L"Experimental: both eyes in one draw call each. Can show artifacts."};
    case OptSpatial:
        return {L"Spatial audio", onOff(settings.spatialAudio),
                L"3D sound (HRTF) heard from your tracked head: sounds come from where they are around you.",
                restart(settings.spatialAudio != startup.spatialAudio)};
    case OptOcclusion:
        return {L"Sound occlusion", onOff(settings.audioOcclusion), L"Sounds behind walls are muffled (with spatial audio).",
                L"", -1, !settings.spatialAudio};
    case OptFireHaptic:
        return {L"Fire vibration", percent(settings.fireHaptic), L"Controller vibration when you shoot.", L"",
                fraction(settings.fireHaptic, 0, 1)};
    case OptDamageHaptic:
        return {L"Damage vibration", percent(settings.damageHaptic), L"Controller vibration when you are hurt.", L"",
                fraction(settings.damageHaptic, 0, 1)};
    case OptVoiceReceive:
        return {L"Hear other players", onOff(v.receive), L"Hear the voices of other VR players in multiplayer."};
    case OptMicrophone:
        return {L"Microphone", v.microphone == 0 ? L"Off" : v.microphone == 1 ? L"Open mic" : L"Hand to mouth",
                v.microphone == 1 ? L"Sends your voice while you speak."
                : v.microphone == 2 ? L"Push to talk: hold either controller to your mouth while you speak."
                                    : L"Others cannot hear you."};
    case OptMicDevice:
        return {L"Microphone device", v.device[0] ? std::wstring(v.device) : std::wstring(L"System default"),
                L"Which microphone voice chat records from.", L"", -1, v.microphone == 0};
    case OptMicSensitivity:
        return {L"Microphone sensitivity", format(L"%.0f", double(v.sensitivity)),
                L"How readily open mic starts sending: raise it if your voice is cut off, lower it if noise gets through.", L"",
                fraction(v.sensitivity, 0, 100), v.microphone != 1};
    case OptMicGain:
        return {L"Microphone boost", percent(v.inputGain), L"Amplifies your microphone before sending.", L"",
                fraction(v.inputGain, 0, 4), v.microphone == 0};
    case OptVoiceVolume:
        return {L"Voice volume", percent(v.volume), L"How loud other players' voices are.", L"", fraction(v.volume, 0, 2), !v.receive};
    case OptRadioVolume:
        return {L"Radio voice volume", percent(v.radioVolume),
                L"Players far away or out of sight are heard over the radio, centred, at this volume.", L"",
                fraction(v.radioVolume, 0, 1), !v.receive};
    case OptVoiceRange:
        return {L"Proximity voice range", format(L"%.0f m", double(v.spatialDistance)),
                L"Within this distance voices come from the player; beyond it they are all radio.", L"",
                fraction(v.spatialDistance, 2, 200), !v.receive};
    case OptVoiceIndicators:
        return {L"Talking indicators", onOff(v.indicators),
                L"Rings over players who are talking, and a dot at your hand while you are sending."};
    case OptHud:
        return {L"HUD", settings.hudMode == 1 ? L"Wrist watches" : L"Head-mounted",
                L"Health and armor on the left wrist, ammo on the right; or the whole HUD in front of you."};
    case OptHudSize:
        return {L"Wrist HUD size", format(L"%.0f cm", double(std::lround(settings.hudWristScale * 100))),
                L"The width the full HUD spans on a wrist.", L"", fraction(settings.hudWristScale, .2f, .6f), settings.hudMode != 1};
    case OptLasers:
        return {L"Aim lasers", onOff(settings.lasers), L"A laser from each weapon shows where it aims."};
    case OptLaserRange:
        return {L"Laser length", format(L"%.0f m", double(settings.laserDistance)), L"How far the aim lasers reach.", L"",
                fraction(settings.laserDistance, 10, 200), !settings.lasers};
    case OptMenuDistance:
        return {L"Menu distance", format(L"%.1f m", double(settings.menuDistance)),
                L"How far away the game's menus (and the head-mounted HUD) float.", L"", fraction(settings.menuDistance, 1.5f, 3)};
    case OptMenuSize:
        return {L"Menu size", format(L"%.1f m", double(settings.menuWidth)), L"How wide the game's menus (and the head-mounted HUD) are.",
                L"", fraction(settings.menuWidth, 1.4f, 2.8f)};
    default: return {};
    }
}
inline OptionsRow optionRow(unsigned item, const VrSettings &settings, const VrSettings &startup, const OptionsContext &context) {
    OptionsRow row = vrOptionRow(item, settings, startup, context.eyeTracking);
    if (!context.vr && !desktopOption(item)) {
        row.dim = row.locked = true;
        row.note = L"VR only";
    } else if (!context.vr && item == OptMicrophone && settings.voice.microphone == 2) {
        row.note = L"Hand to mouth needs a headset: nothing is sent";
    }
    return row;
}
inline std::vector<OptionsRow> optionRows(unsigned page, const VrSettings &settings, const VrSettings &startup,
                                          const OptionsContext &context) {
    std::vector<OptionsRow> rows;
    for (unsigned item = OptionsPageStart[page]; page < OptionsPageCount && item < OptionsPageStart[page + 1]; ++item)
        rows.push_back(optionRow(item, settings, startup, context));
    return rows;
}
// One step of `item` by `direction`, saved: false if nothing changed or it
// could not be saved. On the desktop only the settings that apply there change,
// and the microphone offers Off and Open mic (hand to mouth needs a headset).
inline bool changeOption(unsigned item, int direction, VrSettings &settings, const std::vector<std::wstring> &microphones,
                         const std::wstring &settingsFile, const OptionsContext &context) {
    if (!context.vr && !desktopOption(item)) return false;
    bool saved = true;
    const auto saveText = [&](unsigned key, const wchar_t *text) { saved = saveOption(key, text, settingsFile) && saved; };
    const auto saveSetting = [&](unsigned key, double value) {
        wchar_t text[32];
        std::swprintf(text, 32, L"%g", value);
        saveText(key, text);
    };
    auto &f = settings.foveation;
    auto &v = settings.voice;
    const auto toggle = [&](bool &value) { value = !value; saveSetting(item, value); };
    const auto number = [&](float &value, float step, float low, float high) {
        value = options_layout::stepped(value, direction, step, low, high);
        saveSetting(item, value);
    };
    const auto cycle = [&](int &value, int count) { value = (value + count + direction) % count; saveSetting(item, value); };
    switch (item) {
    case OptMoveDirection: cycle(settings.moveDirection, 3); break;
    case OptTurning: toggle(settings.snapTurn); break;
    case OptTurnSpeed: {
        float degrees = settings.smoothTurnRate * 180 / Pi;
        number(degrees, 30, 30, 360);
        settings.smoothTurnRate = degrees * Pi / 180;
        break;
    }
    case OptSnapAngle: number(settings.snapTurnDegrees, 15, 15, 90); break;
    case OptSwimming: toggle(settings.immersiveSwimming); break;
    case OptGripTurn: number(settings.gripTurnRateDegrees, 30, 0, 360); break;
    case OptScale: number(settings.renderScale, .1f, .5f, 2.f); break; // Applies at once: the eye size is republished every loop.
    case OptFoveation: cycle(f.mode, 3); break;
    case OptInset: number(f.inset, .05f, .25f, .7f); break;
    case OptPeriphery: number(f.periphery, .05f, .25f, .8f); break;
    case OptWaterMirror: toggle(settings.waterMirror); break;
    case OptMirrorScale: number(settings.waterMirrorScale, .25f, .25f, 1); break;
    case OptScopeLive: toggle(settings.scopeAlwaysLive); break;
    case OptScopeZoom: number(settings.scopeIdleMagnification, .5f, 1.5f, 10); break;
    case OptDesktopWorld: toggle(settings.desktopWorld); break;
    case OptInstancing: toggle(settings.singlePassInstancing); break;
    case OptSpatial: toggle(settings.spatialAudio); break;
    case OptOcclusion: toggle(settings.audioOcclusion); break;
    case OptFireHaptic: number(settings.fireHaptic, .1f, 0, 1); break;
    case OptDamageHaptic: number(settings.damageHaptic, .1f, 0, 1); break;
    case OptVoiceReceive: toggle(v.receive); break;
    case OptMicrophone: cycle(v.microphone, context.vr ? 3 : 2); break;
    case OptMicDevice: {
        // "System default", then each capture device.
        const size_t count = microphones.size() + 1;
        size_t at = 0;
        for (size_t i = 0; i < microphones.size(); ++i)
            if (!std::wcsncmp(microphones[i].c_str(), v.device, 31)) at = i + 1;
        at = (at + count + size_t(direction > 0 ? 1 : count - 1)) % count;
        std::fill(std::begin(v.device), std::end(v.device), L'\0');
        if (at) std::wcsncpy(v.device, microphones[at - 1].c_str(), 63);
        saveText(item, v.device);
        break;
    }
    case OptMicSensitivity: number(v.sensitivity, 5, 0, 100); break;
    case OptMicGain: number(v.inputGain, .25f, 0, 4); break;
    case OptVoiceVolume: number(v.volume, .1f, 0, 2); break;
    case OptRadioVolume: number(v.radioVolume, .05f, 0, 1); break;
    case OptVoiceRange: {
        const float step = v.spatialDistance + direction * 1.f >= 40 ? 10.f : 2.f; // Finer when close.
        number(v.spatialDistance, step, 2, 200);
        break;
    }
    case OptVoiceIndicators: toggle(v.indicators); break;
    case OptHud: settings.hudMode = settings.hudMode == 1 ? 0 : 1; saveSetting(item, settings.hudMode); break;
    case OptHudSize: number(settings.hudWristScale, .02f, .2f, .6f); break;
    case OptLasers: toggle(settings.lasers); break;
    case OptLaserRange: number(settings.laserDistance, 10, 10, 200); break;
    case OptMenuDistance: number(settings.menuDistance, .1f, 1.5f, 3); break;
    case OptMenuSize: number(settings.menuWidth, .1f, 1.4f, 2.8f); break;
    default: return false;
    }
    return saved;
}
// Reset page: its keys leave SS2VR.user.ini, so SS2VR.ini's values (the defaults)
// apply again. On the desktop the VR-only keys stay as they are.
inline void resetPage(unsigned page, const std::wstring &settingsFile, const OptionsContext &context) {
    for (unsigned item = OptionsPageStart[page]; page < OptionsPageCount && item < OptionsPageStart[page + 1]; ++item)
        if (context.vr || desktopOption(item))
            WritePrivateProfileStringW(OptionKeys[item].section, OptionKeys[item].key, nullptr, userSettingsFile(settingsFile).c_str());
}
// Capture devices for the microphone row (waveIn names).
inline std::vector<std::wstring> microphones() {
    std::vector<std::wstring> names;
    const UINT count = waveInGetNumDevs();
    for (UINT i = 0; i < count && i < 32; ++i) {
        WAVEINCAPSW caps{};
        if (waveInGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR && caps.szPname[0])
            names.emplace_back(caps.szPname, wcsnlen(caps.szPname, 32));
    }
    return names;
}
} // namespace ss2vr::options_model
