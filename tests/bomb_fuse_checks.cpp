#include "common/bomb_fuse.hpp"
#include <cassert>
#include <cstdio>
using namespace ss2vr;
int main() {
    const Pose bomb{{0, 0, 0, 1}, {.2f, 1.2f, -.4f}};
    const Pose apart{{0, 0, 0, 1}, {-.25f, 1.2f, -.4f}};
    const Pose atFuse{{0, 0, 0, 1}, BombFuse::fuse(bomb) - BombFuse::Flame};
    BombFuse f;
    // Struck far from the bomb: nothing; unstruck at the fuse: nothing.
    assert(!f.update(true, 1, apart, bomb, 0));
    f.clear();
    assert(!f.update(true, 0, atFuse, bomb, 10));
    // Struck and brought to the fuse: fires for Hold, once.
    assert(f.update(true, 1, atFuse, bomb, 20));
    assert(f.update(true, 1, atFuse, bomb, 20 + BombFuse::Hold - 1));
    assert(!f.update(true, 1, atFuse, bomb, 20 + BombFuse::Hold));
    assert(!f.update(true, 1, atFuse, bomb, 2000));
    // Released and struck again: lights again.
    assert(!f.update(true, 0, atFuse, bomb, 2100));
    assert(f.update(true, 1, atFuse, bomb, 2200));
    // Lost tracking: the fire under way finishes; back at the fuse with the
    // trigger still held, no second bomb until it is struck again.
    assert(f.update(false, 1, atFuse, bomb, 2300));
    assert(!f.update(false, 1, atFuse, bomb, 2200 + BombFuse::Hold));
    assert(!f.update(true, 1, atFuse, bomb, 2700));
    f.clear(); // The bomb out of the hand (or controls reset) does not re-arm it either.
    assert(!f.update(true, 1, atFuse, bomb, 2800));
    assert(!f.update(true, 0, atFuse, bomb, 2900));
    // Untracked, a struck lighter at the fuse lights nothing; tracked again, it does.
    assert(!f.update(false, 1, atFuse, bomb, 3000));
    assert(f.update(true, 1, atFuse, bomb, 3100));
    // The fuse turns with the bomb hand.
    const Pose turned{normalize(Quat{0, 0, std::sin(.5f), std::cos(.5f)}), bomb.p};
    const Vec3 a = BombFuse::fuse(bomb), b = BombFuse::fuse(turned);
    assert(dot(a - b, a - b) > 1e-3f);
    std::puts("Bomb fuse checks passed");
}
