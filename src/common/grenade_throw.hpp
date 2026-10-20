#pragma once
#include "math.hpp"
#include <cmath>
#include <cstdint>

// Immersive hand grenades. The grenade button puts a grenade in a hand (the game
// passes the right one, its weapon put away meanwhile);
// a throwing motion of that hand (fast and away from the body) releases it, and
// it flies toward where the other hand's laser points. Pressing the button again
// puts the grenade away. Positions are in tracking space (metres), times in ms.
namespace ss2vr {
struct GrenadeThrow {
    enum class State : uint8_t { Idle, Held, Launching };
    static constexpr float ThrowSpeed = 1.6f;      // Hand speed (m/s) that counts as a throw.
    static constexpr float ThrowForward = .25f;    // Minimum cosine between the motion and forward.
    static constexpr uint64_t Window = 90;         // Speed is measured over this span.
    static constexpr uint64_t LaunchTimeout = 300; // The game must take the throw within this.
    static constexpr unsigned Samples = 16;
    State state = State::Idle;
    unsigned hand = 0;
    uint64_t launchedAt = 0;
    struct Sample { Vec3 p; uint64_t t; };
    Sample history[Samples]{};
    unsigned count = 0, next = 0;

    bool holding() const { return state != State::Idle; }
    void clear() { state = State::Idle; count = next = 0; }
    // The grenade button: take a grenade into `offHand`, or put it away.
    void press(unsigned offHand) {
        if (state == State::Idle) { state = State::Held; hand = offHand & 1; count = next = 0; }
        else if (state == State::Held) clear();
    }
    // The hand's velocity over the last Window, from the recorded samples.
    bool velocity(Vec3 &out) const {
        if (count < 2) return false;
        const Sample &last = history[(next + Samples - 1) % Samples];
        for (unsigned i = 2; i <= count; ++i) {
            const Sample &old = history[(next + Samples - i) % Samples];
            if (last.t - old.t >= Window || i == count) {
                if (last.t <= old.t) return false;
                out = (last.p - old.p) * (1000.f / float(last.t - old.t));
                return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
            }
        }
        return false;
    }
    // One poll of the holding hand. `forward` is the body's horizontal forward.
    // Returns true on the poll the throw starts.
    bool update(bool tracked, Vec3 handPosition, uint64_t now, Vec3 forward) {
        if (state == State::Launching) {
            if (now - launchedAt > LaunchTimeout) clear();
            return false;
        }
        if (state != State::Held) return false;
        if (!tracked || !std::isfinite(handPosition.x) || !std::isfinite(handPosition.y) ||
            !std::isfinite(handPosition.z)) { count = next = 0; return false; }
        if (count && history[(next + Samples - 1) % Samples].t >= now) return false;
        history[next] = {handPosition, now};
        next = (next + 1) % Samples;
        if (count < Samples) ++count;
        Vec3 v;
        if (!velocity(v)) return false;
        const float speed = std::sqrt(dot(v, v));
        const float flat = std::sqrt(forward.x * forward.x + forward.z * forward.z);
        if (!(speed >= ThrowSpeed) || !(flat > 1e-3f)) return false;
        const Vec3 f{forward.x / flat, 0, forward.z / flat};
        if (dot(v, f) < ThrowForward * speed) return false;
        state = State::Launching;
        launchedAt = now;
        return true;
    }
    // The game threw it (or refused, e.g. no grenades left).
    void thrown() { clear(); }
};
// The throw's pose for the game: at the grenade, its -Z toward `target` (the
// laser's end), level (no roll). Falls back to `fallback` as the direction.
inline Pose grenadeLaunchPose(Vec3 from, Vec3 target, bool targetValid, Vec3 fallback) {
    Vec3 d = targetValid ? target - from : fallback;
    float n = std::sqrt(dot(d, d));
    if (!std::isfinite(n) || n < 1e-4f) { d = fallback; n = std::sqrt(dot(d, d)); }
    if (!std::isfinite(n) || n < 1e-4f) { d = {0, 0, -1}; n = 1; }
    d = d * (1 / n);
    const float heading = std::atan2(-d.x, -d.z);
    const float pitch = std::asin(std::fmax(-1.f, std::fmin(1.f, d.y)));
    const Quat up{std::sin(pitch / 2), 0, 0, std::cos(pitch / 2)};
    return {normalize(multiply(yaw(heading), up)), from};
}
} // namespace ss2vr
