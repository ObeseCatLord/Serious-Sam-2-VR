#pragma once
// The VR options panel image (GDI into a top-down 32-bit DIB, straight alpha):
// drawn by the host for its VR panel and by the game for the desktop window.
#include "options_layout.hpp"
#include "options_model.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>

namespace ss2vr {
// Panel text: Segoe UI on Windows; Tahoma under Wine and Proton, which ship it
// but not Segoe UI (whose substitute there is a serif).
inline const wchar_t *uiFontFace() {
    static const bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    return wine ? L"Tahoma" : L"Segoe UI";
}
struct OptionsBitmap {
    HDC dc = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ oldBitmap = nullptr;
    HFONT title = nullptr, row = nullptr, small = nullptr, button = nullptr;
    uint8_t *bits = nullptr;
    uint32_t width = 0, height = 0;
    bool desktop = false; // The desktop window (mouse) rather than the VR panel (pointer and stick).
    ~OptionsBitmap() {
        if (oldBitmap) SelectObject(dc, oldBitmap);
        for (HGDIOBJ o : {HGDIOBJ(dib), HGDIOBJ(title), HGDIOBJ(row), HGDIOBJ(small), HGDIOBJ(button)})
            if (o) DeleteObject(o);
        if (dc) DeleteDC(dc);
    }
    // `buttonImage`: the small "VR options" button image under the game's menus.
    void create(uint32_t w, uint32_t h, bool buttonImage = false) {
        width = w; height = h;
        dc = CreateCompatibleDC(nullptr);
        if (!dc) throw std::runtime_error("GDI options memory DC unavailable");
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = LONG(w);
        info.bmiHeader.biHeight = -LONG(h);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void *data = nullptr;
        dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &data, nullptr, 0);
        if (!dib || !data) throw std::runtime_error("GDI options DIB unavailable");
        bits = static_cast<uint8_t *>(data);
        oldBitmap = SelectObject(dc, dib);
        const auto font = [&](float size, int weight) {
            return CreateFontW(-int(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, uiFontFace());
        };
        if (buttonImage) {
            button = font(h * .46f, FW_BOLD);
            if (!oldBitmap || oldBitmap == HGDI_ERROR || !button) throw std::runtime_error("GDI options button resources unavailable");
        } else {
            title = font(h * .055f, FW_BOLD); row = font(h * .034f, FW_SEMIBOLD); small = font(h * .026f, FW_NORMAL);
            if (!oldBitmap || oldBitmap == HGDI_ERROR || !title || !row || !small)
                throw std::runtime_error("GDI options resources unavailable");
        }
        SetBkMode(dc, TRANSPARENT);
    }
    RECT pixels(const options_layout::Box &b) const {
        return {LONG(b.left * width), LONG(b.top * height), LONG(b.right * width), LONG(b.bottom * height)};
    }
    void text(const std::wstring &value, RECT r, HFONT font, COLORREF color, UINT align) {
        const HGDIOBJ old = SelectObject(dc, font);
        SetTextColor(dc, color);
        DrawTextW(dc, value.c_str(), -1, &r, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, old);
    }
    void paragraph(const std::wstring &value, RECT r, HFONT font, COLORREF color) {
        const HGDIOBJ old = SelectObject(dc, font);
        SetTextColor(dc, color);
        DrawTextW(dc, value.c_str(), -1, &r, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, old);
    }
    void box(RECT r, COLORREF fill, COLORREF edge = CLR_INVALID, int radius = 0) {
        const HBRUSH brush = CreateSolidBrush(fill);
        const HPEN pen = CreatePen(PS_SOLID, edge == CLR_INVALID ? 1 : 2, edge == CLR_INVALID ? fill : edge);
        const HGDIOBJ oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, pen);
        if (radius) RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
        else Rectangle(dc, r.left, r.top, r.right, r.bottom);
        SelectObject(dc, oldBrush); SelectObject(dc, oldPen);
        DeleteObject(brush); DeleteObject(pen);
    }
    // A step arrow (left or right) centred in `r`.
    void arrow(RECT r, bool right, COLORREF color) {
        const LONG cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, s = (r.bottom - r.top) / 4;
        const LONG d = right ? s : -s;
        const POINT p[3] = {{cx - d, cy - s}, {cx + d, cy}, {cx - d, cy + s}};
        const HBRUSH brush = CreateSolidBrush(color);
        const HPEN pen = CreatePen(PS_SOLID, 1, color);
        const HGDIOBJ oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, pen);
        Polygon(dc, p, 3);
        SelectObject(dc, oldBrush); SelectObject(dc, oldPen);
        DeleteObject(brush); DeleteObject(pen);
    }
    // The pointer: a ring on the panel image itself (centre x, y in pixels).
    void pointer(int x, int y) {
        const int r = int(height * .014f) + 2;
        for (int dy = -r - 1; dy <= r + 1; ++dy)
            for (int dx = -r - 1; dx <= r + 1; ++dx) {
                const int px = x + dx, py = y + dy;
                if (px < 0 || py < 0 || px >= int(width) || py >= int(height)) continue;
                const float d = std::sqrt(float(dx * dx + dy * dy));
                uint8_t *p = bits + (size_t(py) * width + px) * 4;
                if (d <= r - 2.5f) { p[0] = 40; p[1] = 235; p[2] = 255; }      // Fill (BGR): yellow.
                else if (d <= r + .5f) { p[0] = 20; p[1] = 20; p[2] = 20; }    // Outline.
                else continue;
                p[3] = 255;
            }
    }
    void opaque(uint8_t alpha) {
        GdiFlush();
        for (size_t i = 0; i < size_t(width) * height; ++i) bits[i * 4 + 3] = alpha;
    }
    void draw(const wchar_t *const *tabs, unsigned tabCount, unsigned page, const std::vector<options_model::OptionsRow> &rows,
              const options_layout::Focus &focus, const options_layout::Hit &hover) {
        namespace L = options_layout;
        using L::Target;
        const unsigned count = unsigned(std::min<size_t>(rows.size(), L::MaxRows));
        const LONG W = LONG(width), H = LONG(height);
        box({0, 0, W, H}, RGB(16, 20, 26));
        box({0, 0, W, LONG(L::TitleBottom * H)}, RGB(24, 40, 60));
        text(L"VR options", {LONG(L::Margin * W), 0, W / 2, LONG(L::TitleBottom * H)}, title, RGB(255, 255, 255), DT_LEFT);
        text(desktop ? L"Click a tab, or the arrows beside a value" : L"Point and pull the trigger, or use the stick", {W / 2, 0, LONG((1 - L::Margin) * W), LONG(L::TitleBottom * H)}, small,
             RGB(150, 170, 190), DT_RIGHT);
        for (unsigned i = 0; i < tabCount; ++i) {
            RECT r = pixels(L::tab(i, tabCount));
            r.left += 3; r.right -= 3;
            const bool current = i == page, hovered = hover.target == Target::Tab && hover.index == i;
            box(r, current ? RGB(40, 120, 190) : hovered ? RGB(52, 64, 80) : RGB(30, 36, 46),
                focus.tabs() && current ? RGB(255, 236, 160) : CLR_INVALID, 12);
            text(tabs[i], r, row, current ? RGB(255, 255, 255) : RGB(190, 200, 214), DT_CENTER);
        }
        int helpRow = -1;
        for (unsigned i = 0; i < count; ++i) {
            const options_model::OptionsRow &o = rows[i];
            const RECT r = pixels(L::row(i)), down = pixels(L::decrease(i)), up = pixels(L::increase(i));
            const bool focused = focus.isRow(count) && focus.rowIndex() == i, hovered = hover.target == Target::Row && hover.index == i;
            if (focused || hovered) helpRow = int(i);
            if (focused || hovered)
                box({r.left, r.top + 2, r.right, r.bottom - 2}, focused ? RGB(34, 82, 128) : RGB(38, 46, 58),
                    focused ? RGB(90, 160, 220) : CLR_INVALID, 10);
            const COLORREF ink = o.dim ? RGB(110, 118, 128) : focused ? RGB(255, 255, 255) : RGB(214, 222, 232);
            text(o.label, {LONG(L::Margin * W), r.top, down.left - 8, r.bottom}, row, ink, DT_LEFT);
            const bool downHot = hovered && hover.direction < 0, upHot = hovered && hover.direction > 0;
            const COLORREF still = o.locked ? RGB(52, 60, 72) : RGB(120, 150, 180);
            arrow(down, false, downHot && !o.locked ? RGB(255, 236, 160) : still);
            arrow(up, true, upHot && !o.locked ? RGB(255, 236, 160) : still);
            const RECT value{down.right, r.top, up.left, r.bottom};
            if (o.fraction >= 0) {
                const LONG y = r.bottom - LONG(H * .005f), left = value.left + 10, right = value.right - 10;
                box({left, y - 2, right, y + 2}, RGB(44, 54, 68));
                box({left, y - 2, left + LONG((right - left) * o.fraction), y + 2}, o.dim ? RGB(80, 90, 100) : RGB(90, 170, 230));
            }
            text(o.value, {value.left, value.top, value.right, value.bottom - (o.fraction >= 0 ? LONG(H * .008f) : 0)}, row, o.dim ? RGB(110, 118, 128) : !o.note.empty() ? RGB(255, 190, 90)
                                       : focused ? RGB(255, 236, 160) : RGB(170, 205, 235), DT_CENTER);
        }
        const RECT help{LONG(L::Margin * W), LONG(L::HelpTop * H), LONG((1 - L::Margin) * W), LONG(L::HelpBottom * H)};
        box({help.left - 6, help.top, help.right + 6, help.bottom}, RGB(22, 28, 36), CLR_INVALID, 10);
        if (helpRow >= 0) {
            const options_model::OptionsRow &o = rows[size_t(helpRow)];
            std::wstring words = o.help;
            if (!o.note.empty()) words += (words.empty() ? L"" : L"  ") + o.note + L".";
            paragraph(words, {help.left + 6, help.top + 4, help.right - 6, help.bottom - 2}, small,
                      o.note.empty() ? RGB(180, 192, 206) : RGB(255, 205, 130));
        } else if (focus.reset(count) || hover.target == Target::Reset)
            paragraph(L"Puts every setting on this page back to its default.", {help.left + 6, help.top + 4, help.right - 6, help.bottom - 2},
                      small, RGB(180, 192, 206));
        const auto button = [&](const L::Box &b, const wchar_t *label, bool focused, bool hovered) {
            const RECT r = pixels(b);
            box(r, hovered ? RGB(60, 120, 180) : RGB(36, 46, 60), focused ? RGB(255, 236, 160) : RGB(70, 90, 110), 14);
            text(label, r, row, RGB(240, 244, 250), DT_CENTER);
        };
        button(L::resetButton(), L"Reset page", focus.reset(count), hover.target == Target::Reset);
        button(L::closeButton(), L"Close", focus.close(count), hover.target == Target::Close);
        text(desktop ? L"Esc: close" : L"Use: change    Menu: close", {LONG(L::ResetRight * W), LONG(L::FooterTop * H), LONG(L::CloseLeft * W),
             LONG(L::FooterBottom * H)}, small, RGB(130, 145, 160), DT_CENTER);
        opaque(240);
    }
    void drawButton(bool hovered) {
        const LONG W = LONG(width), H = LONG(height);
        box({0, 0, W, H}, RGB(0, 0, 0));
        box({2, 2, W - 2, H - 2}, hovered ? RGB(40, 120, 190) : RGB(24, 40, 60), hovered ? RGB(255, 236, 160) : RGB(90, 150, 210), H / 2);
        text(L"VR options", {0, 0, W, H}, button, RGB(255, 255, 255), DT_CENTER);
        GdiFlush();
        // Transparent outside the rounded button (the black corners).
        for (size_t i = 0; i < size_t(width) * height; ++i) {
            uint8_t *p = bits + i * 4;
            p[3] = p[0] || p[1] || p[2] ? 235 : 0;
        }
    }
};
} // namespace ss2vr
