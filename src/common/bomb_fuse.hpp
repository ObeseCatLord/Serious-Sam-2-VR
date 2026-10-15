#pragma once
#include "math.hpp"
#include <cmath>
#include <cstdint>

// Lighting the serious bomb by hand. The bomb's model is two hands: the bomb in
// one, a lighter in the other (split onto the two controllers, remote_render
// setSplitModel). The lighter hand's trigger strikes the lighter; bringing its
// flame to the fuse on top of the bomb lights it, which fires the bomb (the
// native fire, held long enough for the game to take it). Releasing the
// trigger puts the lighter out; it must be struck again for another bomb.
// Poses are controller grip poses in tracking space (metres), times in ms.
namespace ss2vr {
struct BombFuse {
    static constexpr float Strike = .5f;  // Lighter trigger that strikes it.
    static constexpr float Reach = .12f;  // Flame-to-fuse distance that lights the fuse.
    static constexpr uint64_t Hold = 400; // Native fire is held this long.
    // The flame above the lighter hand's thumb, and the fuse on top of the bomb,
    // in each controller's frame (x right, y up, -z forward).
    static constexpr Vec3 Flame{0, .05f, -.05f};
    static constexpr Vec3 Fuse{-.03f, .15f, -.05f};
    bool lit = false, spent = false;
    uint64_t firedAt = 0;
    bool firing = false;

    static Vec3 flame(const Pose &lighter) { return lighter.p + rotate(normalize(lighter.q), Flame); }
    static Vec3 fuse(const Pose &bomb) { return bomb.p + rotate(normalize(bomb.q), Fuse); }
    // A spent lighter stays spent: only releasing its trigger re-arms it.
    void clear() { lit = firing = false; }
    // Returns whether the native fire is pressed this frame. Lost tracking lights
    // nothing, but neither re-arms a spent lighter (its trigger still held, the
    // flame lit a second bomb when tracking came back) nor cuts a fire short.
    bool update(bool tracked, float lighterTrigger, const Pose &lighter, const Pose &bomb, uint64_t now) {
        lit = lighterTrigger > Strike;
        if (!lit) spent = false;
        if (firing && now - firedAt >= Hold) firing = false;
        if (tracked && lit && !spent && !firing) {
            const Vec3 d = flame(lighter) - fuse(bomb);
            if (std::isfinite(d.x + d.y + d.z) && dot(d, d) < Reach * Reach) {
                firing = spent = true;
                firedAt = now;
            }
        }
        return firing;
    }
};
} // namespace ss2vr
