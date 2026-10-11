#pragma once
#include "math.hpp"
#include <array>
#include <cmath>

namespace ss2vr {
inline bool nativeWaterInputMode(uint32_t movementFlags, uint32_t puppetPose) {
    // MovingIn3DArea also accepts flight (bit 1). That is not water admission.
    return !(movementFlags & 2u) && (movementFlags & 4u) &&
        (puppetPose == 3u || puppetPose == 4u);
}
inline bool swimmingJoystickIdle(Vec3 value) {
    return value.x == 0.f && value.y == 0.f && value.z == 0.f;
}
inline bool swimmingGripStreamsMatch(const uint32_t (&captured)[2], const uint32_t (&current)[2]) {
    return captured[0] && captured[1] && captured[0] == current[0] && captured[1] == current[1];
}
// Optional pull-stroke input. Hands are sampled relative to head translation in
// tracking LOCAL space, so moving the whole tracked rig is not a swim stroke.
// It produces a bounded forward control value, never velocity or displacement.
struct SwimmingStrokes {
    uint32_t player = 0, generation = 0, session = 0, reference = 0, pose = 0;
    uint64_t sequence = 0, tick = 0;
    std::array<Vec3, 2> previous{};
    std::array<uint32_t, 2> gripGeneration{};
    bool seeded = false;
    float cached = 0;
    void reset() { *this = {}; }
    float sample(const Input &input, const uint32_t (&gripPoseGeneration)[2],
                 uint32_t owner, uint32_t rig, uint32_t waterPose,
                 uint64_t now, bool enabled) {
        const auto validPosition = [](Pose value) {
            return finite(value) && std::abs(value.p.x) < 100000.f &&
                std::abs(value.p.y) < 100000.f && std::abs(value.p.z) < 100000.f;
        };
        if (!enabled || !owner || !rig || !input.session || !input.sequence || !input.tickMs ||
            !gripPoseGeneration[0] || !gripPoseGeneration[1] ||
            !input.focused || !input.headValid || !validPosition(input.head) ||
            now < input.tickMs || now - input.tickMs > 100 ||
            !input.gripValid[0] || !input.gripValid[1] ||
            !validPosition(input.grip[0]) || !validPosition(input.grip[1]) ||
            ((input.buttons[0] | input.buttons[1]) & (Wheel | WheelOpen | Menu | Recenter)) ||
            (waterPose != 3 && waterPose != 4)) {
            reset(); return 0;
        }
        const auto q = input.head.q;
        const double norm = double(q.x)*q.x+double(q.y)*q.y+double(q.z)*q.z+double(q.w)*q.w;
        if (norm < .99 || norm > 1.01) { reset(); return 0; }
        if (player != owner || generation != rig || session != input.session ||
            reference != input.reference || pose != waterPose ||
            gripGeneration[0] != gripPoseGeneration[0] || gripGeneration[1] != gripPoseGeneration[1]) {
            reset(); player = owner; generation = rig; session = input.session;
            reference = input.reference; pose = waterPose;
            gripGeneration = {gripPoseGeneration[0], gripPoseGeneration[1]};
        }
        if (seeded && input.sequence == sequence && input.tickMs == tick) return cached;
        if (seeded && (input.sequence <= sequence || input.tickMs <= tick)) {
            cached = 0; return 0; // Replays cannot reset the high-water baseline.
        }
        const std::array<Vec3,2> relative{{input.grip[0].p-input.head.p, input.grip[1].p-input.head.p}};
        const bool consecutive = seeded && input.tickMs - tick <= 100;
        const double seconds = consecutive ? double(input.tickMs - tick) / 1000. : 0.;
        const auto before = previous;
        previous = relative; sequence = input.sequence; tick = input.tickMs; seeded = true; cached = 0;
        if (!consecutive) return 0;
        const Vec3 forward = rotate(q, {0,0,-1});
        for (unsigned hand = 0; hand < 2; ++hand) {
            const Vec3 delta = relative[hand] - before[hand];
            const double distance = std::sqrt(double(delta.x)*delta.x+double(delta.y)*delta.y+
                                              double(delta.z)*delta.z);
            if (distance > .5 || distance / seconds > 5.) { cached = 0; return 0; }
            const double pull = -double(dot(delta, forward)) / seconds;
            cached += .5f * float(std::clamp((pull - .2) / 1., 0., 1.));
        }
        return cached;
    }
};
// Convert a desired world direction into the game's current native input basis.
// Columns come from its own GetOperatorMoveDir, including surface pitch rules.
// This only maps controls; native RPC, speed limits, buoyancy and physics remain
// responsible for movement. Unknown/degenerate bases leave the caller unchanged.
inline bool swimmingInputInBasis(const std::array<Vec3, 3> &columns, Vec3 desired, Vec3 &out) {
    auto finiteVector = [](Vec3 v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    };
    if (!finiteVector(desired)) return false;
    double a[3][3]{};
    for (unsigned c = 0; c < 3; ++c) {
        if (!finiteVector(columns[c])) return false;
        a[0][c] = columns[c].x; a[1][c] = columns[c].y; a[2][c] = columns[c].z;
    }
    // The audited native Euler transform is a rotation, never a scale/shear.
    for (unsigned c = 0; c < 3; ++c)
        for (unsigned d = c; d < 3; ++d) {
            double dot = 0;
            for (unsigned r = 0; r < 3; ++r) dot += a[r][c] * a[r][d];
            if (std::abs(dot - (c == d ? 1. : 0.)) > 1e-4) return false;
        }
    const double det = a[0][0] * (a[1][1]*a[2][2]-a[1][2]*a[2][1]) -
        a[0][1] * (a[1][0]*a[2][2]-a[1][2]*a[2][0]) +
        a[0][2] * (a[1][0]*a[2][1]-a[1][1]*a[2][0]);
    if (!std::isfinite(det) || std::abs(det - 1.) > 1e-4) return false;
    const double b[3]{desired.x, desired.y, desired.z};
    double result[3]{};
    for (unsigned c = 0; c < 3; ++c) {
        const unsigned j = (c + 1) % 3, k = (c + 2) % 3;
        result[c] = ((a[1][j]*a[2][k]-a[2][j]*a[1][k])*b[0] +
                     (a[2][j]*a[0][k]-a[0][j]*a[2][k])*b[1] +
                     (a[0][j]*a[1][k]-a[1][j]*a[0][k])*b[2]) / det;
        if (!std::isfinite(result[c]) || std::abs(result[c]) > 4.) return false;
    }
    const Vec3 candidate{float(result[0]), float(result[1]), float(result[2])};
    const double narrowed[3]{candidate.x, candidate.y, candidate.z};
    for (unsigned r = 0; r < 3; ++r) {
        double reconstructed = 0;
        for (unsigned c = 0; c < 3; ++c) reconstructed += a[r][c] * narrowed[c];
        if (std::abs(reconstructed - b[r]) > 1e-5) return false;
    }
    out = candidate;
    return true;
}
} // namespace ss2vr
