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
WheelPointer at(Vec3 hand) { return {true, hand, Head}; }
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
    ring.layout(Ids, Count, WheelAllBubbles);
    assert(ring.count == Count + WheelBubbleCount);
    const float step = 2 * Pi / float(ring.count);
    // Bubbles sit at the bottom, left to right as seen by the user.
    float previousX = -1;
    for (uint32_t b = 0; b != WheelBubbleCount; ++b) {
        const int slot = slotOf(ring, -1, WheelBubble(b));
        assert(slot >= 0);
        const Vec3 p = ring.slotPosition(uint32_t(slot));
        assert(p.y < 0 && p.x > previousX);
        previousX = p.x;
    }
    // Weapons follow clockwise from just left of the bubbles, over the top.
    const int first = slotOf(ring, Ids[0]);
    const Vec3 leftmost = ring.slotPosition(uint32_t(slotOf(ring, -1, WheelBubble::Netricsa)));
    const Vec3 start = ring.slotPosition(uint32_t(first));
    assert(start.x < leftmost.x && near(std::remainder(ring.slotAngle(uint32_t(first)) -
        ring.slotAngle(uint32_t(slotOf(ring, -1, WheelBubble::Netricsa))), 2 * Pi), step));
    for (uint32_t i = 1; i != Count; ++i)
        assert(near(std::remainder(ring.slotAngle(uint32_t(slotOf(ring, Ids[i]))) -
                                   ring.slotAngle(uint32_t(slotOf(ring, Ids[i - 1]))), 2 * Pi), step));
    assert(ring.radius >= WheelMinimumRadius && ring.radius <= WheelMaximumRadius);
    assert(ring.itemSize > .03f && ring.itemSize <= .065f);
    // Items never overlap their neighbours, even with the largest inventory.
    int all[WeaponCount];
    for (uint32_t i = 0; i != WeaponCount; ++i) all[i] = int(i);
    ring.layout(all, WeaponCount, WheelAllBubbles);
    assert(ring.count == WheelSlotCount && near(ring.radius, WheelMaximumRadius));
    const Vec3 a = ring.slotPosition(0), b = ring.slotPosition(1);
    assert(std::sqrt(dot(a - b, a - b)) > ring.itemSize);
    // Without bubbles the first weapon is at the top, as on the stick wheel.
    ring.layout(Ids, Count, 0);
    assert(ring.count == Count && ring.slot[0].weapon == Ids[0] && near(ring.slotPosition(0).x, 0) &&
           ring.slotPosition(0).y > 0);
    // Only the offered bubbles appear; quick save/load are absent outside single player.
    ring.layout(Ids, Count, wheelBubbleBit(WheelBubble::Netricsa) | wheelBubbleBit(WheelBubble::Menu));
    assert(ring.count == Count + 2 && slotOf(ring, -1, WheelBubble::QuickSave) < 0 &&
           slotOf(ring, -1, WheelBubble::QuickLoad) < 0 && slotOf(ring, -1, WheelBubble::Menu) >= 0);
    // An empty inventory still offers the bubbles.
    ring.layout(Ids, 0, WheelAllBubbles);
    assert(ring.count == WheelBubbleCount);
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
}

void hoverPick() {
    WeaponWheel w;
    w.update(true, 0, 0, Ids, Count, true, at(Hand), WheelAllBubbles);
    const WheelRing &ring = w.ring;
    assert(near(ring.frame.p.x, Hand.x) && near(ring.frame.p.y, Hand.y) && near(ring.frame.p.z, Hand.z));
    for (uint32_t i = 0; i != ring.count; ++i) {
        assert(ring.pickHand(slotPoint(w, i), -1) == int(i));
        assert(ring.pickHand(slotPoint(w, i, .6f, .05f), -1) == int(i)); // Reaching short or past is enough.
    }
    // The centre is the cancel zone; far outside and far in depth hover nothing.
    assert(ring.pickHand(Hand, -1) == -1);
    assert(ring.pickHand(slotPoint(w, 0, WheelDeadZone * .9f), -1) == -1);
    assert(ring.pickHand(slotPoint(w, 0, 1 + (WheelOuterReach + .02f) / ring.radius), -1) == -1);
    assert(ring.pickHand(slotPoint(w, 0, 1, WheelDepthReach + .02f), -1) == -1);
    assert(ring.pickHand({NAN, 0, 0}, -1) == -1);
    // Hysteresis: just past the boundary the current item remains hovered.
    const float step = 2 * Pi / float(ring.count), boundary = ring.slotAngle(0) + step * .55f;
    const Vec3 local{std::sin(boundary) * ring.radius, std::cos(boundary) * ring.radius, 0};
    const Vec3 point = ring.frame.p + rotate(ring.frame.q, local);
    assert(ring.pickHand(point, 0) == 0 && ring.pickHand(point, -1) == slotOf(ring, Ids[0]));
    // The ring stays where it opened while the hand moves.
    w.update(true, 0, 0, Ids, Count, true, at(slotPoint(w, 2)), WheelAllBubbles);
    assert(near(w.ring.frame.p.x, Hand.x) && w.hoverSlot == 2);
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
    w.update(true, 0, 0, Ids, Count, true, at(slotPoint(w, 0)), WheelAllBubbles);
    assert(w.hoverSlot == 0);
    // A deflected thumbstick picks by direction and overrides the hand.
    const int top = w.ring.pickStick(0, 1, -1);
    w.update(true, 0, 1, Ids, Count, true, at(slotPoint(w, 0)), WheelAllBubbles);
    assert(w.hoverSlot == top && top != 0 && near(w.ring.slotPosition(uint32_t(top)).x, 0, w.ring.radius * .3f));
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
    releaseSelect();
    cancel();
    bubbles();
    grabPriority();
    stickFallback();
    inventoryChange();
    inputSample();
    std::puts("Weapon wheel layout, hover, release, cancel, bubble and grab-priority checks passed");
}
