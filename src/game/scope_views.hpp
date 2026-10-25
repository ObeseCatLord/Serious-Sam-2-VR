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
    bool alwaysLive = false; // [Scope] AlwaysLive: admitted while unzoomed too.
};
bool scopePreviewNeeded(void *player, const Request &request);
bool copyScopeSourceView(unsigned hand, ScopeSourceView &out);
unsigned scopeSourceBeginFailure() noexcept; // Diagnostic: last beginScopeSource rejection.
bool beginScopeSource(void *player, const Request &request, const ScopeSourceView &view,
                      ScopeCaptureCallback callback, ScopeCaptureCurrent current, void *context);
// Callback-free current-execution predicate, for the native capture callback.
bool scopeSourceExecuting() noexcept;
unsigned scopeSourceFlags() noexcept; // Diagnostics: active, prepared, attempted, queued, executed, fault (bits 0..5).
// The water mirror's source render ([Render] WaterMirror): its camera for this
// request and plane, and the source purpose (ended by endScopeSource).
bool waterMirrorEnabled() noexcept;
float waterMirrorScale() noexcept;
bool waterMirrorView(const Request &request, const double (&plane)[4], unsigned eye, Pose &camera, Fov &fov) noexcept;
bool beginMirrorSource(void *player, const Request &request, const Pose &camera, const Fov &fov, const double (&plane)[4],
                       ScopeCaptureCallback callback, ScopeCaptureCurrent current, void *context);
// A face of the water's environment cube (env_probe): the world from `camera`
// with a square 90-degree view; and where the viewer is this request (world).
bool beginCubeSource(void *player, const Request &request, const Pose &camera, ScopeCaptureCallback callback,
                     ScopeCaptureCurrent current, void *context);
bool waterCubeViewer(const Request &request, float (&out)[3]) noexcept;
bool endScopeSource(bool normalCompletion) noexcept;
} // namespace ss2vr::game
