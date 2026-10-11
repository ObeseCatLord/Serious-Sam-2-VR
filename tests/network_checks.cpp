#include "common/network.hpp"
#include "common/observer_gesture_intents.hpp"
#include "common/controls.hpp"
#include "common/intent_boundary.hpp"
#include "common/native_primary_projection.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace ss2vr;
using namespace ss2vr::network;

static void check(bool value, const char *what) {
    if (!value) {
        std::cerr << what << '\n';
        std::exit(1);
    }
}
static Pose pose(float x, float y, float z) {
    return {{0, 0, 0, 1}, {x, y, z}};
}
static PosePacket validPose(uint32_t sequence = 1, uint32_t generation = 1) {
    PosePacket p{};
    p.clientNonce = 11;
    p.serverNonce = 22;
    p.sequence = sequence;
    p.trackingGeneration = generation;
    p.head = pose(.1f, .2f, .3f);
    p.grip[0] = pose(.4f, 0, 0);
    p.grip[1] = pose(-.4f, 0, 0);
    p.validMask = 7;
    p.primarySampleEligibleMask = HandMask;
    p.primaryNeutralSampleMask = HandMask;
    p.zoomSampleEligibleMask = HandMask;
    p.intentEpoch[0] = p.intentEpoch[1] = 1;
    p.primaryInputGeneration[0] = p.primaryInputGeneration[1] = 1;
    p.zoomInputGeneration[0] = p.zoomInputGeneration[1] = 1;
    p.releasedSerial[0] = p.releasedSerial[1] = 1;
    p.zoomReleasedSerial[0] = p.zoomReleasedSerial[1] = 1;
    p.requestedWeapon[0] = -1;
    p.requestedWeapon[1] = -1;
    p.nativeWeaponId[0] = 2;
    p.nativeWeaponId[1] = -1;
    return p;
}
static std::string encodePose(const PosePacket &p) {
    Message m{};
    m.kind = Kind::Pose;
    m.pose = p;
    std::string wire;
    check(encode(m, wire), "valid pose encodes");
    return wire;
}
// Legacy intent/credit fixtures already describe poses in an identity tracking
// frame. Only tests synthesize that frame; production has no capture-free API.
static LocalPoseCapture fixtureCapture(const PosePacket &p, uint64_t now) {
    LocalPoseCapture c;
    c.frame.avatar = 99;
    c.frame.producer = 7;
    c.frame.session = c.frame.reference = 1;
    c.frame.trackingEpoch = c.frame.trackingGeneration = p.trackingGeneration;
    c.frame.sequence = c.frame.tickMs = now;
    c.head = p.head;
    c.grip[0] = p.grip[0];
    c.grip[1] = p.grip[1];
    return c;
}
class FixturePendingIntents : public PendingIntents {
  public:
    bool retain(const PosePacket &p, uint8_t hands, uint64_t now) {
        return PendingIntents::retain(p, hands, now, fixtureCapture(p, now));
    }
    uint8_t apply(PosePacket &p, uint64_t now) {
        return PendingIntents::apply(p, now, fixtureCapture(p, now));
    }
};
static bool nearPose(const Pose &a, const Pose &b) {
    const auto d = a.p - b.p;
    return dot(d, d) < 1e-10f && std::abs(a.q.x-b.q.x) < 1e-5f &&
           std::abs(a.q.y-b.q.y) < 1e-5f && std::abs(a.q.z-b.q.z) < 1e-5f &&
           std::abs(a.q.w-b.q.w) < 1e-5f;
}
static void rawCaptureChecks() {
    static_assert(WireVersion == 7);
    for (unsigned active = 0; active < 2; ++active) {
        auto packet = validPose();
        packet.validMask = uint8_t(1u | (2u << active));
        packet.physicalDownMask = packet.fireMask = uint8_t(1u << active);
        packet.primaryNeutralSampleMask = 0;
        packet.nativeWeaponId[active] = 2;
        auto capture = fixtureCapture(packet, 1000);
        capture.grip[1-active].p.x = std::numeric_limits<float>::quiet_NaN();
        capture.grip[1-active].q = {0, 0, 0, 0};
        check(composeLocalPose(packet, capture, 1000) && validatePose(packet),
              "an unavailable invalid offhand cannot reject the valid hand or tracked head");
        check(nearPose(packet.grip[1-active], Pose{{}, packet.head.p}),
              "unavailable hand geometry is canonical without reading its invalid raw pose");
        PendingIntents isolated;
        check(isolated.retain(packet, uint8_t(1u << active), 1000, capture) &&
                  isolated.apply(packet, 1000, capture) == (1u << active),
              "valid-hand retention and handoff remain independent of invalid inactive offhand");
    }
    {
        auto headOnly = validPose();
        headOnly.validMask = 1;
        auto capture = fixtureCapture(headOnly, 1000);
        capture.grip[0].p.z = std::numeric_limits<float>::infinity();
        capture.grip[1].q.w = std::numeric_limits<float>::quiet_NaN();
        check(composeLocalPose(headOnly, capture, 1000) && validatePose(headOnly),
              "head articulation survives loss of both hand poses without inventing active hands");
        check(headOnly.validMask == 1 && !headOnly.fireMask && !headOnly.pulseMask,
              "head-only capture grants no firing or retained pulse");
        const auto original = encodePose(headOnly);
        capture.head.p.x = 1e20f;
        check(!composeLocalPose(headOnly, capture, 1000) && encodePose(headOnly) == original,
              "head-only finite overflow cannot hide behind the head clamp or mutate the packet");
    }
    auto first = validPose();
    first.nativeWeaponId[1] = 2;
    first.physicalDownMask = first.fireMask = 1;
    first.primaryNeutralSampleMask = 2;
    auto left = fixtureCapture(first, 1000);
    left.head = pose(HeadHorizontalLimit + .45f, -HeadDownLimit - .4f, 0);
    left.grip[0] = pose(HeadHorizontalLimit + .45f + 1.8f, -HeadDownLimit - .4f, 0);
    left.grip[1] = pose(HeadHorizontalLimit + .45f, -HeadDownLimit - .4f, 0);
    check(composeLocalPose(first, left, 1000), "raw clamp fixture composes through current rig");
    check(nearPose(first.head, pose(HeadHorizontalLimit, -HeadDownLimit, 0)) &&
              nearPose(first.grip[0], pose(HeadHorizontalLimit + HandReachLimit, -HeadDownLimit, 0)),
          "raw head and reach clamps use the existing production limits");
    const auto originalLeftPacket = encodePose(first);
    PendingIntents taps;
    check(taps.retain(first, 1, 1000, left), "raw left capture retained before origin settlement");

    auto second = validPose(2);
    second.nativeWeaponId[1] = 2;
    second.physicalDownMask = second.fireMask = 2;
    second.primaryNeutralSampleMask = 1;
    auto right = fixtureCapture(second, 1010);
    right.frame.origin.p = {.4f, 0, 0};
    right.head = pose(.8f, -.3f, .1f);
    right.grip[0] = pose(1, -.5f, -.3f);
    right.grip[1] = pose(-.4f, -.5f, -.3f);
    check(composeLocalPose(second, right, 1010) && taps.retain(second, 2, 1010, right),
          "right capture keeps its own raw head after a distinct origin settlement");

    auto current = right;
    current.frame.origin.p = {.5f, -.1f, .1f};
    current.frame.turn = .7f;
    current.frame.sequence = current.frame.tickMs = 1020;
    auto outgoing = validPose(3);
    outgoing.nativeWeaponId[1] = 2;
    check(composeLocalPose(outgoing, current, 1020) && taps.apply(outgoing, 1020, current) == 3,
          "both retained hands compose against the current origin and turn");
    const auto expectedLeft = bodyHandTracking(current.frame.origin, current.frame.turn, left.head, left.grip[0]);
    const auto expectedRight = bodyHandTracking(current.frame.origin, current.frame.turn, right.head, right.grip[1]);
    check(nearPose(outgoing.grip[0], expectedLeft) && nearPose(outgoing.grip[1], expectedRight) &&
              !nearPose(outgoing.grip[0], bodyHandTracking(current.frame.origin, current.frame.turn,
                                                         current.head, left.grip[0])),
          "each retained grip uses its captured raw head, not the newer packet head");
    const auto affineOld = rotate(yaw(current.frame.turn), first.grip[0].p - current.frame.origin.p);
    check(dot(outgoing.grip[0].p-affineOld, outgoing.grip[0].p-affineOld) > .01f,
          "recomposition recovers clamped raw motion rather than shifting an old clamped grip");
    auto valid = outgoing;
    check(validatePose(valid), "rebased composite still passes every existing wire bound");
    const auto handed = outgoing;
    const auto handedBytes = encodePose(handed);
    check(encodePose(first) == originalLeftPacket, "retention never mutates the original capture packet");

    auto retry = validPose(4);
    retry.nativeWeaponId[1] = 2;
    current.frame.origin.p.x += .2f;
    current.frame.sequence = current.frame.tickMs = 1030;
    check(composeLocalPose(retry, current, 1030) && taps.apply(retry, 1030, current) == 3 &&
              nearPose(retry.grip[0], bodyHandTracking(current.frame.origin, current.frame.turn,
                                                    left.head, left.grip[0])) &&
              encodePose(handed) == handedBytes,
          "another unsent composition uses immutable raw captures without rewriting handed-off packets");
    const auto beforeCorrection = retry;
    // Native body correction changes the observer anchor, not the local origin.
    current.frame.sequence = current.frame.tickMs = 1040;
    auto afterCorrection = validPose(5);
    afterCorrection.nativeWeaponId[1] = 2;
    check(composeLocalPose(afterCorrection, current, 1040) &&
              taps.apply(afterCorrection, 1040, current) == 3 &&
              nearPose(afterCorrection.grip[0], beforeCorrection.grip[0]) &&
              nearPose(afterCorrection.grip[1], beforeCorrection.grip[1]),
          "native correction with no origin settlement adds no coordinate payment");
    const auto correctedWorld = compose(pose(-.2f, 0, 0), afterCorrection.grip[0]);
    check(std::abs(correctedWorld.p.x - (beforeCorrection.grip[0].p.x-.2f)) < 1e-5f,
          "native correction remains visible through the actual observer body anchor");
    current.frame.sequence = current.frame.tickMs = 1200;
    auto expired = validPose(6);
    expired.nativeWeaponId[1] = 2;
    check(composeLocalPose(expired, current, 1200) && taps.apply(expired, 1200, current) == 2,
          "rebasing never renews either hand's independent original 200ms TTL");
    taps.consume(2);
    check(!taps.apply(expired, 1200, current), "successful handoff consumes the original pending owner once");

    for (unsigned field = 0; field != 6; ++field) {
        PendingIntents retired;
        check(retired.retain(first, 1, 1000, left), "identity case retains original raw capture");
        auto replacement = left;
        replacement.frame.sequence = replacement.frame.tickMs = 1010;
        switch (field) {
        case 0: ++replacement.frame.avatar; break;
        case 1: ++replacement.frame.producer; break;
        case 2: ++replacement.frame.session; break;
        case 3: ++replacement.frame.reference; break;
        case 4: ++replacement.frame.trackingEpoch; replacement.frame.origin.p.x = .3f; break;
        case 5: ++replacement.frame.trackingGeneration; break;
        }
        auto held = first;
        held.trackingGeneration = replacement.frame.trackingGeneration;
        check(!retired.apply(held, 1010, replacement) && !held.fireMask && !retired.primaryAllowed(0),
              "owner/producer/session/reference/recenter/tracking changes retire old capture and require neutral");
        held.fireMask = held.physicalDownMask = 0;
        held.primaryNeutralSampleMask = 3;
        check(retired.observeCapture(held, replacement, 1010) && retired.primaryAllowed(0),
              "only an eligible current neutral rearms the retired hand");
    }
    for (unsigned invalid = 0; invalid != 15; ++invalid) {
        PendingIntents guarded;
        check(guarded.retain(first, 1, 1000, left), "invalid-capture case starts with a retained press");
        auto bad = left;
        bad.frame.sequence = bad.frame.tickMs = 1010;
        const auto nan = std::numeric_limits<float>::quiet_NaN();
        const auto inf = std::numeric_limits<float>::infinity();
        switch (invalid) {
        case 0: bad.head.p.x = nan; break;
        case 1: bad.grip[0].p.z = inf; break;
        case 2: bad.grip[1].q.w = nan; break;
        case 3: bad.frame.origin.p.y = inf; break;
        case 4: bad.frame.turn = nan; break;
        case 5: bad.frame.origin.q = {0,0,0,0}; break;
        case 6: bad.frame.avatar = 0; break;
        case 7: bad.frame.producer = 0; break;
        case 8: bad.frame.session = 0; break;
        case 9: bad.frame.reference = 0; break;
        case 10: bad.frame.trackingEpoch = 0; break;
        case 11: bad.frame.sequence = 0; break;
        case 12: bad.frame.tickMs = 1011; break;
        case 13: bad.frame.tickMs = 810; break;
        case 14: ++bad.frame.trackingGeneration; break;
        }
        auto packet = first;
        const auto untouched = encodePose(packet);
        check(!composeLocalPose(packet, bad, 1010) && encodePose(packet) == untouched,
              "invalid/nonfinite/stale frame cannot partially replace outgoing coordinates");
        check(!guarded.apply(packet, 1010, bad) && !packet.fireMask && !packet.pulseMask &&
                  !guarded.primaryAllowed(0) && !guarded.retain(first, 1, 1010, bad),
              "unusable current provenance cancels intent and cannot synthesize neutral");
    }
    PendingIntents stale;
    check(stale.retain(first, 1, 1000, left), "stale-sequence case retains capture");
    auto older = left;
    older.frame.sequence = 999;
    auto oldPacket = first;
    oldPacket.fireMask = oldPacket.physicalDownMask = 0;
    oldPacket.primaryNeutralSampleMask = 3;
    check(!stale.apply(oldPacket, 1010, older) && !stale.primaryAllowed(0),
          "an older source sequence cannot rearm a pending hand even if it claims neutral");
    older = left;
    ++older.frame.tickMs;
    check(!captureNotOlder(older.frame, left.frame), "one input sequence cannot acquire a different source tick");
    auto huge = left;
    huge.head.p.x = std::numeric_limits<float>::max();
    auto hugePacket = first;
    check(!composeLocalPose(hugePacket, huge, 1000), "finite raw overflow cannot be hidden by a clamp");
}
static void captureBootstrapChecks() {
    for (unsigned unavailable = 0; unavailable != 5; ++unavailable) {
        auto inactive = validPose();
        inactive.physicalDownMask = inactive.fireMask = 1;
        inactive.primaryNeutralSampleMask = 2;
        inactive.requestedWeapon[0] = 2;
        auto capture = fixtureCapture(inactive, 1000);
        capture.head = pose(2, 1.5f, -.5f); // Tracking can precede the first origin initialization.
        switch (unavailable) {
        case 0: capture = {}; break; // No HMD/unknown input owner.
        case 1: inactive.validMask = 0; break; // Positive input, but not enabled before Hello.
        case 2: capture.head.p.x = std::numeric_limits<float>::quiet_NaN(); break;
        case 3: ++capture.frame.avatar; break;
        case 4: ++capture.frame.trackingGeneration; break;
        }
        const auto rawHeld = inactive.physicalDownMask;
        const auto release = inactive.releasedSerial[0];
        PendingIntents pending;
        const bool coordinatesReady = prepareLocalPose(inactive, capture, {}, 99, 1000);
        if (!coordinatesReady) pending.requireNeutral();
        check(!coordinatesReady && !pending.primaryAllowed(0) && !inactive.validMask &&
                  !inactive.fireMask && !inactive.zoomMask && !inactive.pulseMask &&
                  !inactive.primaryNeutralSampleMask && !inactive.primarySampleEligibleMask &&
                  !inactive.zoomSampleEligibleMask && inactive.requestedWeapon[0] == -1 &&
                  inactive.physicalDownMask == rawHeld && inactive.releasedSerial[0] == release &&
                  nearPose(inactive.head, Pose{}),
              "unknown/inactive/mismatched capture excludes all intent without fabricating raw release");
        ConsumptionCredits credits;
        Message hello, decoded;
        hello.kind = Kind::Hello;
        hello.hello.nonce = 11;
        std::string bytes;
        check(credits.grant({11,0,0}) && encode(hello, bytes) && parse(bytes, decoded) == ParseResult::Valid &&
                  decoded.kind == Kind::Hello && !credits.grant({11,0,0}),
              "Hello negotiates independently of coordinate admission and keeps one outstanding credit");
        OrderedPosePolicy peer;
        check(peer.validation.bindCapability(11,22) && credits.acknowledge({11,22,0,{1,1}},11,0),
              "Hello acknowledgement establishes capability without a tracked pose");
        IntentInputBoundary boundary;
        check(boundary.install(1, 1000, 1001) && !boundary.echo(1000,1000,1001),
              "bootstrap ACK cannot admit the pre-ACK cached source sample");
        Ack ack;
        PosePacket observed;
        const auto inactiveBytes = encodePose(inactive);
        check(credits.grant({11,22,1}) && peer.receive(inactive,1002,ack) &&
                  peer.freeze(1002,observed) && !observed.validMask && !observed.fireMask &&
                  !peer.validation.physicalFireAllowed(0) && !peer.validation.zoomAllowed(0) &&
                  peer.finishInterval(ack) && credits.acknowledge(ack,11,22) && !credits.count(),
              "inactive pose still completes the existing consumption/ACK lifecycle without arming input");
        auto active = validPose(2);
        active.physicalDownMask = active.fireMask = 1;
        active.primaryNeutralSampleMask = 2;
        auto fresh = fixtureCapture(active, 1003);
        check(boundary.echo(1003,1003,1003) == 1 && prepareLocalPose(active,fresh,{},99,1003) &&
                  pending.observeCapture(active,fresh,1003),
              "post-ACK enabled tracking can enter the same local capture path");
        pending.filterPrimaryIntents(active);
        check(!active.fireMask && !pending.primaryAllowed(0) &&
                  credits.grant({11,22,2}) && peer.receive(active,1003,ack) &&
                  peer.freeze(1003,observed) && !observed.fireMask &&
                  peer.finishInterval(ack) && credits.acknowledge(ack,11,22),
              "held input across bootstrap remains blocked until a real current neutral");
        active = validPose(3);
        active.releasedSerial[0] = 2;
        fresh = fixtureCapture(active,1004);
        check(prepareLocalPose(active,fresh,{},99,1004) && pending.observeCapture(active,fresh,1004) &&
                  pending.primaryAllowed(0) && peer.receive(active,1004,ack) &&
                  peer.validation.physicalFireAllowed(0) && encodePose(inactive) == inactiveBytes,
              "eligible post-bootstrap neutral rearms normally without mutating the inactive handed packet");
    }
}
static PosePacket gesturePose(uint64_t sample, uint8_t down = HandMask, uint64_t quiet = 900,
                              uint32_t generation = 1) {
    auto p = validPose(uint32_t(sample));
    p.primarySampleEligibleMask = p.primaryNeutralSampleMask = 0;
    p.gestureEligibleMask = HandMask;
    p.gestureDownMask = down;
    p.gestureQuietMask = quiet ? HandMask : 0;
    for (unsigned hand = 0; hand < 2; ++hand) {
        p.nativeWeaponId[hand] = 0;
        p.gestureGeneration[hand] = generation;
        p.gestureSequence[hand] = p.gestureTickMs[hand] = sample;
        p.gestureQuietSequence[hand] = p.gestureQuietTickMs[hand] = quiet;
    }
    return p;
}
static void observerGestureChecks() {
    // A replacement intent context may arrive within the same receiver clock
    // millisecond, still describing the same physical source observation.
    // An old native callback must not consume that replacement's retained edge.
    for (unsigned hand = 0; hand < 2; ++hand) {
        ObserverGestureIntents replaced;
        auto old = gesturePose(1000); old.gesturePulseMask = HandMask;
        check(validatePose(old), "receipt replacement starts with a valid decoded pose");
        replaced.receive(old, old, 5000);
        PosePacket source;
        ObserverGestureIntents::Receipt before, other, after;
        check(replaced.sample(old, hand, 5000, source, before) &&
              replaced.sample(old, 1 - hand, 5000, source, other),
              "both hands capture receipts before replacement");
        auto next = old; ++next.sequence; ++next.intentEpoch[hand];
        check(validatePose(next), "replacement keeps a valid physical observation in a new intent epoch");
        replaced.receive(next, next, 5000);
        check(!replaced.current(next, before, 5000),
              "old receipt cannot validate a same-millisecond replacement context");
        check(!replaced.finish(before),
              "old completion cannot consume a same-millisecond replacement context");
        check(replaced.current(next, other, 5000) && replaced.finish(other),
              "replacement preserves the opposite hand's exact receipt");
        check(replaced.sample(next, hand, 5000, source, after) && replaced.finish(after) &&
              replaced.current(next, after, 5000) && !replaced.finish(after),
              "replacement receipt consumes once and retains its admitted interval");
    }
    for (unsigned change = 0; change < 3; ++change) {
        ObserverGestureIntents replaced;
        auto old = gesturePose(1000); old.gesturePulseMask = HandMask;
        replaced.receive(old, old, 5000);
        PosePacket source;
        ObserverGestureIntents::Receipt before[2];
        for (unsigned hand = 0; hand < 2; ++hand)
            check(replaced.sample(old, hand, 5000, source, before[hand]),
                  "both hands capture before global context replacement");
        auto next = old; ++next.sequence;
        if (change == 0) ++next.trackingGeneration;
        else if (change == 1) ++next.clientNonce;
        else ++next.serverNonce;
        check(validatePose(next), "global replacement pulse is a valid decoded pose");
        replaced.receive(next, next, 5000);
        for (unsigned hand = 0; hand < 2; ++hand) {
            check(!replaced.current(next, before[hand], 5000) && !replaced.finish(before[hand]),
                  "global replacement rejects old receipts despite identical physical stamps and local time");
            ObserverGestureIntents::Receipt after;
            check(replaced.sample(next, hand, 5000, source, after) &&
                  replaced.current(next, after, 5000) && replaced.finish(after) &&
                  replaced.current(next, after, 5000) && !replaced.finish(after),
                  "each global replacement receipt has independent consumption and interval ownership");
        }
    }
    {
        ObserverGestureIntents single;
        auto released = gesturePose(1000, 0); released.gesturePulseMask = 1;
        single.receive(released, released, 5000);
        PosePacket source; ObserverGestureIntents::Receipt receipt;
        check(single.sample(released, 0, 5000, source, receipt) &&
            ObserverGestureIntents::level(source, 0, true), "released retained observer edge enters one interval");
        check(single.finish(receipt) && single.current(released, receipt, 5001) &&
            ObserverGestureIntents::level(source, 0, true), "completed edge survives its admitted native interval");
        for (unsigned tick = 1; tick <= 3; ++tick)
            check(!single.sample(released, 0, 5000 + tick, source, receipt) &&
                !ObserverGestureIntents::level(released, 0, false), "consumed pulse cannot extend latest held level");
    }
    ObserverGestureIntents pending;
    auto pulse = gesturePose(1000); pulse.gesturePulseMask = HandMask;
    auto low = gesturePose(1001, 0, 1001);
    pending.receive(pulse, pulse, 5000);
    pending.receive(low, low, 5001);
    PosePacket held; ObserverGestureIntents::Receipt first, right;
    check(pending.sample(low, 0, 5002, held, first) && held.gestureSequence[0] == 1000 &&
          pending.sample(low, 1, 5002, held, right), "newer low retains both observer pulses");
    check(pending.finish(first) && pending.current(low, first, 5003) && !pending.finish(first) &&
          pending.sample(low, 1, 5003, held, right), "native completion is per-hand and receipt-conditional");
    pending.cancel(1);
    check(!pending.current(low, first, 5003), "cancellation revokes even a completed frozen edge");
    pending.receive(low, pulse, 5004);
    check(!pending.sample(low, 0, 5004, held, first), "duplicate consumed relay cannot replay");
    check(pending.finish(right), "other hand completes independently");
    ObserverGestureIntents delayed;
    delayed.receive(low, low, 5000); delayed.receive(low, pulse, 5001);
    check(delayed.sample(low, 0, 5002, held, first), "out-of-order reliable pulse survives newer low");
    check(!pending.finish(first), "receipt cannot consume another Remote owner");
    auto second = gesturePose(1002); second.gesturePulseMask = 1;
    delayed.receive(second, second, 5003);
    check(delayed.finish(first), "capacity discard preserves the older exact pending receipt");
    delayed.receive(second, second, 5004);
    check(!delayed.sample(second, 0, 5005, held, first), "capacity-discarded edge never becomes debt");
    ObserverGestureIntents stale;
    stale.receive(pulse, pulse, 5000);
    check(!stale.sample(low, 0, 5201, held, first), "observer pulse expires using local receive age");
    stale.receive(low, pulse, 5202);
    check(!stale.sample(low, 0, 5203, held, first), "expired pulse cannot be replayed by duplicate delivery");
    ObserverGestureIntents replaced;
    replaced.receive(pulse, pulse, 5000);
    check(replaced.sample(pulse, 0, 5000, held, first), "capture before native owner replacement");
    auto context = low; ++context.intentEpoch[0];
    replaced.receive(context, context, 5001);
    check(!replaced.finish(first) && !replaced.sample(context, 0, 5002, held, first),
          "intent epoch replacement cancels old observer receipt");
    replaced.receive(context, pulse, 5003);
    check(!replaced.sample(context, 0, 5004, held, first), "old-context delayed pulse rejected");
    auto blocked = pulse; blocked.wheelOrEquipBlockedMask = 1;
    ObserverGestureIntents unavailable; unavailable.receive(pulse, pulse, 5000);
    unavailable.receive(blocked, blocked, 5001);
    check(!unavailable.sample(blocked, 0, 5002, held, first) &&
          unavailable.sample(blocked, 1, 5002, held, right), "wheel loss cancels only affected observer hand");
}
static void manualDown(PosePacket &p, uint8_t hands) {
    p.physicalDownMask = p.fireMask = hands;
    p.primarySampleEligibleMask = HandMask;
    p.primaryNeutralSampleMask = uint8_t(HandMask & ~hands);
}
static void gestureWireChecks() {
    auto p = gesturePose(UINT64_MAX, HandMask, UINT64_MAX - 1, UINT32_MAX);
    p.gesturePulseMask = HandMask;
    manualDown(p, HandMask);
    Message m{}, decoded{};
    m.kind = Kind::Relay; m.relay = {p, UINT32_MAX, UINT32_MAX};
    std::string wire;
    check(encode(m, wire) && wire.size() == 348 && wire.size() <= MaxCarrierAscii,
          "wire7 largest gesture relay remains within the existing ASCII carrier budget");
    check(parse(wire, decoded) == ParseResult::Valid && decoded.relay.pose.gesturePulseMask == 3 &&
              decoded.relay.pose.gestureDownMask == 3 && decoded.relay.pose.fireMask == 3 &&
              decoded.relay.pose.gestureGeneration[1] == UINT32_MAX &&
              decoded.relay.pose.gestureSequence[0] == UINT64_MAX &&
              decoded.relay.pose.gestureTickMs[1] == UINT64_MAX &&
              decoded.relay.pose.gestureQuietSequence[1] == UINT64_MAX - 1 &&
              decoded.relay.pose.gestureQuietTickMs[0] == UINT64_MAX - 1,
          "gesture stamps and independent manual levels round trip for both hands without truncation");
    std::vector<uint8_t> bytes;
    check(detail::unbase64(std::string_view(wire).substr(CarrierTag.size()), bytes), "gesture relay decodes");
    bytes[0] = 6;
    check(parse(std::string(CarrierTag) + detail::base64(bytes), decoded) == ParseResult::Malformed,
          "wire6 cannot silently negotiate the gesture extension");
    bytes[0] = WireVersion; bytes.pop_back();
    check(parse(std::string(CarrierTag) + detail::base64(bytes), decoded) == ParseResult::Malformed,
          "truncated gesture stamp is rejected");
    for (unsigned field = 0; field < 21; ++field) {
        auto bad = gesturePose(1000);
        switch (field) {
        case 0: bad.gestureEligibleMask |= 4; break;
        case 1: bad.gestureDownMask |= 4; break;
        case 2: bad.gestureQuietMask |= 4; break;
        case 3: bad.gesturePulseMask |= 4; break;
        case 4: bad.gestureEligibleMask &= ~1; break;
        case 5: bad.gestureGeneration[0] = 0; break;
        case 6: bad.gestureSequence[0] = 0; break;
        case 7: bad.gestureTickMs[0] = 0; break;
        case 8: bad.gestureQuietSequence[0] = 0; break;
        case 9: bad.gestureQuietTickMs[0] = 0; break;
        case 10: bad.gestureQuietMask &= ~1; break;
        case 11: bad.gestureQuietSequence[0] = 1001; break;
        case 12: bad.gestureQuietTickMs[0] = 1001; break;
        case 13: bad.gestureQuietSequence[0] = 1000; break;
        case 14: bad.gestureQuietSequence[0] = bad.gestureQuietTickMs[0] = 1000; break;
        case 15: bad.nativeWeaponId[0] = 1; break;
        case 16: bad.requestedWeapon[0] = 0; break;
        case 17: bad.wheelOrEquipBlockedMask = 1; break;
        case 18: bad.validMask &= ~2; break;
        case 19: bad.validMask &= ~1; break;
        case 20: bad.grip[0].p.x = std::numeric_limits<float>::infinity(); break;
        }
        m.kind = Kind::Pose; m.pose = bad;
        check(!encode(m, wire), "malformed gesture state cannot be encoded");
        bytes.clear(); detail::put8(bytes, WireVersion); detail::put8(bytes, uint8_t(Kind::Pose));
        detail::put16(bytes, 0); detail::writePosePacket(bytes, bad);
        check(parse(std::string(CarrierTag) + detail::base64(bytes), decoded) == ParseResult::Malformed,
              "untrusted malformed gesture state is rejected by the actual parser");
    }
    auto inactive = validPose();
    inactive.gestureSequence[0] = 1;
    check(!validatePose(inactive), "ineligible gesture cannot smuggle orphan source metadata");
    auto quiet = gesturePose(1000, 0, 1000);
    check(validatePose(quiet), "a current quiet sample may be low");
    auto unarmed = gesturePose(1000, 3, 0);
    check(validatePose(unarmed), "valid unarmed observation is distinct from transport admission");
    invalidateWeaponIntents(p, 1);
    check(!p.gestureSequence[0] && !p.gestureGeneration[0] && p.gestureEligibleMask == 2 &&
              p.gestureDownMask == 2 && p.gesturePulseMask == 2 && p.gestureQuietMask == 2 &&
              p.physicalDownMask == 3 && p.fireMask == 2,
          "hand-local intent invalidation clears G provenance without inventing manual releases");
}
static void gestureAdmissionChecks() {
    PendingIntents pending;
    pending.requireNeutral();
    auto rawNeutral = validPose();
    pending.observeSnapshot(rawNeutral, 990);
    check(pending.primaryAllowed(0) && !pending.gestureAllowed(0),
          "actual manual neutral cannot manufacture a quiet gesture witness");
    auto high = gesturePose(1000, 3, 0);
    check(!pending.retain(high, 0, 1000, fixtureCapture(high, 1000), 3) && !pending.gestureAllowed(0),
          "first gesture high without quiet remains unarmed");
    auto quiet = gesturePose(1010, 0, 1010);
    check(pending.observeCapture(quiet, fixtureCapture(quiet, 1010), 1010) &&
              pending.gestureAllowed(0) && !pending.primaryAllowed(0),
          "fresh quiet arms G while unavailable raw manual action remains unarmed");
    auto throttled = gesturePose(1050, 3, 1010);
    check(pending.retain(throttled, 0, 1050, fixtureCapture(throttled, 1050), 3) &&
              pending.currentGestureIntent(throttled, 0, 1, 1, 1050) &&
              !pending.currentGestureIntent(throttled, 0, 2, 1, 1050) &&
              !pending.currentGestureIntent(throttled, 0, 1, 2, 1050) &&
              !pending.currentGestureIntent(throttled, 0, 1, 1, 1049),
          "quiet survives throttle and currentGestureIntent checks epoch, stream and source sample");
    pending.requireNeutral(1);
    auto after = gesturePose(1060, 3, 1010);
    pending.observeCapture(after, fixtureCapture(after, 1060), 1060);
    check(!pending.gestureAllowed(0) && pending.gestureAllowed(1),
          "a cached quiet witness cannot rearm an interrupted hand or interrupt the other hand");
    auto recovered = gesturePose(1070, 0, 1070);
    pending.observeCapture(recovered, fixtureCapture(recovered, 1070), 1070);
    check(pending.currentGestureIntent(recovered, 0, 1, 1, 1070, false) &&
              !pending.currentGestureIntent(recovered, 0, 1, 1, 1070),
          "new quiet recovers independently and does not pretend to be held");
    auto inconsistent = recovered; inconsistent.gestureDownMask = 1;
    pending.observeSnapshot(inconsistent, 1071);
    pending.filterPrimaryIntents(inconsistent);
    check(!(inconsistent.gestureEligibleMask & 1), "same source sample cannot change low into high");

    PeerState peer; check(peer.bindCapability(11, 22), "gesture peer binds existing capability");
    auto remote = gesturePose(900000, 3, 899999); remote.sequence = 1;
    check(peer.acceptPose(remote, 10) && remote.gestureDownMask == 3 && !remote.fireMask &&
              !peer.physicalFireAllowed(0),
          "sender clocks may differ from receiver clock; persistent quiet is not manual release");
    auto repeated = remote; repeated.sequence = 2;
    check(peer.acceptPose(repeated, 211) && !repeated.gestureEligibleMask,
          "outer packet progress cannot refresh a stale repeated gesture source");
    auto next = gesturePose(900010, 0, 900010); next.sequence = 3;
    check(peer.acceptPose(next, 212) && next.gestureEligibleMask == 3,
          "new source quiet recovers after timeout without comparing clocks across peers");
    auto replacement = gesturePose(900020, 3, 900019, 2); replacement.sequence = 4;
    check(peer.acceptPose(replacement, 213) && replacement.gestureDownMask == 3, "new gesture stream accepted");
    auto stale = gesturePose(900030, 3, 900029, 1); stale.sequence = 5;
    check(peer.acceptPose(stale, 214) && !stale.gestureEligibleMask && peer.gesture[0].generation == 2,
          "retired gesture generation neither grants intent nor rewrites the newer stream");
    auto rewind = replacement; rewind.sequence = 6; rewind.gestureSequence[0]--; rewind.gestureTickMs[0]--;
    rewind.gestureQuietSequence[0]--; rewind.gestureQuietTickMs[0]--;
    check(peer.acceptPose(rewind, 215) && rewind.gestureEligibleMask == 2,
          "source sequence rollback is rejected independently per hand");
    peer.invalidateWeaponGeneration(1, 1);
    auto rebound = gesturePose(900040, 3, 900019, 2); rebound.sequence = 7; rebound.intentEpoch[0] = 2;
    check(peer.acceptPose(rebound, 216) && rebound.gestureEligibleMask == 2,
          "new weapon epoch cannot reuse a pre-replacement quiet witness");
    auto rearmed = gesturePose(900050, 0, 900050, 2); rearmed.sequence = 8; rearmed.intentEpoch[0] = 2;
    check(peer.acceptPose(rearmed, 217) && rearmed.gestureEligibleMask == 3,
          "current epoch plus new quiet recovers the replaced hand");
    auto relabeled = gesturePose(900070, 3, 900060, 2); relabeled.sequence = 9; relabeled.intentEpoch[0] = 2;
    relabeled.gestureQuietTickMs[0] = 900050;
    check(peer.acceptPose(relabeled, 218) && relabeled.gestureEligibleMask == 2,
          "quiet source identity cannot advance its sequence while reusing an older witness tick");
}
static void gestureRetentionChecks() {
    auto high = gesturePose(1000);
    auto capture = fixtureCapture(high, 1000);
    capture.head = pose(.7f, -.4f, 0); capture.grip[0] = pose(1.7f, -.3f, .1f);
    PendingIntents pending;
    check(pending.retain(high, 0, 1000, capture, 1), "gesture edge occupies the existing capture slot");
    auto second = gesturePose(1010, 2);
    auto right = fixtureCapture(second, 1010);
    right.head = pose(.2f, .1f, .2f); right.grip[1] = pose(-.8f, .2f, .3f);
    check(pending.retain(second, 0, 1010, right, 2), "other hand retains its own capture and TTL");
    auto low = gesturePose(1020, 0);
    auto current = fixtureCapture(low, 1020); current.frame.origin.p = {.4f, -.1f, .1f}; current.frame.turn = .6f;
    check(composeLocalPose(low, current, 1020) && !pending.apply(low, 1020, current) &&
              low.gesturePulseMask == 3 && !low.gestureDownMask && !low.fireMask && !low.pulseMask,
          "released G pulses survive separately; apply still returns only manual pulses");
    check(nearPose(low.grip[0], bodyHandTracking(current.frame.origin, current.frame.turn, capture.head, capture.grip[0])) &&
              nearPose(low.grip[1], bodyHandTracking(current.frame.origin, current.frame.turn, right.head, right.grip[1])) &&
              low.gestureSequence[0] == 1000 && low.gestureSequence[1] == 1010 && validatePose(low),
          "both retained G samples use their own raw head/grip against the current origin");
    auto expired = gesturePose(1200, 0);
    check(!pending.apply(expired, 1200, fixtureCapture(expired, 1200)) && expired.gesturePulseMask == 2,
          "origin composition never renews either gesture TTL");
    pending.consume(expired.gesturePulseMask);
    auto duplicate = second;
    check(!pending.retain(duplicate, 0, 1200, right, 2), "consumed gesture source cannot be retained again");

    for (bool separateCalls : {false, true}) {
        PendingIntents combined;
        auto both = gesturePose(1000); manualDown(both, 3);
        auto raw = fixtureCapture(both, 1000);
        if (separateCalls) check(combined.retain(both, 3, 1000, raw), "manual capture admitted first");
        check(combined.retain(both, separateCalls ? 0 : 3, 1000, raw, 3),
              "same captured input coalesces manual and G without another queue");
        auto released = gesturePose(1010, 0); manualDown(released, 0);
        check(combined.apply(released, 1010, fixtureCapture(released, 1010)) == 3 &&
                  released.gesturePulseMask == 3 && !released.fireMask && !released.gestureDownMask && validatePose(released),
              "simultaneous completed manual/G taps remain distinct legal pulses");
    }
    {
        PendingIntents priority;
        auto manual = gesturePose(1000, 0); manualDown(manual, 1);
        auto first = fixtureCapture(manual, 1000); first.grip[0] = pose(.3f, .1f, .2f);
        check(priority.retain(manual, 1, 1000, first), "manual priority fixture retained");
        auto swing = gesturePose(1010, 1); auto newer = fixtureCapture(swing, 1010);
        swing.primarySampleEligibleMask = 3; swing.primaryNeutralSampleMask = 3;
        newer.grip[0] = pose(.8f, .3f, .1f);
        check(!priority.retain(swing, 0, 1010, newer, 1) && priority.mask(1010) == 1 && !priority.gestureMask(1010),
              "different-time gesture cannot overwrite or coalesce into the manual capture");
        auto delivered = swing;
        check(priority.apply(delivered, 1010, newer) == 1 && !delivered.gesturePulseMask &&
                  nearPose(delivered.grip[0], first.grip[0]), "existing manual capture geometry is preserved");
        priority.consume(1);
        check(!priority.retain(swing, 0, 1010, newer, 1), "capacity-discarded G edge cannot become later pulse debt");
    }
    {
        PendingIntents priority;
        auto initial = high; manualDown(initial, 0);
        check(priority.retain(initial, 0, 1000, capture, 1), "G-only slot admitted before a later manual edge");
        auto manual = gesturePose(1010, 0); manualDown(manual, 1);
        auto newer = fixtureCapture(manual, 1010); newer.grip[0] = pose(.8f, .3f, .1f);
        check(priority.retain(manual, 1, 1010, newer) && priority.mask(1010) == 1 && !priority.gestureMask(1010),
              "later manual edge replaces a G-only capture");
        auto delivered = manual;
        check(priority.apply(delivered, 1010, newer) == 1 && !delivered.gesturePulseMask &&
                  nearPose(delivered.grip[0], newer.grip[0]), "replacement carries the actual new manual geometry");
    }
    {
        PendingIntents full;
        check(full.retain(high, 0, 1000, capture, 1), "one G capture fills the hand");
        auto later = gesturePose(1010, 1, 1005); auto raw = fixtureCapture(later, 1010);
        check(!full.retain(later, 0, 1010, raw, 1), "a second G capture is explicitly discarded at capacity");
        auto delivered = later;
        check(!full.apply(delivered, 1010, raw) && delivered.gesturePulseMask == 1 &&
                  delivered.gestureSequence[0] == 1000, "capacity preserves the earliest captured G identity");
        full.consume(1);
        check(!full.retain(later, 0, 1010, raw, 1), "discarded second G cannot reappear after capacity opens");
    }
    for (unsigned altered = 0; altered < 2; ++altered) {
        PendingIntents sameId;
        auto both = high; manualDown(both, 1);
        auto raw = fixtureCapture(both, 1000);
        check(sameId.retain(both, 1, 1000, raw), "manual geometry held immutable for coalescing check");
        auto different = raw;
        if (altered) different.head.p.x += .1f;
        else different.grip[0].p.x += .1f;
        check(!sameId.retain(both, 0, 1000, different, 1),
              "identical source IDs cannot coalesce different captured head/grip data");
        auto outgoing = both;
        check(sameId.apply(outgoing, 1000, raw) == 1 && !outgoing.gesturePulseMask &&
                  nearPose(outgoing.grip[0], raw.grip[0]), "rejected coalescing preserves the manual capture");
    }
    {
        PendingIntents separate;
        auto both = high; manualDown(both, 1);
        check(separate.retain(both, 1, 1000, fixtureCapture(both, 1000), 1), "both source kinds initially retained");
        auto unavailableManual = gesturePose(1010, 0);
        check(!separate.apply(unavailableManual, 1010, fixtureCapture(unavailableManual, 1010)) &&
                  unavailableManual.gesturePulseMask == 1 && !unavailableManual.pulseMask,
              "manual action loss removes only manual retention when gesture provenance remains eligible");
    }
    for (unsigned stamp = 0; stamp < 3; ++stamp) {
        PendingIntents invalidSource;
        auto packet = high; auto raw = fixtureCapture(packet, 1000);
        if (stamp == 0) ++packet.gestureSequence[0];
        if (stamp == 1) ++packet.gestureTickMs[0];
        if (stamp == 2) { raw.frame.sequence = raw.frame.tickMs = 1300; }
        check(!invalidSource.retain(packet, 0, raw.frame.tickMs, raw, 1) && !invalidSource.gestureAllowed(0),
              "future or stale sender gesture stamps cannot acquire a raw capture");
    }
    for (unsigned changed = 0; changed < 8; ++changed) {
        PendingIntents retired;
        check(retired.retain(high, 0, 1000, capture, 1), "G replacement fixture retained");
        auto next = gesturePose(1010); auto raw = fixtureCapture(next, 1010);
        switch (changed) {
        case 0: ++raw.frame.avatar; break;
        case 1: ++raw.frame.producer; break;
        case 2: ++raw.frame.session; break;
        case 3: ++raw.frame.reference; break;
        case 4: ++raw.frame.trackingEpoch; break;
        case 5: ++next.intentEpoch[0]; break;
        case 6: next.nativeWeaponId[0] = 1; break;
        case 7: next.requestedWeapon[0] = 0; break;
        }
        check(!retired.apply(next, 1010, raw) && !(next.gesturePulseMask & 1) && !retired.gestureAllowed(0),
              "owner/capability/replaced-weapon context discards pending G and requires new quiet");
    }
    for (unsigned active = 0; active < 2; ++active) {
        PendingIntents isolated;
        auto one = gesturePose(1000); invalidateGestureIntents(one, uint8_t(1u << (1-active)));
        one.validMask = uint8_t(1 | (2u << active));
        auto raw = fixtureCapture(one, 1000); raw.grip[1-active].p.x = std::numeric_limits<float>::quiet_NaN();
        check(composeLocalPose(one, raw, 1000) && isolated.retain(one, 0, 1000, raw, uint8_t(1u << active)) &&
                  !isolated.apply(one, 1000, raw) && one.gesturePulseMask == (1u << active) && validatePose(one),
              "G retention preserves independence from an invalid inactive offhand");
    }
    {
        PendingIntents overflow;
        check(overflow.retain(high, 0, 1000, capture, 1), "overflow starts with retained G");
        auto next = gesturePose(1010); auto raw = fixtureCapture(next, 1010); raw.head.p.x = 1e20f;
        check(!prepareLocalPose(next, raw, capture.frame, raw.frame.avatar, 1010),
              "production pose preparation rejects finite raw head overflow");
        overflow.requireNeutral(); // Existing rejectLocalCapture boundary.
        check(!overflow.apply(next, 1010, raw) && !next.gesturePulseMask && !next.gestureEligibleMask,
              "finite raw head overflow revokes G rather than hiding behind a clamp");
    }
}
static void pendingHandoffChecks() {
    auto both = gesturePose(1000); manualDown(both, HandMask);
    const auto raw = fixtureCapture(both, 1000);
    {
        PendingIntents pending;
        check(pending.retain(both, 0, 1000, raw, 1), "handoff starts with a G-only capture");
        const auto sent = pending.handoffReceipt(1);
        auto manual = gesturePose(1010, 0); manualDown(manual, 1);
        check(pending.retain(manual, 1, 1010, fixtureCapture(manual, 1010)),
              "native-send reentry replaces G-only capture with later manual input");
        pending.consume(1, sent);
        check(pending.mask(1010) == 1 && !pending.gestureMask(1010),
              "stale G handoff cannot erase the replacement manual capture");
    }
    for (bool gestureFirst : {false, true}) {
        PendingIntents pending;
        check(pending.retain(both, gestureFirst ? 0 : 1, 1000, raw, gestureFirst ? 1 : 0),
              "one source kind is captured before native send");
        const auto sent = pending.handoffReceipt(1);
        auto settled = raw; settled.frame.origin.p.x += .2f; settled.frame.turn = .3f;
        check(pending.retain(both, gestureFirst ? 1 : 0, 1000, settled, gestureFirst ? 0 : 1),
              "same raw input merges another source kind during native-send reentry");
        pending.consume(1, sent);
        check(pending.mask(1000) == (gestureFirst ? 1 : 0) &&
                  pending.gestureMask(1000) == (gestureFirst ? 0 : 1),
              "handoff consumes only submitted kinds, preserving the kind merged during native send");
        pending.consume(1, sent);
        check(pending.active(0, 1000), "repeated stale handoff cannot consume the newly merged kind");
        pending.consume(1, pending.handoffReceipt(1));
        check(!pending.active(0, 1000), "remaining kind is consumed by its own matching handoff");
    }
    {
        PendingIntents pending, other;
        check(pending.retain(both, 3, 1000, raw, 3) && other.retain(both, 3, 1000, raw, 3),
              "both hands and source kinds are ready for normal handoff");
        const auto empty = pending.handoffReceipt(0), first = pending.handoffReceipt(1);
        pending.consume(3, empty);
        other.consume(3, first);
        check(pending.mask(1000) == 3 && pending.gestureMask(1000) == 3 && other.mask(1000) == 3 &&
                  other.gestureMask(1000) == 3, "empty/foreign receipts cannot consume pending input");
        pending.consume(3, first);
        check(pending.mask(1000) == 2 && pending.gestureMask(1000) == 2,
              "a receipt covers only its captured hand even with a larger consume mask");
        const auto second = pending.handoffReceipt(3);
        pending.consume(1, second);
        check(pending.mask(1000) == 2, "consume mask cannot retire a different hand");
        pending.consume(3, second);
        check(!pending.mask(1000) && !pending.gestureMask(1000), "matching MG handoff consumes both kinds");
    }
    for (bool expire : {false, true}) {
        PendingIntents pending;
        check(pending.retain(both, 1, 1000, raw, 1), "interruption starts with an MG receipt");
        const auto sent = pending.handoffReceipt(1);
        const uint64_t quietTime = expire ? 1200 : 1010;
        if (expire) check(!pending.mask(quietTime) && !pending.gestureMask(quietTime), "captured input expires");
        else pending.requireNeutral(1);
        pending.consume(1, sent);
        check(!pending.active(0, quietTime), "late completion cannot revive expired/canceled input");
        auto quiet = gesturePose(quietTime, 0, quietTime); manualDown(quiet, 0);
        check(pending.observeCapture(quiet, fixtureCapture(quiet, quietTime), quietTime),
              "actual fresh neutral/quiet permits recovery after interruption");
        auto fresh = gesturePose(quietTime + 10, 1, quietTime); manualDown(fresh, 1);
        check(pending.retain(fresh, 1, quietTime + 10, fixtureCapture(fresh, quietTime + 10), 1),
              "recovered source retains a new MG capture");
        pending.consume(1, sent);
        check(pending.mask(quietTime + 10) == 1 && pending.gestureMask(quietTime + 10) == 1,
              "pre-interruption receipt cannot retire recovered input");
    }
    for (unsigned changed = 0; changed < 8; ++changed) {
        PendingIntents pending;
        check(pending.retain(both, 1, 1000, raw, 1), "source replacement starts with an MG receipt");
        const auto sent = pending.handoffReceipt(1);
        auto quiet = gesturePose(1010, 0, 1010, 2); manualDown(quiet, 0);
        auto context = fixtureCapture(quiet, 1010);
        switch (changed) {
        case 0: ++context.frame.avatar; break;
        case 1: ++context.frame.producer; break;
        case 2: ++context.frame.session; break;
        case 3: ++context.frame.reference; break;
        case 4: ++context.frame.trackingEpoch; break;
        case 5: ++quiet.trackingGeneration; ++context.frame.trackingGeneration; break;
        case 6: ++quiet.intentEpoch[0]; break;
        case 7: ++quiet.primaryInputGeneration[0]; break;
        }
        check(pending.observeCapture(quiet, context, 1010), "replacement context observes fresh quiet and neutral");
        auto fresh = quiet; fresh.gestureSequence[0] = fresh.gestureSequence[1] = 1020;
        fresh.gestureTickMs[0] = fresh.gestureTickMs[1] = 1020; fresh.gestureDownMask = 1;
        manualDown(fresh, 1); context.frame.sequence = context.frame.tickMs = 1020;
        check(pending.retain(fresh, 1, 1020, context, 1), "replacement source retains MG in existing hand slot");
        pending.consume(1, sent);
        check(pending.mask(1020) == 1 && pending.gestureMask(1020) == 1,
              "stale source-context receipt cannot retire replacement provenance");
    }
    for (unsigned changed = 0; changed < 5; ++changed) {
        PendingIntents pending;
        auto manual = validPose(); manualDown(manual, 1);
        auto capture = fixtureCapture(manual, 1000);
        check(pending.retain(manual, 1, 1000, capture), "exact-identity comparison starts with manual input");
        const auto sent = pending.handoffReceipt(1);
        pending.cancel(1);
        // Deliberately hold the unchanged fields/expiry equal: each distinct
        // raw capture or source identity must independently defeat stale consume.
        switch (changed) {
        case 0: capture.head.p.x += .1f; break;
        case 1: capture.grip[0].p.z += .1f; break;
        case 2: ++capture.frame.sequence; break;
        case 3: manual.nativeWeaponId[0] = 3; break;
        case 4: ++capture.frame.producer; break;
        }
        check(pending.retain(manual, 1, 1000, capture), "canceled slot receives distinct capture at the same expiry");
        pending.consume(1, sent);
        check(pending.mask(1000) == 1, "raw/identity mismatch alone preserves replacement capture");
    }
}
static void gestureIntervalChecks() {
    for (uint8_t held : {uint8_t(0), uint8_t(3)}) {
        OrderedPosePolicy server;
        server.validation.bindCapability(11, 22);
        auto packet = gesturePose(1000, held); packet.sequence = 1; packet.gesturePulseMask = 3;
        Ack ack; PosePacket sample, relay;
        check(server.receive(packet, 10, ack) && server.freeze(11, sample, &relay) &&
                  !sample.fireMask && !sample.pulseMask && sample.gesturePulseMask == 3 &&
                  sample.gestureDownMask == held && relay.gesturePulseMask == 3 &&
                  !server.active.gesturePulseMask && validatePose(relay),
              "freeze grants G pulse for one interval without expanding manual or held G levels");
        check(server.finishInterval(ack) && ack.acceptedSequence == 1 &&
                  server.freeze(12, sample, &relay) && !sample.gesturePulseMask && sample.gestureDownMask == held,
              "normal transport settlement is distinct from one-use G pulse and retained held level");
        packet.sequence = 2;
        check(server.receive(packet, 13, ack) && server.freeze(14, sample) && !sample.gesturePulseMask &&
                  sample.gestureDownMask == held && server.finishInterval(ack),
              "new outer sequence cannot replay the same gesture pulse identity");
        auto fresh = gesturePose(1010, held); fresh.sequence = 3; fresh.gesturePulseMask = 3;
        check(server.receive(fresh, 15, ack) && server.freeze(16, sample), "next gesture interval admitted");
        server.abortInterval();
        check(!server.freeze(17, sample) && !sample.gestureDownMask && !sample.gesturePulseMask &&
                  server.peekInterval(ack) && ack.acceptedSequence == 3,
              "aborted interval discards G but retains the exact existing transport token");
    }
}
static void mixedGameplayOwnershipChecks() {
    OrderedPosePolicy peer;
    check(peer.validation.bindCapability(11, 22) && !peer.gameplayOwned(),
          "Hello capability never claims desktop gameplay");
    auto neutral = validPose();
    neutral.validMask = 0;
    neutral.head = neutral.grip[0] = neutral.grip[1] = Pose{};
    neutral.primarySampleEligibleMask = neutral.primaryNeutralSampleMask = 0;
    neutral.zoomSampleEligibleMask = 0;
    Ack ack;
    PosePacket sample, relay;
    ConsumptionCredits credits;
    check(credits.grant({11, 22, 1}) && peer.receive(neutral, 1000, ack) &&
              peer.freeze(1000, sample, &relay) && !peer.gameplayOwned() &&
              peer.relayRecipientReady(1000),
          "accepted desktop heartbeat grants relay freshness without VR control");
    check(!peer.receive(validPose(2), 1000, ack) && !peer.gameplayOwned(),
          "tracked packet rejected by outstanding native consumption cannot claim gameplay");
    int pawn = 0;
    NativePrimaryInvocation desktop{&pawn, peer.gameplayOwned() ?
        NativePrimaryValue::neutral() : NativePrimaryValue{}, nullptr, false};
    for (unsigned held = 0; held != 2; ++held) {
        desktop.held = held != 0;
        for (unsigned kind = 0; kind != 5; ++kind)
            for (uint32_t byte = 0; byte != 256; ++byte) {
                const uint32_t raw = 0xabcd1200u | byte;
                check(nativePrimaryRead(raw, &pawn, kind, &desktop) == raw,
                      "desktop primary down/press/release/history/held remain untouched");
            }
    }
    check(peer.finishInterval(ack) && credits.acknowledge(ack, 11, 22) &&
              !credits.hasOutstandingCapability(11, 22),
          "desktop neutral interval retires the exact transport credit");
    check(!peer.relayRecipientReady(1000 + MaxPoseAgeMs + 1),
          "desktop relay recipient must remain fresh");
    OrderedPosePolicy occupied;
    check(occupied.validation.bindCapability(11, 22) && occupied.receive(neutral, 1000, ack) &&
              !occupied.receive(validPose(2), 1000, ack) && !occupied.gameplayOwned(),
          "tracked packet rejected by a pending slot cannot claim gameplay");
    auto tracked = validPose(2);
    auto invalid = tracked;
    invalid.head.q.w = std::numeric_limits<float>::quiet_NaN();
    check(!peer.receive(invalid, 1001, ack) && !peer.gameplayOwned(),
          "rejected malformed head cannot claim gameplay");
    invalid = tracked; invalid.serverNonce = 99;
    check(!peer.receive(invalid, 1001, ack) && !peer.gameplayOwned(),
          "rejected foreign capability cannot claim gameplay");
    check(!peer.receive(tracked, 1001, ack, false) && !peer.gameplayOwned(),
          "rate/capacity rejection cannot claim gameplay");
    invalid = tracked; invalid.sequence = 1;
    check(!peer.receive(invalid, 1001, ack) && !peer.gameplayOwned(),
          "replayed tracked packet cannot claim gameplay");
    check(peer.receive(tracked, 1001, ack) && peer.gameplayOwned() &&
              peer.freeze(1001, sample, &relay) && peer.finishInterval(ack),
          "only accepted tracked head claims gameplay through the production receive policy");
    neutral.sequence = 3;
    check(peer.receive(neutral, 1002, ack) && peer.freeze(1002, sample) &&
              !sample.validMask && peer.gameplayOwned() && peer.finishInterval(ack),
          "tracking loss keeps established XR ownership with no valid pose");
    peer.validation.invalidateTracking();
    check(peer.gameplayOwned() && !peer.freeze(1003, sample),
          "tracking invalidation cannot release XR ownership to native input");
    NativePrimaryInvocation lost{&pawn, peer.gameplayOwned() ?
        NativePrimaryValue::neutral() : NativePrimaryValue{}, nullptr, true};
    check(nativePrimaryRead(0xabcd1203u, &pawn, 4, &lost) == 0xabcd1200u,
          "established XR tracking loss suppresses both native fire bits");
    check(peer.retainGameplayOwnership(10, 20, 10, 20) &&
              !peer.retainGameplayOwnership(10, 20, 10, 21) &&
              !peer.retainGameplayOwnership(10, 20, 11, 20) &&
              !peer.retainGameplayOwnership(0, 20, 0, 20) &&
              !peer.retainGameplayOwnership(10, 0, 10, 0),
          "same native owner can retain ownership on capability renewal; replacement cannot");
    const bool retained = peer.retainGameplayOwnership(10, 20, 10, 20);
    peer = {};
    peer.xrGameplayOwned = retained;
    check(!peer.gameplayOwned() && peer.validation.bindCapability(33, 44) && peer.gameplayOwned(),
          "renewal still requires a new bound transport capability");
    peer = {};
    check(peer.validation.bindCapability(55, 66) && !peer.gameplayOwned(),
          "avatar/disconnect reset clears ownership before another capability");
    OrderedPosePolicy sender;
    check(sender.validation.bindCapability(11, 22) && sender.receive(tracked, 2000, ack) &&
              sender.freeze(2000, sample, &relay), "VR source freezes presentation for transport");
    Message message{}, decoded{};
    message.kind = Kind::Relay;
    message.relay = {relay, 99, 1};
    // Native relay rewrites the recipient capability; it does not require the
    // recipient to establish XR gameplay ownership.
    message.relay.pose.clientNonce = 55;
    message.relay.pose.serverNonce = 66;
    neutral.clientNonce = 55; neutral.serverNonce = 66; neutral.sequence = 1;
    check(peer.receive(neutral, 2000, ack) && peer.freeze(2000, sample) &&
              peer.relayRecipientReady(2000) && !peer.gameplayOwned(),
          "headset-free recipient stays eligible for VR peer delivery");
    std::string bytes;
    check(encode(message, bytes) && parse(bytes, decoded) == ParseResult::Valid &&
              decoded.kind == Kind::Relay && decoded.relay.pose.validMask == 7 &&
              decoded.relay.pose.clientNonce == 55 && decoded.relay.pose.serverNonce == 66,
          "existing relay wire carries VR presentation to the desktop capability");
}
int main() {
    mixedGameplayOwnershipChecks();
    observerGestureChecks();
    gestureWireChecks();
    gestureAdmissionChecks();
    gestureRetentionChecks();
    pendingHandoffChecks();
    gestureIntervalChecks();
    rawCaptureChecks();
    captureBootstrapChecks();
    Message hello{};
    hello.kind = Kind::Hello;
    hello.hello.nonce = 0x1122334455667788ull;
    std::string wire;
    check(encode(hello, wire) && wire.starts_with(CarrierTag) && wire.size() <= MaxCarrierAscii,
          "hello carrier is bounded and tagged");
    Message decoded{};
    check(parse(wire, decoded) == ParseResult::Valid && decoded.kind == Kind::Hello &&
              decoded.hello.nonce == hello.hello.nonce,
          "little-endian hello round trips");
    std::vector<uint8_t> oldVersion;
    check(detail::unbase64(std::string_view(wire).substr(CarrierTag.size()), oldVersion),
          "valid hello has a native wire payload");
    oldVersion[0] = 4;
    check(parse(std::string(CarrierTag) + detail::base64(oldVersion), decoded) == ParseResult::Malformed,
          "Wire4 cannot negotiate without retained-shot zoom context");
    auto p = validPose();
    p.physicalDownMask = 1;
    p.primaryNeutralSampleMask = uint8_t(HandMask & ~p.physicalDownMask & p.primarySampleEligibleMask);
    p.fireMask = 1;
    p.releasedSerial[0] = 4;
    wire = encodePose(p);
    check(parse(wire, decoded) == ParseResult::Valid && decoded.pose.sequence == 1 &&
              decoded.pose.grip[0].p.x == .4f && decoded.pose.fireMask == 1,
          "pose round trips through base64");
    Ack epochAck{11, 22, 7, {3, 9}};
    Message ackMessage{};
    ackMessage.kind = Kind::Ack;
    ackMessage.ack = epochAck;
    check(encode(ackMessage, wire) && parse(wire, decoded) == ParseResult::Valid &&
              decoded.ack.intentEpoch[0] == 3 && decoded.ack.intentEpoch[1] == 9,
          "ack epochs round trip without changing its credit token");
    Relay relay{p, 99, 7};
    Message relayMessage{};
    relayMessage.kind = Kind::Relay;
    relayMessage.relay = relay;
    check(encode(relayMessage, wire) && parse(wire, decoded) == ParseResult::Valid &&
              decoded.relay.subjectAvatar == 99 && decoded.relay.subjectIncarnation == 7,
          "relay adds server subject identity");
    p.pulseMask = 1;
    wire = encodePose(p);
    check(parse(wire, decoded) == ParseResult::Valid && decoded.pose.pulseMask == 1,
          "one-step physical pulse round trips with the reliable pose");
    p = validPose();
    p.pulseMask = 1;
    wire = encodePose(p);
    check(parse(wire, decoded) == ParseResult::Valid && !decoded.pose.fireMask && decoded.pose.pulseMask == 1,
          "released short tap remains a pulse rather than a held fire level");
    check(outboundTargetHandle(true, 37, 91) == 91 && outboundTargetHandle(false, 37, 91) == 37,
          "client outbound policy keeps the mapped server wire handle even though it is not local");
    check(!outboundHandlesValid(false, 91) && outboundHandlesValid(true, 91),
          "failed local source resolve rejects send without resolving the mapped server handle");
    check(parse("ordinary engine chat", decoded) == ParseResult::NotMod, "untagged chat is not consumed");
    check(parse("~SS2VR1~not-base64", decoded) == ParseResult::Malformed,
          "tagged invalid base64 is consumed as malformed");
    check(parse(std::string(CarrierTag) + "AQEAAAAA", decoded) == ParseResult::Malformed,
          "truncated tagged payload is malformed");
    auto oversized = std::string(CarrierTag) + std::string(MaxCarrierAscii, 'A');
    check(parse(oversized, decoded) == ParseResult::Malformed, "oversized tagged carrier is rejected");
    p = validPose();
    p.fireMask = 1;
    p.physicalDownMask = 0;
    p.primaryNeutralSampleMask = uint8_t(HandMask & ~p.physicalDownMask & p.primarySampleEligibleMask);
    Message invalid{};
    invalid.kind = Kind::Pose;
    invalid.pose = p;
    check(!encode(invalid, wire), "fire requires a physical down witness");
    p = validPose();
    p.primarySampleEligibleMask = 4;
    invalid.pose = p;
    check(!encode(invalid, wire), "unknown primary eligibility bits are malformed");
    p = validPose();
    p.zoomSampleEligibleMask = 4;
    invalid.pose = p;
    check(!encode(invalid, wire), "unknown zoom eligibility bits are malformed");
    p = validPose();
    p.primaryNeutralSampleMask = 4;
    invalid.pose = p;
    check(!encode(invalid, wire), "unknown primary neutral bits are malformed");
    p = validPose();
    p.physicalDownMask = 1;
    invalid.pose = p;
    check(!encode(invalid, wire), "a raw neutral witness cannot also be physically held");
    p = validPose();
    p.primarySampleEligibleMask = 2;
    invalid.pose = p;
    check(!encode(invalid, wire), "an inactive primary action cannot contribute neutral evidence");
    p = validPose();
    p.nativeWeaponId[0] = 13;
    p.zoomMask = p.zoomPhysicalDownMask = 1;
    p.intentEpoch[0] = 7;
    p.intentEpoch[1] = 8;
    p.releasedSerial[0] = 17;
    p.primaryInputGeneration[0] = 19;
    p.zoomReleasedSerial[0] = 23;
    p.zoomInputGeneration[0] = 29;
    wire = encodePose(p);
    check(parse(wire, decoded) == ParseResult::Valid && decoded.pose.intentEpoch[0] == 7 &&
              decoded.pose.primaryInputGeneration[0] == 19 && decoded.pose.zoomReleasedSerial[0] == 23 &&
              decoded.pose.zoomInputGeneration[0] == 29 && decoded.pose.zoomPhysicalDownMask == 1 &&
              decoded.pose.zoomSampleEligibleMask == HandMask && decoded.pose.primarySampleEligibleMask == HandMask &&
              decoded.pose.primaryNeutralSampleMask == HandMask,
          "wire6 preserves raw action eligibility, streams, serials, and epochs");
    std::vector<uint8_t> truncatedPose;
    check(detail::unbase64(std::string_view(wire).substr(CarrierTag.size()), truncatedPose),
          "wire6 pose has a decodable payload");
    truncatedPose.pop_back();
    check(parse(std::string(CarrierTag) + detail::base64(truncatedPose), decoded) == ParseResult::Malformed,
          "codec loss in wire6 action fields is malformed rather than defaulted");
    p.zoomPhysicalDownMask = 0;
    invalid.pose = p;
    check(!encode(invalid, wire), "held zoom requires active raw-down evidence in the transport original");
    p = validPose();
    p.head.p.x = HeadHorizontalLimit + .001f;
    invalid.pose = p;
    check(!encode(invalid, wire), "head body-relative translation is bounded");
    p = validPose();
    p.head.p = {.2f, -1.1f, .3f};
    p.grip[0].p = p.head.p + Vec3{-.4f, -.4f, -.5f};
    p.grip[1].p = p.head.p + Vec3{.4f, -.4f, -.5f};
    wire = encodePose(p);
    check(parse(wire, decoded) == ParseResult::Valid && decoded.pose.head.p.y == -1.1f &&
              decoded.pose.grip[0].p.y == -1.5f,
          "Deep crouch and head-relative controller poses survive multiplayer replication");
    p.head.p.y = -HeadDownLimit - .01f;
    invalid.pose = p;
    check(!encode(invalid, wire), "Headset vertical travel remains bounded independently of horizontal lean");
    p = validPose();
    p.grip[0].p = {HeadHorizontalLimit + HandReachLimit + .01f, 0, 0};
    invalid.pose = p;
    check(!encode(invalid, wire),
          "Server rejects grips outside the reachable head volume even inside the outer sphere");
    p = validPose();
    p.validMask = 1;
    p.grip[0].p = p.head.p + Vec3{HandReachLimit + .01f, 0, 0};
    invalid.pose = p;
    check(!encode(invalid, wire), "Inactive hands retain the tighter current-head bound");
    p = validPose();
    p.grip[0].p = {HeadHorizontalLimit + .93f, -HeadDownLimit - .93f, 0};
    invalid.pose = p;
    check(!encode(invalid, wire), "Envelope corner distance rejects excessive diagonal reach");
    p = validPose();
    p.head.p = {HeadHorizontalLimit, -HeadDownLimit, 0};
    p.grip[0].p = p.head.p + Vec3{0, -HandReachLimit, 0};
    p.grip[1].p = p.head.p;
    wire = encodePose(p);
    check(parse(wire, decoded) == ParseResult::Valid, "Valid poses at the full crouch/reach boundary encode");
    p.validMask = 1;
    p.grip[0].p = p.grip[1].p = p.head.p;
    wire = encodePose(p);
    check(parse(wire, decoded) == ParseResult::Valid,
          "Canonical inactive hands do not discard deep head tracking");
    for (float nonfinite :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        p = validPose();
        p.grip[0].p.z = nonfinite;
        invalid.pose = p;
        check(!encode(invalid, wire), "Nonfinite controller positions cannot enter the tracking envelope");
    }
    p = validPose();
    const Pose calibration{yaw(.63f), {2, 1, -4}};
    const Pose limitHead{
        normalize(multiply(yaw(.7f), Quat{.2f, 0, .1f, .97f})),
        {HeadHorizontalLimit * std::sqrt(.5f), -HeadDownLimit, HeadHorizontalLimit * std::sqrt(.5f)}};
    for (float turn : {-.7f, 1.6f, Pi}) {
        p = validPose();
        p.head = bodyHeadTracking(calibration, turn, compose(calibration, limitHead));
        for (unsigned hand = 0; hand != 2; ++hand) {
            Pose grip = limitHead;
            grip.p.x += hand ? HandReachLimit : -HandReachLimit;
            p.grip[hand] = bodyHandTracking(calibration, turn, compose(calibration, limitHead),
                                            compose(calibration, grip));
        }
        wire = encodePose(p);
        check(
            parse(wire, decoded) == ParseResult::Valid,
            "Production rig transforms survive turn/recenter rounding at simultaneous head and reach limits");
    }
    p = validPose();
    p.grip[0].q.w = .8f;
    invalid.pose = p;
    check(!encode(invalid, wire), "out of range quaternion is rejected");
    p = validPose();
    p.grip[0].q.w = 1.01f;
    wire = encodePose(p);
    check(parse(wire, decoded) == ParseResult::Valid && decoded.pose.grip[0].q.w == 1.0f,
          "small quaternion drift is normalized");
    p = validPose();
    p.requestedWeapon[0] = 14;
    invalid.pose = p;
    check(!encode(invalid, wire), "reserved weapon id is rejected");
    PeerState peer{};
    check(peer.bindCapability(11, 22), "peer binds nonzero handshake capability");
    p = validPose(1);
    p.validMask = 7;
    p.physicalDownMask = 1;
    p.primaryNeutralSampleMask = uint8_t(HandMask & ~p.physicalDownMask & p.primarySampleEligibleMask);
    p.fireMask = 1;
    p.releasedSerial[0] = 5;
    check(peer.acceptPose(p, 1000) && !peer.physicalFireAllowed(0),
          "held state cannot arm after capability bind");
    p = validPose(2);
    p.validMask = 7;
    p.releasedSerial[0] = 6;
    check(peer.acceptPose(p, 1010) && peer.physicalFireAllowed(0),
          "new serial while physically released arms hand");
    check(!peer.acceptPose(p, 1020), "sequence is monotonic within a generation");
    check(peer.fresh(1210) && !peer.fresh(1211), "caller supplied local tick enforces 200ms freshness");
    peer.invalidateTracking();
    p = validPose(3, 2);
    p.validMask = 5;
    p.releasedSerial[0] = 7; // hand invalid: not a release.
    check(peer.acceptPose(p, 1300) && !peer.physicalFireAllowed(0),
          "tracking invalidity cannot arm physical release");
    p = validPose(4, 2);
    p.validMask = 7;
    p.releasedSerial[0] = 8;
    check(peer.acceptPose(p, 1310) && peer.physicalFireAllowed(0),
          "valid physical release re-arms after tracking invalidation");
    peer.invalidateWeaponGeneration(9);
    p = validPose(5, 2);
    p.intentEpoch[0] = 2;
    p.validMask = 7;
    p.wheelOrEquipBlockedMask = 1;
    p.releasedSerial[0] = 8;
    check(peer.acceptPose(p, 1320) && !peer.physicalFireAllowed(0),
          "wheel/equipment gating is not a release witness");
    p = validPose(6, 2);
    p.intentEpoch[0] = 2;
    p.validMask = 7;
    p.releasedSerial[0] = 9;
    check(peer.acceptPose(p, 1330) && peer.physicalFireAllowed(0),
          "new released serial re-arms after weapon generation change");
    p = validPose(7, 1);
    check(!peer.acceptPose(p, 1340), "old generation cannot replace current tracking");
    p = validPose(6, 3);
    check(!peer.acceptPose(p, 1340), "new generation cannot bypass sequence ordering");
    p = validPose(7, 2);
    p.intentEpoch[0] = 2;
    p.physicalDownMask = p.fireMask = 1;
    p.primaryNeutralSampleMask = uint8_t(HandMask & ~p.physicalDownMask & p.primarySampleEligibleMask);
    p.releasedSerial[0] = 9;
    check(peer.acceptPose(p, 1700) && !peer.physicalFireAllowed(0),
          "expired stream requires new physical release");

    // Action activity is not inferred from hand tracking. An inactive sample
    // may be coalesced out of transport, so its next active held sample cannot
    // establish a release without a new active raw-up witness.
    PeerState actions{};
    check(actions.bindCapability(11, 22), "action-admission peer binds capability epochs");
    auto action = validPose(1);
    action.releasedSerial[0] = action.zoomReleasedSerial[0] = 1;
    check(actions.acceptPose(action, 1800) && actions.physicalFireAllowed(0) && actions.zoomAllowed(0),
          "independent active raw-up samples arm primary and zoom");
    action = validPose(2);
    action.physicalDownMask = action.fireMask = action.zoomPhysicalDownMask = 1;
    action.primaryNeutralSampleMask = uint8_t(HandMask & ~action.physicalDownMask & action.primarySampleEligibleMask);
    action.primarySampleEligibleMask &= uint8_t(~1u);
    action.primaryNeutralSampleMask &= uint8_t(~1u);
    action.zoomSampleEligibleMask &= uint8_t(~1u);
    ++action.primaryInputGeneration[0];
    ++action.zoomInputGeneration[0];
    check(actions.acceptPose(action, 1810) && !action.fireMask && !actions.physicalFireAllowed(0) &&
              !actions.zoomAllowed(0),
          "inactive raw-held actions revoke admission without fabricating a release");
    action = validPose(3);
    action.primaryInputGeneration[0] = 2;
    action.zoomInputGeneration[0] = 2;
    action.physicalDownMask = action.fireMask = action.zoomPhysicalDownMask = action.zoomMask = 1;
    action.primaryNeutralSampleMask = uint8_t(HandMask & ~action.physicalDownMask & action.primarySampleEligibleMask);
    action.nativeWeaponId[0] = 13;
    check(actions.acceptPose(action, 1820) && !action.fireMask && !action.zoomMask,
          "coalesced inactive-to-active held samples remain blocked until a real neutral");
    action = validPose(4);
    action.primaryInputGeneration[0] = 2;
    action.zoomInputGeneration[0] = 2;
    action.releasedSerial[0] = action.zoomReleasedSerial[0] = 2;
    check(actions.acceptPose(action, 1830) && actions.physicalFireAllowed(0) && actions.zoomAllowed(0),
          "new active raw-up serials establish each action's own neutral");
    action = validPose(5);
    action.primaryInputGeneration[0] = 2;
    action.zoomInputGeneration[0] = 2;
    action.releasedSerial[0] = action.zoomReleasedSerial[0] = 2;
    action.physicalDownMask = action.fireMask = action.zoomPhysicalDownMask = action.zoomMask = 1;
    action.primaryNeutralSampleMask = uint8_t(HandMask & ~action.physicalDownMask & action.primarySampleEligibleMask);
    action.nativeWeaponId[0] = 13;
    check(actions.acceptPose(action, 1840) && action.fireMask == 1 && action.zoomMask == 1,
          "admitted primary and zoom held levels remain independent after neutral");
    action = validPose(6);
    action.primaryInputGeneration[0] = 2;
    action.zoomInputGeneration[0] = 3;
    action.releasedSerial[0] = 2;
    action.zoomReleasedSerial[0] = 2;
    action.physicalDownMask = action.fireMask = action.zoomPhysicalDownMask = action.zoomMask = 1;
    action.primaryNeutralSampleMask = uint8_t(HandMask & ~action.physicalDownMask & action.primarySampleEligibleMask);
    action.nativeWeaponId[0] = 13;
    check(actions.acceptPose(action, 1850) && action.fireMask == 1 && !action.zoomMask &&
              actions.physicalFireAllowed(0) && !actions.zoomAllowed(0),
          "a zoom stream change revokes only zoom admission while primary remains live");

    PeerState epochs{};
    check(epochs.bindCapability(11, 22) && epochs.intentEpoch[0] == 1 && epochs.intentEpoch[1] == 1,
          "successful bind starts nonzero per-hand epochs");
    epochs.invalidateWeaponGeneration(9, 1);
    epochs.invalidateWeaponGeneration(9, 1);
    check(epochs.intentEpoch[0] == 3 && epochs.intentEpoch[1] == 1,
          "repeated same-generation invalidations advance only their affected hand");
    epochs.intentEpoch[0] = UINT32_MAX;
    epochs.invalidateWeaponGeneration(9, 1);
    epochs.invalidateWeaponGeneration(9, 1);
    check(!epochs.intentEpoch[0], "epoch exhaustion fails closed without reusing a value");
    PeerState staleEpoch{};
    check(staleEpoch.bindCapability(11, 22), "stale-epoch trace binds capability");
    auto zeroEpoch = validPose(1);
    zeroEpoch.intentEpoch[0] = 0;
    zeroEpoch.releasedSerial[0] = 99;
    zeroEpoch.zoomReleasedSerial[0] = 99;
    check(staleEpoch.acceptPose(zeroEpoch, 1860) && !staleEpoch.physicalFireAllowed(0) &&
              !staleEpoch.zoomAllowed(0) && !staleEpoch.observedReleasedSerial[0] &&
              !staleEpoch.observedZoomReleasedSerial[0],
          "zero epoch filters all intents before it can poison either release baseline");
    auto currentEpoch = validPose(2);
    currentEpoch.releasedSerial[0] = currentEpoch.zoomReleasedSerial[0] = 1;
    check(staleEpoch.acceptPose(currentEpoch, 1870) && staleEpoch.physicalFireAllowed(0) &&
              staleEpoch.zoomAllowed(0),
          "current epoch establishes neutral after discarded stale raw input");
    ConsumptionCredits credits;
    check(credits.grant({11, 22, 1}) && !credits.grant({11, 22, 2}) &&
              credits.hasOutstandingCapability(11, 22),
          "one capability cannot submit another pose before native consumption");
    check(credits.grant({12, 23, 1}) && credits.grant({13, 24, 1}) && credits.grant({14, 25, 1}) &&
              credits.full() && !credits.grant({15, 0, 0}),
          "four recovery capabilities exhaust aggregate credit including Hello grants");
    Ack consumed{11, 22, 1};
    check(credits.retire(consumed) && credits.count() == 3 && !credits.retire({11, 22, 3}),
          "only the exact completed pose token returns credit");
    check(!credits.retire({13, 23, 1}) && credits.count() == 3,
          "an ACK from another capability cannot free reliable credit");
    FixturePendingIntents pending;
    auto leftTap = validPose();
    leftTap.physicalDownMask = leftTap.fireMask = 1;
    leftTap.primaryNeutralSampleMask = uint8_t(HandMask & ~leftTap.physicalDownMask & leftTap.primarySampleEligibleMask);
    check(pending.retain(leftTap, 1, 100), "quick left tap is retained while snapshot credit is full");
    auto rightTap = leftTap;
    rightTap.physicalDownMask = rightTap.fireMask = 2;
    rightTap.primaryNeutralSampleMask = uint8_t(HandMask & ~rightTap.physicalDownMask & rightTap.primarySampleEligibleMask);
    check(pending.retain(rightTap, 2, 101) && pending.active(0, 101) && pending.active(1, 101),
          "overlapping hand taps retain one immutable intent per hand");
    check(pending.mask(101) == 3, "retained taps become a bounded aggregate pulse mask");
    auto rightWheel = validPose();
    rightWheel.wheelOrEquipBlockedMask = 2;
    pending.observeSnapshot(rightWheel);
    check(pending.active(0, 102) && !pending.active(1, 102),
          "right wheel snapshot cancels only its own pending tap");
    check(!pending.active(0, 300), "unsubmitted quick taps expire within a bounded window");
    pending.requireNeutral(1);
    check(!pending.retain(leftTap, 1, 301), "recovery rejects retained held input before neutral");
    auto neutral = validPose();
    neutral.releasedSerial[0] = 3;
    pending.observeSnapshot(neutral);
    check(pending.retain(leftTap, 1, 302), "real neutral permits the next left tap");

    // Drive the same credit, immutable intent, and receive/freeze/finish methods
    // used by the adapter, including a real delayed receive/consumption window.
    ConsumptionCredits window;
    OrderedPosePolicy stream;
    check(stream.validation.bindCapability(11, 22), "ordered stream binds capability");
    Ack ack{};
    auto latest = validPose(1);
    latest.releasedSerial[0] = latest.releasedSerial[1] = 1;
    check(window.grant({11, 22, 1}) && stream.receive(latest, 1000, ack),
          "first neutral snapshot occupies the native consumption slot");
    check(!stream.finishInterval(ack) && window.hasOutstandingCapability(11, 22),
          "arrival alone cannot return consumption credit");
    FixturePendingIntents taps;
    taps.observeSnapshot(latest);
    auto press = latest;
    press.physicalDownMask = press.fireMask = 1;
    press.primaryNeutralSampleMask = uint8_t(HandMask & ~press.physicalDownMask & press.primarySampleEligibleMask);
    press.grip[0] = pose(.65f, .1f, 0);
    check(taps.retain(press, 1, 1010), "left tap retained behind outstanding native pose");
    auto otherPress = press;
    otherPress.physicalDownMask = otherPress.fireMask = 2;
    otherPress.primaryNeutralSampleMask = uint8_t(HandMask & ~otherPress.physicalDownMask & otherPress.primarySampleEligibleMask);
    otherPress.grip[1] = pose(-.6f, .2f, 0);
    check(taps.retain(otherPress, 2, 1011), "right tap overlaps without rewriting left intent");
    latest = validPose(2);
    latest.releasedSerial[0] = latest.releasedSerial[1] = 2;
    latest.grip[0] = pose(.2f, 0, 0);
    latest.grip[1] = pose(-.2f, 0, 0);
    taps.observeSnapshot(latest);
    check(!window.grant({11, 22, 2}), "release remains buffered while current capability is full");
    PosePacket sample{};
    check(stream.freeze(1050, sample) && !sample.fireMask && window.count() == 1,
          "delayed neutral reaches native interval without early ACK");
    check(stream.finishInterval(ack) && window.acknowledge(ack, 11, 22) && !window.acknowledge(ack, 11, 22),
          "completed exact token refreshes once; replay cannot refresh capability");
    const auto pulse = taps.apply(latest, 1060);
    check(pulse == 3 && !latest.fireMask && latest.grip[0].p.x == .65f && latest.grip[1].p.x == -.6f &&
              latest.nativeWeaponId[0] == press.nativeWeaponId[0] &&
              latest.trackingGeneration == press.trackingGeneration,
          "released taps carry each original grip, weapon ID and tracking generation");
    check(window.grant({11, 22, 2}) && stream.receive(latest, 1070, ack),
          "buffered taps enter the reliable stream only after prior consumption");
    taps.consume(pulse); // Native send success, exactly as in submit.
    auto unchanged = latest;
    check(!taps.apply(unchanged, 1071) && !unchanged.pulseMask,
          "successful native submission consumes both local intents exactly once");
    check(stream.freeze(1080, sample) && sample.fireMask == 3 && !sample.physicalDownMask,
          "delayed released taps fire for their consumption interval");
    check(stream.finishInterval(ack) && window.acknowledge(ack, 11, 22),
          "tap credit retires only at completed simulation boundary");
    check(stream.freeze(1090, sample) && !sample.fireMask && !stream.finishInterval(ack),
          "released tap cannot become held or produce another consumption ACK");
    latest = validPose(3);
    latest.physicalDownMask = latest.fireMask = 1;
    latest.primaryNeutralSampleMask = uint8_t(HandMask & ~latest.physicalDownMask & latest.primarySampleEligibleMask);
    latest.releasedSerial[0] = 2;
    check(window.grant({11, 22, 3}) && stream.receive(latest, 1100, ack) && stream.freeze(1110, sample) &&
              sample.fireMask == 1,
          "authorized held level enters the native cadence interval");
    check(stream.finishInterval(ack) && window.acknowledge(ack, 11, 22) && stream.freeze(1120, sample) &&
              sample.fireMask == 1,
          "fresh held level remains eligible across native intervals");
    latest = validPose(4);
    latest.releasedSerial[0] = 3;
    check(window.grant({11, 22, 4}) && stream.receive(latest, 1130, ack) && stream.freeze(1140, sample) &&
              !sample.fireMask,
          "next ordered physical release stops held native firing");
    check(stream.finishInterval(ack) && window.acknowledge(ack, 11, 22),
          "release consumption returns its exact reliable credit");

    // Codec -> retained taps -> native interval -> relay -> exact consumption.
    // The two presses deliberately come from different physical head positions.
    OrderedPosePolicy movingRig;
    ConsumptionCredits movingCredit;
    FixturePendingIntents movingTaps;
    auto neutralRig = validPose(1);
    neutralRig.head = neutralRig.grip[0] = neutralRig.grip[1] = pose(0, 0, 0);
    neutralRig.nativeWeaponId[1] = 2;
    neutralRig.releasedSerial[0] = neutralRig.releasedSerial[1] = 1;
    check(movingRig.validation.bindCapability(11, 22) && movingCredit.grant({11, 22, 1}),
          "Moving rig binds the original capability and one consumption credit");
    wire = encodePose(neutralRig);
    check(parse(wire, decoded) == ParseResult::Valid && movingRig.receive(decoded.pose, 2000, ack) &&
              movingRig.freeze(2001, sample) && movingRig.finishInterval(ack) &&
              movingCredit.acknowledge(ack, 11, 22),
          "Encoded neutral snapshot arms both hands through the production receiver");
    movingTaps.observeSnapshot(neutralRig);
    auto leftPress = neutralRig;
    leftPress.grip[0] = pose(HandReachLimit, 0, 0);
    leftPress.physicalDownMask = leftPress.fireMask = 1;
    leftPress.primaryNeutralSampleMask = uint8_t(HandMask & ~leftPress.physicalDownMask & leftPress.primarySampleEligibleMask);
    check(movingTaps.retain(leftPress, 1, 2010), "Boundary left tap keeps its original grip");
    auto rightPress = neutralRig;
    rightPress.head.p.x = .1f;
    rightPress.grip[1].p.x = .1f - HandReachLimit;
    rightPress.physicalDownMask = rightPress.fireMask = 2;
    rightPress.primaryNeutralSampleMask = uint8_t(HandMask & ~rightPress.physicalDownMask & rightPress.primarySampleEligibleMask);
    check(movingTaps.retain(rightPress, 2, 2011), "Boundary right tap uses its own later head position");
    auto movingRelease = neutralRig;
    movingRelease.sequence = 2;
    movingRelease.head.p.x = -.1f;
    movingRelease.grip[0].p = movingRelease.grip[1].p = movingRelease.head.p;
    movingRelease.releasedSerial[0] = movingRelease.releasedSerial[1] = 2;
    check(movingTaps.apply(movingRelease, 2020) == 3 &&
              !handTranslationValid(movingRelease.head.p, movingRelease.grip[0].p),
          "Latest head and independently retained grips reproduce the former codec failure");
    wire = encodePose(movingRelease);
    check(parse(wire, decoded) == ParseResult::Valid && decoded.pose.grip[0].p.x == leftPress.grip[0].p.x &&
              decoded.pose.grip[1].p.x == rightPress.grip[1].p.x && movingCredit.grant({11, 22, 2}) &&
              movingRig.receive(decoded.pose, 2030, ack),
          "Head motion cannot suppress a retained tap or the other hand's outgoing update");
    movingTaps.consume(3);
    PosePacket frozenRelay;
    check(movingRig.freeze(2031, sample, &frozenRelay) && sample.fireMask == 3 &&
              frozenRelay.fireMask == 0 && frozenRelay.pulseMask == 3 &&
              frozenRelay.physicalDownMask == 0 && movingRig.active.pulseMask == 0,
          "Frozen observer relay retains both accepted taps without fabricating held fire");
    relayMessage.relay = {frozenRelay, 99, 7};
    check(encode(relayMessage, wire) && parse(wire, decoded) == ParseResult::Valid &&
              decoded.relay.pose.pulseMask == 3 && !decoded.relay.pose.fireMask &&
              decoded.relay.pose.grip[0].p.x == leftPress.grip[0].p.x &&
              decoded.relay.pose.grip[1].p.x == rightPress.grip[1].p.x,
          "Accepted released taps survive observer relay encoding with their historical grips");
    check(movingRig.freeze(2032, sample, &frozenRelay) && !sample.pulseMask &&
              !sample.fireMask && !frozenRelay.pulseMask && !frozenRelay.fireMask,
          "Next freeze cannot re-emit an already consumed observer pulse");
    check(movingRig.finishInterval(ack) && movingCredit.acknowledge(ack, 11, 22) &&
              !movingCredit.acknowledge(ack, 11, 22),
          "Retained rig taps return their exact consumption credit only once");
    // A retained pulse can coexist with a new physical hold that has no live
    // fire admission. The pulse must not become persistent ordinary relay fire.
    for (uint8_t ordinaryFire : {uint8_t(0), uint8_t(1)}) {
        OrderedPosePolicy relayRig;
        auto baseline = validPose(1);
        check(relayRig.validation.bindCapability(11, 22) &&
                  relayRig.receive(baseline, 3000, ack) && relayRig.freeze(3000, sample) &&
                  relayRig.finishInterval(ack), "Relay mixed-pulse baseline admits neutral");
        auto mixed = baseline;
        mixed.sequence = 2;
        mixed.physicalDownMask = mixed.pulseMask = 1;
        mixed.primaryNeutralSampleMask = 2;
        mixed.fireMask = ordinaryFire;
        check(relayRig.receive(mixed, 3010, ack) &&
                  relayRig.freeze(3010, sample, &frozenRelay) && sample.fireMask == 1 &&
                  frozenRelay.fireMask == ordinaryFire && frozenRelay.physicalDownMask == 1 &&
                  frozenRelay.pulseMask == 1,
              "Relay preserves ordinary admission independently of same-hand historical pulse");
        relayMessage.relay = {frozenRelay, 99, 7};
        check(encode(relayMessage, wire) && parse(wire, decoded) == ParseResult::Valid &&
                  decoded.relay.pose.fireMask == ordinaryFire && decoded.relay.pose.pulseMask == 1,
              "Mixed pulse relay survives real wire codec without inventing held fire");
        check(!relayRig.freeze(3010 + MaxPoseAgeMs + 1, sample, &frozenRelay) &&
                  !frozenRelay.sequence && !frozenRelay.fireMask && !frozenRelay.pulseMask,
              "Expired interval emits no relay snapshot or pulse");
    }
    for (unsigned scenario = 0; scenario != 3; ++scenario) {
        OrderedPosePolicy relayRig;
        auto baseline = validPose(1);
        baseline.nativeWeaponId[0] = 13;
        check(relayRig.validation.bindCapability(11, 22) &&
                  relayRig.receive(baseline, 4000, ack) && relayRig.freeze(4000, sample) &&
                  relayRig.finishInterval(ack), "Historical zoom relay has genuine neutral baseline");
        auto historical = baseline;
        historical.sequence = 2;
        historical.pulseMask = historical.pulseZoomMask = 1;
        check(relayRig.receive(historical, 4010, ack), "Historical zoom pulse reaches pending slot");
        if (scenario == 1)
            relayRig.validation.invalidateWeaponGeneration(2, 1);
        const uint64_t freezeTime = scenario == 2 ? 4010 + MaxPoseAgeMs + 1 : 4010;
        const bool frozen = relayRig.freeze(freezeTime, sample, &frozenRelay);
        if (scenario == 0) {
            relayMessage.relay = {frozenRelay, 99, 7};
            check(frozen && !frozenRelay.fireMask && frozenRelay.pulseMask == 1 &&
                      frozenRelay.pulseZoomMask == 1 && !frozenRelay.zoomMask &&
                      encode(relayMessage, wire) && parse(wire, decoded) == ParseResult::Valid &&
                      decoded.relay.pose.pulseZoomMask == 1,
                  "Released historical zoom remains a pulse context through frozen relay codec");
        } else if (scenario == 1) {
            check(frozen && !frozenRelay.fireMask && !frozenRelay.pulseMask &&
                      !frozenRelay.pulseZoomMask && !frozenRelay.zoomMask,
                  "Revocation between receive and freeze cannot revive observer pulse or zoom");
        } else {
            check(!frozen && !frozenRelay.sequence && !frozenRelay.fireMask &&
                      !frozenRelay.pulseMask && !frozenRelay.pulseZoomMask,
                  "Expiry before first pulse freeze emits an empty observer snapshot");
        }
    }
    movingRelease.sequence = 3;
    movingRelease.grip[0].p = movingRelease.grip[1].p = movingRelease.head.p;
    movingRelease.releasedSerial[0] = movingRelease.releasedSerial[1] = 3;
    check(!movingTaps.apply(movingRelease, 2040), "Successful send does not replay retained rig taps");
    wire = encodePose(movingRelease);
    check(parse(wire, decoded) == ParseResult::Valid && movingCredit.grant({11, 22, 3}) &&
              movingRig.receive(decoded.pose, 2041, ack) && movingRig.freeze(2042, sample) &&
              !sample.fireMask && movingRig.finishInterval(ack) && movingCredit.acknowledge(ack, 11, 22),
          "The next encoded neutral update flows normally after historical tap consumption");

    // Recovery retains old aggregate debt while requiring new physical neutral.
    check(window.grant({11, 22, 5}), "old-capability pose remains in native transport");
    taps.requireNeutral();
    check(!taps.retain(press, 1, 1200), "recovery cannot retain an already held trigger");
    check(window.grant({31, 0, 0}) && !window.grant({31, 0, 0}),
          "recovery allows only one Hello for the new nonce");
    check(stream.validation.bindCapability(31, 32), "server rotates capability on recovery Hello");
    check(window.acknowledge({31, 32, 0}, 31, 0) && !window.acknowledge({31, 32, 0}, 31, 32),
          "initial server nonce requires a matching Hello credit; duplicate cannot refresh");
    auto old = validPose(5);
    check(!stream.receive(old, 1210, ack) && ack.clientNonce == 11 && ack.acceptedSequence == 5 &&
              !window.acknowledge(ack, 31, 32) && window.count() == 0,
          "old queued pose is explicitly discarded; ACK retires old debt without reviving state");
    latest = validPose(1, 2);
    latest.clientNonce = 31;
    latest.serverNonce = 32;
    latest.physicalDownMask = latest.fireMask = 1;
    latest.primaryNeutralSampleMask = uint8_t(HandMask & ~latest.physicalDownMask & latest.primarySampleEligibleMask);
    check(stream.receive(latest, 1220, ack) && stream.freeze(1230, sample) && !sample.fireMask,
          "new capability cannot arm held input before a genuine neutral");
    check(stream.finishInterval(ack), "blocked held snapshot is still processed and ACKed");
    latest.sequence = 2;
    latest.physicalDownMask = latest.fireMask = 0;
    latest.primaryNeutralSampleMask = uint8_t(HandMask & ~latest.physicalDownMask & latest.primarySampleEligibleMask);
    latest.releasedSerial[0] = 4;
    taps.observeSnapshot(latest);
    check(stream.receive(latest, 1240, ack) && stream.freeze(1250, sample) &&
              stream.validation.physicalFireAllowed(0) && stream.finishInterval(ack),
          "new-generation real neutral arms only after processing");

    FixturePendingIntents cancellation;
    check(cancellation.retain(press, 1, 1300) && cancellation.retain(otherPress, 2, 1301),
          "cancellation scenario retains original taps for both hands");
    auto changed = validPose();
    changed.nativeWeaponId[1] = 3;
    check(cancellation.apply(changed, 1310) == 1 && changed.grip[0].p.x == .65f &&
              changed.nativeWeaponId[1] == 3 && changed.grip[1].p.x == -.4f,
          "right weapon change cancels right intent while preserving left tap and right equipment");
    cancellation = {};
    check(cancellation.retain(press, 1, 1311), "epoch trace retains a primary pulse");
    changed = press;
    ++changed.intentEpoch[0];
    check(!cancellation.apply(changed, 1312), "pending intent cannot cross an epoch replacement");
    changed.trackingGeneration = 2;
    check(!cancellation.apply(changed, 1320), "new tracking generation cancels old immutable taps");
    check(!cancellation.retain(press, 1, 1321), "old held generation cannot rearm canceled tap");
    cancellation = {};
    check(cancellation.retain(press, 1, 1400), "expiry scenario retains a tap");
    latest = validPose();
    check(!cancellation.apply(latest, 1600), "expired tap cannot enter a newly available credit");

    // A full server slot, replay, rate discard, or stale capability returns the
    // submitted token, and delayed stale arrival never gains new freshness.
    OrderedPosePolicy blocked;
    blocked.validation.bindCapability(11, 22);
    auto first = validPose(1);
    check(blocked.receive(first, 2000, ack), "server stores one immutable pending snapshot");
    auto second = validPose(2);
    check(!blocked.receive(second, 2001, ack) && ack.acceptedSequence == 2 && blocked.pending.sequence == 1 &&
              blocked.validation.lastSequence == 1,
          "full server slot discards exact new token without changing pending validation");
    check(!blocked.freeze(2201, sample) && blocked.finishInterval(ack) && ack.acceptedSequence == 1,
          "expired queued snapshot is discarded at native boundary without refreshing its age");
    check(!blocked.receive(second, 2202, ack, false) && ack.acceptedSequence == 2,
          "rate-rejected snapshot explicitly returns its own token");
    check(window.grant({31, 32, 3}) && !window.acknowledge({31, 32, 3}, 31, 32, false) && !window.count(),
          "late exact ACK retires credit but cannot revive an expired capability lease");
    check(!window.acknowledge({31, 32, 99}, 31, 32),
          "unexpected current-nonce ACK cannot refresh capability");

    OrderedPosePolicy delayedReplace;
    check(delayedReplace.validation.bindCapability(11, 22), "replacement trace binds its capability");
    auto replaceNeutral = validPose(1);
    replaceNeutral.releasedSerial[0] = replaceNeutral.zoomReleasedSerial[0] = 1;
    check(delayedReplace.receive(replaceNeutral, 2250, ack) && delayedReplace.freeze(2251, sample) &&
              delayedReplace.finishInterval(ack),
          "replacement trace first establishes real release witnesses");
    auto delayedOld = validPose(2);
    delayedOld.physicalDownMask = delayedOld.fireMask = delayedOld.zoomPhysicalDownMask = delayedOld.zoomMask = 1;
    delayedOld.primaryNeutralSampleMask = uint8_t(HandMask & ~delayedOld.physicalDownMask & delayedOld.primarySampleEligibleMask);
    delayedOld.nativeWeaponId[0] = 13;
    check(delayedReplace.receive(delayedOld, 2252, ack), "old-epoch held packet enters the receive slot");
    delayedReplace.validation.invalidateWeaponGeneration(77, 1);
    check(delayedReplace.freeze(2253, sample) && !sample.fireMask && !sample.zoomMask &&
              delayedReplace.finishInterval(ack) && ack.intentEpoch[0] == 2,
          "receive/freeze recheck drops a replaced hand while preserving its exact consumption token");
    auto replacementNeutral = validPose(3);
    replacementNeutral.intentEpoch[0] = 2;
    replacementNeutral.releasedSerial[0] = replacementNeutral.zoomReleasedSerial[0] = 2;
    check(delayedReplace.receive(replacementNeutral, 2254, ack) && delayedReplace.freeze(2255, sample) &&
              delayedReplace.finishInterval(ack),
          "post-replacement active raw-up samples re-arm the new epoch");
    auto replacementHeld = replacementNeutral;
    replacementHeld.sequence = 4;
    replacementHeld.physicalDownMask = replacementHeld.fireMask = replacementHeld.zoomPhysicalDownMask =
        replacementHeld.zoomMask = 1;
    replacementHeld.primaryNeutralSampleMask = uint8_t(HandMask & ~replacementHeld.physicalDownMask & replacementHeld.primarySampleEligibleMask);
    replacementHeld.nativeWeaponId[0] = 13;
    check(delayedReplace.receive(replacementHeld, 2256, ack) && delayedReplace.freeze(2257, sample) &&
              sample.fireMask == 1 && sample.zoomMask == 1,
          "new-epoch held intent follows its own neutral rather than the delayed old packet");
    OrderedPosePolicy firstRetained;
    firstRetained.validation.bindCapability(11, 22);
    auto firstRetainedPose = validPose(1);
    firstRetainedPose.nativeWeaponId[0] = 13;
    firstRetainedPose.pulseMask = firstRetainedPose.pulseZoomMask = 1;
    check(firstRetained.receive(firstRetainedPose, 2270, ack) && firstRetained.freeze(2271, sample) &&
              sample.fireMask == 1 && sample.pulseZoomMask == 1 && intervalZoomMask(sample) == 1,
          "The first post-ACK real neutral may admit a retained released shot without losing its zoom");
    auto consumedZoomShot = sample;
    firstRetained.validation.zoomReleaseObserved[0] = false;
    firstRetained.validation.filterWeaponIntents(consumedZoomShot);
    check(!consumedZoomShot.fireMask && !consumedZoomShot.pulseMask && !consumedZoomShot.pulseZoomMask,
          "Use-boundary zoom revocation cannot downgrade an already composed historical shot");
    // Drive the real raw producer, hysteresis gate, ACK timestamp boundary,
    // retained intent, and native interval policy together. Clear/down latches
    // in the dead band must not launder a cumulative pre-boundary release.
    for (unsigned hand = 0; hand < 2; ++hand) {
        const uint8_t bit = uint8_t(1u << hand), other = uint8_t(HandMask ^ bit);
        for (bool streamChange : {false, true}) {
            OrderedPosePolicy causal;
            check(causal.validation.bindCapability(11, 22), "Causal producer trace binds native capability");
            auto initial = validPose(1);
            check(causal.receive(initial, 3000, ack) && causal.freeze(3001, sample) && causal.finishInterval(ack),
                  "Both raw primary streams begin admitted from actual neutral");
            FixturePendingIntents retained;
            retained.observeSnapshot(initial);
            TriggerGate nativeGate;
            bool physical = false;
            uint32_t serial = 1;
            nativeGate.update(0, true);
            for (int n = 0; n < 6; ++n)
                check(samplePrimaryNeutral(0, true, true, physical, serial), "Pre-boundary release samples accumulate");
            IntentInputBoundary boundary;
            check(boundary.install(1, 10, 2990), "Initial local ACK boundary installs");
            if (!streamChange) {
                causal.validation.invalidateWeaponGeneration(12, bit);
                check(boundary.install(2, 10, 3002), "Replacement ACK installs the affected hand's new epoch");
                retained.requireNeutral(bit);
            }
            uint32_t carrierSequence = 1;
            uint64_t inputSequence = 10, tick = 3002;
            auto produce = [&](float trigger) {
                auto result = validPose(++carrierSequence);
                ++inputSequence;
                ++tick;
                result.intentEpoch[hand] = boundary.echo(inputSequence, tick, tick);
                result.primaryInputGeneration[hand] = streamChange ? 2 : 1;
                const bool neutralSample = samplePrimaryNeutral(trigger, true, true, physical, serial);
                result.primaryNeutralSampleMask &= uint8_t(~bit);
                if (neutralSample) result.primaryNeutralSampleMask |= bit;
                result.releasedSerial[hand] = serial;
                if (physical) result.physicalDownMask |= bit;
                if (nativeGate.update(trigger, true)) result.fireMask |= bit;
                return result;
            };
            auto deadBand = produce(.4f);
            retained.observeSnapshot(deadBand);
            check(!deadBand.physicalDownMask && !(deadBand.primaryNeutralSampleMask & bit) &&
                      causal.receive(deadBand, tick, ack) && causal.freeze(tick, sample) &&
                      !causal.validation.physicalFireAllowed(hand) &&
                      causal.validation.physicalFireAllowed(1 - hand) && causal.finishInterval(ack),
                  "Post-boundary dead-band input cannot reuse a pre-boundary release; other hand remains admitted");
            auto stillHeld = produce(.8f);
            check((stillHeld.fireMask & bit) && !retained.retain(stillHeld, bit, tick),
                  "An armed native gate cannot retain a shot without a new explicit raw neutral");
            auto clientHeld = stillHeld;
            retained.filterPrimaryIntents(clientHeld);
            check(!(clientHeld.fireMask & bit) &&
                      !retained.currentPrimaryIntent(clientHeld, hand, clientHeld.intentEpoch[hand],
                                                     clientHeld.primaryInputGeneration[hand]),
                  "The actual submit/use helpers suppress client held intent as well as retained pulses");
            check(causal.receive(stillHeld, tick, ack) && causal.freeze(tick, sample) &&
                      !(sample.fireMask & bit) && !(sample.pulseMask & bit) && causal.finishInterval(ack),
                  "The server rejects the .4 to .8 trace after either an epoch or logical stream boundary");
            auto newNeutral = produce(.1f);
            retained.observeSnapshot(newNeutral);
            check(causal.receive(newNeutral, tick, ack) && causal.freeze(tick, sample) &&
                      causal.validation.physicalFireAllowed(hand) && causal.finishInterval(ack),
                  "A genuinely new below-threshold sample restores only the affected admission");
            auto newPress = produce(.8f);
            check(retained.retain(newPress, bit, tick) && retained.apply(newPress, tick) == bit &&
                      causal.receive(newPress, tick, ack) && causal.freeze(tick, sample) &&
                      (sample.fireMask & bit) && (sample.pulseMask & bit) && causal.finishInterval(ack),
                  "New raw neutral followed by press restores native held and retained shot behavior");
            retained.filterPrimaryIntents(newPress);
            check(retained.currentPrimaryIntent(newPress, hand, newPress.intentEpoch[hand],
                                                newPress.primaryInputGeneration[hand]),
                  "Client native use accepts its exact admitted held intent");
            auto released = produce(.1f);
            retained.observeSnapshot(released);
            check(!retained.currentPrimaryIntent(released, hand, released.intentEpoch[hand],
                                                 released.primaryInputGeneration[hand]) &&
                      retained.currentPrimaryIntent(released, hand, released.intentEpoch[hand],
                                                    released.primaryInputGeneration[hand], false) &&
                      nativeCommandQuery(0, 1, 2, true) == 1,
                  "An admitted ordinary physical release remains a native release edge without active fire");
            auto cachedHeld = newPress;
            retained.requireNeutral(bit); // ACK arrives AFTER native snapshot/command publication.
            check(!retained.currentPrimaryIntent(cachedHeld, hand, cachedHeld.intentEpoch[hand],
                                                 cachedHeld.primaryInputGeneration[hand]),
                  "An intervening ACK immediately revokes a cached native fire intent before consumption");
            check(!retained.currentPrimaryIntent(released, hand, released.intentEpoch[hand],
                                                  released.primaryInputGeneration[hand], false),
                  "ACK revocation also rejects a stale cached release context");
            check((newPress.primaryNeutralSampleMask & other) == other,
                  "Current neutral witness remains independently attributed to the opposite hand");
        }
    }
    FixturePendingIntents cachedNeutral;
    cachedNeutral.requireNeutral(1);
    auto cachedZero = validPose();
    cachedZero.intentEpoch[0] = 0;
    cachedNeutral.observeSnapshot(cachedZero);
    auto heldAfterZero = validPose(2);
    heldAfterZero.intentEpoch[0] = 2;
    heldAfterZero.physicalDownMask = heldAfterZero.fireMask = 1;
    heldAfterZero.primaryNeutralSampleMask = 2;
    check(!cachedNeutral.retain(heldAfterZero, 1, 3100),
          "An epoch-zero cached neutral cannot arm retained input in a new epoch");
    auto staleCopied = validPose(), staleReturned = staleCopied;
    PeerState liveAdmission;
    liveAdmission.bindCapability(11, 22);
    liveAdmission.invalidateWeaponGeneration(1, 1);
    liveAdmission.filterWeaponIntents(staleReturned);
    check(staleCopied.intentEpoch[0] == staleReturned.intentEpoch[0] &&
              !currentIntentSample(staleCopied, staleReturned, 0, liveAdmission.intentEpoch[0]) &&
              currentIntentSample(staleCopied, staleReturned, 1, liveAdmission.intentEpoch[1]),
          "Matching stale muzzle echoes cannot match live admission; unaffected tracking remains valid");
    staleCopied.intentEpoch[0] = staleReturned.intentEpoch[0] = 2;
    check(currentIntentSample(staleCopied, staleReturned, 0, liveAdmission.intentEpoch[0]) &&
              !staleCopied.fireMask && !staleReturned.fireMask,
          "Current-epoch tracked muzzle input is eligible independently of primary fire");
    ++staleReturned.sequence;
    check(!currentIntentSample(staleCopied, staleReturned, 0, liveAdmission.intentEpoch[0]),
          "A native callback cannot replace the exact captured authority sample");
    PeerState retiredStream;
    for (unsigned hand = 0; hand < 2; ++hand) {
        const uint8_t bit = uint8_t(1u << hand);
        PeerState riderBoundary;
        riderBoundary.bindCapability(11, 22);
        auto request = validPose();
        request.requestedWeapon[0] = 12;
        request.requestedWeapon[1] = 13;
        auto copied = request, pending = request, active = request;
        riderBoundary.invalidateWeaponGeneration(2, bit);
        cancelWeaponRequests(copied, bit);
        cancelWeaponRequests(pending, bit);
        cancelWeaponRequests(active, bit);
        check(copied.requestedWeapon[hand] == -1 && pending.requestedWeapon[hand] == -1 &&
                  active.requestedWeapon[hand] == -1 && copied.requestedWeapon[1 - hand] >= 0,
              "Rider/equipment boundaries cancel copied and retained equip requests per hand");
        riderBoundary.filterWeaponIntents(request);
        check(request.requestedWeapon[hand] == -1 && request.requestedWeapon[1 - hand] >= 0 &&
                  !currentIntentSample(copied, request, hand, riderBoundary.intentEpoch[hand]),
              "A fresh delayed old-epoch equip request cannot execute after dismount");
        request.intentEpoch[hand] = riderBoundary.intentEpoch[hand];
        request.requestedWeapon[hand] = 16;
        copied = request;
        riderBoundary.filterWeaponIntents(request);
        check(request.requestedWeapon[hand] == 16 &&
                  currentIntentSample(copied, request, hand, riderBoundary.intentEpoch[hand]),
              "A genuinely current-epoch weapon selection remains available without a firing requirement");
    }
    retiredStream.bindCapability(11, 22);
    auto streamNeutral = validPose(1);
    streamNeutral.primaryInputGeneration[0] = streamNeutral.zoomInputGeneration[0] = 2;
    streamNeutral.releasedSerial[0] = streamNeutral.zoomReleasedSerial[0] = 2;
    check(retiredStream.acceptPose(streamNeutral, 2280), "Current logical streams establish their own real neutral");
    auto oldStream = validPose(2);
    oldStream.physicalDownMask = oldStream.fireMask = oldStream.zoomPhysicalDownMask = oldStream.zoomMask = 1;
    oldStream.primaryNeutralSampleMask = uint8_t(HandMask & ~oldStream.physicalDownMask & oldStream.primarySampleEligibleMask);
    oldStream.nativeWeaponId[0] = 13;
    oldStream.releasedSerial[0] = oldStream.zoomReleasedSerial[0] = 99;
    check(retiredStream.acceptPose(oldStream, 2281) && !oldStream.fireMask && !oldStream.zoomMask &&
              retiredStream.observedPrimaryInputGeneration[0] == 2 && retiredStream.observedReleasedSerial[0] == 2 &&
              retiredStream.observedZoomInputGeneration[0] == 2 && retiredStream.observedZoomReleasedSerial[0] == 2,
          "A retired stream with a newer carrier sequence cannot revive itself or poison current release baselines");
    FixturePendingIntents aggregateTap;
    ConsumptionCredits debt;
    check(debt.grant({41, 42, 1}) && debt.grant({43, 44, 1}) && debt.grant({45, 46, 1}) &&
              debt.grant({47, 48, 1}) && debt.full() && aggregateTap.retain(press, 1, 2300),
          "tap remains retained with all four aggregate recovery credits occupied");
    auto freed = validPose();
    freed.clientNonce = 47;
    freed.serverNonce = 48;
    check(!debt.acknowledge({41, 42, 1}, 47, 48) && !debt.full() && !debt.grant({47, 48, 2}) &&
              aggregateTap.apply(freed, 2310) == 1,
          "old ACK frees aggregate debt while current capability remains outstanding and tap immutable");
    check(debt.acknowledge({47, 48, 1}, 47, 48) && debt.grant({47, 48, 2}) && freed.grip[0].p.x == .65f,
          "retained original tap can send only after its current-capability consumption ACK");
    aggregateTap.consume(1);
    check(!aggregateTap.apply(freed, 2311), "aggregate-bound tap is consumed only once");
    auto wheelTap = validPose();
    check(aggregateTap.retain(press, 1, 2400) && aggregateTap.retain(otherPress, 2, 2401),
          "wheel scenario begins with both immutable taps");
    wheelTap.wheelOrEquipBlockedMask = 2;
    check(aggregateTap.apply(wheelTap, 2410) == 1 && wheelTap.grip[0].p.x == .65f,
          "right wheel cancels its own pending tap without moving left grip");
    wheelTap.wheelOrEquipBlockedMask = 0;
    wheelTap.physicalDownMask = wheelTap.fireMask = 2;
    wheelTap.primaryNeutralSampleMask = uint8_t(HandMask & ~wheelTap.physicalDownMask & wheelTap.primarySampleEligibleMask);
    check(aggregateTap.apply(wheelTap, 2420) == 1 && !aggregateTap.retain(wheelTap, 2, 2420),
          "closing right wheel while held cannot rearm or replay canceled right tap");
    OrderedPosePolicy replaced;
    replaced.validation.bindCapability(11, 22);
    check(replaced.receive(first, 2500, ack) && replaced.freeze(2510, sample) &&
              replaced.finishInterval(ack) && ack.clientNonce == 11 && ack.serverNonce == 22,
          "HELLO replacing a staged interval explicitly discards the original immutable token");
    replaced = OrderedPosePolicy{};
    replaced.validation.bindCapability(31, 32);
    check(!replaced.finishInterval(ack), "new HELLO peer has no old consumption token to acknowledge");
    check(replaced.receive(latest, 2520, ack) == false,
          "old-capability snapshot remains rejected after staged peer replacement");
    replaced = OrderedPosePolicy{};
    replaced.validation.bindCapability(11, 22);
    check(replaced.receive(first, 2530, ack) && replaced.discardPending(ack) && ack.clientNonce == 11 &&
              ack.acceptedSequence == 1 && !replaced.discardPending(ack),
          "HELLO replacing an unprepared packet discards its exact token once");
    // Transport ownership is distinct from whether the wrapper returned:
    // pre-call failure can revoke, but an interrupted native call is ambiguous.
    for (const auto phase : {TransportPhase::BeforeCall, TransportPhase::EnteredCall,
                             TransportPhase::ReturnedCall}) {
        ConsumptionCredits transport;
        const ConsumptionToken selected{11,22,11}, unrelated{31,32,12};
        check(transport.grant(selected) && transport.grant(unrelated), "handoff credits granted");
        revokeUnsubmitted(transport, selected, phase);
        check(transport.count() == (phase == TransportPhase::BeforeCall ? 1u : 2u) &&
                  transport.hasOutstandingCapability(31,32),
              "only a definitely unsubmitted exact token is revoked");
        if (phase != TransportPhase::BeforeCall)
            check(transport.acknowledge({11,22,11},11,22),
                  "ambiguous/returned submission retains its exact server ACK ownership");
    }
    // An ACK is observed before handoff, without losing its existing slot.
    OrderedPosePolicy unsentAck;
    check(unsentAck.validation.bindCapability(11,22), "unsent ACK stream binds");
    auto unsentPose = validPose(1);
    check(unsentAck.receive(unsentPose,2550,ack) && unsentAck.freeze(2551,sample) &&
              unsentAck.peekInterval(ack) && unsentAck.awaitingConsumption,
          "peeking an outgoing ACK retains the actual receive-slot ownership");
    const Ack selectedAck = ack;
    unsentAck.abortInterval();
    check(unsentAck.peekInterval(ack) && ack.acceptedSequence == selectedAck.acceptedSequence &&
              !unsentAck.hasActive && unsentAck.awaitingConsumption,
          "a failed ACK send leaves a discard token in the same inactive slot");
    check(!unsentAck.confirmInterval({11,22,2}) &&
              !unsentAck.confirmInterval({31,32,1}) && unsentAck.awaitingConsumption,
          "wrong sequence or replacement capability cannot confirm another ACK");
    check(unsentAck.confirmInterval(selectedAck) && !unsentAck.confirmInterval(selectedAck),
          "confirmed handoff retires the exact slot once, without replay");
    OrderedPosePolicy deletedPending;
    check(deletedPending.validation.bindCapability(11,22) &&
              deletedPending.receive(unsentPose,2552,ack) &&
              deletedPending.hasPending && !deletedPending.awaitingConsumption &&
              deletedPending.freeze(2553,sample), "lifecycle pending occupies only one original slot");
    deletedPending.abortInterval();
    check(!deletedPending.hasPending && deletedPending.peekInterval(ack) &&
              ack.acceptedSequence == 1 && !deletedPending.hasActive &&
              deletedPending.confirmInterval(ack),
          "lifecycle discard promotes pending into the original active slot without gameplay");

    // Interrupted native work never retries a partially executed shot. The same
    // immutable credit token is eventually discarded; no extra queue is used.
    OrderedPosePolicy interrupted;
    ConsumptionCredits interruptedCredit;
    check(interrupted.validation.bindCapability(11, 22), "abort stream binds");
    auto beforeAbort = validPose(1);
    beforeAbort.nativeWeaponId[0] = beforeAbort.nativeWeaponId[1] = 13;
    check(interrupted.receive(beforeAbort, 2600, ack) &&
              interrupted.freeze(2601, sample) && interrupted.finishInterval(ack),
          "abort stream first arms native weapons");
    auto firingBeforeAbort = beforeAbort;
    firingBeforeAbort.sequence = 2;
    firingBeforeAbort.physicalDownMask = firingBeforeAbort.fireMask = 3;
    firingBeforeAbort.primaryNeutralSampleMask = 0;
    firingBeforeAbort.zoomPhysicalDownMask = firingBeforeAbort.zoomMask = 3;
    firingBeforeAbort.pulseMask = firingBeforeAbort.pulseZoomMask = 3;
    check(interruptedCredit.grant({11, 22, 2}) &&
              interrupted.receive(firingBeforeAbort, 2602, ack) &&
              interrupted.freeze(2603, sample) && sample.fireMask == 3 &&
              intervalZoomMask(sample) == 3, "abort begins with real admitted shot/zoom");
    interrupted.abortInterval();
    check(!interrupted.hasActive && interrupted.awaitingConsumption &&
              !interrupted.validation.fresh(2604) &&
              !interrupted.freeze(2604, sample) && !sample.fireMask &&
              !sample.pulseMask && !sample.zoomMask && !sample.pulseZoomMask &&
              interruptedCredit.count() == 1,
          "abort blocks replay while retaining the exact outstanding discard credit");
    auto delayedNeutral = beforeAbort;
    delayedNeutral.sequence = 3;
    delayedNeutral.releasedSerial[0] = delayedNeutral.releasedSerial[1] = 2;
    delayedNeutral.zoomReleasedSerial[0] = delayedNeutral.zoomReleasedSerial[1] = 2;
    check(!interrupted.receive(delayedNeutral, 2605, ack),
          "an outstanding interrupted token still bounds the receive slot");
    check(interrupted.finishInterval(ack) && ack.acceptedSequence == 2 &&
              ack.intentEpoch[0] == 2 && ack.intentEpoch[1] == 2 &&
              interruptedCredit.acknowledge(ack, 11, 22) &&
              !interrupted.finishInterval(ack),
          "normal boundary discards exactly once using current rearm epochs");
    check(interrupted.receive(delayedNeutral, 2606, ack) &&
              interrupted.freeze(2607, sample) &&
              !interrupted.validation.physicalFireAllowed(0) &&
              !interrupted.validation.zoomAllowed(0) && interrupted.finishInterval(ack),
          "delayed pre-interruption neutral cannot rearm at an old epoch");
    auto afterAbort = delayedNeutral;
    afterAbort.sequence = 4;
    afterAbort.intentEpoch[0] = afterAbort.intentEpoch[1] = 2;
    check(interrupted.receive(afterAbort, 2608, ack) &&
              interrupted.freeze(2609, sample) && interrupted.finishInterval(ack) &&
              interrupted.validation.physicalFireAllowed(0) &&
              interrupted.validation.zoomAllowed(0),
          "a new current-epoch physical neutral permits recovery");
    auto resumedShot = firingBeforeAbort;
    resumedShot.sequence = 5;
    resumedShot.intentEpoch[0] = resumedShot.intentEpoch[1] = 2;
    check(interrupted.receive(resumedShot, 2610, ack) &&
              interrupted.freeze(2611, sample) && sample.fireMask == 3 &&
              intervalZoomMask(sample) == 3 && interrupted.finishInterval(ack),
          "ordinary native input resumes after the fresh neutral boundary");
    interrupted.abortInterval(); // An interrupted held interval has no new token.
    check(!interrupted.freeze(2612, sample) && !interrupted.finishInterval(ack),
          "aborting a repeated held interval fabricates no credit token");
    // Historical shot context must survive credit pressure in BOTH directions:
    // a zoomed tap after release, and an unzoomed tap after newly holding zoom.
    for (unsigned hand = 0; hand < 2; ++hand) {
        const uint8_t bit = uint8_t(1u << hand), other = uint8_t(3u ^ bit);
        for (bool historicallyZoomed : {false, true}) {
            FixturePendingIntents retainedZoom;
            auto shot = validPose();
            shot.nativeWeaponId[0] = shot.nativeWeaponId[1] = 13;
            shot.physicalDownMask = shot.fireMask = bit;
            shot.primaryNeutralSampleMask = uint8_t(HandMask & ~shot.physicalDownMask & shot.primarySampleEligibleMask);
            shot.zoomMask = historicallyZoomed ? bit : 0;
            shot.zoomPhysicalDownMask = shot.zoomMask;
            check(retainedZoom.retain(shot, bit, 3000), "native shot retains its zoom context");
            auto newest = shot;
            newest.sequence = 2;
            newest.physicalDownMask = newest.fireMask = 0;
            newest.primaryNeutralSampleMask = uint8_t(HandMask & ~newest.physicalDownMask & newest.primarySampleEligibleMask);
            newest.zoomMask = uint8_t(other | (historicallyZoomed ? 0 : bit));
            newest.zoomPhysicalDownMask = newest.zoomMask;
            newest.grip[hand].p.x += .1f;
            newest.releasedSerial[hand] = 2;
            check(retainedZoom.apply(newest, 3010) == bit &&
                      newest.pulseZoomMask == (historicallyZoomed ? bit : 0) &&
                      newest.grip[hand].p.x == shot.grip[hand].p.x,
                  "historical grip and zoom travel together without replacing desired zoom");
            const auto desired = newest.zoomMask;
            wire = encodePose(newest);
            check(parse(wire, decoded) == ParseResult::Valid && decoded.pose.zoomMask == desired &&
                      decoded.pose.pulseZoomMask == newest.pulseZoomMask,
                  "both zoom contexts round trip through native carrier");
            auto retry = newest;
            check(retainedZoom.apply(retry, 3011) == bit && retry.pulseZoomMask == newest.pulseZoomMask,
                  "failed native send retains identical historical zoom context");
            OrderedPosePolicy zoomServer;
            zoomServer.validation.bindCapability(11, 22);
            auto armed = newest;
            armed.sequence = 1;
            armed.physicalDownMask = armed.fireMask = armed.pulseMask = armed.zoomMask =
                armed.pulseZoomMask = armed.zoomPhysicalDownMask = 0;
            armed.primaryNeutralSampleMask = uint8_t(HandMask & ~armed.physicalDownMask & armed.primarySampleEligibleMask);
            armed.releasedSerial[0] = armed.releasedSerial[1] = 1;
            armed.zoomReleasedSerial[0] = armed.zoomReleasedSerial[1] = 1;
            check(zoomServer.receive(armed, 3011, ack) && zoomServer.freeze(3011, sample) &&
                      zoomServer.finishInterval(ack),
                  "fresh active raw-up samples arm primary and zoom before retained history arrives");
            check(zoomServer.receive(decoded.pose, 3012, ack) && zoomServer.freeze(3013, sample) &&
                      sample.fireMask == bit && intervalZoomMask(sample) == uint8_t(other | newest.pulseZoomMask),
                  "authoritative pulse interval uses historical zoom even when latest level is opposite");
            check(zoomServer.finishInterval(ack) && ack.acceptedSequence == 2 &&
                      zoomServer.freeze(3014, sample) && !sample.pulseMask && !sample.pulseZoomMask &&
                      intervalZoomMask(sample) == desired,
                  "next native interval resumes latest desired zoom without retaining a tap");
            retainedZoom.consume(bit);
            check(!retainedZoom.apply(retry, 3015) && !retry.pulseZoomMask && retry.zoomMask == desired,
                  "consumed zoom context cannot replay or clear unrelated held zoom");
            retainedZoom = {};
            check(retainedZoom.retain(shot, bit, 3020), "blocked scenario retains zoom context");
            retry.wheelOrEquipBlockedMask = bit;
            retry.zoomMask &= uint8_t(~bit);
            check(!retainedZoom.apply(retry, 3021) && !retry.pulseZoomMask,
                  "wheel cancellation removes historical shot and its zoom context together");

            OrderedPosePolicy unarmedZoomServer;
            unarmedZoomServer.validation.bindCapability(11, 22);
            auto unarmed = newest;
            unarmed.releasedSerial[0] = unarmed.releasedSerial[1] = 0;
            auto zoomNeutral = armed;
            zoomNeutral.primarySampleEligibleMask = 0;
            zoomNeutral.primaryNeutralSampleMask = 0;
            check(unarmedZoomServer.receive(zoomNeutral, 3099, ack) &&
                      unarmedZoomServer.freeze(3099, sample) && unarmedZoomServer.finishInterval(ack),
                  "zoom can establish its own release admission while primary remains unarmed");
            check(unarmedZoomServer.receive(unarmed, 3100, ack) &&
                      unarmedZoomServer.freeze(3101, sample) && !sample.fireMask &&
                      !sample.pulseMask && !sample.pulseZoomMask &&
                      sample.zoomMask == desired && intervalZoomMask(sample) == desired,
                  "rejected shot cannot override either hand's current zoom intent");
            check(unarmedZoomServer.finishInterval(ack) && ack.acceptedSequence == unarmed.sequence &&
                      !unarmedZoomServer.finishInterval(ack) &&
                      unarmedZoomServer.freeze(3102, sample) && !sample.fireMask &&
                      !sample.pulseMask && !sample.pulseZoomMask && sample.zoomMask == desired,
                  "rejected shot is acknowledged once and its zoom context never replays");
        }
    }
    FixturePendingIntents oppositeHistories;
    auto twoShots = validPose();
    twoShots.nativeWeaponId[0] = twoShots.nativeWeaponId[1] = 13;
    twoShots.fireMask = twoShots.physicalDownMask = 3;
    twoShots.primaryNeutralSampleMask = uint8_t(HandMask & ~twoShots.physicalDownMask & twoShots.primarySampleEligibleMask);
    twoShots.zoomMask = 1;
    twoShots.zoomPhysicalDownMask = 1;
    check(oppositeHistories.retain(twoShots, 3, 4000), "two hands retain opposite zoom histories");
    auto twoNewest = twoShots;
    twoNewest.fireMask = twoNewest.physicalDownMask = 0;
    twoNewest.primaryNeutralSampleMask = uint8_t(HandMask & ~twoNewest.physicalDownMask & twoNewest.primarySampleEligibleMask);
    twoNewest.zoomMask = 2;
    twoNewest.zoomPhysicalDownMask = 2;
    check(oppositeHistories.apply(twoNewest, 4001) == 3 && twoNewest.pulseZoomMask == 1 &&
              intervalZoomMask(twoNewest) == 1,
          "simultaneous histories stay independent from opposite current desired levels");
    oppositeHistories.requireNeutral(1);
    check(oppositeHistories.apply(twoNewest, 4002) == 2 && !twoNewest.pulseZoomMask &&
              twoNewest.zoomMask == 2 && intervalZoomMask(twoNewest) == 0,
          "left cancellation preserves the right unzoomed shot's historical override");
    FixturePendingIntents streamHistories;
    auto streamShots = twoShots;
    streamShots.zoomMask = streamShots.zoomPhysicalDownMask = 2;
    check(streamHistories.retain(streamShots, 3, 4050),
          "primary histories capture one unzoomed and one zoomed hand independently");
    auto streamChanged = streamShots;
    streamChanged.fireMask = streamChanged.physicalDownMask = 0;
    streamChanged.primaryNeutralSampleMask = uint8_t(HandMask & ~streamChanged.physicalDownMask & streamChanged.primarySampleEligibleMask);
    streamChanged.zoomMask = streamChanged.zoomPhysicalDownMask = 0;
    ++streamChanged.zoomInputGeneration[1];
    check(streamHistories.apply(streamChanged, 4051) == 1 && !streamChanged.pulseZoomMask,
          "zoom stream replacement cancels only its zoomed history and preserves opposite unzoomed primary");
    check(!oppositeHistories.apply(twoNewest, 4200) && !twoNewest.pulseZoomMask &&
              intervalZoomMask(twoNewest) == 2,
          "expired historical context restores current desired level without a retained override");
    oppositeHistories = {};
    check(oppositeHistories.retain(twoShots, 3, 4300), "tracking-generation scenario retains zoom context");
    ++twoNewest.trackingGeneration;
    check(!oppositeHistories.apply(twoNewest, 4301) && !twoNewest.pulseZoomMask,
          "tracking-generation change cancels both historical zoom contexts");
    oppositeHistories = {};
    check(oppositeHistories.retain(twoShots, 3, 4400), "capability scenario retains zoom context");
    twoNewest.trackingGeneration = twoShots.trackingGeneration;
    oppositeHistories.requireNeutral(); // Existing capability reset's retention boundary.
    check(!oppositeHistories.apply(twoNewest, 4401) && !twoNewest.pulseZoomMask,
          "capability retirement makes prior shot zoom context inaccessible");
    FixturePendingIntents expiringZoom;
    auto zoomReleased = twoShots;
    zoomReleased.fireMask = zoomReleased.physicalDownMask = zoomReleased.zoomMask = 0;
    zoomReleased.primaryNeutralSampleMask = uint8_t(HandMask & ~zoomReleased.physicalDownMask & zoomReleased.primarySampleEligibleMask);
    check(expiringZoom.retain(twoShots, 1, 4500) && expiringZoom.apply(zoomReleased, 4501) == 1 &&
              zoomReleased.pulseZoomMask == 1,
          "expiry scenario has a live nonzero historical zoom mask");
    check(!expiringZoom.apply(zoomReleased, 4700) && !zoomReleased.pulseZoomMask &&
              !intervalZoomMask(zoomReleased),
          "nonzero historical zoom expires with its retained shot");
    for (unsigned hand = 0; hand < 2; ++hand) {
        auto zoom = validPose();
        zoom.nativeWeaponId[hand] = 13;
        zoom.zoomMask = uint8_t(1u << hand);
        zoom.zoomPhysicalDownMask = zoom.zoomMask;
        check(validatePose(zoom), "tracked sniper held zoom is valid");
        for (int failure = 0; failure < 6; ++failure) {
            auto invalidZoom = zoom;
            switch (failure) {
            case 0: invalidZoom.validMask &= uint8_t(~1u); break;
            case 1: invalidZoom.validMask &= uint8_t(~(2u << hand)); break;
            case 2: invalidZoom.wheelOrEquipBlockedMask = invalidZoom.zoomMask; break;
            case 3: invalidZoom.nativeWeaponId[hand] = 2; break;
            case 4: invalidZoom.requestedWeapon[hand] = 13; break;
            case 5: invalidZoom.zoomMask = 4; break;
            }
            check(!validatePose(invalidZoom), "invalid held zoom is rejected before native routing");
        }
        auto invalidPulse = zoom;
        invalidPulse.zoomMask = 0;
        invalidPulse.pulseZoomMask = uint8_t(1u << hand);
        check(!validatePose(invalidPulse), "historical zoom requires its matching shot pulse");
        invalidPulse.pulseMask = invalidPulse.pulseZoomMask;
        invalidPulse.nativeWeaponId[hand] = 2;
        check(!validatePose(invalidPulse), "historical zoom requires a matching native sniper type");
        auto bothHands = validPose();
        bothHands.nativeWeaponId[0] = bothHands.nativeWeaponId[1] = 13;
        bothHands.physicalDownMask = bothHands.fireMask = bothHands.pulseMask =
            bothHands.zoomMask = bothHands.pulseZoomMask = 3;
        bothHands.primaryNeutralSampleMask = uint8_t(HandMask & ~bothHands.physicalDownMask & bothHands.primarySampleEligibleMask);
        bothHands.zoomPhysicalDownMask = 3;
        invalidateWeaponIntents(bothHands, uint8_t(1u << hand));
        const auto surviving = uint8_t(3u ^ (1u << hand));
        check(bothHands.fireMask == surviving && bothHands.pulseMask == surviving &&
                  bothHands.zoomMask == surviving && bothHands.pulseZoomMask == surviving &&
                  bothHands.physicalDownMask == 3 && validatePose(bothHands),
              "native equipment invalidation retires one hand's complete context without fake release");
                  bothHands.primaryNeutralSampleMask = uint8_t(HandMask & ~bothHands.physicalDownMask & bothHands.primarySampleEligibleMask);
    }
    {
        PeerState current;
        check(current.bindCapability(11,22),"Live frozen zoom filtering binds existing capability");
        auto neutral=validPose(1);
        neutral.nativeWeaponId[0]=13;
        check(current.acceptPose(neutral,5000),"Native sniper receives actual neutral before held zoom");
        auto held=neutral;held.sequence=2;
        held.zoomMask=held.zoomPhysicalDownMask=1;
        check(current.acceptPose(held,5001) && intervalZoomMask(held)==1,
              "Captured native interval initially has admitted held zoom");
        const auto captured=held;
        auto unavailable=held;unavailable.sequence=3;
        unavailable.zoomMask=0;unavailable.zoomSampleEligibleMask&=uint8_t(~1u);
        ++unavailable.zoomInputGeneration[0];
        check(current.acceptPose(unavailable,5002),"New action loss updates live admission without weapon replacement");
        current.filterWeaponIntents(held);
        check(currentIntentSample(captured,held,0,current.intentEpoch[0]) &&
              intervalZoomMask(captured)==1 && intervalZoomMask(held)==0,
              "Same frozen sequence/epoch does not preserve zoom rejected by current action admission");
    }
    std::cout << "Portable SS2 VR network codec, zoom context and peer checks passed\n";
}
