#include "common/ride_handles.hpp"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace ss2vr;
static Vec3 apply(const Matrix34 &m, Vec3 p) {
    return {m.m[0] * p.x + m.m[1] * p.y + m.m[2] * p.z + m.m[3], m.m[4] * p.x + m.m[5] * p.y + m.m[6] * p.z + m.m[7],
            m.m[8] * p.x + m.m[9] * p.y + m.m[10] * p.z + m.m[11]};
}
static bool near(Vec3 a, Vec3 b) { return std::abs(a.x - b.x) + std::abs(a.y - b.y) + std::abs(a.z - b.z) < 1e-4f; }
int main() {
    // A palette found among constant rows, not elsewhere.
    const Matrix34 palette{{.5f, 0, 0, 1, 0, .5f, 0, 2, 0, 0, .5f, 3}};
    std::vector<std::array<float, 4>> rows(10, std::array<float, 4>{9, 9, 9, 9});
    for (unsigned i = 0; i < 3; ++i) for (unsigned j = 0; j < 4; ++j) rows[4 + i][j] = palette.m[i * 4 + j];
    assert(findPaletteRows(rows, palette) == 4);
    rows[5][1] += .01f;
    assert(findPaletteRows(rows, palette) == -1);
    // A quarter turn about +Y through (1,0,0): (1,1,0) stays, (2,0,0) goes to (1,0,-1); then the palette applies.
    const Matrix34 turned = turnedPalette(palette, {1, 0, 0}, {0, 1, 0}, 1.5707963f);
    assert(near(apply(turned, {1, 1, 0}), apply(palette, {1, 1, 0})));
    assert(near(apply(turned, {2, 0, 0}), apply(palette, {1, 0, -1})));
    // No turn: the palette itself.
    assert(near(apply(turnedPalette(palette, {1, 2, 3}, {0, 0, 1}, 0), {4, 5, 6}), apply(palette, {4, 5, 6})));
    assert(rideHandleLayoutProfile(2741, 2806) == RideHandleProfile::Fighter);
    assert(rideHandleLayoutProfile(2464, 2626) == RideHandleProfile::Saucer);
    assert(rideHandleFirstTriangle(RideHandleProfile::Fighter) == 1788);
    // Two grips side by side (x = +-0.2): a yoke rolling about -Z through their midpoint.
    RideHandleGeometry g;
    g.copied = true;
    for (unsigned hand = 0; hand < 2; ++hand) {
        auto &h = g.handles[hand];
        h.vertices = 12;
        for (unsigned v = 0; v < 12; ++v)
            h.positions[v] = {(hand ? -.2f : .2f) + ((v & 1) ? .05f : -.05f), 1.3f + .1f * float(v / 2) / 5.f, -.19f};
    }
    RideHandleRig rig;
    assert(fitRideHandleRig(g, rig) && rig.valid);
    assert(rig.axis[0].z == -1 && std::abs(rig.pivot[0].x) < 1e-4f && std::abs(rig.pivot[1].y - 1.35f) < .01f);
    // A right turn lowers the right handle (+X) and raises the left one.
    const Matrix34 identity{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}};
    const Matrix34 right = turnedPalette(identity, rig.pivot[0], rig.axis[0], .3f);
    assert(apply(right, {.2f, 1.35f, -.19f}).y < 1.3f && apply(right, {-.2f, 1.35f, -.19f}).y > 1.4f);
    // Two handles one above the other: not a pair.
    for (unsigned v = 0; v < 12; ++v) g.handles[1].positions[v] = g.handles[0].positions[v] + Vec3{0, .5f, 0};
    assert(!fitRideHandleRig(g, rig));
    std::puts("ride handles ok");
}
