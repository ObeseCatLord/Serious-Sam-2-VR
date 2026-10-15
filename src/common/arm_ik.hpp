#pragma once
#include "model_tree.hpp"
#include "two_hand.hpp"
#include <algorithm>
#include <cmath>

// Two-bone arm IK for a remote VR player's body (networked VRIK). The engine's
// skinned bones are posed by the native animation; this turns the upper arm
// about the shoulder and the forearm about the elbow so the wrist reaches a
// target, keeping the elbow on the side the animation bent it to, and
// optionally turns the hand to a target orientation. Frames are model-space
// 3x4 matrices with the joint at the translation (posed bone = canonical
// skinning matrix x bind, see remote_render.cpp). The result is one rigid
// model-space delta per level: bones under the hand take `hand`, other bones
// under the forearm `lower`, other bones under the upper arm `upper`.
namespace ss2vr {
struct ArmPose {
    Matrix34 upper, lower, hand;
};
struct ArmDeltas {
    Matrix34 upper, lower, hand;
};
inline Vec3 matrixTranslation(const Matrix34 &m) { return {m.m[3], m.m[7], m.m[11]}; }
inline Vec3 matrixApply(const Matrix34 &m, Vec3 p) {
    return {m.m[0] * p.x + m.m[1] * p.y + m.m[2] * p.z + m.m[3], m.m[4] * p.x + m.m[5] * p.y + m.m[6] * p.z + m.m[7],
            m.m[8] * p.x + m.m[9] * p.y + m.m[10] * p.z + m.m[11]};
}
// Rotation `q` about point `p`.
inline Matrix34 rotationAbout(Quat q, Vec3 p) {
    q = normalize(q);
    return matrix(Pose{q, p - rotate(q, p)});
}
inline Matrix34 rotationPart(const Matrix34 &m) {
    Matrix34 r = m;
    r.m[3] = r.m[7] = r.m[11] = 0;
    return r;
}
inline bool solveArm(const ArmPose &arm, Vec3 target, const Matrix34 *handTarget, ArmDeltas &out) {
    const Vec3 s = matrixTranslation(arm.upper), e = matrixTranslation(arm.lower), w = matrixTranslation(arm.hand);
    const auto length = [](Vec3 v) { return std::sqrt(dot(v, v)); };
    const float a = length(e - s), b = length(w - e);
    Vec3 d = target - s;
    float dl = length(d);
    if (!finiteMatrix(arm.upper) || !finiteMatrix(arm.lower) || !finiteMatrix(arm.hand) || !std::isfinite(dl) ||
        !(a > 1e-3f) || !(b > 1e-3f) || !(dl > 1e-4f) || !std::isfinite(target.x + target.y + target.z))
        return false;
    d = d * (1 / dl);
    dl = std::clamp(dl, std::abs(a - b) + 1e-4f, a + b - 1e-4f);
    const Vec3 reach = s + d * dl;
    // Bend plane: the animation's elbow, else down.
    // (Each fallback is that direction less its part along d.)
    Vec3 pole = (e - s) - d * dot(d, e - s);
    if (length(pole) < 1e-4f) pole = Vec3{0, -1, 0} + d * d.y;
    if (length(pole) < 1e-4f) pole = Vec3{1, 0, 0} - d * d.x;
    pole = pole * (1 / length(pole));
    const float cosA = std::clamp((a * a + dl * dl - b * b) / (2 * a * dl), -1.f, 1.f);
    const Vec3 elbow = s + d * (a * cosA) + pole * (a * std::sqrt(std::max(0.f, 1 - cosA * cosA)));
    const Quat q1 = rotationBetween((e - s) * (1 / a), (elbow - s) * (1 / a));
    out.upper = rotationAbout(q1, s);
    const Vec3 w1 = matrixApply(out.upper, w);
    const Vec3 forearm = w1 - elbow, toReach = reach - elbow;
    const float fl = length(forearm), tl = length(toReach);
    if (!(fl > 1e-4f) || !(tl > 1e-4f)) return false;
    const Quat q2 = rotationBetween(forearm * (1 / fl), toReach * (1 / tl));
    out.lower = affineMultiply(rotationAbout(q2, elbow), out.upper);
    out.hand = out.lower;
    if (handTarget) {
        // Turn the hand (now at `reach`) from its moved orientation to the target's.
        const Matrix34 moved = rotationPart(affineMultiply(out.lower, arm.hand));
        Matrix34 inverseMoved;
        if (!affineInverse(moved, inverseMoved)) return false;
        const Matrix34 turn = affineMultiply(rotationPart(*handTarget), inverseMoved);
        Matrix34 about = turn;
        const Vec3 shifted = reach - matrixApply(turn, reach);
        about.m[3] = shifted.x; about.m[7] = shifted.y; about.m[11] = shifted.z;
        out.hand = affineMultiply(about, out.lower);
    }
    // A turn rotationBetween cannot make (about half a turn: it gives none) left
    // the upper arm unturned while the forearm turned about the computed elbow,
    // up to half a metre from the real joint: such a pose stays native instead.
    if (length(matrixApply(out.upper, e) - elbow) > 1e-3f || length(matrixApply(out.lower, w) - reach) > 1e-3f)
        return false;
    return finiteMatrix(out.upper) && finiteMatrix(out.lower) && finiteMatrix(out.hand);
}
} // namespace ss2vr
