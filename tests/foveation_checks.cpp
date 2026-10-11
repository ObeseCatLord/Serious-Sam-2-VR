#include "common/foveation.hpp"
#include <cassert>
#include <cmath>
#include <vector>
#ifdef NDEBUG
#error Foveation checks require active assertions
#endif
using namespace ss2vr;

namespace {
bool near(float a, float b, float e = 1e-4f) { return std::abs(a - b) <= e; }
const Fov symmetric{-.8f, .8f, .8f, -.8f};
const Fov asymmetric{-.9f, .7f, .75f, -.85f};

void extents() {
    assert(foveationExtent(3584, .5f) == 1792 && foveationExtent(3584, .4f) % 2 == 0);
    assert(foveationExtent(101, .5f) % 2 == 0 && foveationExtent(10, .01f) >= 2);
    assert(foveationFits(1000, 1000, 500, 500, 400, 400));
    assert(!foveationFits(1000, 1000, 800, 800, 700, 700)); // Both images share one eye buffer.
    assert(!foveationFits(1000, 1000, 500, 500, 0, 400));
}
void centredCrop() {
    // The FOV centre maps to the image centre on a symmetric FOV.
    const auto r = insetCrop(symmetric, 1000, 800, 400, 320, 0, 0);
    assert(r.x == 300 && r.y == 240 && r.w == 400 && r.h == 320);
    const Fov f = rectFov(symmetric, 1000, 800, r);
    assert(near(std::tan(f.left), -std::tan(f.right)) && near(std::tan(f.up), -std::tan(f.down)));
    assert(near(std::tan(f.right) - std::tan(f.left), .4f * 2 * std::tan(.8f)));
}
void exactCover() {
    // A crop's FOV maps back onto exactly its pixels on an asymmetric FOV.
    const float tx = .2f, ty = -.1f;
    const auto r = insetCrop(asymmetric, 1200, 1300, 480, 520, tx, ty);
    const Fov f = rectFov(asymmetric, 1200, 1300, r);
    const auto back = insetCrop(asymmetric, 1200, 1300, 480, 520,
                                (std::tan(f.left) + std::tan(f.right)) * .5f, (std::tan(f.up) + std::tan(f.down)) * .5f);
    assert(back.x == r.x && back.y == r.y);
    const float l = std::tan(asymmetric.left), rr = std::tan(asymmetric.right);
    assert(near(std::tan(f.left), l + (rr - l) * r.x / 1200.f));
}
void clamped() {
    // Gaze at the edge keeps the crop inside the image.
    const auto r = insetCrop(symmetric, 1000, 1000, 400, 400, 10, -10);
    assert(r.x == 600 && r.y == 600);
    const auto nan = insetCrop(symmetric, 1000, 1000, 400, 400, std::nanf(""), 0);
    assert(nan.x == 300 && nan.y == 300); // Non-finite gaze falls back to the centre.
}
void gaze() {
    float tx = 0, ty = 0;
    // Looking straight ahead from an unrotated eye: tangent centre.
    assert(gazeTangent({0, 0, 0, 1}, {0, 0, -1}, symmetric, tx, ty) && near(tx, 0) && near(ty, 0));
    // Right and up of forward.
    assert(gazeTangent({0, 0, 0, 1}, {.2f, .1f, -1}, symmetric, tx, ty) && near(tx, .2f) && near(ty, .1f));
    // A yawed eye: gaze straight ahead in world appears to the right of the eye's forward.
    const float a = .3f;
    assert(gazeTangent({0, std::sin(a / 2), 0, std::cos(a / 2)}, {0, 0, -1}, symmetric, tx, ty) &&
           near(tx, std::tan(a), 1e-3f) && near(ty, 0));
    // Backward gaze is rejected; extreme gaze is clamped to the FOV.
    assert(!gazeTangent({0, 0, 0, 1}, {0, 0, 1}, symmetric, tx, ty));
    assert(gazeTangent({0, 0, 0, 1}, {5, 0, -1}, symmetric, tx, ty) && near(tx, std::tan(.8f)));
}
void feather() {
    std::vector<uint8_t> image(20 * 10 * 4, 0);
    for (size_t i = 3; i < image.size(); i += 4) image[i] = 255;
    featherAlpha(image.data(), 20, 10, 4);
    const auto alpha = [&](int x, int y) { return image[(y * 20 + x) * 4 + 3]; };
    assert(alpha(0, 0) == 0 && alpha(0, 5) == 0 && alpha(19, 5) == 0 && alpha(10, 9) == 0);
    assert(alpha(10, 5) == 255 && alpha(4, 5) == 255 && alpha(15, 5) == 255);
    assert(alpha(2, 5) > 0 && alpha(2, 5) < 255 && alpha(2, 5) == alpha(17, 5));
    assert(image[(5 * 20 + 2) * 4] == 0); // Colour is untouched.
}
} // namespace

int main() {
    extents();
    centredCrop();
    exactCover();
    clamped();
    gaze();
    feather();
    return 0;
}
