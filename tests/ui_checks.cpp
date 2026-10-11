#include "common/color.hpp"
#include "common/controls.hpp"
#include "common/head_comfort.hpp"
#include "common/lasers.hpp"
#include "common/settings.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace ss2vr;
static void check(bool value, const char *message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
static bool near(float a, float b, float epsilon = .0001f) {
    return std::abs(a - b) < epsilon;
}
int main() {
    ComfortAnchor anchor;
    Pose head{{}, {0, 1.7f, 0}};
    anchor.update(head, true, 100);
    Pose initial = anchor.panel({0, 0, -2.2f});
    head.q = yaw(34 * Pi / 180);
    head.p.x = .01f;
    anchor.update(head, true, 120);
    auto still = anchor.panel({0, 0, -2.2f});
    check(near(still.p.x, initial.p.x) && near(still.p.z, initial.p.z) && near(anchor.heading, 0),
          "Small head rotation/translation cannot drag the panel");
    head.q = yaw(40 * Pi / 180);
    anchor.update(head, true, 140);
    check(anchor.followingYaw && near(anchor.heading, 1.8f * Pi / 180),
          "Threshold crossing begins following at the bounded yaw rate");
    for (uint64_t t = 160; t < 1500; t += 20)
        anchor.update(head, true, t);
    check(!anchor.followingYaw && near(anchor.heading, 32 * Pi / 180),
          "Following stops at eight degrees residual, not eight degrees below the entry threshold");
    Pose moved = anchor.panel({0, 0, -2.2f});
    auto offset = rotate(inverse(moved.q), moved.p - anchor.origin);
    check(near(offset.x, 0) && near(offset.z, -2.2f),
          "Facing and panel center follow the same anchor transform");
    float priorHeading = anchor.heading;
    head.q = {std::sin(Pi / 4), 0, 0, std::cos(Pi / 4)};
    anchor.update(head, true, 1500);
    check(near(anchor.heading, priorHeading), "Vertical gaze preserves the last horizontal heading");
    anchor.reset();
    head.q = yaw(179 * Pi / 180);
    anchor.update(head, true, 2000);
    head.q = yaw(-179 * Pi / 180);
    anchor.update(head, true, 2020);
    check(!anchor.followingYaw, "Crossing the pi boundary is a two-degree turn, not a full revolution");
    anchor.update(head, false, 2040);
    check(!anchor.initialized, "Lost tracking/visibility invalidates the anchor");
    head.q = yaw(.5f);
    anchor.update(head, true, 2060);
    check(near(anchor.heading, .5f), "Tracking recovery reseeds without chasing an obsolete pose");
    head.q = yaw(.9f);
    anchor.update(head, true, 2080, true);
    check(near(anchor.heading, .9f), "Explicit recenter reseeds on its rising edge");
    head.q = yaw(1.5f);
    anchor.update(head, true, 3000);
    check(near(anchor.heading, 1.5f), "A long frame gap reseeds instead of taking a large follow step");
    Pose close = anchor.panel({0, 0, -2.2f});
    Pose walkingHead = head;
    walkingHead.p = close.p + rotate(close.q, {0, 0, .5f});
    check(!comfortablePanel(close, walkingHead), "Approaching a panel violates its explicit distance guard");
    walkingHead.p = close.p + rotate(close.q, {0, 0, -1.5f});
    check(!comfortablePanel(close, walkingHead), "A panel behind the viewer cannot remain eligible");
    WheelLayout layout;
    Fov eyes[2] = {{-.8f, .8f, .7f, -.7f}, {-.8f, .8f, .7f, -.7f}};
    head = {{}, {0, 1.7f, 0}};
    check(layout.reserve(head, eyes), "A shared wheel layout reserves both panels at first open");
    auto firstRight = layout.anchor[1];
    head.q = yaw(1.f); // Second hand opens later; its reservation does not change.
    check(layout.reserve(head, eyes), "Opening the second wheel retains the active reservation");
    check(near(firstRight.p.x, layout.anchor[1].p.x) && layout.anchor[0].p.x < layout.anchor[1].p.x,
          "Staggered wheel opening retains the original non-overlapping reservation");
    for (unsigned h = 0; h != 2; ++h)
        for (float corner : {-1.f, 1.f}) {
            Vec3 point =
                layout.anchor[h].p + rotate(layout.anchor[h].q, {corner * layout.diameter / 2, 0, 0});
            float angle = std::atan2(point.x, -point.z);
            check(std::abs(angle) < .8f, "Wheel edges stay inside the binocular horizontal FOV");
        }
    MenuNavigation menu;
    Input input{};
    input.primaryActiveMask = 3;
    input.primaryInputGeneration[0] = input.primaryInputGeneration[1] = 1;
    input.buttons[0] = Use;
    input.trigger[0] = 1;
    input.axis[0][1] = 1;
    check(menu.sample(input, true, true) == MenuAction::None,
          "Held Use/trigger/stick on menu entry cannot select or navigate");
    input.buttons[0] = 0;
    input.trigger[0] = 0;
    input.axis[0][1] = 0;
    menu.sample(input, true, true);
    input.buttons[0] = Use;
    check(menu.sample(input, true, true) == MenuAction::Confirm,
          "A valid released-then-pressed Use confirms");
    menu.sample(input, true, false);
    check(menu.sample(input, true, true) == MenuAction::None,
          "Focus/tracking recovery with Use still held cannot issue Enter");
    input = {};
    menu.sample(input, true, true);
    input.buttons[0] = Menu | Use;
    input.axis[0][1] = 1;
    check(menu.sample(input, true, true) == MenuAction::Back,
          "Simultaneous menu actions produce one deterministic Back tap");
    input = {};
    menu.sample(input, true, true);
    input.axis[0][0] = 1;
    menu.sample(input, true, false);
    check(menu.sample(input, true, true) == MenuAction::None,
          "Recovered deflected stick waits for a neutral observation");
    input = {};
    menu.sample(input, true, true, true);
    input.trigger[1] = 1;
    check(menu.sample(input, true, true, true) == MenuAction::None,
          "Pointer trigger cannot also synthesize keyboard Enter");
    check(menu.sample(input, true, true, false) == MenuAction::None,
          "Leaving pointer mode while trigger held requires a real release");
    Pose panel{{}, {0, 0, -2}};
    auto centerHit = pointAtPanel({}, panel, 2, 1);
    check(centerHit.valid && near(centerHit.u, .5f) && near(centerHit.v, .5f),
          "Menu ray maps the center of the actual LOCAL quad");
    auto upperLeft = pointAtPanel(Pose{{}, {-.75f, .3f, 0}}, panel, 2, 1);
    check(upperLeft.valid && near(upperLeft.u, .125f) && near(upperLeft.v, .2f),
          "Menu cursor preserves top-left image coordinates");
    check(!pointAtPanel(Pose{yaw(Pi), {}}, panel, 2, 1).valid &&
              !pointAtPanel(Pose{yaw(Pi / 2), {}}, panel, 2, 1).valid &&
              !pointAtPanel(Pose{{}, {2, 0, 0}}, panel, 2, 1).valid,
          "Back-facing, parallel and out-of-panel rays cannot select a menu");
    Pose turnedPanel = compose(Pose{yaw(.8f), {3, 1, 2}}, panel);
    Pose turnedAim{yaw(.8f), {3, 1, 2}};
    auto turnedHit = pointAtPanel(turnedAim, turnedPanel, 2, 1);
    check(turnedHit.valid && near(turnedHit.u, .5f) && near(turnedHit.v, .5f),
          "Threshold-follow panel rotation and translation use the same pointer transform");
    Input menuInput;
    menuInput.sequence = 11;
    menuInput.tickMs = 100;
    menuInput.session = 2;
    menuInput.reference = 3;
    menuInput.focused = menuInput.headValid = menuInput.handValid[1] = 1;
    menuInput.primaryActiveMask = 2;
    menuInput.primaryInputGeneration[1] = 1;
    menuInput.trigger[1] = 1; // New input must not click an older neutral ray.
    MenuPointer pointer{10, 100, 20, 2, 3, 4, 1, 1, .5f, .5f, 0, 1};
    check(menuPointerEligible(pointer, menuInput, 20, 4, 101),
          "Newer input can preserve a coherent presented pointer sample");
    TriggerGate pointerTrigger;
    check(!pointerTrigger.update(pointer.trigger, true) && pointerTrigger.armed,
          "Later trigger press cannot activate earlier neutral pointer coordinates");
    pointer.inputSequence = 11;
    pointer.trigger = 1;
    check(pointerTrigger.update(pointer.trigger, menuPointerEligible(pointer, menuInput, 20, 4, 101)),
          "Presented press activates coordinates from its own input sample");
    check(!menuPointerEligible(pointer, menuInput, 21, 4, 101) &&
              !menuPointerEligible(pointer, menuInput, 20, 5, 101),
          "Changed visual frame or menu identity rejects the old presented pointer");
    menuInput.primaryActiveMask = 0;
    check(!menuPointerEligible(pointer, menuInput, 20, 4, 101),
          "Inactive trigger with tracked hand cannot click or supply a menu neutral");
    menuInput.primaryActiveMask = 2;
    ++menuInput.primaryInputGeneration[1];
    check(!menuPointerEligible(pointer, menuInput, 20, 4, 101),
          "Rebound logical trigger rejects a pointer captured in the old stream");
    menuInput.focused = 0;
    check(!menuPointerEligible(pointer, menuInput, 20, 4, 101),
          "Newer focus loss invalidates an otherwise coherent pointer");
    Input controller{};
    controller.hand[0] = {yaw(.4f), {0, 1, -.3f}};
    controller.grip[0] = {yaw(.1f), {.02f, .9f, -.1f}};
    controller.gripValid[0] = 1;
    Pose weapon = weaponTracking(controller, 0);
    check(near(weapon.p.y, .9f) && near(yawAngle(weapon.q), .4f),
          "Weapon uses physical grip position and the runtime aim forward orientation");
    VrSettings settings;
    settings.gripOffset[12] = {yaw(.2f), {.1f, .02f, -.03f}};
    Pose aligned = calibratedGrip(controller, 0, 12, settings);
    auto localOffset = compose(inverse(weapon), aligned);
    check(near(localOffset.p.x, .1f) && near(localOffset.p.y, .02f) && near(localOffset.p.z, -.03f) &&
              near(yawAngle(localOffset.q), .2f),
          "Weapon calibration is applied once in the physical grip/aim basis");
    Pose unknown = calibratedGrip(controller, 0, -1, settings);
    check(near(unknown.p.x, weapon.p.x) && near(yawAngle(unknown.q), yawAngle(weapon.q)),
          "Unknown weapon IDs retain tracked placement");
    LaserAim samples[2];
    LaserFrame frame;
    frame.owner = 4;
    frame.generation = 3;
    frame.sequence = 10;
    frame.now = 200;
    for (unsigned h = 0; h != 2; ++h) {
        frame.weapon[h] = 5 + h;
        frame.handValid[h] = true;
        samples[h].owner = frame.owner;
        samples[h].generation = frame.generation;
        samples[h].sequence = frame.sequence;
        samples[h].weapon = frame.weapon[h];
        samples[h].tickMs = 100;
        samples[h].valid = true;
    }
    auto frozen = freezeLaserPair(samples, frame);
    check(frozen[0].valid && frozen[1].valid, "Both lasers freeze at the same age boundary");
    frame.now++;
    check(!laserEligible(samples[0], frame, 0) && frozen[0].valid && frozen[1].valid,
          "Time passing between eyes cannot expire half of the frozen laser pair");
    frame.now = 200;
    frame.selecting[0] = true;
    check(!laserEligible(samples[0], frame, 0), "An equip intent suppresses that hand's cached beam");
    frame.selecting[0] = false;
    frame.weapon[0]++;
    check(!laserEligible(samples[0], frame, 0), "Weapon instance replacement invalidates the beam");
    frame.weapon[0]--;
    frame.wheel[0] = true;
    frozen = freezeLaserPair(samples, frame);
    check(!frozen[0].valid && frozen[1].valid,
          "Left weapon wheel hides only its own laser, retaining independent right aim");
    frame.wheel[0] = false;
    frame.handValid[1] = false;
    frozen = freezeLaserPair(samples, frame);
    check(frozen[0].valid && !frozen[1].valid,
          "Right controller loss never borrows the left hand's valid beam");
    frame.handValid[1] = true;
    samples[0].end.x = std::numeric_limits<float>::quiet_NaN();
    frozen = freezeLaserPair(samples, frame);
    check(!frozen[0].valid && frozen[1].valid,
          "Invalid collision endpoint rejects only the affected hand");
    samples[0].end.x = 0;
    frame.requestSequence = 1;
    check(!laserEligible(samples[0], frame, 0) && !laserEligible(samples[1], frame, 1),
          "A replacement native world request cannot reuse either previous laser");
    frame.requestSequence = 0;
    frame.body.p.x = .04f;
    check(!laserEligible(samples[0], frame, 0), "Body movement beyond the cache tolerance hides the beam");
    Request leanRequest;
    leanRequest.sequence = 81;
    leanRequest.input.sequence = 42;
    leanRequest.session = leanRequest.input.session = 3;
    leanRequest.reference = leanRequest.input.reference = 4;
    leanRequest.trackingGeneration = 9;
    Pose leanBody{yaw(.3f), {10, 2, -5}};
    leanRequest.input.head = {{}, {.3f, -1.1f, .2f}};
    const Pose leanHead = worldHeadTracking(leanBody, {}, 0, leanRequest.input.head);
    const auto probe = makeHeadProbe(leanBody, leanHead);
    check(probe.valid && near(dot(probe.direction, probe.direction), 1) &&
              near(probe.length, std::sqrt(1.34f)),
          "Native head probe retains the full deep crouch and diagonal lean segment");
    check(!makeHeadProbe(leanBody, leanBody).valid && !makeHeadProbe({}, Pose{{}, {.0005f, 0, 0}}).valid &&
              !makeHeadProbe({}, Pose{{}, {MaximumHeadTranslation + 1, 0, 0}}).valid,
          "Zero/tiny or out-of-rig segments never manufacture a containment result");
    const auto sampleLean = [&](bool hit, float distance) {
        return recordHeadObstruction(leanBody, leanHead, 7, leanRequest, 1000, probe, hit, distance);
    };
    const auto freezeLean = [&](const HeadObstruction &sample, uint64_t now = 1050) {
        return freezeHeadVisibility(sample, leanBody, leanHead, 7, leanRequest, now, .05f);
    };
    const auto clearLean = sampleLean(false, probe.length);
    check(freezeLean(clearLean).available && near(freezeLean(clearLean).value, 1),
          "A finite native miss at the exact segment endpoint is explicitly clear");
    const auto partialLean = sampleLean(true, probe.length - .025f);
    const auto fullLean = sampleLean(true, probe.length - .06f);
    check(near(freezeLean(partialLean).value, .5f) && near(freezeLean(fullLean).value, 0) &&
              near(freezeLean(sampleLean(true, 0)).value, 0),
          "Head fade ramps over its configured depth and reaches opaque black beyond it");
    const auto frozenLean = freezeLean(partialLean, 1100);
    check(frozenLean.available && !freezeLean(partialLean, 1101).available &&
              !freezeLean(partialLean, 999).available && near(frozenLean.value, .5f),
          "One frozen visibility survives time between eyes; expiry and clock rollback reject new samples");
    for (float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -1.f,
                      probe.length + .01f})
        check(!sampleLean(false, bad).sampled && !sampleLean(true, bad).sampled,
              "Raw invalid hit distances reject both native hits and misses");
    check(!sampleLean(false, probe.length - .01f).sampled && !sampleLean(true, probe.length).sampled,
          "Native strict-hit status and reported distance must agree");
    for (int field = 0; field < 6; ++field) {
        Request changed = leanRequest;
        if (field == 0)
            ++changed.sequence; // Same input in a different render request.
        if (field == 1)
            ++changed.input.sequence;
        if (field == 2)
            ++changed.session;
        if (field == 3)
            ++changed.reference;
        if (field == 4)
            ++changed.trackingGeneration;
        if (field == 5)
            ++changed.input.reference;
        check(!freezeHeadVisibility(fullLean, leanBody, leanHead, 7, changed, 1050, .05f).available,
              "Replaced request/input/session/reference/epoch cannot reuse head obstruction");
    }
    check(!freezeHeadVisibility(fullLean, leanBody, leanHead, 8, leanRequest, 1050, .05f).available,
          "Native player replacement invalidates the query");
    for (bool rotation : {false, true}) {
        Pose changed = leanBody;
        if (rotation)
            changed.q.x = .00001f;
        else
            changed.p.x += .00001f;
        check(!freezeHeadVisibility(fullLean, changed, leanHead, 7, leanRequest, 1050, .05f).available,
              "Even tiny body movement or rotation rejects stale geometry without an invented tolerance");
    }
    Pose changedHead = leanHead;
    changedHead.p.y += .00001f;
    check(!freezeHeadVisibility(fullLean, leanBody, changedHead, 7, leanRequest, 1050, .05f).available,
          "Changed physical head target cannot reuse a previous query");
    HeadObstruction signedZero = fullLean;
    signedZero.body.q.x = -0.f;
    check(freezeLean(signedZero).available, "Numerical pose equality accepts signed zero");
    for (unsigned hand = 0; hand < 2; ++hand) {
        leanRequest.input.handValid[hand] = 0;
        leanRequest.input.buttons[hand] = Wheel;
    }
    check(freezeLean(fullLean).available,
          "Head-query matching does not depend on tracked hands or weapon wheel input");
    check(freezeLean(fullLean).value == 0 && !freezeLean({}).available && freezeLean({}).value == 1 &&
              freezeLean(fullLean).value == 0,
          "Hit/unavailable/hit schedules explicitly clear an unavailable presentation instead of persisting "
          "it");
    check(!VrSettings{}.headFade, "Experimental head fade remains opt-in by default");
    uint8_t leftColors[]{10, 80, 200, 0, 255, 32, 16, 100};
    uint8_t rightColors[]{10, 80, 200, 0, 255, 32, 16, 100};
    const auto dimming = makeOpaqueDimming(frozenLean.value);
    dimOpaquePixels(leftColors, 2, dimming);
    dimOpaquePixels(rightColors, 2, dimming);
    check(std::equal(std::begin(leftColors), std::end(leftColors), std::begin(rightColors)) &&
              leftColors[0] == 5 && leftColors[1] == 40 && leftColors[2] == 100 && leftColors[3] == 255 &&
              leftColors[4] == 128 && leftColors[7] == 255,
          "The shared fade dims RGB identically in both eyes and keeps alpha opaque");
    bgraToRgba(rightColors, rightColors, 2);
    check(rightColors[0] == leftColors[2] && rightColors[1] == leftColors[1] &&
              rightColors[2] == leftColors[0] && rightColors[3] == 255,
          "The same fading remains correct through host RGBA conversion");
    dimOpaquePixels(leftColors, 2, makeOpaqueDimming(0));
    check(leftColors[0] == 0 && leftColors[2] == 0 && leftColors[3] == 255,
          "Full obstruction blackens world pixels without transparent leakage");
    dimOpaquePixels(rightColors, 2, makeOpaqueDimming(std::numeric_limits<float>::quiet_NaN()));
    check(rightColors[0] == 100 && rightColors[3] == 255,
          "Invalid visibility cannot blacken an unqueried world");
    std::cout
        << "UI comfort, lifecycle, wheel layout, menu recovery, grip, laser and head fade checks passed\n";
}
