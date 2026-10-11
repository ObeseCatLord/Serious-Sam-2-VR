#include "common/controls.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#ifdef NDEBUG
#error Weapon wheel checks require active assertions
#endif
using namespace ss2vr;

namespace {
bool near(float a, float b, float e = 1e-4f) { return std::abs(a - b) <= e; }
const Pose Head{{0, 0, 0, 1}, {0, 1.6f, 0}};
const Vec3 Hand{.25f, 1.1f, -.45f}; // Right hand, low and in front of the user.
const int Ids[]{5, 3, 12, 6, 13};
constexpr uint32_t Count = sizeof(Ids) / sizeof(Ids[0]);
// A pointer at this grip position, aiming straight ahead (-Z) from the grip.
WheelPointer at(Vec3 hand) { return {true, hand, Head, {{}, hand}}; }
// Ring centre for a ring opened at Hand: WheelForwardOffset ahead along the aim.
const Vec3 Centre{Hand.x, Hand.y, Hand.z - WheelForwardOffset};
// World position of a slot for a ring opened at Hand.
Vec3 slotPoint(const WeaponWheel &w, uint32_t slot, float scale = 1, float depth = 0) {
    Vec3 local = w.ring.slotPosition(slot) * scale;
    local.z = depth;
    return w.ring.frame.p + rotate(w.ring.frame.q, local);
}
int slotOf(const WheelRing &ring, int weapon, WheelBubble bubble = WheelBubble::None) {
    for (uint32_t i = 0; i != ring.count; ++i)
        if (ring.slot[i].weapon == weapon && ring.slot[i].bubble == bubble)
            return int(i);
    return -1;
}
// Opens at Hand, hovers the slot by moving the hand there, then releases.
int chooseSlot(WeaponWheel &w, int (*find)(const WheelRing &), uint32_t bubbles = WheelAllBubbles) {
    w.update(false, 0, 0, Ids, Count, true, at(Hand), bubbles);
    w.update(true, 0, 0, Ids, Count, true, at(Hand), bubbles);
    assert(w.open && w.ring.anchored && w.hover == -1);
    const int slot = find(w.ring);
    assert(slot >= 0);
    const Vec3 target = slotPoint(w, uint32_t(slot));
    w.update(true, 0, 0, Ids, Count, true, at(target), bubbles);
    assert(w.hoverSlot == slot);
    return w.update(false, 0, 0, Ids, Count, true, at(target), bubbles);
}

void layout() {
    WheelRing ring;
    ring.layout(Ids, Count, WheelAllBubbles); // Right hand.
    assert(ring.count == Count + WheelBubbleCount && ring.weapons == Count);
    // Weapons alone fill the ring, clockwise from the top.
    const float step = 2 * Pi / float(Count);
    assert(ring.slot[0].weapon == Ids[0] && near(ring.slotPosition(0).x, 0) && ring.slotPosition(0).y > 0);
    for (uint32_t i = 1; i != Count; ++i)
        assert(near(std::remainder(ring.slotAngle(uint32_t(slotOf(ring, Ids[i]))) -
                                   ring.slotAngle(uint32_t(slotOf(ring, Ids[i - 1]))), 2 * Pi), step));
    for (uint32_t i = 0; i != Count; ++i)
        assert(near(std::sqrt(dot(ring.slotPosition(i), ring.slotPosition(i))), ring.radius) &&
               near(ring.slotSize(i), ring.itemSize));
    assert(ring.radius >= WheelMinimumRadius && ring.radius <= WheelMaximumRadius);
    assert(ring.itemSize > .03f && ring.itemSize <= .065f);
    // Bubbles: a compact arc beyond the ring on the outer (right) side, top to
    // bottom in enum order, outside every weapon's pick band.
    float previousY = 1;
    for (uint32_t b = 0; b != WheelBubbleCount; ++b) {
        const int slot = slotOf(ring, -1, WheelBubble(b));
        assert(slot >= int(Count));
        const Vec3 p = ring.slotPosition(uint32_t(slot));
        assert(p.x > ring.radius && p.y < previousY && near(ring.slotSize(uint32_t(slot)), WheelBubbleSize));
        assert(near(std::sqrt(dot(p, p)), ring.radius + WheelBubbleGap));
        assert(std::sqrt(dot(p, p)) - WheelBubbleSpacing * .75f > ring.radius + WheelOuterReach);
        previousY = p.y;
        if (b) {
            const Vec3 q = ring.slotPosition(uint32_t(slotOf(ring, -1, WheelBubble(b - 1))));
            assert(near(std::sqrt(dot(p - q, p - q)), WheelBubbleSpacing, 2e-3f));
        }
    }
    // A left-hand wheel mirrors the arc to its left.
    WheelRing left;
    left.layout(Ids, Count, WheelAllBubbles, -1);
    for (uint32_t i = Count; i != left.count; ++i)
        assert(near(left.slotPosition(i).x, -ring.slotPosition(i).x) &&
               near(left.slotPosition(i).y, ring.slotPosition(i).y));
    // Items never overlap their neighbours, even with the largest inventory.
    int all[WeaponCount];
    for (uint32_t i = 0; i != WeaponCount; ++i) all[i] = int(i);
    ring.layout(all, WeaponCount, WheelAllBubbles);
    assert(ring.count == WheelSlotCount);
    const Vec3 a = ring.slotPosition(0), b = ring.slotPosition(1);
    assert(std::sqrt(dot(a - b, a - b)) > ring.itemSize);
    // Only the offered bubbles appear; quick save/load are absent outside single player.
    ring.layout(Ids, Count, wheelBubbleBit(WheelBubble::Netricsa) | wheelBubbleBit(WheelBubble::Menu));
    assert(ring.count == Count + 2 && slotOf(ring, -1, WheelBubble::QuickSave) < 0 &&
           slotOf(ring, -1, WheelBubble::QuickLoad) < 0 && slotOf(ring, -1, WheelBubble::Menu) >= 0);
    // An empty inventory still offers the bubbles.
    ring.layout(Ids, 0, WheelAllBubbles);
    assert(ring.count == WheelBubbleCount && ring.weapons == 0);
}

void frame() {
    const Pose f = wheelRingFrame(Hand, Head);
    const Vec3 toward = rotate(f.q, {0, 0, 1}), right = rotate(f.q, {1, 0, 0}), up = rotate(f.q, {0, 1, 0});
    Vec3 head{Head.p.x - Hand.x, 0, Head.p.z - Hand.z};
    head = head * (1 / std::sqrt(dot(head, head)));
    assert(near(dot(toward, head), 1) && near(up.y, 1) && near(f.p.x, Hand.x) && near(f.p.z, Hand.z));
    // +X is the user's right: from the head, the right axis points rightward of the ring.
    assert(cross(head * -1, Vec3{0, 1, 0}).x * right.x + cross(head * -1, Vec3{0, 1, 0}).z * right.z > .99f);
    // A hand directly below the head falls back to the head's heading.
    const Pose turned{yaw(Pi / 2), Head.p};
    const Pose below = wheelRingFrame({Head.p.x, .9f, Head.p.z}, turned);
    assert(near(dot(rotate(below.q, {0, 0, -1}), rotate(turned.q, {0, 0, -1})), 1));
    // A pointer opens the ring WheelForwardOffset ahead along its aim.
    const Pose ahead = wheelRingFrame(at(Hand));
    assert(near(ahead.p.x, Centre.x) && near(ahead.p.y, Centre.y) && near(ahead.p.z, Centre.z));
    WheelPointer pitched = at(Hand);
    pitched.aim.q = {std::sin(-Pi / 8), 0, 0, std::cos(-Pi / 8)}; // Aiming 45 degrees down.
    const Pose low = wheelRingFrame(pitched);
    assert(near(low.p.y, Hand.y - WheelForwardOffset * std::sin(Pi / 4)) && near(rotate(low.q, {0, 1, 0}).y, 1));
}

void hoverPick() {
    WeaponWheel w;
    w.update(true, 0, 0, Ids, Count, true, at(Hand), WheelAllBubbles);
    const WheelRing &ring = w.ring;
    assert(near(ring.frame.p.x, Centre.x) && near(ring.frame.p.y, Centre.y) && near(ring.frame.p.z, Centre.z));
    for (uint32_t i = 0; i != ring.count; ++i)
        assert(ring.pickHand(slotPoint(w, i), -1) == int(i));
    for (uint32_t i = 0; i != Count; ++i) // Reaching short, past or through the plane is enough.
        assert(ring.pickHand(slotPoint(w, i, .6f, .03f), -1) == int(i) &&
               ring.pickHand(slotPoint(w, i, 1, -.1f), -1) == int(i));
    // The centre is the cancel zone; far outside and far in depth hover nothing.
    assert(ring.pickHand(ring.frame.p, -1) == -1);
    assert(ring.pickHand(slotPoint(w, 0, WheelDeadZone * .9f), -1) == -1);
    assert(ring.pickHand(slotPoint(w, 0, 1 + (WheelOuterReach + .01f) / ring.radius), -1) == -1);
    assert(ring.pickHand(slotPoint(w, 0, 1, -WheelDepthReach - .02f), -1) == -1);
    assert(ring.pickHand(slotPoint(w, 0, 1, WheelDepthReach + .02f), -1) == -1); // No aim: no pointing.
    assert(ring.pickHand({NAN, 0, 0}, -1) == -1);
    // Hysteresis: just past the boundary the current item remains hovered.
    const float step = 2 * Pi / float(Count), boundary = ring.slotAngle(0) + step * .55f;
    const Vec3 local{std::sin(boundary) * ring.radius, std::cos(boundary) * ring.radius, 0};
    const Vec3 point = ring.frame.p + rotate(ring.frame.q, local);
    assert(ring.pickHand(point, 0) == 0 && ring.pickHand(point, -1) == slotOf(ring, Ids[1]));
    // The ring stays where it opened while the hand moves.
    w.update(true, 0, 0, Ids, Count, true, at(slotPoint(w, 2)), WheelAllBubbles);
    assert(near(w.ring.frame.p.z, Centre.z) && w.hoverSlot == 2);
    // Hover publishes the WheelUi encoding: weapon list index or -2-kind.
    const int netricsa = slotOf(w.ring, -1, WheelBubble::Netricsa);
    w.update(true, 0, 0, Ids, Count, true, at(slotPoint(w, uint32_t(netricsa))), WheelAllBubbles);
    assert(w.hover == wheelBubbleHover(WheelBubble::Netricsa) && w.hover == -2);
    assert(wheelHoveredBubble(w.hover) == WheelBubble::Netricsa);
    assert(wheelHoveredBubble(-1) == WheelBubble::None && wheelHoveredBubble(0) == WheelBubble::None &&
           wheelHoveredBubble(-2 - int32_t(WheelBubbleCount)) == WheelBubble::None);
    const int colt = slotOf(w.ring, 12);
    w.update(true, 0, 0, Ids, Count, true, at(slotPoint(w, uint32_t(colt))), WheelAllBubbles);
    assert(w.hover == 2 && Ids[w.hover] == 12);
}

// Pointing: from where the wheel opened, aiming the controller at an item hovers it.
void pointing() {
    WeaponWheel w;
    w.update(true, 0, 0, Ids, Count, true, at(Hand), WheelAllBubbles);
    assert(w.open && w.hoverSlot == -1); // The aim ray starts on the cancel centre.
    const auto aimAt = [&](Vec3 target, Vec3 from = Hand) {
        const Vec3 d = target - from;
        const float yawAngle = std::atan2(-d.x, -d.z), pitch = std::atan2(d.y, std::sqrt(d.x * d.x + d.z * d.z));
        WheelPointer p = at(from);
        p.aim.q = multiply(yaw(yawAngle), Quat{std::sin(pitch / 2), 0, 0, std::cos(pitch / 2)});
        return p;
    };
    for (uint32_t i = 0; i != w.ring.count; ++i) {
        const auto p = aimAt(slotPoint(w, i));
        assert(near(dot(rotate(p.aim.q, {0, 0, -1}), slotPoint(w, i) - Hand) /
                    std::sqrt(dot(slotPoint(w, i) - Hand, slotPoint(w, i) - Hand)), 1, 1e-3f));
        assert(w.ring.pick(p.hand, p.aim.p, p.aim.q, true, -1) == int(i));
    }
    // Pointing away from the ring, or backwards, hovers nothing.
    WheelPointer away = at(Hand);
    away.aim.q = yaw(Pi);
    assert(w.ring.pick(away.hand, away.aim.p, away.aim.q, true, -1) == -1);
    // Releasing while pointing at a bubble triggers it.
    const int load = slotOf(w.ring, -1, WheelBubble::QuickLoad);
    w.update(true, 0, 0, Ids, Count, true, aimAt(slotPoint(w, uint32_t(load))), WheelAllBubbles);
    assert(w.hover == wheelBubbleHover(WheelBubble::QuickLoad));
    w.update(false, 0, 0, Ids, Count, true, aimAt(slotPoint(w, uint32_t(load))), WheelAllBubbles);
    assert(w.action == WheelBubble::QuickLoad && w.selected == -1);
}

void releaseSelect() {
    WeaponWheel w;
    const int selected = chooseSlot(w, [](const WheelRing &r) { return slotOf(r, 6); });
    assert(selected == 6 && w.action == WheelBubble::None && !w.open && w.hover == -1);
    // The commit is reported once.
    assert(w.update(false, 0, 0, Ids, Count, true, at(Hand), WheelAllBubbles) == -1 && w.selected == -1);
}

void cancel() {
    WeaponWheel w;
    w.update(true, 0, 0, Ids, Count, true, at(Hand), WheelAllBubbles);
    w.update(true, 0, 0, Ids, Count, true, at(slotPoint(w, 1)), WheelAllBubbles);
    assert(w.hoverSlot == 1);
    // Back to the centre, then release: nothing is chosen.
    w.update(true, 0, 0, Ids, Count, true, at(Hand), WheelAllBubbles);
    assert(w.update(false, 0, 0, Ids, Count, true, at(Hand), WheelAllBubbles) == -1);
    assert(w.action == WheelBubble::None && !w.open);
    // Tracking loss while hovering cancels instead of committing.
    w.update(true, 0, 0, Ids, Count, true, at(Hand), WheelAllBubbles);
    w.update(true, 0, 0, Ids, Count, true, at(slotPoint(w, 1)), WheelAllBubbles);
    w.update(true, 0, 0, Ids, Count, false, at(slotPoint(w, 1)), WheelAllBubbles);
    assert(!w.open && w.update(false, 0, 0, Ids, Count, true, at(slotPoint(w, 1)), WheelAllBubbles) == -1);
    assert(w.action == WheelBubble::None);
    // A pose-less sample on release cannot commit the previous hover.
    w.update(true, 0, 0, Ids, Count, true, at(Hand), WheelAllBubbles);
    w.update(true, 0, 0, Ids, Count, true, at(slotPoint(w, 1)), WheelAllBubbles);
    assert(w.update(false, 0, 0, Ids, Count, true, {}, WheelAllBubbles) == -1 && w.action == WheelBubble::None);
}

void bubbles() {
    static WheelBubble wanted;
    for (uint32_t b = 0; b != WheelBubbleCount; ++b) {
        wanted = WheelBubble(b);
        WeaponWheel w;
        const int selected = chooseSlot(w, [](const WheelRing &r) { return slotOf(r, -1, wanted); });
        assert(selected == -1 && w.action == wanted && !w.open);
        w.update(false, 0, 0, Ids, Count, true, at(Hand), WheelAllBubbles);
        assert(w.action == WheelBubble::None); // Triggered once, on release only.
    }
    // Holding over a bubble triggers nothing until release.
    WeaponWheel w;
    w.update(true, 0, 0, Ids, Count, true, at(Hand), WheelAllBubbles);
    const int save = slotOf(w.ring, -1, WheelBubble::QuickSave);
    for (int i = 0; i != 3; ++i) {
        w.update(true, 0, 0, Ids, Count, true, at(slotPoint(w, uint32_t(save))), WheelAllBubbles);
        assert(w.action == WheelBubble::None && w.open);
    }
}

void grabPriority() {
    WeaponWheel w;
    w.update(false, 0, 0, Ids, Count, true, at(Hand), WheelAllBubbles);
    assert(w.update(true, 0, 0, Ids, Count, true, at(Hand), WheelAllBubbles, true) == -1 && !w.open);
    // The press that started a grab never opens the wheel later, even once the
    // object no longer has priority; its release commits nothing.
    w.update(true, 0, 0, Ids, Count, true, at(Hand), WheelAllBubbles, false);
    assert(!w.open);
    assert(w.update(false, 0, 0, Ids, Count, true, at(Hand), WheelAllBubbles) == -1 && w.action == WheelBubble::None);
    // A fresh press without priority opens normally; priority gained while open does not close it.
    w.update(true, 0, 0, Ids, Count, true, at(Hand), WheelAllBubbles, false);
    assert(w.open);
    w.update(true, 0, 0, Ids, Count, true, at(slotPoint(w, 0)), WheelAllBubbles, true);
    assert(w.open && w.hoverSlot == 0);
}

void stickFallback() {
    WeaponWheel w;
    w.update(true, 0, 0, Ids, Count, true, at(Hand), WheelAllBubbles);
    w.update(true, 0, 0, Ids, Count, true, at(slotPoint(w, 2)), WheelAllBubbles);
    assert(w.hoverSlot == 2);
    // A deflected thumbstick picks weapons by direction and overrides the hand.
    const int top = w.ring.pickStick(0, 1, -1);
    w.update(true, 0, 1, Ids, Count, true, at(slotPoint(w, 2)), WheelAllBubbles);
    assert(w.hoverSlot == top && top == 0 && near(w.ring.slotPosition(0).x, 0));
    for (float a = 0; a < 2 * Pi; a += .1f) // Bubbles are never stick targets.
        assert(w.ring.pickStick(std::sin(a), std::cos(a), -1) < int(Count));
    // Without a pose the stick alone still works, as on the original wheel.
    WeaponWheel stick;
    stick.update(true, 0, 1, Ids, Count, true);
    assert(stick.open && !stick.ring.anchored && stick.hover == 0);
    assert(stick.update(false, 0, 1, Ids, Count, true) == Ids[0]);
}

void inventoryChange() {
    WeaponWheel w;
    w.update(true, 0, 0, Ids, Count, true, at(Hand), WheelAllBubbles);
    const int colt = slotOf(w.ring, 12);
    const Vec3 target = slotPoint(w, uint32_t(colt));
    w.update(true, 0, 0, Ids, Count, true, at(target), WheelAllBubbles);
    assert(w.hoverSlot == colt);
    // A weapon picked up while open re-lays the ring and drops the moved hover;
    // the next sample hovers whatever now lies under the hand.
    const int more[]{5, 3, 12, 6, 13, 8};
    w.update(true, 0, 0, more, 6, true, at(target), WheelAllBubbles);
    assert(w.ring.count == 6 + WheelBubbleCount);
    assert(w.hoverSlot == w.ring.pickHand(target, -1));
    // A committed weapon is always a member of the current list.
    const int selected = w.update(false, 0, 0, more, 6, true, at(target), WheelAllBubbles);
    bool member = selected == -1;
    for (int id : more) member |= id == selected;
    assert(member);
    // Oversized inventories are rejected rather than read past the array.
    WeaponWheel bad;
    assert(bad.update(true, 0, 0, Ids, WeaponCount + 1, true, at(Hand)) == -1 && !bad.open);
}

void meshPlacement() {
    WeaponWheel w;
    w.update(true, 0, 0, Ids, Count, true, at(Hand), WheelAllBubbles);
    const WheelRing &ring = w.ring;
    // A pickup mesh whose origin is off its box centre (centre .3 m forward, 0.6 m long).
    const Vec3 boxMin{-.1f, -.05f, .0f}, boxMax{.1f, .05f, .6f};
    const Vec3 centre = (boxMin + boxMax) * .5f;
    const float extent = .6f;
    for (float spin : {0.f, 1.f, 2.5f}) {
        const float size = wheelModelSize(ring.itemSize, false);
        const auto placed = wheelModelPlacement(ring.frame, ring.slotPosition(3), centre, extent, size, spin);
        assert(placed.scale > 0 && near(placed.scale * extent, size));
        // The box centre lands on the slot whatever the spin.
        const Vec3 at = placed.pose.p + rotate(placed.pose.q, centre * placed.scale);
        const Vec3 slot = slotPoint(w, 3);
        assert(near(at.x, slot.x) && near(at.y, slot.y) && near(at.z, slot.z));
        // Spinning keeps the mesh upright about the ring's vertical axis.
        assert(near(rotate(placed.pose.q, {0, 1, 0}).y, 1));
    }
    // About 9.5 cm, larger when hovered, smaller only on a crowded ring.
    assert(near(wheelModelSize(.065f, false), .095f) && near(wheelModelSize(.065f, true), .095f * 1.3f));
    int all[WeaponCount];
    for (uint32_t i = 0; i != WeaponCount; ++i) all[i] = int(i);
    WheelRing crowded;
    crowded.layout(all, WeaponCount, WheelAllBubbles);
    // With the bubbles off the ring even a full inventory keeps 9.5 cm meshes.
    assert(near(wheelModelSize(crowded.itemSize, false), .095f) && wheelModelSize(.04f, false) < .07f);
    // Degenerate boxes and non-finite input never place a mesh.
    assert(wheelModelPlacement(ring.frame, {}, {}, 0, .1f, 0).scale == 0);
    assert(wheelModelPlacement(ring.frame, {}, {}, NAN, .1f, 0).scale == 0);
    assert(wheelModelPlacement(ring.frame, {}, {}, 1, .1f, INFINITY).scale == 0);
}

void meshBounds() {
    // Two render meshes (a body and a long barrel) and an authored configuration
    // box that is far smaller, as on the stock rocket launcher pickup.
    const WheelBox body{{-.2f, -.1f, -.3f}, {.2f, .3f, .4f}}, barrel{{-.1f, 0, -1.6f}, {.1f, .2f, -.2f}};
    const WheelBox stored{{-.3f, -.3f, -.3f}, {.3f, .3f, .3f}};
    const WheelBox both[]{body, barrel};
    auto bounds = wheelModelBounds(both, 2, stored);
    assert(bounds.fromMeshes && near(bounds.extent, 2.f) && near(bounds.centre.z, -.6f) &&
           near(bounds.centre.y, .1f) && near(bounds.centre.x, 0));
    // The drawn length is normalised from the meshes, never the stored box.
    const float size = wheelModelSize(.065f, false);
    const auto placed = wheelModelPlacement({}, {}, bounds.centre, bounds.extent, size, 0);
    assert(near(placed.scale * 2.f, size) && placed.scale * 3 < size / (stored.max.x - stored.min.x));
    // Invalid mesh boxes (non-finite, inverted, empty) are skipped.
    const WheelBox junk[]{{{NAN, 0, 0}, {1, 1, 1}}, {{1, 1, 1}, {0, 0, 0}}, {{0, 0, 0}, {0, 0, 0}}, body};
    bounds = wheelModelBounds(junk, 4, stored);
    assert(bounds.fromMeshes && near(bounds.extent, .7f));
    // Without any usable mesh box the stored configuration box is the fallback.
    bounds = wheelModelBounds(junk, 3, stored);
    assert(!bounds.fromMeshes && near(bounds.extent, .6f) && near(bounds.centre.x, 0));
    bounds = wheelModelBounds(nullptr, 0, stored);
    assert(!bounds.fromMeshes && near(bounds.extent, .6f));
    // Nothing usable at all: no extent, so the weapon keeps its icon.
    bounds = wheelModelBounds(junk, 3, WheelBox{});
    assert(bounds.extent == 0 && !bounds.fromMeshes);
    assert(!wheelBoxValid(WheelBox{{0, 0, 0}, {60, 1, 1}}));
}

void inputSample() {
    // Full Input path: grip position is the pointer, epochs and admission apply.
    Input input{};
    input.headValid = 1;
    input.head = Head;
    input.handValid[1] = input.gripValid[1] = 1;
    input.hand[1] = {{0, 0, 0, 1}, Hand + Vec3{0, .05f, -.08f}}; // Aim pose ahead of the grip.
    input.grip[1] = {{0, 0, 0, 1}, Hand};
    input.wheelInputEpoch[1] = 1;
    input.wheelAdmissionMask = 2;
    WeaponWheel w;
    assert(w.sample(input, 1, Ids, Count, true, WheelAllBubbles) == -1 && !w.open);
    input.buttons[1] = Wheel;
    w.sample(input, 1, Ids, Count, true, WheelAllBubbles);
    assert(w.open && near(w.ring.frame.p.x, Hand.x) && near(w.ring.frame.p.y, Hand.y));
    const int menu = slotOf(w.ring, -1, WheelBubble::Menu);
    input.grip[1].p = slotPoint(w, uint32_t(menu));
    w.sample(input, 1, Ids, Count, true, WheelAllBubbles);
    assert(w.hover == wheelBubbleHover(WheelBubble::Menu));
    // A new squeeze epoch (action loss) cancels before any release can commit.
    input.wheelInputEpoch[1] = 2;
    input.buttons[1] = 0;
    assert(w.sample(input, 1, Ids, Count, true, WheelAllBubbles) == -1 && w.action == WheelBubble::None);
    assert(!w.open);
    // The other hand's wheel is independent.
    WeaponWheel left;
    assert(left.sample(input, 0, Ids, Count, true, WheelAllBubbles) == -1 && !left.open);
    // An invalid head leaves the wheel stick-only, never anchored at a stale pose.
    input.headValid = 0;
    input.buttons[1] = Wheel;
    w.sample(input, 1, Ids, Count, true, WheelAllBubbles);
    assert(w.open && !w.ring.anchored && w.ring.pickHand(Hand, -1) == -1);
}
} // namespace

int main() {
    layout();
    frame();
    hoverPick();
    pointing();
    releaseSelect();
    cancel();
    bubbles();
    grabPriority();
    stickFallback();
    inventoryChange();
    meshPlacement();
    meshBounds();
    inputSample();
    std::puts("Weapon wheel layout, hover, release, cancel, bubble, grab-priority, mesh placement and mesh bounds checks passed");
}
