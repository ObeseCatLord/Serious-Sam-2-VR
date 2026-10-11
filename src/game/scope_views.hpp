#pragma once
#include "common/scope_pose.hpp"
#include "scope_capture_command.hpp"
namespace ss2vr::game {
// Pointer-free copy of one completed preview observation. Its request/model
// provenance remains required in the final draw; this is not an image handle.
struct ScopeSourceView {
    ScopePoseObservation pose;
    ScopeOpticalProjection opticalProjection;
    Pose camera;
    Fov fov;
    unsigned hand = 2;
};
bool scopePreviewNeeded(void *player, const Request &request);
bool copyScopeSourceView(unsigned hand, ScopeSourceView &out);
unsigned scopeSourceBeginFailure() noexcept; // Diagnostic: last beginScopeSource rejection.
bool beginScopeSource(void *player, const Request &request, const ScopeSourceView &view,
                      ScopeCaptureCallback callback, ScopeCaptureCurrent current, void *context);
// Callback-free current-execution predicate, for the native capture callback.
bool scopeSourceExecuting() noexcept;
bool endScopeSource(bool normalCompletion) noexcept;
} // namespace ss2vr::game
