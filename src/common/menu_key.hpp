#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// A menu drawn over black without the world behind it (MenuKeyed): the black
// around it becomes transparent so the world shows through. Only the black
// connected to the image's edge is keyed; black inside the menu (text outlines,
// borders, shadows) stays opaque. Keying every dark pixel let the world through
// them. Pixels are 32-bit B,G,R,A; alpha is written 0 (keyed) or 255.
namespace ss2vr {
inline void keyEdgeBlack(uint8_t *bgra, uint32_t width, uint32_t height, uint8_t threshold = 12) {
    if (!bgra || !width || !height) return;
    const size_t count = size_t(width) * height;
    for (size_t i = 0; i < count; ++i) bgra[i * 4 + 3] = 255;
    const auto dark = [&](size_t i) {
        const uint8_t *p = bgra + i * 4;
        return p[0] < threshold && p[1] < threshold && p[2] < threshold;
    };
    std::vector<uint32_t> stack;
    const auto visit = [&](size_t i) {
        if (bgra[i * 4 + 3] && dark(i)) { bgra[i * 4 + 3] = 0; stack.push_back(uint32_t(i)); }
    };
    for (uint32_t x = 0; x < width; ++x) { visit(x); visit(size_t(height - 1) * width + x); }
    for (uint32_t y = 0; y < height; ++y) { visit(size_t(y) * width); visit(size_t(y) * width + width - 1); }
    while (!stack.empty()) {
        const size_t i = stack.back();
        stack.pop_back();
        const uint32_t x = uint32_t(i % width), y = uint32_t(i / width);
        if (x) visit(i - 1);
        if (x + 1 < width) visit(i + 1);
        if (y) visit(i - width);
        if (y + 1 < height) visit(i + width);
    }
}
} // namespace ss2vr
