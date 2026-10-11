#pragma once
#include "math.hpp"
#include <cmath>
#include <cstdint>

namespace ss2vr {
// The tracking origin is captured when gameplay first becomes active. If the
// headset was lying still on a desk at that moment, the origin is far below the
// user's real head and the view would float above the player. A worn headset is
// never perfectly still for two seconds, so a motionless capture marks the origin
// provisional; once that headset has moved away and settled, recentre once.
struct WearCalibration {
    enum class State : uint8_t { Idle, Observing, Provisional };
    State state = State::Idle;
    Pose captured{};
    uint64_t capturedAt = 0, settledSince = 0;
    Vec3 settledAt{};

    static constexpr uint64_t StillWindowMs = 2000, SettleWindowMs = 1000;
    static constexpr float StillDistance = .0025f, StillCosine = .9999966f; // ~0.3 degrees.
    static constexpr float AwayDistance = .25f, SettleDistance = .04f;

    void reset() { *this = {}; }
    // Call when the origin is captured from this head pose.
    void captureOrigin(const Pose &head, uint64_t now) {
        state = finite(head) ? State::Observing : State::Idle;
        captured = head;
        capturedAt = now;
        settledSince = 0;
    }
    // Feed every head sample. Returns true exactly once, when a provisional origin
    // should be recaptured from the current head pose.
    bool sample(const Pose &head, uint64_t now, const Vec3 &origin) {
        if (!finite(head) || now < capturedAt) return false;
        if (state == State::Observing) {
            const Vec3 d = head.p - captured.p;
            const float cosine = std::abs(head.q.x * captured.q.x + head.q.y * captured.q.y +
                                          head.q.z * captured.q.z + head.q.w * captured.q.w);
            if (std::sqrt(dot(d, d)) > StillDistance || cosine < StillCosine)
                state = State::Idle; // Moving: the headset was being worn.
            else if (now - capturedAt >= StillWindowMs)
                state = State::Provisional;
            return false;
        }
        if (state != State::Provisional) return false;
        const Vec3 away = head.p - origin;
        if (std::sqrt(dot(away, away)) < AwayDistance) {
            settledSince = 0;
            return false;
        }
        const Vec3 drift = head.p - settledAt;
        if (!settledSince || std::sqrt(dot(drift, drift)) > SettleDistance) {
            settledSince = now ? now : 1;
            settledAt = head.p;
            return false;
        }
        if (now - settledSince < SettleWindowMs) return false;
        state = State::Idle;
        return true;
    }
};
} // namespace ss2vr
