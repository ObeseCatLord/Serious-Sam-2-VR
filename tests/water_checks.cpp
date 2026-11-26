#include "common/water_reflection.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace ss2vr;
static bool near(float a, float b, float e = 1e-5f) { return std::abs(a - b) < e; }
int main() {
    // Centre camera turned 0.3 rad left (world -> view = Ry(0.3)^T); the left eye
    // sits 3.2 cm to the camera's left: in world space that is along -camera x.
    const float yaw = .3f, c = std::cos(yaw), s = std::sin(yaw);
    const float view[12]{c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0};
    const Matrix34 toLeft{{1, 0, 0, .032f, 0, 1, 0, 0, 0, 0, 1, 0}}; // x_eye = x_centre + 0.032
    const auto change = replay::eyeChange(toLeft, Fov{-.9f, .9f, .8f, -.8f}, Fov{-.9f, .9f, .8f, -.8f});
    float offset[3];
    assert(water::eyeOffset(view, change, offset));
    // Eye position in centre view space is (-0.032, 0, 0); camera x in world is (c, 0, -s).
    assert(near(offset[0], -.032f * c) && near(offset[1], 0) && near(offset[2], .032f * s));
    replay::EyeChange invalid;
    assert(!water::eyeOffset(view, invalid, offset));
    // The inverse of a turned, scaled model rotation.
    const float rows[12]{2 * c, 0, 2 * s, 9, 0, 2, 0, 9, -2 * s, 0, 2 * c, 9};
    float inv[9];
    assert(water::invert3(rows, inv));
    for (unsigned r = 0; r < 3; ++r)
        for (unsigned k = 0; k < 3; ++k) {
            float v = 0;
            for (unsigned j = 0; j < 3; ++j) v += rows[4 * r + j] * inv[3 * j + k];
            assert(near(v, r == k ? 1.f : 0.f));
        }
    const float flat[12]{};
    assert(!water::invert3(flat, inv));
    {
        // In VR a water draw never keeps the engine's own reflection (screen-mapped,
        // it follows the head): world-fixed, or else neutral. Every combination of
        // reasons: the engine's image stays only under the lab switch, or when a
        // program without the cube variants has no findable reflection lookup.
        using water::Reflection;
        namespace reason = water::reason;
        assert(water::reflectionFor(0) == Reflection::World);
        for (unsigned why = 0; why < 512; ++why) {
            const Reflection chosen = water::reflectionFor(why);
            const bool noVariants = why & (reason::NoVariant | reason::NoPixelVariant | reason::NotStereo);
            if (why & reason::Lab) assert(chosen == Reflection::Engine);
            else if (chosen == Reflection::Engine) assert(noVariants && (why & reason::NoSampler));
            // World-fixed only with the rendered cube, its anchor, the model's rotation, the camera and both variants.
            assert((chosen == Reflection::World) == !(why & (reason::Lab | reason::NotWorld)));
            if (chosen == Reflection::NeutralCube) assert(!noVariants && (why & reason::NotWorld));
            if (chosen == Reflection::NeutralImage) assert(noVariants && !(why & reason::NoSampler));
        }
        // No cube yet (a new level, a device reset, a jump), no anchor, no rotation: neutral through the variants.
        assert(water::reflectionFor(reason::NoCube) == Reflection::NeutralCube);
        assert(water::reflectionFor(reason::NoAnchor) == Reflection::NeutralCube);
        assert(water::reflectionFor(reason::NoCube | reason::NoRotation | reason::NoCamera) == Reflection::NeutralCube);
        // A pixel program without the cube variant, or a draw outside the pass: the image is replaced.
        assert(water::reflectionFor(reason::NoPixelVariant) == Reflection::NeutralImage);
        assert(water::reflectionFor(reason::NotStereo | reason::NoCube) == Reflection::NeutralImage);
        // The image's texel is the cube's times the variants' gain (they look the same on the water).
        assert(water::neutralImageLevel(200, .45f) == 90 && water::neutralImageLevel(150, .45f) == 68);
        assert(water::neutralImageLevel(200, 0) == 0 && water::neutralImageLevel(200, -1) == 0 && water::neutralImageLevel(200, 9) == 255);
        assert(water::neutralImageLevel(200, std::nanf("")) == 0);
    }
    std::puts("Water reflection checks passed");
}
