#pragma once
#include <cmath>
#include <cstdint>

// Menu button prompts for Index controllers. The game's menus show Xbox A/B/X/Y
// images; Index has only A and B on each hand (host bindings: left A Reload,
// left B Menu, right A Jump, right B Use). In the native menus (bridge.cpp) menu
// A (confirm) is right A, B (back) right B, X (syscmdF1) left A and Y (F2) left
// B. So each prompt is drawn as the Index button's own image, A or B, with a
// small hand mark, R or L, at its bottom-right corner.
namespace ss2vr {
// Sam2Game's prompt image slots, in its order (texture slots A, B, X, Y).
enum class MenuPrompt : uint8_t { A, B, X, Y, None };
struct PromptMark {
    MenuPrompt image = MenuPrompt::None; // The image drawn in place of the prompt's own.
    char hand = 0;                       // 'L' or 'R'; 0 for no mark.
};
constexpr PromptMark indexPromptMark(MenuPrompt prompt) noexcept {
    switch (prompt) {
    case MenuPrompt::A: return {MenuPrompt::A, 'R'};
    case MenuPrompt::B: return {MenuPrompt::B, 'R'};
    case MenuPrompt::X: return {MenuPrompt::A, 'L'};
    case MenuPrompt::Y: return {MenuPrompt::B, 'L'};
    default: return {};
    }
}
// The mark's glyph cell for an icon drawn over x0,y0 - x1,y1, where x0,y0 is
// the image's top-left texel and x1,y1 its bottom-right (whatever the axes'
// directions): a cell `cell` of the icon's height, `aspect` (glyph cell width
// over height) wide, whose bottom-right corner sits just past the icon's.
// The images are a round button filling the box with a large letter: the
// overhang keeps the mark on the rim and the dark plate around it, clear of
// that letter; the cell's descender band lifts the capital off the bottom.
struct PromptMarkBox {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
constexpr float PromptMarkCell = .5f, PromptMarkOverhangX = .1f, PromptMarkOverhangY = .12f;
inline bool promptMarkBox(float x0, float y0, float x1, float y1, float aspect, PromptMarkBox &out) noexcept {
    const float w = x1 - x0, h = y1 - y0;
    if (!std::isfinite(w) || !std::isfinite(h) || !std::isfinite(aspect) || w == 0 || h == 0 || aspect < .1f ||
        aspect > 3.f)
        return false;
    const float cellHeight = h * PromptMarkCell;
    // Width follows the icon's own x direction, sized from its height.
    const float cellWidth = std::copysign(std::fabs(cellHeight) * aspect, w);
    const float right = x1 + w * PromptMarkOverhangX, bottom = y1 + h * PromptMarkOverhangY;
    out = {right - cellWidth, bottom - cellHeight, right, bottom};
    return true;
}
// The outline (a dark copy at eight offsets, as the HUD font's drop shadow) is
// this fraction of the cell height around the light glyph.
constexpr float PromptMarkOutline = .07f;
// Vertex colours are 0xAARRGGBB. The mark fades with the icon (menu transitions
// fade its colour's alpha): `rgb` at `opacity` of the icon's alpha.
constexpr uint32_t promptMarkColor(uint32_t iconColor, uint32_t rgb, float opacity) noexcept {
    const float alpha = float(iconColor >> 24) * (opacity < 0 ? 0 : opacity > 1 ? 1 : opacity);
    return uint32_t(alpha + .5f) << 24 | (rgb & 0xffffffu);
}
} // namespace ss2vr
