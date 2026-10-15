#pragma once

#include "common/protocol.hpp"
#include "common/scope_pose.hpp"
#include "common/idle_weapon_trace.hpp"
#include "common/ride_render_observation.hpp"
#include "common/ride_grip_frame.hpp"
#include <cstdint>
#include <windows.h>

namespace ss2vr::game::remote_render {

using HookInstallerRva = bool (*)(HMODULE, uint32_t, void *, void **);

bool initialize(HMODULE engine, HMODULE core, HMODULE sam, HookInstallerRva install, bool enableHeadTracking,
                bool observeRideControl=false,bool produceGrips=false);

// Pin before native simulation; check lifecycle entries before native mutation.
// A two-handed first-person model held by the right hand (the Serious Bomb):
// its left arm (the bone subtree `leftArm`) follows the left controller. `left`
// is the left controller's world pose and `right` the holding hand's rotation,
// in the world the model is placed in; set before the model renders, cleared
// with instance 0.
void setSplitModel(uintptr_t instance, const Pose &left, Quat right, bool valid) noexcept;
void noteSimulationThread();
bool checkNativeThread();
// Passive observers decline foreign/unknown ownership without poisoning it.
bool ownsNativeThread();
// Borrow only within a validated native placement or out-of-render muzzle
// extent. Returns copied values; no model pointer or allocation is retained.
bool copyModelConfigurationStretch(void *instance,uint32_t expectedConfigurationVtable,
                                   IdleConfigIdentity &identity,Vec3 &stretch);
bool idleProjectionConfigured() noexcept;
bool copyIdleRaster(void *instance,IdleRasterCopy &out);
// ID2-only policy selected by the current submission owner; copies raw values
// without changing the single-affine raster/geometry admission policy.
bool copyIdlePalette(void *instance,IdlePaletteCopy &out,IdlePaletteBoundaryCopy *diagnostic=nullptr);
// Integer-only current globals vs exact latest producer; no native writes/callbacks.
uint32_t idlePaletteProjectionSequence(const IdleProjectionProbe &,const IdlePaletteCopy &) noexcept;
// Existing render bank and actual DIP borrow only. No buffer/program, whole-draw
// affine or physical-grip admission is supplied by this Main mapping observation.
bool rideDrawPossible() noexcept;
bool copyCurrentRideMainDraw(RideMainDrawCopy &out);
bool rideMainDrawCurrent(const RideMainDrawCopy &copy) noexcept;
bool claimRideGpuAttempt(const RideMainDrawCopy &copy) noexcept;
void recordRideMainDraw(const RideMainDrawCopy &copy,const RideDrawGpuCopy *gpu=nullptr) noexcept;
ScopeRasterStatus copyScopeRaster(void *instance, Matrix34 &affine, ScopeSurfaceLayout &layout);

// Called on the simulation thread after the original CPlayerOnStep.
void observePlayer(void *player);
void invalidatePlayer(void *player);

// One exact native world invocation owns the existing bank. Stereo keeps its
// token through UI publication/cleanup; mono keeps it through its original draw.
uint32_t freezePair(uint32_t localPlayer=0);
void useFrozenPair(bool enabled);
uint32_t beginMonoPresentation(uint32_t localPlayer=0);
// Historical copied world observation, only on a normal original mono return.
// Retirement remains unconditional and separate, including diagnostic failure.
void completeMonoPresentation(uint32_t owner,bool completed);
void retirePresentation(uint32_t owner) noexcept;
bool suppressNestedPresentation() noexcept;
bool presentationSuppressionCurrent() noexcept;
void invalidatePresentationForReset(bool activeDraw) noexcept;
void restorePresentationSuppression(bool previous) noexcept;
// Caller owns IPC slot and snapshot lock; validates remote identity through Ready.
bool commitPair(Slot &slot, const Request &request, bool localEligible, uint32_t owner,
                const RideGripRig *gripRig=nullptr);

} // namespace ss2vr::game::remote_render
