#pragma once
#include "math.hpp"
#include <cstring>

namespace ss2vr {
// Fixed virtual handle centre for the matched stock ID1 assembly, in native
// pre-stretch model coordinates. Reference: V27 request31/eye0/right idle pose.
// This does not define physical skin contact, a rest pose or another weapon.
constexpr Vec3 Id1ModelHandleReference{-.00960000045598f, -.00474976426398f, -.536467618906f};
// Stock ID13 virtual handle-section centre in the authored native Idle model
// frame. Both inspected variants and own consumed Black draws agree on identity.
// This convention does not define skin contact or another weapon's reference.
constexpr Vec3 Id13ModelHandleReference{0.f, .003122344828160041f, .006718217565474383f};
// Grip anchor per native weapon, in pre-stretch first-person model coordinates:
// the point placed at the controller's grip. First-person models put their
// origin near the camera with the gun up to a metre away, so every weapon needs
// one. Revolvers, uzi and double shotgun use their authored Hand bone (logged by
// the private weapon audit, V101); zap gun and sniper keep their measured
// handles; the rest are handle points chosen on each model and checked against
// the controller position in lab captures.
struct WeaponGrip {
    uint32_t nativeId;
    Vec3 reference;
};
inline constexpr WeaponGrip WeaponGrips[] = {
    {0, {0.f, .11f, .02f}},          // Circular saw: top trigger handle (V111 side view)
    {1, Id1ModelHandleReference},     // Zap gun
    {2, {0.f, -.075f, -.091f}},      // Auto shotgun
    {3, {-.0379f, -.021f, -.2626f}}, // Double shotgun: Hand bone
    {4, {0.f, .0999f, -.0632f}},     // Uzi: Hand bone
    {5, {0.f, -.059f, .264f}},       // Minigun: rear handle
    {6, {0.f, -.118f, -.058f}},      // Rocket launcher: rear grip
    {7, {0.f, .058f, .008f}},        // Grenade launcher: rear grip
    {8, {0.f, .041f, 0.f}},          // Plasma rifle
    {9, {0.f, -.08f, .226f}},        // Klodovik
    {10, {0.f, -.065f, .117f}},      // Cannon: rear handle
    {11, {0.f, -.118f, -.531f}},     // Serious bomb
    {12, {.0114f, -.0016f, -.3308f}},// Colt revolvers: Hand bone
    {13, Id13ModelHandleReference},   // Sniper
    {15, {0.f, .003f, -.113f}},      // Beam gun: rear grip
    {16, {0.f, 0.f, -.3f}},          // Flamer
};
// Per-weapon display scale so Sam's hands are the same size whatever he holds.
// Weapons whose forearm is visible (zap gun, uzi, Klodovik, bomb, revolvers)
// are sized so the forearm behind the glove is ~13 cm thick (V111 side views
// at equal distance); the rest keep the flat-screen-matched size
// 1 / (|grip in camera space| * tan(weaponFov / 2)) from the V101 audit.
inline constexpr float WeaponDisplayScales[WeaponCount] = {
    .98f, 3.18f, 1.29f, 1.51f, 3.51f, .95f, .79f, 1.09f, 2.22f, .89f, .87f, 1.2f, 1.29f, 1.30f, 1.f, 1.14f, 1.2f};
constexpr float weaponDisplayScale(uint32_t nativeId) {
    return nativeId < WeaponCount ? WeaponDisplayScales[nativeId] : 1.f;
}
constexpr bool weaponAlignmentSupported(uint32_t nativeId) {
    for (const auto &grip : WeaponGrips)
        if (grip.nativeId == nativeId) return true;
    return false;
}
// Binding freshness is independent of having an authored grip correction.
// The installed native inventory has seventeen slots; slot fourteen is unused.
constexpr bool weaponModelBindingSupported(uint32_t nativeId) {
    return nativeId < WeaponCount && nativeId != 14;
}
inline bool weaponModelHandleReference(uint32_t nativeId,Vec3 &out) {
    out={};
    for (const auto &grip : WeaponGrips)
        if (grip.nativeId == nativeId) {
            out = grip.reference;
            return true;
        }
    return false;
}

inline bool alignmentBorrowPhase(unsigned renderDepth, bool placement) {
    return renderDepth == (placement ? 1u : 0u);
}
// Stack-owned wrapper provenance, not a cached native object or new ownership
// policy. A desktop sniper's original render delegates to the original base.
struct WeaponRenderRoute {
    uintptr_t weapon=0;
    unsigned depth=0;
    bool sniper=false,desktop=false,originalActive=false;
    const WeaponRenderRoute *parent=nullptr;
};
inline bool nativeDesktopSniperPlacement(const WeaponRenderRoute *route,uintptr_t weapon,
                                         int eye,bool nativePlacementCaller) {
    if(!route || !route->parent || !weapon || eye!=-1 || !nativePlacementCaller)return false;
    const auto &parent=*route->parent;
    return route->weapon==weapon && parent.weapon==weapon && route->depth==2 && parent.depth==1 &&
        !route->sniper && parent.sniper && route->desktop && parent.desktop &&
        route->originalActive && parent.originalActive;
}
inline bool id1RenderedStretch(uint32_t selector, Vec3 base, Vec3 &out) {
    if (!std::isfinite(base.x) || !std::isfinite(base.y) || !std::isfinite(base.z)) return false;
    out = base;
    if (!selector) {
        // Exact native SetStretch floor, including signed zero and nonuniform
        // scale. The native branch flips X, floors all components, then renders.
        const auto floor = [](float x) {
            constexpr float epsilon = std::bit_cast<float>(uint32_t{0x3727c5ac});
            return std::copysign(std::max(std::fabs(x),epsilon),x);
        };
        out = {floor(-base.x), floor(base.y), floor(base.z)};
    }
    return true;
}
// Value-only identities. The copied instance address is never dereferenced from
// a cache; consumption resolves the current native handle and copies it anew.
struct WeaponAlignmentBinding {
    uint32_t owner=0, weapon=0, model=0, instance=0, selector=0, hand=2;
    uint32_t configuration=0, file=0;
    int32_t resource=-1;
    Vec3 baseStretch{};
    uint32_t nativeId=0;
    bool operator==(const WeaponAlignmentBinding &other) const {
        return nativeId==other.nativeId && owner==other.owner && weapon==other.weapon && model==other.model &&
            instance==other.instance && selector==other.selector && hand==other.hand &&
            configuration==other.configuration && file==other.file && resource==other.resource &&
            std::memcmp(&baseStretch,&other.baseStretch,sizeof(Vec3))==0;
    }
};
inline bool weaponModelCalibrationMatches(const WeaponAlignmentBinding &retained,
                                          const WeaponAlignmentBinding &current,
                                          bool alignmentApplied) {
    return weaponModelBindingSupported(retained.nativeId) &&
           alignmentApplied == weaponAlignmentSupported(retained.nativeId) && retained == current;
}
} // namespace ss2vr
