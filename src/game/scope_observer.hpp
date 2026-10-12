#pragma once
#include "common/scope_pose.hpp"
namespace ss2vr::game {
struct ScopeDrawBinding {
    void *modelInstance = nullptr; // Borrowed only for the active native callback.
    uint64_t requestSequence = 0, inputSequence = 0;
    uint32_t ownerHandle = 0, weaponHandle = 0, modelHandle = 0, generation = 0;
    unsigned hand = 2;
};
bool currentScopeDraw(ScopeDrawBinding &out);
bool weaponModelDrawActive() noexcept;
struct ScopeRasterObservation {
    ScopePoseObservation pose{};
    unsigned hand = 2;
    float nearDepth = 0, farDepth = 1;
    bool opaqueMode = false;
    Matrix44 capClip{};
    bool capClipValid = false;
};
bool currentScopeRaster(ScopeRasterObservation &out);
// Pinned ordinary gun-command route and native query bookkeeping only; this
// is not an all-issuers/failure-total assertion about actual GPU query state.
bool currentScopeQueryBoundary();
bool recordScopeGeometry(const ScopeRasterObservation &, const ScopeCapGeometry &, const ScopeUvTransform &,
                         bool opaqueColorCandidate);
void retireScopeGeometry(bool fatal = false) noexcept;
void recordScopeObservation(const ScopeDrawBinding &binding, const Matrix34 &affine,
                            const ScopeSurfaceLayout &layout);
// Copy diagnostic evidence only while its exact native eye remains active.
bool observedScopePose(unsigned hand, ScopePoseObservation &out);
} // namespace ss2vr::game
