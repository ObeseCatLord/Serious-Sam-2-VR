#include "common/options_layout.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace ss2vr::options_layout;
int main() {
    // Tabs split the bar; nothing overlaps.
    const unsigned tabs = 5;
    for (unsigned i = 0; i < tabs; ++i) {
        const Box b = tab(i, tabs);
        const Hit h = hit((b.left + b.right) / 2, (b.top + b.bottom) / 2, tabs, MaxRows);
        assert(h.target == Target::Tab && h.index == i);
    }
    // Rows: the decrease arrow steps down, the increase arrow and the rest of the row step up.
    for (unsigned i = 0; i < MaxRows; ++i) {
        const Box d = decrease(i), up = increase(i), r = row(i);
        const float v = (r.top + r.bottom) / 2;
        Hit h = hit((d.left + d.right) / 2, v, tabs, MaxRows);
        assert(h.target == Target::Row && h.index == i && h.direction == -1);
        h = hit((up.left + up.right) / 2, v, tabs, MaxRows);
        assert(h.target == Target::Row && h.index == i && h.direction == 1);
        h = hit(.2f, v, tabs, MaxRows);
        assert(h.target == Target::Row && h.index == i && h.direction == 1);
        assert(r.bottom <= HelpTop + 1e-6f);
    }
    // A short page: below its last row is not a row.
    assert(hit(.2f, (row(4).top + row(4).bottom) / 2, tabs, 4).target == Target::None);
    assert(hit(.1f, (FooterTop + FooterBottom) / 2, tabs, 4).target == Target::Reset);
    assert(hit(.9f, (FooterTop + FooterBottom) / 2, tabs, 4).target == Target::Close);
    assert(hit(.5f, (FooterTop + FooterBottom) / 2, tabs, 4).target == Target::None);
    assert(hit(-.1f, .5f, tabs, 4).target == Target::None && hit(.5f, 1.2f, tabs, 4).target == Target::None);
    assert(hit(NAN, .5f, tabs, 4).target == Target::None);
    // Focus wraps through the tab bar, rows, Reset and Close.
    Focus f;
    f.index = 0;
    f.move(-1, 3);
    assert(f.close(3));
    f.move(1, 3);
    assert(f.tabs());
    f.move(1, 3);
    assert(f.isRow(3) && f.rowIndex() == 0);
    f.index = 9;
    f.clamp(3);
    assert(f.close(3));
    // Stepping snaps to the step, wraps past the ends.
    assert(std::abs(stepped(.3f, 1, .1f, 0, 1) - .4f) < 1e-5f);
    assert(std::abs(stepped(1, 1, .1f, 0, 1) - 0) < 1e-5f);
    assert(std::abs(stepped(0, -1, .1f, 0, 1) - 1) < 1e-5f);
    assert(std::abs(stepped(150, 1, 30, 30, 360) - 180) < 1e-3f);
    assert(std::abs(fraction(.5f, 0, 2) - .25f) < 1e-6f && fraction(5, 0, 2) == 1 && fraction(1, 1, 1) == 0);
    std::puts("options layout ok");
}
