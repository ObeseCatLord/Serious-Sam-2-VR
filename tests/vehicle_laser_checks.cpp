#include "common/lasers.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace ss2vr;
static void check(bool ok, const char *message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static bool near(Vec3 a, Vec3 b, float tolerance = .00001f) {
    return dot(a-b, a-b) < tolerance*tolerance;
}
int main() {
    const Pose origin{normalize(multiply(yaw(.6f), Quat{.12f, .02f, .15f, .98f})), {8, 2, -11}};
    const Vec3 forward = rotate(origin.q, {0, 0, -1});
    Pose result;
    check(nativeLaserPose(origin, forward*17, result) && near(result.p, origin.p) &&
          near(rotate(result.q, {1,0,0}), rotate(origin.q, {1,0,0})) &&
          near(rotate(result.q, {0,1,0}), rotate(origin.q, {0,1,0})),
          "An already aligned native beam preserves actual XYZ origin and native roll");
    for (const Vec3 direction : {Vec3{1,2,-3}, Vec3{0,1,0}, Vec3{0,-1,0},
                                 Vec3{.001f,.002f,-1}, forward*-1,
                                 forward*-1 + rotate(origin.q,{.0005f,0,0})}) {
        check(nativeLaserPose(origin, direction, result) && near(result.p, origin.p) &&
              near(rotate(result.q, {0,0,-1}), direction*(1/std::sqrt(dot(direction,direction)))),
              "Full three-axis original direction, including opposite and vertical rays, is retained");
    }
    const Pose before = result;
    check(!nativeLaserPose(origin, {}, result) && near(result.p, before.p) &&
          near(rotate(result.q,{0,0,-1}),rotate(before.q,{0,0,-1})),
          "Zero direction rejects without publishing a fabricated native pose");
    check(!nativeLaserPose(origin, {std::numeric_limits<float>::quiet_NaN(),0,1}, result),
          "Nonfinite native direction rejects rather than normalizing to identity");
    auto invalid = origin; invalid.q = {0,0,0,0};
    check(!nativeLaserPose(invalid, forward, result), "Degenerate native quaternion rejects");
    invalid = origin; invalid.p.x = std::bit_cast<float>(uint32_t(0x7f61b1e6));
    check(!nativeLaserPose(invalid, forward, result), "Native unset sentinel rejects");
    NativeModelScratch scratch;
    check(scratch.idle(), "Retired native model scratch admits the query phase");
    for (auto &count : scratch.counts) {
        count = 1;
        check(!scratch.idle(), "Every shared native record count blocks destructive evaluation");
        count = 0;
    }
    scratch.evaluated = 0x1234;
    check(!scratch.idle(), "Outstanding native evaluation blocks even empty record arrays");
    LaserAim samples[2];
    LaserFrame frame;
    frame.kind = LaserSourceKind::Vehicle;
    frame.vehicle = {{4,12,2,3,true},21,22,23,24,25,26,27,28};
    frame.owner = 4; frame.weapon[1] = 12; frame.generation = 3;
    frame.sequence = 10; frame.requestSequence = 30; frame.now = 200; frame.handValid[1] = true;
    samples[1] = {origin,{},origin.p+forward*10,4,12,3,10,100,true,true};
    samples[1].kind = frame.kind;
    samples[1].vehicle = frame.vehicle;
    samples[1].requestSequence = frame.requestSequence;
    auto frozen = freezeLaserPair(samples,frame);
    check(!frozen[0].valid && frozen[1].valid, "One original mounted source freezes for both eyes");
    frame.wheel[1] = frame.selecting[1] = true;
    check(laserEligible(samples[1],frame,1), "Stale handheld selection state does not suppress mounted aim");
    const auto originalFrame = frame;
    auto rejects = [&] {
        check(!laserEligible(samples[1],frame,1), "Callback/source replacement rejects stale mounted sample");
        frame = originalFrame;
    };
    frame.vehicle.rider.seat++; rejects();
    frame.vehicle.rider.state = 0; rejects();
    frame.vehicle.rider.ride++; rejects();
    frame.vehicle.model++; rejects();
    frame.vehicle.instance++; rejects();
    frame.vehicle.resource++; rejects();
    frame.vehicle.config++; rejects();
    frame.vehicle.action++; rejects();
    frame.vehicle.blast++; rejects();
    frame.vehicle.attachment++; rejects();
    frame.vehicle.mechanism++; rejects();
    frame.requestSequence++; rejects();
    frame.generation++; rejects();
    frame.handValid[1] = false; rejects();
    frame.kind = LaserSourceKind::Handheld; rejects();
    frame.now = 201; rejects();
    check(frozen[1].valid && frozen[1].vehicle == originalFrame.vehicle,
          "Once frozen both eyes keep the same admitted values independent of later mutations");
    {
        // A seated beam kept relative to its body anchor survives a later request and
        // a turned anchor (the turret follows the head), but not a new source or age.
        LaserAim relative = samples[1];
        relative.relative = true;
        relative.local = Pose{{0, 0, 0, 1}, {0, .3f, -.5f}};
        relative.localEnd = {0, .3f, -10.5f};
        relative.distance = 10;
        LaserFrame later = originalFrame;
        later.requestSequence += 3;
        later.sequence += 2;
        later.body = Pose{yaw(.7f), {1, 0, 0}};
        check(laserEligible(relative, later, 1), "A seated relative beam survives a later request and a turned anchor");
        later.vehicle.mechanism++;
        check(!laserEligible(relative, later, 1), "A seated relative beam never moves to another source");
        later = originalFrame;
        later.now = 201;
        check(!laserEligible(relative, later, 1), "A seated relative beam still expires after 100 ms");
        relative.localEnd.x = std::numeric_limits<float>::quiet_NaN();
        check(!laserEligible(relative, originalFrame, 1), "A seated relative beam needs a finite end");
    }
    std::cout << "Native laser pose, scratch, source and two-eye admission checks passed; no runtime executed\n";
}
