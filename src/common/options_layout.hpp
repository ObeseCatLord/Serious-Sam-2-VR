#pragma once
// VR options panel layout (host): where the tabs, rows and buttons sit on the
// panel image, in panel units (u right, v down, both 0..1), and what a pointer
// hit there means. Shared by the drawing and the pointer so they agree.
#include <algorithm>
#include <cmath>

namespace ss2vr::options_layout {
constexpr unsigned MaxRows = 10;
constexpr float TitleBottom = .09f, TabTop = .10f, TabBottom = .17f, RowsTop = .19f, RowHeight = .058f,
                HelpTop = .785f, HelpBottom = .865f, FooterTop = .885f, FooterBottom = .965f;
constexpr float Margin = .03f, ValueLeft = .52f, ArrowWidth = .07f;
constexpr float ResetLeft = .03f, ResetRight = .30f, CloseLeft = .78f, CloseRight = .97f;
struct Box {
    float left = 0, top = 0, right = 0, bottom = 0;
    bool contains(float u, float v) const noexcept { return u >= left && u < right && v >= top && v < bottom; }
};
inline Box tab(unsigned index, unsigned count) noexcept {
    const float width = (1 - 2 * Margin) / float(std::max(count, 1u));
    return {Margin + index * width, TabTop, Margin + (index + 1) * width, TabBottom};
}
inline Box row(unsigned index) noexcept {
    const float top = RowsTop + index * RowHeight;
    return {Margin * .5f, top, 1 - Margin * .5f, top + RowHeight};
}
// The value's step arrows within a row: decrease left of the value, increase right.
inline Box decrease(unsigned index) noexcept {
    const Box r = row(index);
    return {ValueLeft, r.top, ValueLeft + ArrowWidth, r.bottom};
}
inline Box increase(unsigned index) noexcept {
    const Box r = row(index);
    return {r.right - Margin * .5f - ArrowWidth, r.top, r.right - Margin * .5f, r.bottom};
}
inline Box resetButton() noexcept { return {ResetLeft, FooterTop, ResetRight, FooterBottom}; }
inline Box closeButton() noexcept { return {CloseLeft, FooterTop, CloseRight, FooterBottom}; }

enum class Target { None, Tab, Row, Reset, Close };
struct Hit {
    Target target = Target::None;
    unsigned index = 0; // Tab or row.
    int direction = 0;  // Row: -1 the decrease arrow, +1 the increase arrow or the rest of the row.
    bool operator==(const Hit &o) const noexcept { return target == o.target && index == o.index && direction == o.direction; }
    bool operator!=(const Hit &o) const noexcept { return !(*this == o); }
};
inline Hit hit(float u, float v, unsigned tabs, unsigned rows) noexcept {
    if (!(u >= 0 && u <= 1 && v >= 0 && v <= 1)) return {};
    for (unsigned i = 0; i < tabs; ++i)
        if (tab(i, tabs).contains(u, v)) return {Target::Tab, i, 0};
    for (unsigned i = 0; i < std::min(rows, MaxRows); ++i)
        if (row(i).contains(u, v)) return {Target::Row, i, decrease(i).contains(u, v) ? -1 : 1};
    if (resetButton().contains(u, v)) return {Target::Reset, 0, 0};
    if (closeButton().contains(u, v)) return {Target::Close, 0, 0};
    return {};
}

// Stick and button focus: the tab bar, each row, then Reset and Close.
struct Focus {
    unsigned index = 1; // 0 tabs, 1..rows the rows, rows + 1 Reset, rows + 2 Close.
    static unsigned count(unsigned rows) noexcept { return rows + 3; }
    void clamp(unsigned rows) noexcept { index = std::min(index, count(rows) - 1); }
    void move(int down, unsigned rows) noexcept {
        const unsigned n = count(rows);
        index = (index + n + (down > 0 ? 1 : n - 1)) % n;
    }
    bool tabs() const noexcept { return index == 0; }
    bool isRow(unsigned rows) const noexcept { return index >= 1 && index <= rows; }
    unsigned rowIndex() const noexcept { return index - 1; }
    bool reset(unsigned rows) const noexcept { return index == rows + 1; }
    bool close(unsigned rows) const noexcept { return index == rows + 2; }
};
// A numeric value stepped by `direction` steps, wrapping past either end.
inline float stepped(float value, int direction, float step, float low, float high) noexcept {
    const float next = std::round((value + direction * step) / step) * step;
    return next > high + step * .5f ? low : next < low - step * .5f ? high : std::clamp(next, low, high);
}
// Where a value sits between its limits (the row's slider), 0..1.
inline float fraction(float value, float low, float high) noexcept {
    return high > low ? std::clamp((value - low) / (high - low), 0.f, 1.f) : 0.f;
}
} // namespace ss2vr::options_layout
