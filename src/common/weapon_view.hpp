#pragma once
#include "model_tree.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
namespace ss2vr {
struct WeaponWorldView {
    Matrix34 view{};
    Matrix44 projection{};
    float nearDepth = 0, farDepth = 1;
    bool valid = false;
};
inline bool finiteProjection(const Matrix44 &p) {
    for (float v : p.m)
        if (!std::isfinite(v)) return false;
    return p.m[0] > 0 && p.m[5] > 0 && p.m[14] == -1 && p.m[15] == 0;
}
inline bool validDepthRange(float nearDepth, float farDepth) {
    return std::isfinite(nearDepth) && std::isfinite(farDepth) &&
           nearDepth >= 0 && farDepth <= 1 && farDepth > nearDepth;
}
inline bool sameWeaponView(const Matrix34 &a, const Matrix34 &b) {
    for (unsigned i = 0; i != 12; ++i)
        if (!std::isfinite(a.m[i]) || !std::isfinite(b.m[i]) ||
            std::abs(a.m[i] - b.m[i]) > .001f) return false;
    return true;
}
// A view's camera position: -R^T t for the view [R | t] (world -> view, row-major).
inline Vec3 viewCamera(const Matrix34 &v) {
    return {-(v.m[0] * v.m[3] + v.m[4] * v.m[7] + v.m[8] * v.m[11]),
            -(v.m[1] * v.m[3] + v.m[5] * v.m[7] + v.m[9] * v.m[11]),
            -(v.m[2] * v.m[3] + v.m[6] * v.m[7] + v.m[10] * v.m[11])};
}
// The game's own camera motion on top of the eye (view bob on landing, damage
// and explosion shake) moves the root view a few centimetres and degrees from
// the eye: the world was still drawn for this eye. Another camera altogether
// (its camera more than ShakeReach metres away, or more than ShakeTurn in any
// rotation element) is not. The cameras are compared, not the views'
// translations: a translation is the camera position turned by the view's
// rotation, so hundreds of metres from the world origin (where levels are
// played) a tenth of a degree of shake already moved it more than ShakeReach.
constexpr float ShakeReach = .3f, ShakeTurn = .2f;
inline bool shakenWeaponView(const Matrix34 &a, const Matrix34 &b) {
    for (unsigned i = 0; i != 12; ++i) {
        if (!std::isfinite(a.m[i]) || !std::isfinite(b.m[i])) return false;
        if (i % 4 != 3 && std::abs(a.m[i] - b.m[i]) > ShakeTurn) return false;
    }
    const Vec3 d = viewCamera(a) - viewCamera(b);
    return std::isfinite(d.x + d.y + d.z) && dot(d, d) <= ShakeReach * ShakeReach;
}
inline bool sameProjectionXY(const Matrix44 &a, const Matrix44 &b) {
    // Native execution may revise Z clipping. Every other coefficient must
    // retain this exact prepared asymmetric eye's projection provenance.
    for (unsigned i = 0; i != 16; ++i) {
        if (i == 10 || i == 11) continue;
        if (!std::isfinite(a.m[i]) || !std::isfinite(b.m[i]) ||
            std::abs(a.m[i] - b.m[i]) > .00001f) return false;
    }
    return true;
}
// The water mirror's projection: its whole depth row is the oblique near
// plane at the water (common/water_mirror.hpp); x, y and w identify it.
inline bool sameProjectionXYW(const Matrix44 &a, const Matrix44 &b) {
    for (unsigned i = 0; i != 16; ++i) {
        if (i >= 8 && i < 12) continue;
        if (!std::isfinite(a.m[i]) || !std::isfinite(b.m[i]) || std::abs(a.m[i] - b.m[i]) > .00001f) return false;
    }
    return true;
}
inline WeaponWorldView executedWeaponView(const WeaponWorldView &prepared,
                                          const Matrix34 &view, const Matrix44 &projection,
                                          float nearDepth, float farDepth) {
    const bool valid = prepared.valid && finiteMatrix(view) && finiteProjection(projection) &&
                       validDepthRange(nearDepth, farDepth) &&
                       sameWeaponView(prepared.view, view) &&
                       sameProjectionXY(prepared.projection, projection) &&
                       nearDepth == prepared.nearDepth && farDepth == prepared.farDepth;
    return {view, projection, nearDepth, farDepth, valid};
}
// Only the fingerprinted, first-root execution capture may use this adapter.
// Native background collection partitions full-depth root [0,1] into world
// [0,float32(0.9)] plus sky [float32(0.9),1]. Validate a value copy; never change
// native prepared commands, accept arbitrary subranges, or alter restoration.
inline WeaponWorldView executedRootWeaponView(const WeaponWorldView &prepared,
                                              const Matrix34 &view,const Matrix44 &projection,
                                              float nearDepth,float farDepth) {
    auto expected=prepared;
    if(std::bit_cast<uint32_t>(prepared.nearDepth)==0 &&
       std::bit_cast<uint32_t>(prepared.farDepth)==0x3f800000u &&
       std::bit_cast<uint32_t>(nearDepth)==0 && std::bit_cast<uint32_t>(farDepth)==0x3f666666u)
        expected.farDepth=farDepth;
    return executedWeaponView(expected,view,projection,nearDepth,farDepth);
}
// Ephemeral native Render invocation ordering, not simulation/gameplay state.
struct WeaponViewPass {
    WeaponWorldView world;
    unsigned stage = 0;
    bool failed = false;
    bool graphicsSetupReached = false;
    void graphicsSetup() { graphicsSetupReached = true; }
    bool advance(unsigned expected) {
        if (failed || stage != expected) {
            failed = true;
            return false;
        }
        ++stage;
        return true;
    }
    bool projection(Matrix44 &out) {
        graphicsSetup(); // Native caller installs even a rejected/fallback result.
        if (!world.valid || !advance(0)) { failed = true; return false; }
        out = world.projection;
        return true;
    }
    bool view(Matrix34 &out) {
        graphicsSetup();
        if (!advance(1)) return false;
        out = world.view;
        return true;
    }
    bool depth(float &nearDepth, float &farDepth) {
        graphicsSetup();
        if (!advance(2)) return false;
        nearDepth = world.nearDepth;
        farDepth = world.farDepth;
        return true;
    }
    bool placed(bool success) {
        if (!success) { failed = true; return false; }
        return advance(3);
    }
    bool restored() { return advance(4); }
    bool complete() const { return !failed && (stage == 0 || stage == 5); }
    bool cleanupRequired() const { return graphicsSetupReached && !complete(); }
};
// Used by the render wrappers, including suppression barriers for unowned
// nested calls. Native exceptions are not caught by this context guard.
template<class T> struct ScopedWeaponContext {
    T *&slot;
    T *previous;
    ScopedWeaponContext(T *&slot, T *current) : slot(slot), previous(slot) { slot = current; }
    ~ScopedWeaponContext() { slot = previous; }
    ScopedWeaponContext(const ScopedWeaponContext &) = delete;
};
} // namespace ss2vr
