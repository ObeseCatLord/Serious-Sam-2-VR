#pragma once
#include "common/settings.hpp"
#include <string>
#include <windows.h>

// VR options on the desktop: without a headset driving the game, a "VR options"
// button sits under the game's menus and opens the VR options panel as a window
// (mouse). Settings that only apply in VR are greyed out; sound and voice chat
// can be changed. Saved to SS2VR.user.ini like the VR panel, which the game
// reloads. Its windows run on their own thread.
namespace ss2vr::game::desktop_options {
// Once, at startup: the settings file and the settings as the game started.
void start(const std::wstring &settingsFile, const VrSettings &startup) noexcept;
// Main thread, every frame: whether a game menu that takes input is up in
// desktop play, and the game's window.
void frame(bool desktopMenu, HWND window) noexcept;
} // namespace ss2vr::game::desktop_options
