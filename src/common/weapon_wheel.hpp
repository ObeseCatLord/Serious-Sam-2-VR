#pragma once
#include "math.hpp"
#include "ui.hpp"
#include <algorithm>
#include <cmath>

namespace ss2vr {
// Hand-anchored weapon wheel geometry. Weapon icons and action bubbles float on
// a ring around the point where the hand opened the wheel; reaching onto an item
// and releasing selects it. Geometry only: native inventory, equip validation
// and player commands remain their owners.
enum class WheelBubble : int32_t { None = -1, Netricsa, Menu, QuickSave, QuickLoad };
inline constexpr uint32_t WheelBubbleCount = 4, WheelSlotCount = WeaponCount + WheelBubbleCount;
inline constexpr uint32_t wheelBubbleBit(WheelBubble bubble) {
    return bubble == WheelBubble::None ? 0u : 1u << uint32_t(bubble);
}
inline constexpr uint32_t WheelAllBubbles = (1u << WheelBubbleCount) - 1;
// WheelUi::hover keeps its fixed layout: [0,count) is a weapon list index, -1 is
// nothing, and -2-kind names a hovered bubble.
inline constexpr int32_t wheelBubbleHover(WheelBubble bubble) {
    return -2 - int32_t(bubble);
}
inline WheelBubble wheelHoveredBubble(int32_t hover) {
    return hover <= -2 && hover > -2 - int32_t(WheelBubbleCount) ? WheelBubble(-2 - hover) : WheelBubble::None;
}
inline const wchar_t *wheelBubbleName(WheelBubble bubble) {
    switch (bubble) {
    case WheelBubble::Netricsa: return L"Netricsa";
    case WheelBubble::Menu: return L"Game menu";
    case WheelBubble::QuickSave: return L"Quick save";
    case WheelBubble::QuickLoad: return L"Quick load";
    default: return L"";
    }
}
// Arc length per weapon; the ring radius grows with the weapon count within reach.
inline constexpr float WheelItemSpacing = .075f, WheelMinimumRadius = .12f, WheelMaximumRadius = .2f;
// The ring centre sits this far ahead of the opening hand, along its pointing
// direction, so the user points or reaches into the wheel.
inline constexpr float WheelForwardOffset = .15f;
// Action bubbles form a separate short arc beyond the ring on the hand's outer
// side (right of a right-hand ring, left of a left-hand one), top to bottom.
inline constexpr float WheelBubbleGap = .11f, WheelBubbleSpacing = .06f, WheelBubbleSize = .048f;
// A weapon hovers inside this radial band of the ring; a bubble near its centre.
inline constexpr float WheelDeadZone = .4f, WheelOuterReach = .055f, WheelBubbleReach = .6f;
// Reach mode: the hand within this depth of the ring plane hovers by position.
// Nearer the user it points: its aim ray, up to WheelPointReach, hovers instead.
inline constexpr float WheelDepthReach = .16f, WheelPointDepth = .04f, WheelPointReach = .8f;
// Stick fallback keeps the original thumbstick selection threshold.
inline constexpr float WheelStickDeadZone = .3f;
// Ring frame in tracking reference space: upright, local +Z facing the user's
// head and +X to the user's right. A centre directly below the head falls back
// to the head's heading.
inline Pose wheelRingFrame(Vec3 centre, Pose head) {
    const Vec3 away{centre.x - head.p.x, 0, centre.z - head.p.z};
    const float heading = dot(away, away) > .0025f ? std::atan2(-away.x, -away.z) : horizontalHeading(head.q);
    return {yaw(heading), centre};
}
struct WheelSlot {
    int32_t weapon = -1, index = -1; // Native weapon id and its WheelUi list index.
    WheelBubble bubble = WheelBubble::None;
};
struct WheelRing {
    Pose frame;
    float radius = 0, itemSize = 0;
    uint32_t count = 0, weapons = 0; // Weapon slots first, then bubbles.
    bool anchored = false;
    WheelSlot slot[WheelSlotCount];
    // Weapons run clockwise from the top. Bubbles sit on the arc beyond the
    // ring on the outer side: outer > 0 for a right hand, < 0 for a left hand.
    void layout(const int *ids, uint32_t weaponCount, uint32_t bubbles, float outer = 1) {
        count = 0;
        weapons = std::min(weaponCount, WeaponCount);
        const float step = weapons ? 2 * Pi / weapons : 0;
        radius = std::clamp(weapons * WheelItemSpacing / (2 * Pi), WheelMinimumRadius, WheelMaximumRadius);
        const float chord = weapons > 1 ? 2 * radius * std::sin(Pi / weapons) : 2 * radius;
        itemSize = std::min(.065f, .82f * chord);
        for (uint32_t i = 0; i != weapons; ++i) {
            slot[count] = {ids[i], int32_t(i), WheelBubble::None};
            angle[count] = float(i) * step;
            position[count++] = {std::sin(float(i) * step) * radius, std::cos(float(i) * step) * radius, 0};
        }
        uint32_t bubbleCount = 0;
        for (uint32_t b = 0; b != WheelBubbleCount; ++b)
            bubbleCount += (bubbles >> b) & 1u;
        const float side = outer < 0 ? -1.f : 1.f, arc = radius + WheelBubbleGap;
        const float centre = side > 0 ? Pi / 2 : 3 * Pi / 2, bubbleStep = WheelBubbleSpacing / arc;
        for (uint32_t b = 0, j = 0; b != WheelBubbleCount; ++b)
            if ((bubbles >> b) & 1u) {
                const float a = centre - side * ((float(bubbleCount) - 1) * .5f - float(j++)) * bubbleStep;
                slot[count] = {-1, -1, WheelBubble(b)};
                angle[count] = a;
                position[count++] = {std::sin(a) * arc, std::cos(a) * arc, 0};
            }
    }
    // Clockwise from up, as seen by the user.
    float slotAngle(uint32_t i) const {
        return i < count ? angle[i] : 0;
    }
    Vec3 slotPosition(uint32_t i) const {
        return i < count ? position[i] : Vec3{};
    }
    // Diameter of the slot's disc.
    float slotSize(uint32_t i) const {
        return i < weapons ? itemSize : WheelBubbleSize;
    }
    // Nearest weapon slot by direction; bubbles are not on the ring.
    int nearest(float direction, int current) const {
        if (!weapons || !std::isfinite(direction))
            return -1;
        const float step = 2 * Pi / weapons;
        int best = -1;
        float bestDistance = 0;
        for (uint32_t i = 0; i != weapons; ++i) {
            const float distance = std::abs(std::remainder(direction - angle[i], 2 * Pi));
            if (int(i) == current && distance <= .6f * step)
                return current; // Hysteresis: hovering survives a boundary jitter.
            if (best < 0 || distance < bestDistance) {
                best = int(i);
                bestDistance = distance;
            }
        }
        return best;
    }
    int pickStick(float x, float y, int current) const {
        if (!std::isfinite(x) || !std::isfinite(y) || std::sqrt(x * x + y * y) < WheelStickDeadZone)
            return -1;
        return nearest(std::atan2(x, y), current);
    }
    // A point in the ring plane (frame-local x, y): a bubble near its centre,
    // else a weapon sector within the ring's radial band, else nothing.
    int pickPlanar(float x, float y, int current) const {
        if (!std::isfinite(x) || !std::isfinite(y))
            return -1;
        int best = -1;
        float bestDistance = 0;
        for (uint32_t i = weapons; i < count; ++i) {
            const float dx = x - position[i].x, dy = y - position[i].y;
            const float distance = std::sqrt(dx * dx + dy * dy);
            const float reach = WheelBubbleSpacing * (int(i) == current ? .75f : WheelBubbleReach);
            if (distance <= reach && (best < 0 || distance < bestDistance)) {
                best = int(i);
                bestDistance = distance;
            }
        }
        if (best >= 0)
            return best;
        const float planar = std::sqrt(x * x + y * y);
        if (planar < WheelDeadZone * radius || planar > radius + WheelOuterReach)
            return -1;
        return nearest(std::atan2(x, y), current < int(weapons) ? current : -1);
    }
    // The hand's grip point, in the frame's reference space. Within the ring's
    // depth band it hovers by position; nearer the user its aim ray points.
    int pickHand(Vec3 hand, int current) const {
        return pick(hand, hand, Quat{}, false, current);
    }
    int pick(Vec3 hand, Vec3 aimOrigin, Quat aim, bool aimValid, int current) const {
        if (!anchored || !count || !finite(frame) || !std::isfinite(hand.x) || !std::isfinite(hand.y) ||
            !std::isfinite(hand.z))
            return -1;
        const Quat into = inverse(frame.q);
        const Vec3 local = rotate(into, hand - frame.p);
        if (std::abs(local.z) <= WheelPointDepth || (local.z < 0 && local.z >= -WheelDepthReach) ||
            (local.z > 0 && local.z <= WheelDepthReach && !aimValid))
            return pickPlanar(local.x, local.y, current);
        if (local.z < 0 || !aimValid)
            return -1;
        const Vec3 origin = rotate(into, aimOrigin - frame.p), direction = rotate(into, rotate(aim, {0, 0, -1}));
        if (!std::isfinite(origin.z) || !std::isfinite(direction.z) || direction.z > -.2f)
            return -1;
        const float distance = -origin.z / direction.z;
        if (!(distance >= 0) || distance > WheelPointReach)
            return -1;
        const Vec3 hit = origin + direction * distance;
        return pickPlanar(hit.x, hit.y, current);
    }

private:
    float angle[WheelSlotCount]{};
    Vec3 position[WheelSlotCount]{};
};
// One hand's sampled pointer for the ring: the weapon grip point, the aim pose
// and the head, all from the same input sample. Invalid samples cannot hover.
struct WheelPointer {
    bool valid = false;
    Vec3 hand{};
    Pose head{};
    Pose aim{};
};
inline WheelPointer wheelPointer(const Input &input, unsigned hand) {
    if (hand >= 2 || !input.handValid[hand] || !input.headValid)
        return {};
    const Pose tracked = weaponTracking(input, hand);
    if (!finite(tracked) || !finite(input.head) || !finite(input.hand[hand]))
        return {};
    return {true, tracked.p, input.head, {normalize(input.hand[hand].q), input.hand[hand].p}};
}
// Where a wheel opened by this pointer is centred: ahead of the grip along the
// aim direction, upright and facing the head.
inline Pose wheelRingFrame(const WheelPointer &pointer) {
    const Vec3 forward = rotate(pointer.aim.q, {0, 0, -1});
    const float length = std::sqrt(dot(forward, forward));
    const Vec3 ahead = std::isfinite(length) && length > .5f ? forward * (WheelForwardOffset / length) : Vec3{};
    return wheelRingFrame(pointer.hand + ahead, pointer.head);
}
// Axis-aligned model-space box in the native Box3f layout (min, then max).
struct WheelBox {
    Vec3 min{}, max{};
};
static_assert(sizeof(WheelBox) == 24);
inline bool wheelBoxValid(const WheelBox &box) {
    const Vec3 size = box.max - box.min;
    return std::isfinite(box.min.x) && std::isfinite(box.min.y) && std::isfinite(box.min.z) &&
           std::isfinite(box.max.x) && std::isfinite(box.max.y) && std::isfinite(box.max.z) && size.x >= 0 &&
           size.y >= 0 && size.z >= 0 && std::max({size.x, size.y, size.z}) > .001f &&
           std::max({size.x, size.y, size.z}) < 50.f;
}
// What a display mesh is normalised by. The render meshes' own boxes give the
// drawn geometry: their union is what the engine's configuration recalculation
// forms. A stock pickup's stored configuration box is authored independently
// (larger or smaller than the mesh), so it is used only when no mesh box is.
struct WheelModelBounds {
    Vec3 centre{};
    float extent = 0;
    bool fromMeshes = false;
};
inline WheelModelBounds wheelModelBounds(const WheelBox *meshes, uint32_t count, const WheelBox &configuration) {
    WheelBox merged{};
    bool any = false;
    for (uint32_t i = 0; meshes && i != count; ++i) {
        if (!wheelBoxValid(meshes[i]))
            continue;
        merged = any ? WheelBox{{std::min(merged.min.x, meshes[i].min.x), std::min(merged.min.y, meshes[i].min.y),
                                 std::min(merged.min.z, meshes[i].min.z)},
                                {std::max(merged.max.x, meshes[i].max.x), std::max(merged.max.y, meshes[i].max.y),
                                 std::max(merged.max.z, meshes[i].max.z)}}
                     : meshes[i];
        any = true;
    }
    if (!any)
        merged = configuration;
    if (!wheelBoxValid(merged))
        return {};
    const Vec3 size = merged.max - merged.min;
    return {(merged.min + merged.max) * .5f, std::max({size.x, size.y, size.z}), any};
}
// Floating weapon meshes are about 9.5 cm long, smaller only on a crowded ring.
inline float wheelModelSize(float itemSize, bool hovered) {
    return std::min(.095f, 1.7f * itemSize) * (hovered ? 1.3f : 1.f);
}
// A mesh whose unit-stretch box has this centre and longest extent: uniformly
// scaled to `size`, spun about the ring's upright axis, box centred on the slot.
struct WheelModelPlacement {
    Pose pose;
    float scale = 0;
};
inline WheelModelPlacement wheelModelPlacement(Pose ring, Vec3 slot, Vec3 centre, float extent, float size,
                                               float spin) {
    if (!std::isfinite(extent) || extent <= 0 || !std::isfinite(size) || size <= 0 || !std::isfinite(spin) ||
        !finite(ring))
        return {};
    const float scale = size / extent;
    const Quat orientation = normalize(multiply(ring.q, yaw(spin)));
    const Pose pose{orientation, ring.p + rotate(ring.q, slot) - rotate(orientation, centre * scale)};
    return finite(pose) && std::isfinite(scale) ? WheelModelPlacement{pose, scale} : WheelModelPlacement{};
}
} // namespace ss2vr
