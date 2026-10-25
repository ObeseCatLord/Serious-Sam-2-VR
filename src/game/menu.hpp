#pragma once
#include <cstdint>
#include <windows.h>
namespace ss2vr::game::menus {
using HookInstaller = bool (*)(HMODULE, uint32_t, void *, void **);
bool initialize(HMODULE sam, HookInstaller install);
bool active();
uint32_t captureGeneration();
bool pointerAvailable();
// Queue one native menu system command (syscmdOk 0x0B, syscmdBack 0x0C, Left/
// Right/Up/Down 0x0D-0x10, F1/F2 0x15/0x16) for the next menu poll. No OS input,
// so it works without window focus. Returns false when no native menu is open.
bool queueCommand(int command);
// A movie (the title attract demo, Extras, in-level films) plays in front of the
// menus: the main menu stays current but its dispatcher does not run.
bool movieOnTop();
// A menu over a level in VR (the pause menu): the game is told the menu hides
// the world, so it draws the menu alone (over black, which the bridge keys out)
// while the headset keeps the last world frame behind it.
bool worldHidden();
// A menu that takes input is up (not a loading screen or a movie). Main thread.
bool interactiveMenu();
} // namespace ss2vr::game::menus
