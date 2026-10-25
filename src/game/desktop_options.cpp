#include "desktop_options.hpp"
#include "common/options_bitmap.hpp"
#include "common/options_model.hpp"
#include "common/win_settings.hpp"
#include "game.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <memory>

namespace ss2vr::game::desktop_options {
namespace {
using namespace ss2vr::options_model;
namespace L = options_layout;
constexpr wchar_t ButtonClass[] = L"SS2VRDesktopOptionsButton", PanelClass[] = L"SS2VRDesktopOptions";
constexpr UINT_PTR TickTimer = 1;
const OptionsContext Desktop{false, false};

std::wstring file;
VrSettings startupSettings;
std::atomic<bool> started{false}, menuUp{false};
std::atomic<HWND> gameWindow{nullptr};

// UI thread only.
struct Panel {
    HWND window = nullptr;
    std::unique_ptr<OptionsBitmap> bitmap;
    unsigned page = 3; // Voice chat: the page that matters most without a headset.
    L::Hit hover;
    VrSettings settings;
    std::vector<std::wstring> microphones;
};
struct Button {
    HWND window = nullptr;
    std::unique_ptr<OptionsBitmap> bitmap;
    bool hover = false, tracking = false;
};
Panel panel;
Button button;
// Lab (SS2VR_LAB_DESKTOP_OPTIONS=1): desktop mode while the headset plays, and a
// scripted pass through the windows with real mouse messages, page images
// (desktop-*.ppm beside the settings file) and logged results.
bool labTest() {
    static const bool on = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_DESKTOP_OPTIONS", v, 2) == 1 && v[0] == L'1'; }();
    return on;
}
void dump(const OptionsBitmap &bitmap, const wchar_t *name) {
    GdiFlush();
    const std::wstring path = file.substr(0, file.find_last_of(L"\\/") + 1) + name;
    if (FILE *out = _wfopen(path.c_str(), L"wb")) {
        std::fprintf(out, "P6\n%u %u\n255\n", bitmap.width, bitmap.height);
        for (size_t i = 0; i < size_t(bitmap.width) * bitmap.height; ++i) {
            const uint8_t *px = bitmap.bits + i * 4;
            const uint8_t rgb[3]{px[2], px[1], px[0]};
            std::fwrite(rgb, 1, 3, out);
        }
        std::fclose(out);
    }
}

RECT clientOnScreen(HWND window) {
    RECT r{};
    GetClientRect(window, &r);
    POINT corner[2]{{r.left, r.top}, {r.right, r.bottom}};
    MapWindowPoints(window, nullptr, corner, 2);
    return {corner[0].x, corner[0].y, corner[1].x, corner[1].y};
}
void drawPanel() {
    RECT client{};
    GetClientRect(panel.window, &client);
    const uint32_t w = uint32_t(std::max<LONG>(client.right, 64)), h = uint32_t(std::max<LONG>(client.bottom, 48));
    if (!panel.bitmap || panel.bitmap->width != w || panel.bitmap->height != h) {
        panel.bitmap.reset();
        auto fresh = std::make_unique<OptionsBitmap>();
        fresh->create(w, h);
        fresh->desktop = true;
        panel.bitmap = std::move(fresh);
    }
    L::Focus none;
    none.index = ~0u >> 1; // No keyboard focus: the mouse hovers.
    panel.bitmap->draw(OptionsPages, OptionsPageCount, panel.page, optionRows(panel.page, panel.settings, startupSettings, Desktop),
                      none, panel.hover);
}
void activate(const L::Hit &hit) {
    switch (hit.target) {
    case L::Target::Tab:
        if (hit.index < OptionsPageCount) panel.page = hit.index;
        break;
    case L::Target::Row:
        if (hit.index < pageItems(panel.page)) {
            const unsigned item = OptionsPageStart[panel.page] + hit.index;
            if (changeOption(item, hit.direction < 0 ? -1 : 1, panel.settings, panel.microphones, file, Desktop))
                log("Desktop VR options: %ls/%ls changed (direction %d)", OptionKeys[item].section, OptionKeys[item].key,
                    hit.direction);
            else
                log("Desktop VR options: %ls/%ls left as is (%s)", OptionKeys[item].section, OptionKeys[item].key,
                    desktopOption(item) ? "not saved" : "VR only");
        }
        break;
    case L::Target::Reset:
        resetPage(panel.page, file, Desktop);
        panel.settings = loadSettings(file);
        log("Desktop VR options: page %ls reset", OptionsPages[panel.page]);
        break;
    case L::Target::Close: DestroyWindow(panel.window); return;
    default: return;
    }
    InvalidateRect(panel.window, nullptr, FALSE);
}
L::Hit hitAt(LPARAM at) {
    RECT client{};
    GetClientRect(panel.window, &client);
    if (client.right <= 0 || client.bottom <= 0) return {};
    const float u = float(short(LOWORD(at))) / float(client.right), v = float(short(HIWORD(at))) / float(client.bottom);
    return L::hit(u, v, OptionsPageCount, pageItems(panel.page));
}
void paint(HWND window, OptionsBitmap &bitmap) {
    PAINTSTRUCT ps{};
    const HDC dc = BeginPaint(window, &ps);
    BitBlt(dc, 0, 0, LONG(bitmap.width), LONG(bitmap.height), bitmap.dc, 0, 0, SRCCOPY);
    EndPaint(window, &ps);
}
LRESULT CALLBACK panelProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    try {
        switch (message) {
        case WM_PAINT:
            drawPanel();
            paint(window, *panel.bitmap);
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEMOVE: {
            const L::Hit hit = hitAt(l);
            if (hit != panel.hover) {
                panel.hover = hit;
                InvalidateRect(window, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{sizeof track, TME_LEAVE, window, 0};
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            panel.hover = {};
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: activate(hitAt(l)); return 0;
        case WM_MOUSEWHEEL: // Over a row: steps its value.
            if (panel.hover.target == L::Target::Row)
                activate({L::Target::Row, panel.hover.index, GET_WHEEL_DELTA_WPARAM(w) > 0 ? 1 : -1});
            return 0;
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) DestroyWindow(window);
            return 0;
        case WM_DESTROY: {
            panel.window = nullptr;
            log("Desktop VR options closed");
            if (const HWND game = gameWindow.load()) SetForegroundWindow(game); // Back to the game's menu.
            return 0;
        }
        default: break;
        }
    } catch (const std::exception &e) {
        log("Desktop VR options failed: %s", e.what());
        if (message != WM_DESTROY) DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
void openPanel() {
    if (panel.window) { SetForegroundWindow(panel.window); return; }
    const HWND game = gameWindow.load();
    if (!game) return;
    panel.settings = loadSettings(file);
    panel.microphones = microphones();
    panel.hover = {};
    // Centred over the game, 4:3 like the VR panel, at most 1024 wide.
    const RECT g = clientOnScreen(game);
    const LONG gw = std::max<LONG>(g.right - g.left, 640), gh = std::max<LONG>(g.bottom - g.top, 480);
    LONG w = std::min<LONG>(1024, gw * 4 / 5), h = w * 3 / 4;
    if (h > gh * 9 / 10) { h = gh * 9 / 10; w = h * 4 / 3; }
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU, ex = WS_EX_DLGMODALFRAME;
    RECT frame{0, 0, w, h};
    AdjustWindowRectEx(&frame, style, FALSE, ex);
    const LONG fw = frame.right - frame.left, fh = frame.bottom - frame.top;
    panel.window = CreateWindowExW(ex, PanelClass, L"VR options", style, g.left + (gw - fw) / 2, g.top + (gh - fh) / 2, fw, fh, game,
                                   nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!panel.window) { log("Desktop VR options window unavailable (%lu)", GetLastError()); return; }
    ShowWindow(panel.window, SW_SHOW);
    SetForegroundWindow(panel.window);
    log("Desktop VR options opened (%ldx%ld)", w, h);
}
void drawButton() {
    RECT client{};
    GetClientRect(button.window, &client);
    const uint32_t w = uint32_t(std::max<LONG>(client.right, 16)), h = uint32_t(std::max<LONG>(client.bottom, 8));
    if (!button.bitmap || button.bitmap->width != w || button.bitmap->height != h) {
        button.bitmap.reset();
        auto fresh = std::make_unique<OptionsBitmap>();
        fresh->create(w, h, true);
        button.bitmap = std::move(fresh);
        SetWindowRgn(button.window, CreateRoundRectRgn(0, 0, LONG(w) + 1, LONG(h) + 1, LONG(h), LONG(h)), TRUE);
    }
    button.bitmap->drawButton(button.hover);
}
void labStep(bool shown) {
    static unsigned step = 0, ticks = 0;
    if (step > 12 || (!shown && step == 0) || ++ticks < 10) return; // One second per step.
    ticks = 0;
    const auto click = [](const L::Box &b) {
        RECT c{};
        GetClientRect(panel.window, &c);
        const LPARAM at = MAKELPARAM(int((b.left + b.right) * .5f * float(c.right)), int((b.top + b.bottom) * .5f * float(c.bottom)));
        SendMessageW(panel.window, WM_MOUSEMOVE, 0, at);
        SendMessageW(panel.window, WM_LBUTTONDOWN, MK_LBUTTON, at);
        SendMessageW(panel.window, WM_LBUTTONUP, 0, at);
    };
    const auto page = [](const wchar_t *name) {
        drawPanel();
        dump(*panel.bitmap, name);
    };
    const HWND game = gameWindow.load();
    switch (step++) {
    case 0: {
        const RECT g = clientOnScreen(game), b = clientOnScreen(button.window);
        log("Lab desktop options button at %ld,%ld-%ld,%ld in game client %ld,%ld-%ld,%ld", b.left, b.top, b.right, b.bottom, g.left,
            g.top, g.right, g.bottom);
        drawButton();
        dump(*button.bitmap, L"desktop-button.ppm");
        SendMessageW(button.window, WM_MOUSEMOVE, 0, MAKELPARAM(4, 4));
        SendMessageW(button.window, WM_LBUTTONUP, 0, MAKELPARAM(4, 4));
        break;
    }
    case 1: if (panel.window) page(L"desktop-voice.ppm"); else { log("Lab desktop options: panel did not open"); step = 99; } break;
    case 2: click(L::tab(2, OptionsPageCount)); break;                                     // Sound
    case 3: click(L::increase(OptOcclusion - OptionsPageStart[2])); page(L"desktop-sound.ppm"); break;
    case 4: click(L::tab(0, OptionsPageCount)); break;                                     // Movement
    case 5: click(L::increase(OptSnapAngle - OptionsPageStart[0])); page(L"desktop-movement.ppm"); break; // VR only.
    case 6: click(L::tab(3, OptionsPageCount)); break;                                     // Voice chat
    case 7: click(L::increase(OptVoiceVolume - OptionsPageStart[3])); break;
    case 8: click(L::increase(OptMicrophone - OptionsPageStart[3])); click(L::increase(OptMicrophone - OptionsPageStart[3])); break;
    case 9: page(L"desktop-voice-changed.ppm"); click(L::resetButton()); break;
    case 10: page(L"desktop-voice-reset.ppm"); click(L::closeButton()); break;
    case 11: log("Lab desktop options: panel %s", panel.window ? "still open" : "closed"); break;
    default: break;
    }
}
LRESULT CALLBACK buttonProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    try {
        switch (message) {
        case WM_PAINT:
            drawButton();
            paint(window, *button.bitmap);
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE; // The game keeps the focus.
        case WM_MOUSEMOVE:
            if (!button.hover) { button.hover = true; InvalidateRect(window, nullptr, FALSE); }
            if (!button.tracking) {
                TRACKMOUSEEVENT track{sizeof track, TME_LEAVE, window, 0};
                button.tracking = TrackMouseEvent(&track) != FALSE;
            }
            return 0;
        case WM_MOUSELEAVE:
            button.hover = button.tracking = false;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: openPanel(); return 0;
        case WM_TIMER: {
            // Under the game's menu, bottom centre of its window, while a menu is up and the panel is closed.
            const HWND game = gameWindow.load();
            const bool show = game && IsWindowVisible(game) && !IsIconic(game) && menuUp.load() && !panel.window;
            if (show) {
                const RECT g = clientOnScreen(game);
                const LONG gh = g.bottom - g.top, bh = std::clamp<LONG>(gh / 16, 28, 64), bw = bh * 4;
                SetWindowPos(window, HWND_TOP, (g.left + g.right - bw) / 2, g.bottom - bh - gh / 40, bw, bh,
                             SWP_NOACTIVATE | SWP_SHOWWINDOW);
            } else if (IsWindowVisible(window)) {
                ShowWindow(window, SW_HIDE);
                button.hover = false;
            }
            if (!menuUp.load() && panel.window) DestroyWindow(panel.window); // The menu closed (e.g. back in play).
            if (labTest()) labStep(show);
            return 0;
        }
        default: break;
        }
    } catch (const std::exception &e) {
        log("Desktop VR options button failed: %s", e.what());
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
DWORD WINAPI uiThread(void *) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW panelClass{};
    panelClass.cbSize = sizeof panelClass;
    panelClass.lpfnWndProc = panelProc;
    panelClass.hInstance = instance;
    panelClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)) /* IDC_ARROW */;
    panelClass.lpszClassName = PanelClass;
    WNDCLASSEXW buttonClass = panelClass;
    buttonClass.lpfnWndProc = buttonProc;
    buttonClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)) /* IDC_HAND */;
    buttonClass.lpszClassName = ButtonClass;
    const HWND game = gameWindow.load();
    if (!RegisterClassExW(&panelClass) || !RegisterClassExW(&buttonClass) ||
        !(button.window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, ButtonClass, L"VR options", WS_POPUP, 0, 0, 1, 1,
                                          game, nullptr, instance, nullptr)) ||
        !SetTimer(button.window, TickTimer, 100, nullptr)) {
        log("Desktop VR options unavailable (%lu)", GetLastError());
        return 0;
    }
    log("Desktop VR options ready");
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
} // namespace

void start(const std::wstring &settingsFile, const VrSettings &startup) noexcept {
    try {
        file = settingsFile;
        startupSettings = startup;
    } catch (...) {
    }
}
void frame(bool desktopMenu, HWND window) noexcept {
    menuUp.store(desktopMenu && window);
    if (window) gameWindow.store(window);
    // The windows are owned by the game's: made on the first menu that shows the button.
    if (!desktopMenu || !window || file.empty() || started.exchange(true)) return;
    if (const HANDLE thread = CreateThread(nullptr, 0, uiThread, nullptr, 0, nullptr)) CloseHandle(thread);
    else log("Desktop VR options thread unavailable (%lu)", GetLastError());
}
} // namespace ss2vr::game::desktop_options
