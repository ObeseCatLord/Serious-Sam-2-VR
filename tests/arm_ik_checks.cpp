#include "common/arm_ik.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace ss2vr;
static bool near(Vec3 a, Vec3 b, float e = 1e-3f) {
    return std::abs(a.x - b.x) < e && std::abs(a.y - b.y) < e && std::abs(a.z - b.z) < e;
}
static float len(Vec3 v) { return std::sqrt(dot(v, v)); }
int main() {
    // A right arm hanging down from a shoulder at (0.2, 1.4, 0): upper arm 0.28 m,
    // forearm 0.26 m, elbow bent slightly backward (+Z).
    const Vec3 s{.2f, 1.4f, 0}, e{.2f, 1.12f, .02f}, w{.2f, .86f, -.02f};
    ArmPose arm{matrix(Pose{{0, 0, 0, 1}, s}), matrix(Pose{{0, 0, 0, 1}, e}), matrix(Pose{{0, 0, 0, 1}, w})};
    const float a = len(e - s), b = len(w - e);
    // Reach forward to (0.2, 1.3, -0.4).
    const Vec3 target{.2f, 1.3f, -.4f};
    ArmDeltas d;
    assert(solveArm(arm, target, nullptr, d));
    const Vec3 elbow = matrixApply(d.upper, e), wrist = matrixApply(d.lower, w);
    assert(near(wrist, target));
    assert(std::abs(len(elbow - s) - a) < 1e-3f && std::abs(len(wrist - elbow) - b) < 1e-3f); // Rigid bones.
    assert(near(matrixApply(d.upper, s), s) && near(matrixApply(d.lower, e), elbow)); // The forearm turns about the moved elbow.
    assert(near(matrixApply(d.hand, w), target));
    // Elbow stays on the bend side (the animation bent it toward +Z).
    const Vec3 dir = (target - s) * (1 / len(target - s));
    const Vec3 offAxis = (elbow - s) - dir * dot(dir, elbow - s);
    assert(offAxis.z > 0 || offAxis.y < 0);
    // Out of reach: the arm straightens toward the target.
    const Vec3 far{.2f, 1.4f, -2};
    assert(solveArm(arm, far, nullptr, d));
    const Vec3 stretched = matrixApply(d.lower, w);
    assert(std::abs(len(stretched - s) - (a + b)) < 2e-3f && stretched.z < -.5f);
    // Hand orientation: the hand frame ends at the target rotation, at the wrist.
    const Matrix34 handTarget = matrix(Pose{normalize(Quat{.3f, 0, 0, .95f}), {}});
    assert(solveArm(arm, target, &handTarget, d));
    const Matrix34 posed = affineMultiply(d.hand, arm.hand);
    for (unsigned i : {0u, 1u, 2u, 4u, 5u, 6u, 8u, 9u, 10u}) assert(std::abs(posed.m[i] - handTarget.m[i]) < 1e-3f);
    assert(near(matrixTranslation(posed), target));
    // A straight arm hanging down reaching straight down: the bend plane falls
    // back to a direction across the arm; the bones stay rigid, the wrist reaches.
    {
        const Vec3 se{.2f, 1.12f, 0}, sw{.2f, .86f, 0};
        ArmPose straight{matrix(Pose{{0, 0, 0, 1}, s}), matrix(Pose{{0, 0, 0, 1}, se}), matrix(Pose{{0, 0, 0, 1}, sw})};
        const Vec3 below{.2f, 1.f, 0};
        assert(solveArm(straight, below, nullptr, d));
        const Vec3 bentElbow = matrixApply(d.upper, se), reached = matrixApply(d.lower, sw);
        assert(near(reached, below) && std::abs(len(bentElbow - s) - .28f) < 1e-3f &&
               std::abs(len(reached - bentElbow) - .26f) < 1e-3f);
        // Straight up at full reach needs half a turn of the upper arm, which
        // rotationBetween does not make: refused (bones stay native), not detached.
        assert(!solveArm(straight, {.2f, 1.94f, 0}, nullptr, d));
    }
    // Degenerate input is refused.
    ArmPose folded = arm;
    folded.lower = folded.upper;
    assert(!solveArm(folded, target, nullptr, d));
    assert(!solveArm(arm, {NAN, 0, 0}, nullptr, d));
    std::puts("Arm IK checks passed");
}
