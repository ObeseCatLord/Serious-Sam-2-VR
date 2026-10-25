#include "common/menu_key.hpp"
#include <cassert>
#include <cstdio>
using namespace ss2vr;
int main() {
    // 7x7 black image with a grey ring (1..5) holding black at its centre (3,3),
    // and a dark-but-not-black pixel outside it.
    const uint32_t w = 7, h = 7;
    std::vector<uint8_t> image(w * h * 4, 0);
    const auto set = [&](uint32_t x, uint32_t y, uint8_t v) { for (int c = 0; c < 3; ++c) image[(y * w + x) * 4 + c] = v; };
    for (uint32_t i = 1; i <= 5; ++i) { set(i, 1, 200); set(i, 5, 200); set(1, i, 200); set(5, i, 200); }
    set(6, 0, 40);
    keyEdgeBlack(image.data(), w, h);
    const auto alpha = [&](uint32_t x, uint32_t y) { return image[(y * w + x) * 4 + 3]; };
    assert(alpha(0, 0) == 0 && alpha(0, 6) == 0 && alpha(3, 0) == 0); // Edge black: keyed.
    assert(alpha(1, 1) == 255 && alpha(5, 3) == 255);                  // The ring: opaque.
    assert(alpha(3, 3) == 255 && alpha(2, 2) == 255);                  // Black inside the ring: opaque.
    assert(alpha(6, 0) == 255);                                        // Not black: opaque.
    std::puts("menu key ok");
}
