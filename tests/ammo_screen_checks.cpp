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
    // One-place readouts (bomb, revolvers) grow a place only for counts that need it.
    const auto one = [](int32_t count) {
        const auto t = ammoReadoutText(count, 1);
        std::array<int, 4> d{-9, -9, -9, -9};
        for (unsigned i = 0; i != t.places; ++i) d[i] = t.digit[i];
        return d;
    };
    assert((one(0) == std::array<int, 4>{0, -9, -9, -9}));
    assert((one(6) == std::array<int, 4>{6, -9, -9, -9}));
    assert((one(12) == std::array<int, 4>{1, 2, -9, -9}));
    assert((one(345) == std::array<int, 4>{3, 4, 5, -9}));
    assert(ammoReadoutText(12345, 1).places == 4 && ammoReadoutText(5, 0).places == 1);
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
    {   // A one-place panel as narrow as the revolvers' and the bomb's keeps its digit inside.
        for (const auto *screen : {ammoScreen(11), ammoScreen(12)}) {
            unsigned lit = 0;
            bool inside = true;
            ammoReadoutTriangles(8, screen->width, screen->height, [&](AmmoReadoutLayer l, float u0, float v0, float u1,
                                                                       float v1, float u2, float v2) {
                lit += l == AmmoReadoutLayer::Lit;
                for (float c : {u0, v0, u1, v1, u2, v2}) inside = inside && std::isfinite(c) && std::fabs(c) <= 1.0001f;
            }, screen->places);
            assert(inside && lit == 7 * 4);
        }
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
    // Every gun that uses ammo has a readout, once; the saw, the zap gun
    // (unlimited ammo) and the unused slot have none.
    for (int id : {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 16}) assert(ammoScreen(id) && ammoScreen(id)->nativeId == unsigned(id));
    assert(!ammoScreen(0) && !ammoScreen(1) && !ammoScreen(14) && !ammoScreen(-1) && !ammoScreen(17));
    for (size_t i = 0; i != std::size(AmmoScreens); ++i)
        for (size_t j = i + 1; j != std::size(AmmoScreens); ++j) assert(AmmoScreens[i].nativeId != AmmoScreens[j].nativeId);
    for (const auto &s : AmmoScreens) {
        assert(s.bone && *s.bone);
        assert(std::fabs(dot(s.right, s.up)) < 1e-3f);
        assert(std::fabs(dot(s.up, s.up) - 1) < 2e-3f && std::fabs(dot(s.right, s.right) - 1) < 2e-3f);
        const Quat q = s.bind.q;
        assert(std::fabs(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w - 1) < 2e-3f);
        // The readout faces the shooter (+Z, behind the gun) or up.
        const Vec3 n = cross(s.right, s.up);
        assert(n.z > 0 || n.y > .5f);
        // Wide enough for its places (at most 1.5 x 0.85 of the panel height
        // a digit, plus spacing), and a handful of centimetres on the gun.
        assert(s.places >= 1 && s.places <= AmmoReadoutPlaces);
        assert(s.width / s.height >= (s.places * .55f + (s.places - 1) * .2f) * 1.5f / 2 * .9f);
        assert(s.height >= .007f && s.height <= .025f && s.width <= .05f);
        // Every layer opaque; the lit segments brighter than the halo, the
        // ghosts barely above the panel.
        const auto luma = [](uint32_t c) { return ((c >> 24) & 255) + ((c >> 16) & 255) + ((c >> 8) & 255); };
        for (uint32_t c : s.colour) assert((c & 255) == 255);
        assert(luma(s.colour[3]) > luma(s.colour[2]) && luma(s.colour[2]) > luma(s.colour[1]) &&
               luma(s.colour[1]) > luma(s.colour[0]) && luma(s.colour[1]) < 3 * 0x40);
    }
    {   // Counts: the inventory ammo, or the cylinder's rounds left (six
        // after a reload, one down per shot, never below none).
        const auto &colt = *ammoScreen(12);
        assert(colt.load == 6 && colt.places == 1 && ammoScreen(11)->places == 1 && !ammoScreen(11)->load);
        assert(ammoScreenCount(colt, -1, 0) == 6 && ammoScreenCount(colt, -1, 4) == 2);
        assert(ammoScreenCount(colt, -1, 9) == 0 && ammoScreenCount(colt, -1, -3) == 6);
        assert(ammoScreenCount(*ammoScreen(2), 37, 5) == 37 && ammoScreenCount(*ammoScreen(11), 2, 0) == 2);
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
        // Klodovik's wrist readout sits on a bone turned a quarter on its bind:
        // at the bind pose the frame is still the measured one.
        const auto &klodovik = *ammoScreen(9);
        assert(ammoScreenFrame(klodovik, identity(), {1, 1, 1}, matrix(klodovik.bind), f));
        const Vec3 kn = cross(klodovik.right, klodovik.up);
        assert(near(f.centre, klodovik.centre + kn * AmmoScreenLift, 1e-4f));
        assert(near(f.up, klodovik.up * (klodovik.height / 2), 1e-4f));
        // Degenerate placements are refused.
        Matrix34 flat = identity();
        flat.m[0] = flat.m[5] = flat.m[10] = 0;
        assert(!ammoScreenFrame(uzi, flat, {1, 1, 1}, bind, f));
    }
    std::puts("ammo screen ok");
}
