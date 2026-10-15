#include "common/color.hpp"
#include "common/controls.hpp"
#include "common/ui.hpp"
#include "common/intent_boundary.hpp"
#include "common/rider.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace ss2vr;
static void check(bool b, const char *what) {
    if (!b) {
        std::cerr << what << '\n';
        std::exit(1);
    }
}
static bool near(float a, float b) {
    return std::abs(a - b) < 1e-4f;
}
int main() {
    {
        // Rig recentering uses the same projected -Z heading as the UI, not
        // Euler yaw: looking up/down or rolling cannot change a known heading.
        for (float heading : {-179.f, -40.f, 40.f, 179.f})
            for (float pitchDegrees : {-50.f, 50.f})
                for (float rollDegrees : {-20.f, 20.f}) {
                    const float angle=heading*Pi/180, tilt=pitchDegrees*Pi/180, bank=rollDegrees*Pi/180;
                    const Quat pitch{std::sin(tilt/2),0,0,std::cos(tilt/2)};
                    const Quat roll{0,0,std::sin(bank/2),std::cos(bank/2)};
                    const Pose head{multiply(multiply(yaw(angle),pitch),roll),{.1f,1.6f,-.2f}};
                    const Pose origin{yaw(horizontalHeading(head.q)),head.p};
                    check(near(signedAngle(horizontalHeading(origin.q)-angle),0),
                          "Rig reference retains known yaw with simultaneous head pitch and roll");
                    const Pose relative=bodyHeadTracking(origin,0,head);
                    const Vec3 forward=rotate(relative.q,{0,0,-1});
                    check(near(forward.x,0) && near(forward.y,std::sin(tilt)) && near(forward.z,-std::cos(tilt)),
                          "Recenter removes horizontal heading and retains native pitch response");
                    const auto expected=multiply(pitch,roll);
                    check(std::abs(relative.q.x*expected.x+relative.q.y*expected.y+
                                   relative.q.z*expected.z+relative.q.w*expected.w)>.99999f,
                          "Recenter preserves the full relative head rotation, including roll");
                    const Vec3 reach{.25f,-.3f,-.5f};
                    const Pose hand{head.q,head.p+rotate(head.q,reach)};
                    const Pose trackedHand=bodyHandTracking(origin,0,head,hand);
                    const Vec3 expectedReach=rotate(expected,reach);
                    check(near(trackedHand.p.x,expectedReach.x) && near(trackedHand.p.y,expectedReach.y) &&
                          near(trackedHand.p.z,expectedReach.z),
                          "The same recentered reference preserves controller-to-head geometry");
                }
        const Quat vertical{std::sin(Pi/4),0,0,std::cos(Pi/4)};
        check(near(horizontalHeading(vertical),0) && near(horizontalHeading(vertical,.7f),.7f),
              "Undefined vertical heading uses initial zero or the previous recenter reference");
    }
    {
        Input input; input.axis[0][0]=.25f; input.axis[0][1]=.5f;
        const auto zero=[](Vec3 v){return v.x==0 && v.y==0 && v.z==0;};
        auto move=horizontalStickMovement(input,false,0);
        check(near(move.x,.25f)&&near(move.y,0)&&near(move.z,-.5f),
              "Horizontal native stick convention is right/up/backward");
        auto turned=horizontalStickMovement(input,false,1.57079632679f);
        check(near(turned.x,-.5f)&&near(turned.y,0)&&near(turned.z,-.25f),
              "Calibrated quarter turn applies exactly once to movement");
        check(zero(horizontalStickMovement(input,true,0)),
              "An open movement-hand wheel cancels horizontal demand");
        input.axis[0][0]=std::numeric_limits<float>::quiet_NaN();
        check(zero(horizontalStickMovement(input,false,0)),
              "Nonfinite stick input cannot become native movement");
        input.axis[0][0]=1.01f;
        check(zero(horizontalStickMovement(input,false,0)),
              "Out-of-range axes fail neutral");
        input.axis[0][0]=0;
        check(zero(horizontalStickMovement(input,false,std::numeric_limits<float>::infinity())),
              "Invalid turn cannot become a movement basis");
    }
    // Native alpha must survive pre-UI dimming, including black and passthrough.
    for (float visibility : {0.f, .5f, 1.f}) {
        uint8_t pixels[]{0, 1, 255, 17, 254, 128, 3, 231};
        const auto lut=makeOpaqueDimming(visibility);
        dimRgbPixels(pixels,2,lut);
        check(pixels[3]==17 && pixels[7]==231 && pixels[2]==uint8_t(std::lround(255*visibility)) &&
                  pixels[4]==uint8_t(std::lround(254*visibility)),
              "Pre-UI dimming preserves native alpha and original channel quantization");
        dimOpaquePixels(pixels,2,makeOpaqueDimming(1));
        check(pixels[3]==255 && pixels[7]==255 && pixels[2]==uint8_t(std::lround(255*visibility)),
              "Final transport makes alpha opaque without dimming completed UI RGB again");
    }
    uint8_t colors[] = {10, 20, 30, 40, 11, 21, 31, 41};
    bgraToRgba(colors, colors, 2);
    check(colors[0] == 30 && colors[2] == 10 && colors[3] == 40 && colors[4] == 31 && colors[6] == 11 &&
              colors[7] == 41,
          "RGBA conversion preserves channel order and alpha, including in-place conversion");
    {
        Input input;
        input.session = input.reference = input.focused = input.headValid = 1;
        input.handValid[0] = input.handValid[1] = 1;
        input.zoomActiveMask = 3;
        input.zoomInputGeneration[0] = input.zoomInputGeneration[1] = 1;
        input.sequence = 10; input.tickMs = 1000;
        HeldZoomInput leftZoom, rightZoom;
        check(!leftZoom.sample(input,0,true,20,1,7,1000), "Equip boundary requires a later neutral");
        check(!leftZoom.sample(input,0,true,20,1,7,1001) && !leftZoom.gate.armed,
              "Cached pre-equip neutral cannot arm held zoom");
        ++input.sequence; input.tickMs = 1002;
        check(!leftZoom.sample(input,0,true,20,1,7,1002) && leftZoom.gate.armed,
              "A later actual eligible neutral arms zoom");
        ++input.sequence; input.tickMs = 1003; input.zoomDownMask = 1;
        check(leftZoom.sample(input,0,true,20,1,7,1003), "Native sniper click admits independent left zoom");
        check(!rightZoom.sample(input,1,true,21,1,7,1003), "Other hand retains independent admission");
        check(!leftZoom.sample(input,0,false,20,1,7,1004), "Wheel or tracking loss disarms zoom");
        ++input.sequence; input.tickMs = 1005;
        check(!leftZoom.sample(input,0,true,20,1,7,1005), "Held click after blocking cannot reactivate");
        input.zoomDownMask = 0; ++input.sequence; input.tickMs = 1006;
        leftZoom.sample(input,0,true,20,1,7,1006);
        input.zoomDownMask = 1; ++input.sequence; input.tickMs = 1007;
        check(leftZoom.sample(input,0,true,20,1,7,1007), "Release then press restores admitted zoom");
        // A change in the other hand alters render publication, not this
        // existing tracking-context key or actual left source identity.
        check(leftZoom.sample(input,0,true,20,1,7,1007), "Other-hand equip preserves held left zoom");
        check(!leftZoom.sample(input,0,true,22,1,7,1007), "Same-type weapon replacement loses old release");
        input.zoomDownMask = 0; ++input.sequence; input.tickMs = 1008;
        leftZoom.sample(input,0,true,22,1,7,1008);
        ++input.zoomInputGeneration[0];
        check(!leftZoom.sample(input,0,true,22,1,7,1008) && !leftZoom.gate.armed,
              "Changed logical action cannot reuse an earlier neutral sample");
        input.zoomSourceButton[0] = Sprint; input.zoomSourceButton[1] = Jump;
        input.buttons[0] = Sprint | Jump | Use; input.buttons[1] = Jump | Sprint;
        check(contextualSniperButtons(input,0,true) == (Jump|Use) &&
              contextualSniperButtons(input,1,false) == (Jump|Sprint) &&
              contextualSniperButtons(input,1,true) == Sprint,
              "Contextual sniper zoom consumes exactly its hand's sampled logical action");
    }
    TriggerGate left, right;
    {
        RiderIdentity onFoot{7, 0, 0, 0, false}, mounted{7, 9, 11, 3, true};
        check(onFoot.handheld() && !onFoot.seated() && mounted.seated() && !mounted.handheld(),
              "Native seated identity selects a mode without a vehicle class/physics-bit policy");
        auto otherSeat = mounted;
        ++otherSeat.seat;
        check(otherSeat != mounted, "Seat changes invalidate the existing rig/input transaction");
        Pose nativeView{yaw(.7f), {3, 4, 5}}, nativeBody{yaw(1.2f), {3, 3, 5}}, seatAnchor;
        check(nativeRiderAnchor(mounted, nativeView, std::bit_cast<uint32_t>(.25f), nativeBody, seatAnchor) &&
                  near(seatAnchor.p.x, 3) && near(seatAnchor.p.y, 4.25f) && near(seatAnchor.p.z, 5) &&
                  near(yawAngle(seatAnchor.q), 1.2f),
              "Seated anchor retains native eye height/position and replaces operator aim with body orientation");
        Pose walkedAnchor;
        check(nativeRiderAnchor(onFoot, nativeView, std::bit_cast<uint32_t>(.25f), nativeBody, walkedAnchor) &&
                  near(yawAngle(walkedAnchor.q), .7f),
              "On-foot native view orientation remains the reference");
        auto invalidBody = nativeBody;
        invalidBody.q = {0, 0, 0, 0};
        check(!nativeRiderAnchor(mounted, nativeView, 0, invalidBody, seatAnchor),
              "A degenerate native body pose cannot become a seated tracking anchor");
        invalidBody = nativeBody;
        invalidBody.p.x = std::bit_cast<float>(0x7f61b1e6u);
        check(!nativeRiderAnchor(mounted, nativeView, 0, invalidBody, seatAnchor),
              "Native finite unset markers are rejected at the mounted anchor boundary");
    }
    {
        const RiderIdentity seat{7, 9, 11, 3, true};
        const Quat pitch{std::sin(.23f), 0, 0, std::cos(.23f)};
        const Quat roll{0, 0, std::sin(-.17f), std::cos(-.17f)};
        const Pose body{multiply(yaw(.8f), multiply(pitch, roll)), {9, 3, -4}};
        Pose anchor;
        check(nativeRiderAnchor(seat, Pose{yaw(-.6f), {9, 4, -4}},
                              std::bit_cast<uint32_t>(.2f), body, anchor),
              "A seat may pitch, yaw and roll without flattening the tracked rig");
        const Pose origin{};
        const Pose head{multiply(pitch, yaw(.12f)), {.13f, -.2f, .08f}};
        const Pose leftEye = compose(head, Pose{Quat{}, {-.032f, 0, 0}});
        const Pose rightEye = compose(head, Pose{Quat{}, {.032f, 0, 0}});
        const auto leftWorld = worldEyeTracking(anchor, origin, 0, head, leftEye);
        const auto rightWorld = worldEyeTracking(anchor, origin, 0, head, rightEye);
        const auto recoveredHead = compose(inverse(anchor), worldHeadTracking(anchor, origin, 0, head));
        const auto recoveredLeft = compose(inverse(anchor), leftWorld);
        const auto recoveredRight = compose(inverse(anchor), rightWorld);
        check(near(recoveredHead.p.x, head.p.x) && near(recoveredHead.p.y, head.p.y) &&
                  near(recoveredHead.p.z, head.p.z) && near(recoveredHead.q.x, head.q.x) &&
                  near(recoveredHead.q.y, head.q.y) && near(recoveredHead.q.z, head.q.z) &&
                  near(recoveredHead.q.w, head.q.w),
              "All head translation and rotation axes survive a rotated seat transform");
        const auto eyeSeparation = recoveredRight.p - recoveredLeft.p;
        check(near(std::sqrt(dot(eyeSeparation, eyeSeparation)), .064f) &&
                  !near(leftWorld.p.x, rightWorld.p.x),
              "Mounted stereo retains independent eye positions and physical IPD");
        const Pose hand{multiply(roll, yaw(-.3f)), {-.25f, -.35f, -.45f}};
        const auto recoveredHand = compose(inverse(anchor), worldHandTracking(anchor, origin, 0, head, hand));
        check(near(recoveredHand.p.x, hand.p.x) && near(recoveredHand.p.y, hand.p.y) &&
                  near(recoveredHand.p.z, hand.p.z) && near(recoveredHand.q.z, hand.q.z),
              "Mounted hand aim retains full pose independently of native operator look");
        Input captured{}, latest{};
        captured.session = captured.focused = captured.headValid = 1;
        captured.reference = 4;
        captured.sequence = 20;
        captured.tickMs = 1000;
        latest = captured;
        ++latest.sequence;
        latest.tickMs = 1001;
        check(compatibleRiderInput(seat, seat, captured, latest, 3, 3, 1002),
              "A newer compatible XR input does not relabel or invalidate captured mounted controls");
        auto other = seat;
        ++other.seat;
        check(!compatibleRiderInput(seat, other, captured, latest, 3, 3, 1002) &&
                  !compatibleRiderInput(seat, RiderIdentity{7, 0, 0, 0, false}, captured, latest, 3, 3, 1002) &&
                  !compatibleRiderInput(seat, seat, captured, latest, 3, 4, 1002),
              "Seat, mount and rig transitions cannot reuse a captured native command transaction");
        latest.reference = 5;
        check(!compatibleRiderInput(seat, seat, captured, latest, 3, 3, 1002),
              "Reference-space replacement rejects captured mounted controls");
        latest = captured;
        latest.tickMs = 1200;
        check(!compatibleRiderInput(seat, seat, captured, latest, 3, 3, 1200),
              "A fresh latest sample cannot launder stale captured controls");
        latest = captured;
        latest.tickMs = 1003;
        check(!compatibleRiderInput(seat, seat, captured, latest, 3, 3, 1002),
              "Future timestamps cannot admit mounted controls");
        captured.handValid[1] = 1;
        latest = captured;
        ++latest.sequence;
        check(trackedHandCurrent(captured, latest, 1), "Compatible newer hand tracking preserves captured aim");
        latest.handValid[1] = 0;
        check(compatibleRiderInput(seat, seat, captured, latest, 3, 3, 1002) &&
                  !trackedHandCurrent(captured, latest, 1),
              "Head-valid newer input cannot revive captured mounted aim after known hand loss");
        latest.handValid[1] = 1;
        latest.hand[1].q.x = std::numeric_limits<float>::quiet_NaN();
        check(!trackedHandCurrent(captured, latest, 1), "Nonfinite current hand invalidates captured aim");
        const RiderIdentity foot{7, 0, 0, 0, false};
        check(sameHandheldRig(foot, foot, 3, 3) && !sameHandheldRig(foot, seat, 3, 3) &&
                  !sameHandheldRig(foot, foot, 3, 4),
              "Native getters cannot commit borrowed handheld output after rider/generation replacement");
        TriggerGate mountedGate;
        mountedGate.update(0, true);
        const float held = mountedGate.update(.8f, true);
        check(nativeCommandQuery(held, 0, 1, true) == 1 &&
                  nativeCommandQuery(mountedGate.update(.8f, true), held, 1, true) == 0 &&
                  nativeCommandQuery(mountedGate.update(.1f, true), held, 2, true) == 1,
              "Native mounted primary has one press, stable hold and ordinary release");
        check(!nativeCommandQuery(1, 0, 1, false) && !nativeCommandQuery(0, 1, 2, false),
              "Incompatible native contexts suppress both press and release edges");
    }
    ActionStream stream;
    for (unsigned hand = 0; hand < 2; ++hand) {
        Input previous{}, current{};
        previous.primaryActiveMask = 3;
        previous.primaryInputGeneration[0] = previous.primaryInputGeneration[1] = 1;
        previous.trigger[0] = previous.trigger[1] = .8f;
        current = previous;
        ++current.primaryInputGeneration[hand];
        TriggerGate oldGate;
        oldGate.update(0, true);
        const float oldValue = oldGate.update(.8f, true);
        TriggerGate newGate;
        const float newValue = newGate.update(.8f, true);
        const bool sameStream = primaryCommandHistoryCompatible(previous, current, hand, 1, 1, false);
        check(!sameStream && !nativeCommandQuery(newValue, sameStream ? oldValue : 0, 2, true) &&
                  primaryCommandHistoryCompatible(previous, current, 1 - hand, 1, 1, false),
              "Coalesced mounted stream replacement while held cannot fabricate release or disturb other hand");
        current = previous;
        const bool sameEpoch = primaryCommandHistoryCompatible(previous, current, hand, 1, 2, true);
        check(!sameEpoch && !nativeCommandQuery(0, sameEpoch ? oldValue : 0, 2, true),
              "Handheld admission replacement cannot carry an older command edge into a new epoch");
        const bool mountedEpoch = primaryCommandHistoryCompatible(previous, current, hand, 1, 2, false);
        check(mountedEpoch && !nativeCommandQuery(1, mountedEpoch ? oldValue : 0, 1, true),
              "Handheld ACK changes do not restart a held native vehicle fire command");
        check(primaryCommandHistoryCompatible(previous, current, hand, 1, 1, true) &&
                  nativeCommandQuery(0, oldValue, 2, true) == 1,
              "Ordinary same-stream and same-epoch trigger release remains intact");
    }
    check(!stream.sample(false) && stream.generation == 1, "Initially unbound action is not neutral input");
    check(stream.sample(true) && stream.generation == 1, "Bound logical action retains its stream");
    check(!stream.sample(false) && stream.generation == 2, "Action loss advances provenance without release");
    check(stream.sample(true) && stream.generation == 2, "Coalesced action recovery retains the lost-stream boundary");
    stream.invalidate();
    check(stream.generation == 3 && !stream.active, "Profile/remap invalidation requires a new sample");
    stream.generation = UINT32_MAX;
    stream.invalidate();
    check(!stream.generation && !stream.sample(true), "Action generation exhaustion fails closed without wrap");
    {
        const float invalidValues[]{-.01f, 1.01f, -std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(), std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
        for (float invalid : invalidValues) {
            ActionStream primary[2];
            const auto released = primaryActionSample(primary[0], true, 0.f);
            const auto other = primaryActionSample(primary[1], true, .8f);
            TriggerGate gate;
            bool physicalDown = false;
            uint32_t releasedSerial = 0;
            check(released.active && released.value == 0.f && released.generation == 1 &&
                  !gate.update(released.value, released.active), "Actual zero remains an eligible release");
            const auto held = primaryActionSample(primary[0], true, 1.f);
            check(held.active && held.value == 1.f && gate.update(held.value, held.active),
                  "Actual one remains an eligible press");
            samplePrimaryNeutral(held.value, held.active, true, physicalDown, releasedSerial);
            const auto lost = primaryActionSample(primary[0], true, invalid);
            check(!lost.active && lost.value == 0.f && lost.generation == 2 &&
                  !samplePrimaryNeutral(lost.value, lost.active, true, physicalDown, releasedSerial) &&
                  physicalDown && releasedSerial == 0 && !gate.update(lost.value, lost.active) && !gate.armed,
                  "Malformed primary is unavailable, never a neutral witness or press");
            const auto stillLost = primaryActionSample(primary[0], true, invalid);
            check(!stillLost.active && stillLost.generation == lost.generation,
                  "Repeated malformed samples preserve one action-loss boundary");
            const auto recovered = primaryActionSample(primary[0], true, .8f);
            check(recovered.active && recovered.generation == lost.generation &&
                  !gate.update(recovered.value, recovered.active),
                  "Recovery while held cannot rearm after malformed input");
            Input previous{}, current{};
            previous.primaryActiveMask = current.primaryActiveMask = 3;
            previous.primaryInputGeneration[0] = held.generation;
            current.primaryInputGeneration[0] = recovered.generation;
            previous.primaryInputGeneration[1] = current.primaryInputGeneration[1] = other.generation;
            check(!primaryCommandHistoryCompatible(previous, current, 0, 1, 1, false) &&
                  primaryCommandHistoryCompatible(previous, current, 1, 1, 1, false) && primary[1].active,
                  "Skipped invalid publication still retires only the affected hand's command history");
            const auto neutral = primaryActionSample(primary[0], true, .1f);
            check(samplePrimaryNeutral(neutral.value, neutral.active, true, physicalDown, releasedSerial) &&
                  !physicalDown && releasedSerial == 1 && !gate.update(neutral.value, neutral.active) &&
                  gate.update(recovered.value, recovered.active),
                  "Only a real post-loss release restores physical trigger admission");
        }
        ActionStream initiallyInvalid;
        check(!primaryActionSample(initiallyInvalid, true, -1.f).active && initiallyInvalid.generation == 1 &&
              !primaryActionSample(initiallyInvalid, false, 0.f).active,
              "Invalid or inactive first samples do not establish an active stream");
        for (float valid : {0.f, .1f, .2f, .65f, .8f, 1.f}) {
            const auto sample = primaryActionSample(initiallyInvalid, true, valid);
            check(sample.active && sample.value == valid && sample.generation == 1,
                  "Valid analog values retain exact hysteresis and stream identity");
        }
        initiallyInvalid.generation = UINT32_MAX;
        check(!primaryActionSample(initiallyInvalid, true, -1.f).active && !initiallyInvalid.generation &&
              !primaryActionSample(initiallyInvalid, true, 0.f).active,
              "Malformed loss at exhausted generation cannot wrap or recover");
    }
    IntentInputBoundary inputBoundary, otherBoundary;
    check(inputBoundary.install(1, 10, 100), "Validated ACK establishes the initial input boundary");
    check(!inputBoundary.echo(11, 99, 101) && !inputBoundary.echo(10, 101, 101) &&
              !inputBoundary.echo(11, 100, 101) && !inputBoundary.echo(11, 102, 101),
          "Cached, pre-ACK, same-tick and future inputs cannot be restamped");
    check(inputBoundary.echo(11, 101, 101) == 1, "Only genuinely post-ACK input echoes the new epoch");
    check(otherBoundary.install(1, 10, 100) && inputBoundary.install(2, 11, 101) &&
              !inputBoundary.echo(11, 101, 102) && otherBoundary.echo(11, 101, 102) == 1,
          "Replacement admission is per-hand and preserves the other hand's current input");
    check(!inputBoundary.install(1, 999, 999) && !inputBoundary.install(2, 999, 999) &&
              !inputBoundary.install(0, 999, 999) && inputBoundary.echo(12, 102, 102) == 2,
          "Regressed, duplicate and credit-only ACK metadata cannot refresh or poison boundaries");
    check(!left.update(1, true), "Held trigger must not arm on session entry");
    check(!left.update(0, true) && left.update(.8, true), "Released trigger arms left hand");
    check(!right.update(.8, true), "Right trigger independent of armed left");
    check(!left.update(.8, false) && !left.update(.8, true), "Focus/tracking loss requires fresh release");
    check(!left.update(0, true) && left.update(.8, true), "Release after loss rearms");
    check(!left.update(std::numeric_limits<float>::quiet_NaN(), true), "Invalid tracking suppresses trigger");
    Input chord{};
    chord.focused = 1;
    chord.buttons[0] = chord.buttons[1] = Wheel | Sprint;
    chord.trigger[0] = chord.trigger[1] = .9f;
    StickChord chordSticks;
    applyRecenterChord(chord, true, chordSticks, 1000);
    check(recenterHeld(chord) && chord.trigger[0] == .9f && chord.trigger[1] == .9f &&
              (chord.buttons[0] & Wheel) && (chord.buttons[1] & Wheel) &&
              !((chord.buttons[0] | chord.buttons[1]) & Sprint),
          "Recenter preserves physical grips/triggers and suppresses sprint");
    {
        // The chord as the host samples it each frame: `l`/`r` stick clicks at `t` ms.
        StickChord chordState;
        const auto sample = [&](bool l, bool r, uint64_t t) {
            Input in{};
            in.focused = 1;
            in.buttons[0] = l ? Sprint : 0u;
            in.buttons[1] = r ? Sprint : 0u;
            applyRecenterChord(in, false, chordState, t);
            return in;
        };
        Input sticks = sample(true, false, 0);
        check(!recenterHeld(sticks) && (sticks.buttons[0] & Sprint), "One stick click alone stays a stick click");
        sticks = sample(true, true, 120);
        check(recenterHeld(sticks) && !((sticks.buttons[0] | sticks.buttons[1]) & Sprint),
              "Both stick clicks together recenter and suppress sprint and grenade");
        // Left let go first: the still-held right click is no grenade or sprint.
        sticks = sample(false, true, 400);
        check(!recenterHeld(sticks) && !(sticks.buttons[1] & Sprint), "A stick left held after the chord stays quiet");
        sticks = sample(false, false, 450);
        sticks = sample(false, true, 600);
        check(!recenterHeld(sticks) && (sticks.buttons[1] & Sprint), "Released, the stick clicks normally again");
        sample(false, false, 650);
        // Sprinting (left held) and then the right click: a grenade, not a recentre.
        sample(true, false, 1000);
        sticks = sample(true, true, 1600);
        check(!recenterHeld(sticks) && (sticks.buttons[0] & Sprint) && (sticks.buttons[1] & Sprint),
              "The right click while sprinting is no recentre");
        sticks = sample(true, true, 3000);
        check(!recenterHeld(sticks), "Holding both after a late click never becomes a recentre (or levelling)");
    }
    TriggerGate chordGate;
    chordGate.update(0, true);
    chordGate.update(chord.trigger[0], !recenterHeld(chord));
    chord.buttons[0] = chord.buttons[1] = Wheel;
    check(!chordGate.update(chord.trigger[0], !recenterHeld(chord)),
          "Ending recenter with a physically held trigger cannot synthesize a shot");
    WeaponWheel chordWheel;
    int chordIds[] = {2, 5};
    chordWheel.update(true, 0, 1, chordIds, 2, true);
    chordWheel.update(true, 0, 1, chordIds, 2, false);
    chordWheel.update(true, 0, 1, chordIds, 2, true);
    check(!chordWheel.open, "Ending recenter while gripping cannot reopen a cancelled wheel");
    int ids[] = {2, 5, 8, 12};
    WeaponWheel l, r;
    check(l.update(true, 0, 1, ids, 4, true) == -1 && l.hover == 0 && l.open,
          "Up selects top sector while held");
    check(!r.open, "Second wheel stays independent");
    check(l.update(false, 0, 1, ids, 4, true) == 2 && !l.open, "Release commits hovered weapon");
    l.update(true, 1, 0, ids, 4, true);
    check(l.update(false, 0, 0, ids, 4, true) == -1, "Neutral release cancels");
    l.update(true, 1, 0, ids, 4, true);
    l.update(true, 1, 0, ids, 4, false);
    check(l.update(false, 1, 0, ids, 4, true) == -1, "Tracking loss cancels instead of committing");
    l.update(true, -1, 0, ids, 4, true);
    check(l.hover == 3, "Left axis wraps sector numbering");
    check(l.update(false, -1, 0, ids, 0, true) == -1, "Empty inventory cancels selection");
    auto walking = rotate(yaw(-Pi / 2), Vec3{0, 0, -1});
    check(near(walking.x, 1) && near(walking.z, 0),
          "Snap right rotates forward movement into positive native X");
    auto a = Pose{yaw(Pi / 2), {1, 2, 3}};
    auto b = Pose{yaw(-Pi / 3), {.2f, .5f, -.4f}};
    auto restored = compose(inverse(a), compose(a, b));
    check(near(restored.p.x, b.p.x) && near(restored.p.y, b.p.y) && near(restored.p.z, b.p.z),
          "Rigid transform inverse preserves tracking position");
    auto t = matrix(a);
    auto v = rotate(a.q, {0, 0, -1});
    check(near(t.m[2] * -1, v.x) && near(t.m[6] * -1, v.y) && near(t.m[10] * -1, v.z),
          "Matrix and quaternion use same camera basis");
    auto origin = Pose{yaw(.5f), {2, 1, 3}}, tracked = compose(origin, b);
    auto relative = relativeTracking(origin, tracked);
    check(near(relative.p.x, b.p.x) && near(relative.p.z, b.p.z),
          "Recenter preserves relative controller position");
    Pose distantHead{{}, {HeadHorizontalLimit + .25f, 0, 0}};
    Pose physicalLeft{{}, {HeadHorizontalLimit + .218f, 0, 0}}, physicalRight{{}, {HeadHorizontalLimit + .282f, 0, 0}};
    auto clippedLeft = worldEyeTracking({}, {}, 0, distantHead, physicalLeft);
    auto clippedRight = worldEyeTracking({}, {}, 0, distantHead, physicalRight);
    check(near(clippedRight.p.x - clippedLeft.p.x, .064f) &&
              near((clippedRight.p.x + clippedLeft.p.x) * .5f, HeadHorizontalLimit),
          "Bounded head translation preserves the physical eye separation");
    for (Vec3 displacement : {Vec3{.2f, 0, 0}, Vec3{0, -.2f, 0}, Vec3{0, 0, .2f}}) {
        Pose moved{{}, displacement};
        Pose actual = worldEyeTracking({}, {}, 0, moved, moved);
        check(near(actual.p.x, displacement.x) && near(actual.p.y, displacement.y) &&
                  near(actual.p.z, displacement.z),
              "Headset lateral, crouch and forward translation reach the native eye camera");
    }
    const float half = Pi / 8;
    for (Quat orientation : {Quat{std::sin(half), 0, 0, std::cos(half)}, yaw(Pi / 4),
                             Quat{0, 0, std::sin(half), std::cos(half)}}) {
        Pose trackedRotation{orientation, {}};
        Pose actual = worldEyeTracking({}, {}, 0, trackedRotation, trackedRotation);
        check(near(actual.q.x, orientation.x) && near(actual.q.y, orientation.y) &&
                  near(actual.q.z, orientation.z) && near(actual.q.w, orientation.w),
              "Headset pitch, yaw and roll remain intact through the native camera adapter");
        actual = worldHandTracking({}, {}, 0, {}, trackedRotation);
        check(near(actual.q.x, orientation.x) && near(actual.q.y, orientation.y) &&
                  near(actual.q.z, orientation.z),
              "Weapon orientation retains all three rotational degrees of freedom");
    }
    Pose crouched{{}, {.35f, -1.1f, .2f}};
    const Pose crouchedHead = worldHeadTracking({}, {}, 0, crouched);
    check(near(crouchedHead.p.y, -1.1f) && near(crouchedHead.p.x, .35f) && near(crouchedHead.p.z, .2f),
          "A deep physical crouch keeps metre-for-metre height and horizontal lean");
    const Pose leftGrip{yaw(.3f), crouched.p + Vec3{-.4f, -.4f, -.5f}};
    const Pose rightGrip{yaw(-.7f), crouched.p + Vec3{.4f, -.4f, -.5f}};
    const Pose nativeBody{yaw(.9f), {30, 2, -17}};
    for (Pose head : {crouched, Pose{{}, {1.2f, -1.1f, .2f}}}) {
        const Pose headInSpace = compose(origin, head);
        const Pose worldHead = worldHeadTracking(nativeBody, origin, .4f, headInSpace);
        const Quat basis = multiply(nativeBody.q, yaw(.4f));
        for (Pose grip : {leftGrip, rightGrip}) {
            grip.p = grip.p + (head.p - crouched.p);
            const Pose worldHand =
                worldHandTracking(nativeBody, origin, .4f, headInSpace, compose(origin, grip));
            const Vec3 expected = rotate(basis, grip.p - head.p), actual = worldHand.p - worldHead.p;
            check(near(actual.x, expected.x) && near(actual.y, expected.y) && near(actual.z, expected.z),
                  "Both hands keep their physical head-relative offsets through crouch, bounds, recenter and "
                  "turn");
            check(
                headTranslationValid(bodyHeadTracking(origin, .4f, headInSpace).p) &&
                    handTranslationValid(bodyHeadTracking(origin, .4f, headInSpace).p,
                                         bodyHandTracking(origin, .4f, headInSpace, compose(origin, grip)).p),
                "Local rig poses satisfy the same multiplayer tracking volume");
        }
        const Pose firstEye{{}, head.p + Vec3{-.032f, 0, 0}}, secondEye{{}, head.p + Vec3{.032f, 0, 0}};
        const Vec3 eyeDelta =
            worldEyeTracking(nativeBody, origin, .4f, headInSpace, compose(origin, secondEye)).p -
            worldEyeTracking(nativeBody, origin, .4f, headInSpace, compose(origin, firstEye)).p;
        check(near(std::sqrt(dot(eyeDelta, eyeDelta)), .064f),
              "The complete bounded crouching rig preserves physical stereo separation");
    }
    const Pose boundedReach = bodyHandTracking({}, 0, crouched, Pose{{}, crouched.p + Vec3{3, 0, 0}});
    check(near(boundedReach.p.x - crouchedHead.p.x, HandReachLimit) && near(boundedReach.p.y, -1.1f),
          "Exceptional controller reach is bounded relative to the tracked head without changing crouch "
          "height");
    const Quat combined =
        normalize(multiply(yaw(.6f), multiply(Quat{.2f, 0, 0, .98f}, Quat{0, 0, -.15f, .99f})));
    const Pose fullHead{
        combined,
        {HeadHorizontalLimit * std::sqrt(.5f), -HeadDownLimit, HeadHorizontalLimit * std::sqrt(.5f)}};
    const Pose cantedLeft{multiply(combined, yaw(.015f)),
                          fullHead.p + rotate(combined, {-.030f, .002f, .001f})};
    const Pose cantedRight{multiply(combined, yaw(-.012f)),
                           fullHead.p + rotate(combined, {.034f, -.001f, -.002f})};
    const Pose tiltedBody{multiply(yaw(.8f), combined), {20, 3, -40}};
    const Pose actualLeft =
        worldEyeTracking(tiltedBody, origin, -.7f, compose(origin, fullHead), compose(origin, cantedLeft));
    const Pose actualRight =
        worldEyeTracking(tiltedBody, origin, -.7f, compose(origin, fullHead), compose(origin, cantedRight));
    const Quat completeBasis = multiply(tiltedBody.q, yaw(-.7f));
    const Vec3 expectedEyes = rotate(completeBasis, cantedRight.p - cantedLeft.p);
    const Vec3 actualEyes = actualRight.p - actualLeft.p;
    const Quat expectedLeftRotation = multiply(completeBasis, cantedLeft.q);
    check(near(actualEyes.x, expectedEyes.x) && near(actualEyes.y, expectedEyes.y) &&
              near(actualEyes.z, expectedEyes.z) && near(actualLeft.q.x, expectedLeftRotation.x) &&
              near(actualLeft.q.y, expectedLeftRotation.y) && near(actualLeft.q.z, expectedLeftRotation.z) &&
              near(actualLeft.q.w, expectedLeftRotation.w),
          "Combined head/body rotations and asymmetric canted eyes preserve the full rig at simultaneous "
          "limits");
    Pose heightBase{{}, {2, 1, 3}};
    check(applyNativeViewHeight(heightBase, std::bit_cast<uint32_t>(.125f)) && near(heightBase.p.y, 1.125f) &&
              heightBase.p.x == 2 && heightBase.p.z == 3,
          "Pre-animation anchor preserves the native view-height correction");
    check(!applyNativeViewHeight(heightBase, 0x7f61b1e6) && near(heightBase.p.y, 1.125f),
          "Finite native interpolation sentinel must not become headset translation");
    Pose modelLocal{yaw(.15f), {.2f, -.1f, -.3f}};
    Pose shotLocal{yaw(-.08f), {.25f, -.08f, -.65f}};
    Pose grip{yaw(-.7f), {3, 1, 8}};
    auto cachedModel = compose(inverse(a), compose(a, modelLocal));
    auto firstShot = retargetShot(a, compose(a, shotLocal), cachedModel.p, grip);
    Pose movedBody{yaw(.85f), {111, 5, -100}};
    auto movedShot = retargetShot(movedBody, compose(movedBody, shotLocal), cachedModel.p, grip);
    check(near(firstShot.p.x, movedShot.p.x) && near(firstShot.p.y, movedShot.p.y) &&
              near(firstShot.p.z, movedShot.p.z),
          "Body movement between model calibration and firing cannot displace the muzzle");
    auto firstAxis = rotate(firstShot.q, {0, 0, -1});
    auto movedAxis = rotate(movedShot.q, {0, 0, -1});
    check(near(firstAxis.x, movedAxis.x) && near(firstAxis.y, movedAxis.y) && near(firstAxis.z, movedAxis.z),
          "Body yaw cannot add an extra rotation to the independently aimed hand");
    auto longShot = retargetShot({}, Pose{{}, {0, 0, -4}}, {}, grip);
    auto muzzle = longShot.p - grip.p;
    check(near(std::sqrt(dot(muzzle, muzzle)), .5f), "Native muzzle displacement remains bounded");
    const Vec3 charge{.13f, -.07f, .8f};
    auto chargedShot = retargetShot(a, compose(a, shotLocal), cachedModel.p + charge, grip, .5f, charge);
    const Vec3 expectedCharge = rotate(grip.q, charge);
    check(near(chargedShot.p.x - firstShot.p.x, expectedCharge.x) &&
          near(chargedShot.p.y - firstShot.p.y, expectedCharge.y) &&
          near(chargedShot.p.z - firstShot.p.z, expectedCharge.z),
          "Native charge is retained exactly once outside the attachment bound");
    auto movedChargedShot = retargetShot(movedBody, compose(movedBody, shotLocal),
                                        cachedModel.p + charge, grip, .5f, charge);
    check(near(movedChargedShot.p.x, chargedShot.p.x) &&
          near(movedChargedShot.p.y, chargedShot.p.y) &&
          near(movedChargedShot.p.z, chargedShot.p.z),
          "Charge residual remains camera-relative across player movement");
    // A model origin can be much farther from its own attachment than the
    // controller grip is. Correct the reference before applying the grip bound.
    const Vec3 reference{-.01f, -.005f, -.54f};
    const Vec3 nativeAttachment{.012f, .008f, -.8f};
    const Quat nativeRotation = normalize(multiply(yaw(.27f), Quat{.08f, 0, 0, .99f}));
    for (const Vec3 stretch : {Vec3{1,1,1}, Vec3{-1,1,1}, Vec3{-1.1f,.9f,.8f}}) {
        const auto aligned = modelAnchorDisplacement(nativeRotation, stretch, reference);
        const auto attachment = rotate(nativeRotation, {stretch.x*nativeAttachment.x,
            stretch.y*nativeAttachment.y, stretch.z*nativeAttachment.z});
        const Vec3 neutralRoot{.2f, -.1f, -.3f};
        const Pose nativeAttachmentPose{nativeRotation, neutralRoot + attachment};
        const auto alignedShot = retargetShot(a, compose(a,nativeAttachmentPose),
                                             neutralRoot + charge, grip, .5f, charge, aligned);
        const Pose alignedModel{normalize(multiply(grip.q,nativeRotation)),
                                 grip.p + rotate(grip.q,charge + aligned)};
        const auto renderedAttachment = alignedModel.p + rotate(alignedModel.q,
            {stretch.x*nativeAttachment.x,stretch.y*nativeAttachment.y,stretch.z*nativeAttachment.z});
        check(near(alignedShot.p.x,renderedAttachment.x) && near(alignedShot.p.y,renderedAttachment.y) &&
              near(alignedShot.p.z,renderedAttachment.z),
              "Static handle correction keeps the model and muzzle together for mirrored/nonuniform stretch");
        const auto renderedReference = alignedModel.p + rotate(alignedModel.q,
            {stretch.x*reference.x,stretch.y*reference.y,stretch.z*reference.z});
        const auto chargedGrip = grip.p + rotate(grip.q,charge);
        check(near(renderedReference.x,chargedGrip.x) && near(renderedReference.y,chargedGrip.y) &&
              near(renderedReference.z,chargedGrip.z),
              "Static reference anchors exactly once while preserving native charge");
        const auto movedAligned = retargetShot(movedBody,compose(movedBody,nativeAttachmentPose),
                                               neutralRoot + charge,grip,.5f,charge,aligned);
        check(near(movedAligned.p.x,alignedShot.p.x) && near(movedAligned.p.y,alignedShot.p.y) &&
              near(movedAligned.p.z,alignedShot.p.z),
              "Static alignment remains invariant to native body movement");
    }
    const auto excessiveAlignedShot = retargetShot({},Pose{{},{0,0,-4}}, {}, grip,
                                                   .5f,charge,{0,0,.54f});
    const auto withoutCharge = excessiveAlignedShot.p - grip.p - rotate(grip.q,charge);
    check(near(std::sqrt(dot(withoutCharge,withoutCharge)),.5f),
          "Aligned grip-to-attachment reach remains bounded before native charge");
    Fov f{-.9f, .7f, .8f, -.6f};
    auto p = projection(f, .1f, 1000);
    auto projectX = [&](float x) { return (p.m[0] * x - p.m[2]); };
    check(near(projectX(std::tan(f.left)), -1) && near(projectX(std::tan(f.right)), 1),
          "Asymmetric XR FOV maps image edges");
    check(near((p.m[10] * -.1f + p.m[11]) / .1f, -1), "Native projection near plane convention");
    check(offsetof(Shared, latest) == 48 && sizeof(Pose) == 28 && sizeof(Request) == 480 &&
              sizeof(Shared) == 2056 && offsetof(Shared, slot) == 672 &&
              offsetof(Shared, pixels) == 1992 && sizeof(Slot) == 584,
          "Cross-architecture ABI layout");
    InputSampleBoundary interruptedInput{7,3,100,2000,42};
    Input interruptedSample{};
    interruptedSample.session = 7;
    interruptedSample.reference = 3;
    interruptedSample.sequence = 100;
    interruptedSample.tickMs = 1999;
    interruptedSample.primaryActiveMask = 1;
    interruptedSample.primaryInputGeneration[0] = 1;
    TriggerGate interruptedTrigger;
    auto sampleInterruptedTrigger = [&](float value) {
        return interruptedTrigger.update(value,
            interruptedInput.permits(interruptedSample,2100,42));
    };
    check(!sampleInterruptedTrigger(0) && !interruptedTrigger.armed &&
              !sampleInterruptedTrigger(1) && !interruptedTrigger.armed,
          "cached neutral or held input cannot arm after native interruption");
    interruptedSample.sequence = 101;
    interruptedSample.tickMs = 2001;
    check(!sampleInterruptedTrigger(1) && !interruptedTrigger.armed,
          "a later held sample still requires a later actual release");
    interruptedSample.sequence = 102;
    interruptedSample.tickMs = 2002;
    check(!sampleInterruptedTrigger(0) && interruptedTrigger.armed &&
              sampleInterruptedTrigger(1),
          "a post-interruption neutral permits the existing trigger gate to resume");
    interruptedSample.tickMs = 1999;
    interruptedSample.session = 8;
    check(!interruptedInput.permits(interruptedSample,2100,42),
          "changed session cannot relabel a pre-interruption timestamp");
    interruptedSample.tickMs = 2003;
    interruptedSample.sequence = 1;
    check(interruptedInput.permits(interruptedSample,2100,42),
          "a new session with a later timestamp can start a new sequence");
    interruptedSample.session = 7;
    check(interruptedInput.permits(interruptedSample,2100,43) &&
              !interruptedInput.permits(interruptedSample,2100,42),
          "a new host producer may restart sequence without accepting cached old producer data");
    interruptedSample.tickMs = 2101;
    check(!interruptedInput.permits(interruptedSample,2100,43),
          "future input timestamps never satisfy interruption admission");
    std::cout << "Controls, pose transforms, asymmetric projection and protocol checks passed\n";
}
