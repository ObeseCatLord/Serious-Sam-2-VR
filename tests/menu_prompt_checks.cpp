#include "common/menu_prompt.hpp"
#include <cassert>
#include <cstdio>
using namespace ss2vr;
int main() {
    // Index: confirm/back are the right hand's A/B; the X/Y functions its left A/B.
    static_assert(indexPromptMark(MenuPrompt::A).image == MenuPrompt::A && indexPromptMark(MenuPrompt::A).hand == 'R');
    static_assert(indexPromptMark(MenuPrompt::B).image == MenuPrompt::B && indexPromptMark(MenuPrompt::B).hand == 'R');
    static_assert(indexPromptMark(MenuPrompt::X).image == MenuPrompt::A && indexPromptMark(MenuPrompt::X).hand == 'L');
    static_assert(indexPromptMark(MenuPrompt::Y).image == MenuPrompt::B && indexPromptMark(MenuPrompt::Y).hand == 'L');
    static_assert(indexPromptMark(MenuPrompt::None).image == MenuPrompt::None && !indexPromptMark(MenuPrompt::None).hand);
    // A 20x20 icon at 100,200 (y down): a half-height cell at its bottom-right
    // corner, overhanging it a little, clear of the icon's centre (its letter).
    PromptMarkBox box;
    assert(promptMarkBox(100, 200, 120, 220, .6f, box));
    assert(box.x1 > 120 && box.x1 < 123 && box.y1 > 220 && box.y1 < 223);
    assert(std::fabs((box.y1 - box.y0) - 10) < 1e-4f && std::fabs((box.x1 - box.x0) - 6) < 1e-4f);
    assert(box.x0 > 110 && box.y0 > 210); // Past the centre in both directions.
    // y up (the image's top at the larger y): the mark still sits at its bottom-right.
    assert(promptMarkBox(100, 220, 120, 200, .6f, box));
    assert(box.y1 < 200 && box.y0 > box.y1 && box.y0 < 210 && box.x0 > 110);
    // Wide icons do not stretch the glyph: its width follows the height.
    assert(promptMarkBox(0, 0, 40, 20, .5f, box) && std::fabs((box.x1 - box.x0) - 5) < 1e-4f);
    // Degenerate or unusable input: no mark.
    assert(!promptMarkBox(0, 0, 0, 20, .6f, box));
    assert(!promptMarkBox(0, 0, 20, 20, 0, box));
    assert(!promptMarkBox(0, 0, 20, 20, std::nanf(""), box));
    assert(!promptMarkBox(0, 0, INFINITY, 20, .6f, box));
    // Colours follow the icon's alpha (menu fades).
    static_assert(promptMarkColor(0xffffffffu, 0xffffff, 1) == 0xffffffffu);
    static_assert(promptMarkColor(0x80ffffffu, 0x000000, 1) == 0x80000000u);
    static_assert(promptMarkColor(0xff123456u, 0x000000, .5f) == 0x80000000u);
    static_assert(promptMarkColor(0x00ffffffu, 0xffffff, 1) == 0x00ffffffu);
    static_assert(promptMarkColor(0xffffffffu, 0xffffff, 2) == 0xffffffffu);
    std::puts("menu prompt ok");
}
