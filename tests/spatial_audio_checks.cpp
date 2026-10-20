// Occlusion tracking for spatial audio: tracing cadence and budget, world
// positions from the listener, own sounds left dry, smoothing and filters.
#include "common/spatial_audio.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <algorithm>
using namespace ss2vr;
using namespace ss2vr::spatial_audio;

int main() {
    Tracker t;
    // The listener 10 m along x, turned 90 degrees left (facing -x).
    const float h = std::sqrt(.5f);
    const Pose listener{{0, h, 0, h}, {10, 1.7f, 0}};
    t.position(1, {0, 0, -5});  // 5 m ahead
    t.position(2, {.2f, -.3f, 0}); // the player's own (at the head)
    for (uint32_t id = 10; id < 30; ++id) t.position(id, {1, 0, float(id)});
    uint32_t ids[TracesPerFrame];
    Vec3 ends[TracesPerFrame];
    {
        // Ahead of a listener facing -x: 5 m further along -x.
        Tracker one;
        one.position(1, {0, 0, -5});
        assert(one.due(listener, 1000, ids, ends, TracesPerFrame) == 1 && ids[0] == 1);
        assert(std::abs(ends[0].x - 5) < 1e-4f && std::abs(ends[0].y - 1.7f) < 1e-4f && std::abs(ends[0].z) < 1e-4f);
    }
    unsigned n = t.due(listener, 1000, ids, ends, TracesPerFrame);
    assert(n == TracesPerFrame);
    for (unsigned i = 0; i < n; ++i) { assert(ids[i] != 2); t.traced(ids[i], false, 0, 1, 1000); } // Own: never traced.
    // The rest are due next; the traced ones wait 0.2 s.
    uint32_t first[TracesPerFrame];
    std::copy(ids, ids + n, first);
    n = t.due(listener, 1050, ids, ends, TracesPerFrame);
    assert(n == TracesPerFrame);
    for (unsigned i = 0; i < n; ++i) {
        for (uint32_t f : first) assert(ids[i] != f);
        t.traced(ids[i], false, 0, 1, 1050);
    }
    n = t.due(listener, 1100, ids, ends, TracesPerFrame);
    assert(n == 5); // 21 traceable sounds in all.
    for (unsigned i = 0; i < n; ++i) t.traced(ids[i], false, 0, 1, 1100);
    assert(t.due(listener, 1150, ids, ends, TracesPerFrame) == 0);
    assert(t.due(listener, 1200, ids, ends, TracesPerFrame) == TracesPerFrame);
    // A wall 2 m out occludes the sound 5 m out; a hit at the sound itself does not.
    t.traced(1, true, 2, 5, 1300);
    assert(t.find(1)->target == 1);
    t.traced(10, true, 9.6f, 10, 1300);
    assert(t.find(10)->target == 0);
    // Smoothing: about 63% after 75 ms, nearly all after 0.4 s.
    t.step(.075f);
    assert(std::abs(t.find(1)->occlusion - .632f) < .01f);
    t.step(.4f);
    assert(t.find(1)->occlusion > .99f);
    const Filter open = occlusionFilter(0), shut = occlusionFilter(1);
    assert(open.gain == 1 && open.gainHF == 1 && open.send == 1);
    assert(shut.gain < .7f && shut.gainHF < .2f && shut.send < .8f && shut.gainHF < shut.gain);
    // Moving close turns a sound into the player's own: dry.
    t.position(1, {0, 0, -.3f});
    assert(t.find(1)->target == 0);
    t.remove(1);
    assert(!t.find(1));
    std::puts("spatial audio checks passed");
}
