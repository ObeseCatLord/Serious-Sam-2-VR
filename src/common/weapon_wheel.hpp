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
// Arc length per item; the radius grows with the item count within reach.
inline constexpr float WheelItemSpacing = .075f, WheelMinimumRadius = .12f, WheelMaximumRadius = .2f;
// The hand must leave the centre (cancel) zone and stay near the ring plane.
inline constexpr float WheelDeadZone = .4f, WheelOuterReach = .09f, WheelDepthReach = .16f;
// Stick fallback keeps the original thumbstick selection threshold.
inline constexpr float WheelStickDeadZone = .3f;
// Ring frame in tracking reference space: origin at the opening hand, upright,
// local +Z facing the user's head and +X to the user's right. A hand directly
// below the head falls back to the head's heading.
inline Pose wheelRingFrame(Vec3 hand, Pose head) {
    const Vec3 away{hand.x - head.p.x, 0, hand.z - head.p.z};
    const float heading = dot(away, away) > .0025f ? std::atan2(-away.x, -away.z) : horizontalHeading(head.q);
    return {yaw(heading), hand};
}
struct WheelSlot {
    int32_t weapon = -1, index = -1; // Native weapon id and its WheelUi list index.
    WheelBubble bubble = WheelBubble::None;
};
struct WheelRing {
    Pose frame;
    float radius = 0, itemSize = 0;
    uint32_t count = 0;
    bool anchored = false;
    WheelSlot slot[WheelSlotCount];
    // Bubbles sit together at the bottom, left to right in enum order; weapons
    // run clockwise from just left of them, over the top. Without bubbles the
    // first weapon is at the top, as on the original stick wheel.
    void layout(const int *ids, uint32_t weapons, uint32_t bubbles) {
        count = 0;
        uint32_t bubbleCount = 0;
        for (uint32_t b = 0; b != WheelBubbleCount; ++b)
            bubbleCount += (bubbles >> b) & 1u;
        weapons = std::min(weapons, WeaponCount);
        const uint32_t total = weapons + bubbleCount;
        const float step = total ? 2 * Pi / total : 0;
        const float leftmost = (float(bubbleCount) - 1) * .5f;
        radius = std::clamp(total * WheelItemSpacing / (2 * Pi), WheelMinimumRadius, WheelMaximumRadius);
        const float chord = total > 1 ? 2 * radius * std::sin(Pi / total) : 2 * radius;
        itemSize = std::min(.065f, .82f * chord);
        for (uint32_t b = 0, j = 0; b != WheelBubbleCount; ++b)
            if ((bubbles >> b) & 1u) {
                slot[count] = {-1, -1, WheelBubble(b)};
                angle[count++] = Pi + (leftmost - float(j++)) * step;
            }
        for (uint32_t i = 0; i != weapons; ++i) {
            slot[count] = {ids[i], int32_t(i), WheelBubble::None};
            angle[count++] = bubbleCount ? Pi + (leftmost + 1 + float(i)) * step : float(i) * step;
        }
    }
    // Clockwise from up, as seen by the user.
    float slotAngle(uint32_t i) const {
        return i < count ? angle[i] : 0;
    }
    Vec3 slotPosition(uint32_t i) const {
        const float a = slotAngle(i);
        return {std::sin(a) * radius, std::cos(a) * radius, 0};
    }
    int nearest(float direction, int current) const {
        if (!count || !std::isfinite(direction))
            return -1;
        const float step = 2 * Pi / count;
        int best = -1;
        float bestDistance = 0;
        for (uint32_t i = 0; i != count; ++i) {
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
    // The hand's tracked point, in the same reference space as the frame.
    int pickHand(Vec3 hand, int current) const {
        if (!anchored || !count || !finite(frame) || !std::isfinite(hand.x) || !std::isfinite(hand.y) ||
            !std::isfinite(hand.z))
            return -1;
        const Vec3 local = rotate(inverse(frame.q), hand - frame.p);
        const float planar = std::sqrt(local.x * local.x + local.y * local.y);
        if (!std::isfinite(planar) || planar < WheelDeadZone * radius || planar > radius + WheelOuterReach ||
            std::abs(local.z) > WheelDepthReach)
            return -1;
        return nearest(std::atan2(local.x, local.y), current);
    }

private:
    float angle[WheelSlotCount]{};
};
// One hand's sampled pointer for the ring: the weapon grip point and the head,
// both from the same input sample. Invalid samples cannot hover by position.
struct WheelPointer {
    bool valid = false;
    Vec3 hand{};
    Pose head{};
};
inline WheelPointer wheelPointer(const Input &input, unsigned hand) {
    if (hand >= 2 || !input.handValid[hand] || !input.headValid)
        return {};
    const Pose tracked = weaponTracking(input, hand);
    if (!finite(tracked) || !finite(input.head))
        return {};
    return {true, tracked.p, input.head};
}
} // namespace ss2vr
