#pragma once
#include "palette_provenance.hpp"
#include "scope_uv.hpp"
#include <array>
#include <cstring>

namespace ss2vr {
// Read-only views of one completed native palette producer. This is evidence
// about the draw, not permission to replace a cap or render a magnified image.
struct ScopePaletteView {
    std::span<const uintptr_t> instances;
    std::span<const PaletteModelRange> models;
    std::span<const PaletteMeshRange> meshes;
    std::span<const PaletteDrawRange> draws;
    std::span<const uint32_t> surfaceNames;
    std::span<const PaletteBone> bones;
    std::span<const PaletteMap> mappings;
    std::span<const Matrix34> worlds, palette;
};
// Indices are valid only during this one producer callback. They must never be
// retained in the per-eye bank or used in another native render invocation.
struct ScopeDrawSelection {
    Matrix34 affine{};
    int32_t owner = -1, mesh = -1, draw = -1, palette = -1;
};
inline bool selectScopeDraw(const ScopePaletteView &v, uintptr_t instance,
                            uint32_t scopeName, uint32_t boneName, ScopeDrawSelection &out) {
    out = {};
    if (v.instances.size() != v.models.size() || v.worlds.size() != v.models.size() ||
        v.surfaceNames.size() != v.draws.size() || v.palette.size() < v.mappings.size())
        return false;
    const int32_t owner = paletteBodyOwner(v.instances, instance);
    if (owner < 1)
        return false;
    std::vector<int32_t> owners;
    if (!paletteDrawOwners(v.models, v.meshes, v.draws, v.mappings, owners))
        return false;
    int32_t selected = -1;
    for (size_t i = 0; i < v.draws.size(); ++i)
        if (owners[i] == owner && v.surfaceNames[i] == scopeName) {
            if (selected >= 0)
                return false;
            selected = int32_t(i);
        }
    if (selected < 0)
        return false;
    const auto draw = v.draws[size_t(selected)];
    if (draw.count != 1 || draw.first < 0)
        return false;
    const size_t slot = size_t(draw.first);
    const auto mapping = v.mappings[slot];
    if (mapping.bone < 0 || size_t(mapping.bone) >= v.bones.size())
        return false;
    const auto bone = v.bones[size_t(mapping.bone)];
    if (bone.owner != owner || !bone.named || bone.name != boneName)
        return false;
    const Matrix34 candidate = affineMultiply(v.worlds[size_t(owner)], v.palette[slot]);
    Matrix34 inverse;
    if (!finiteMatrix(v.worlds[size_t(owner)]) || !finiteMatrix(v.palette[slot]) ||
        !affineInverse(candidate, inverse))
        return false;
    out = {candidate, owner, draw.mesh, selected, draw.first};
    return true;
}
inline bool observeScopeAffine(const ScopePaletteView &v, uintptr_t instance,
                               uint32_t scopeName, uint32_t boneName, Matrix34 &out) {
    ScopeDrawSelection selected;
    if (!selectScopeDraw(v, instance, scopeName, boneName, selected)) return false;
    out = selected.affine;
    return true;
}
// Copied native descriptor evidence only. Supported counts/formats, filenames
// and tracking generation do not establish the lifetime or bytes of a buffer.
struct ScopeChannelLayout {
    uint32_t offset = 0;
    uint8_t format = 0, buffer = 0;
    bool operator==(const ScopeChannelLayout &) const = default;
};
struct ScopeSurfaceLayout {
    int32_t vertices = 0, triangles = 0;
    std::array<ScopeChannelLayout,4> channels{}; // positions, indices, weights, local indices
    bool operator==(const ScopeSurfaceLayout &) const = default;
};
struct ScopePoseObservation {
    Matrix34 affine{};
    uint64_t requestSequence = 0, inputSequence = 0;
    uint32_t ownerHandle = 0, weaponHandle = 0, modelHandle = 0, generation = 0;
    bool valid = false;
    // Names and evaluated pose do not establish loaded stock geometry.
    bool contentVerified = false;
    ScopeSurfaceLayout layout{};
    ScopeCapGeometry geometry{}; // Separate from full loaded-content/material verification.
    ScopeOpticalFrame opticalFrame{}; // Derived from this admitted current draw only; no image implied.
    ScopeImageCoordinates imageCoordinates{}; // Actual native UV interface only, not color/image verification.
    bool opaqueColorCandidate = false; // Matching pre-draw color observations only, not image permission.
    float nativeBaseFovRadians = 0; // Actual root projection before XR override; zero means unavailable.
    ScopeNativeZoom zoom{}; // Same admitted native weapon/draw; never the other hand's shared owner FOV.
};
// Correspondence of one source preview to a current native final-eye draw.
// Geometry is independently hashed from that draw; neither this comparison nor
// a true valid bit alone admits an image. Zero/unavailable angle always declines.
// Both unzoomed, or both zoomed with the same native interpolation.
inline bool scopeZoomSame(const ScopeNativeZoom &a,const ScopeNativeZoom &b) noexcept {
    if (!a.valid || !b.valid) return !a.valid && !b.valid;
    return scopeNativeZoom(1,1,a.startMultiplier,a.endMultiplier,a.progress).valid &&
        a.startMultiplier == b.startMultiplier && a.endMultiplier == b.endMultiplier && a.progress == b.progress;
}
// unzoomed: an always-live scope ([Scope] AlwaysLive) also admits the
// unzoomed sniper (its magnification travels with the source projection).
inline bool scopeImagePoseMatches(const ScopePoseObservation &a,const ScopePoseObservation &b,
                                  bool unzoomed = false) noexcept {
    if (unzoomed) {
        ScopePoseObservation za = a, zb = b;
        if (!scopeZoomSame(a.zoom,b.zoom)) return false;
        if (!a.zoom.valid) { za.zoom = zb.zoom = ScopeNativeZoom{1,1,1,true}; }
        return scopeImagePoseMatches(za,zb,false);
    }
    return a.valid && b.valid && a.requestSequence && a.requestSequence == b.requestSequence &&
        a.inputSequence == b.inputSequence && a.ownerHandle && a.ownerHandle == b.ownerHandle &&
        a.weaponHandle && a.weaponHandle == b.weaponHandle && a.modelHandle && a.modelHandle == b.modelHandle &&
        a.generation && a.generation == b.generation && a.layout == b.layout &&
        !std::memcmp(&a.affine,&b.affine,sizeof(a.affine)) &&
        finiteMatrix(a.affine) && std::isfinite(a.nativeBaseFovRadians) &&
        a.nativeBaseFovRadians > 0 && a.nativeBaseFovRadians == b.nativeBaseFovRadians &&
        a.zoom.valid && b.zoom.valid &&
        scopeNativeZoom(1,1,a.zoom.startMultiplier,a.zoom.endMultiplier,a.zoom.progress).valid && a.zoom.startMultiplier == b.zoom.startMultiplier &&
        a.zoom.endMultiplier == b.zoom.endMultiplier && a.zoom.progress == b.zoom.progress;
}
// A source image rendered from an earlier frame's pose of the same weapon (the
// frame before; late-latched requests can advance the sequence several times
// per frame, so the bound is a staleness guard). The lens draw reprojects it
// into the current optic frame; zoom need not match (magnification is the
// source's own and travels with its projection).
inline constexpr uint64_t ScopeSourceMaxRequestAge = 64; // Staleness guard only (slow frames latch many requests).
inline bool scopeImageSourceRecent(const ScopePoseObservation &source,const ScopePoseObservation &current,
                                   bool unzoomed = false) noexcept {
    return source.valid && current.valid && source.requestSequence && current.requestSequence > source.requestSequence &&
        current.requestSequence - source.requestSequence <= ScopeSourceMaxRequestAge && source.ownerHandle &&
        source.ownerHandle == current.ownerHandle && source.weaponHandle && source.weaponHandle == current.weaponHandle &&
        source.modelHandle && source.modelHandle == current.modelHandle && source.generation &&
        source.generation == current.generation && source.layout == current.layout &&
        finiteMatrix(source.affine) && finiteMatrix(current.affine) && (unzoomed || (source.zoom.valid && current.zoom.valid));
}
inline bool scopeObservationThread(uint32_t owner, uint32_t current, bool mainThread) {
    return owner && owner != UINT32_MAX && owner == current && mainThread;
}
struct ScopeObservationBank {
    std::array<ScopePoseObservation, 2> samples{};
    void beginDraw(unsigned hand) {
        if (hand < samples.size()) samples[hand] = {};
    }
    void finishDraw(unsigned hand, const ScopePoseObservation &sample, bool completed) {
        if (hand < samples.size()) samples[hand] = completed ? sample : ScopePoseObservation{};
    }
    bool copy(unsigned hand, uint64_t request, uint64_t input, uint32_t owner, uint32_t weapon,
              uint32_t model, uint32_t generation, bool pairFault, ScopePoseObservation &out) const {
        out = {};
        if (hand >= samples.size() || pairFault) return false;
        const auto &sample = samples[hand];
        if (!sample.valid || sample.requestSequence != request || sample.inputSequence != input ||
            sample.ownerHandle != owner || sample.weaponHandle != weapon ||
            sample.modelHandle != model || sample.generation != generation)
            return false;
        out = sample;
        return true;
    }
};
} // namespace ss2vr
