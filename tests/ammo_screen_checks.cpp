#include "common/ammo_screen.hpp"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace ss2vr;
static bool near(Vec3 a, Vec3 b, float e = 1e-5f) {
    return std::fabs(a.x - b.x) < e && std::fabs(a.y - b.y) < e && std::fabs(a.z - b.z) < e;
}
static Matrix34 identity() { return {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}}; }
int main() {
    // Places: blank leading places, a lone zero, a fourth place from 1000, clamping.
    const auto digits = [](int32_t count) {
        const auto t = ammoReadoutText(count);
        std::array<int, 4> d{-9, -9, -9, -9};
        for (unsigned i = 0; i != t.places; ++i) d[i] = t.digit[i];
        return d;
    };
    assert((digits(0) == std::array<int, 4>{-1, -1, 0, -9}));
    assert((digits(7) == std::array<int, 4>{-1, -1, 7, -9}));
    assert((digits(40) == std::array<int, 4>{-1, 4, 0, -9}));
    assert((digits(305) == std::array<int, 4>{3, 0, 5, -9}));
    assert((digits(999) == std::array<int, 4>{9, 9, 9, -9}));
    assert((digits(1000) == std::array<int, 4>{1, 0, 0, 0}));
    assert((digits(12345) == std::array<int, 4>{9, 9, 9, 9}));
    assert((digits(-3) == std::array<int, 4>{-1, -1, 0, -9}));
    // Segment masks: 8 lights all seven, 1 the two right ones, 7 adds the top.
    assert(SevenSegmentDigits[8] == 0x7f && SevenSegmentDigits[1] == 0x06 && SevenSegmentDigits[7] == 0x07);
    // Triangles stay inside the panel; lit and unlit segments partition all 21.
    for (int32_t count : {0, 8, 88, 123, 888, 999, 1000}) {
        unsigned layers[AmmoReadoutLayerCount]{};
        bool inside = true;
        ammoReadoutTriangles(count, .046f, .022f, [&](AmmoReadoutLayer layer, float u0, float v0, float u1, float v1,
                                                       float u2, float v2) {
            ++layers[unsigned(layer)];
            for (float c : {u0, v0, u1, v1, u2, v2}) inside = inside && std::isfinite(c) && std::fabs(c) <= 1.0001f;
        });
        // Also on the narrow uzi panel, where four places shrink to fit.
        ammoReadoutTriangles(count, .0136f, .0072f, [&](AmmoReadoutLayer, float u0, float v0, float u1, float v1,
                                                        float u2, float v2) {
            for (float c : {u0, v0, u1, v1, u2, v2}) inside = inside && std::isfinite(c) && std::fabs(c) <= 1.0001f;
        });
        assert(inside);
        assert(layers[0] == 8);                            // Chamfered panel fan.
        assert(layers[1] % 4 == 0 && layers[3] % 4 == 0); // Hexagons: four triangles each.
        assert((layers[1] + layers[3]) / 4 == 7 * ammoReadoutText(count).places); // Seven segments a place.
        assert(layers[2] == layers[3]);                    // A halo under each lit segment.
    }
    {   // 888 lights everything; a blank place shows only ghosts.
        unsigned lit = 0;
        ammoReadoutTriangles(888, .0136f, .0072f, [&](AmmoReadoutLayer l, float, float, float, float, float, float) {
            lit += l == AmmoReadoutLayer::Lit;
        });
        assert(lit == 21 * 4);
        lit = 0;
        ammoReadoutTriangles(1, .0136f, .0072f, [&](AmmoReadoutLayer l, float, float, float, float, float, float) {
            lit += l == AmmoReadoutLayer::Lit;
        });
        assert(lit == 2 * 4);
    }
    // Every weapon with a screen; none for others.
    assert(ammoScreen(4) && ammoScreen(5) && ammoScreen(6) && !ammoScreen(12) && !ammoScreen(-1));
    for (const auto &s : AmmoScreens) {
        assert(std::fabs(dot(s.right, s.up)) < 1e-3f);
        assert(std::fabs(dot(s.up, s.up) - 1) < 1e-3f && std::fabs(dot(s.right, s.right) - 1) < 1e-3f);
        // The screen faces the shooter (+Z, behind the gun) or up.
        const Vec3 n = cross(s.right, s.up);
        assert(n.z > 0 || n.y > .5f);
    }
    {   // At the bind pose with an identity placement the frame is the measured
        // screen, lifted toward its front.
        const auto &uzi = *ammoScreen(4);
        AmmoScreenFrame f;
        const Matrix34 bind = matrix(uzi.bind);
        assert(ammoScreenFrame(uzi, identity(), {1, 1, 1}, bind, f));
        const Vec3 normal = cross(uzi.right, uzi.up);
        assert(near(f.centre, uzi.centre + normal * AmmoScreenLift));
        assert(near(f.right, uzi.right * (uzi.width / 2)) && near(f.up, uzi.up * (uzi.height / 2)));
        // The minigun's screen on its quarter-turned display bone.
        const auto &minigun = *ammoScreen(5);
        assert(ammoScreenFrame(minigun, identity(), {1, 1, 1}, matrix(minigun.bind), f));
        assert(near(f.centre, minigun.centre + Vec3{0, 0, AmmoScreenLift}, 1e-4f));
        assert(near(f.right, Vec3{minigun.width / 2, 0, 0}, 1e-4f));
        // A live bone moved and turned carries the screen with it.
        const Pose moved{yaw(.4f), {.02f, -.1f, .03f}};
        const Matrix34 live = matrix(compose(moved, minigun.bind));
        assert(ammoScreenFrame(minigun, identity(), {1, 1, 1}, live, f));
        const Vec3 expect = moved.p + rotate(moved.q, minigun.centre);
        const Vec3 lifted = rotate(moved.q, Vec3{0, 0, AmmoScreenLift});
        assert(near(f.centre, expect + lifted, 1e-4f));
        // Placement scale enlarges it; a mirrored (left-hand) model keeps the
        // digits reading left to right from the front.
        Matrix34 scaled = scaledModel(identity(), 3);
        assert(ammoScreenFrame(uzi, scaled, {-1, 1, 1}, bind, f));
        assert(near(f.centre, Vec3{-uzi.centre.x, uzi.centre.y, uzi.centre.z} * 3 + normal * AmmoScreenLift, 1e-4f));
        assert(dot(cross(f.right, f.up), normal) > 0);
        assert(std::fabs(std::sqrt(dot(f.right, f.right)) - 3 * uzi.width / 2) < 1e-4f);
        // Degenerate placements are refused.
        Matrix34 flat = identity();
        flat.m[0] = flat.m[5] = flat.m[10] = 0;
        assert(!ammoScreenFrame(uzi, flat, {1, 1, 1}, bind, f));
    }
    std::puts("ammo screen ok");
}
