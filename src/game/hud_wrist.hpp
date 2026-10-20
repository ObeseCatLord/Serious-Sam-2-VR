#pragma once
// Wrist HUD ([HUD] Mode): which of the game's HUD elements is drawing, and
// so which watch-style wrist panel its draws belong to. The game's CHUD
// renders each element through its vtable (slot 0x28); the exported element
// vtables are patched to note the element around that call.
#include <windows.h>

namespace ss2vr::game::hud_wrist {

enum Side : int { Head = -1, Left = 0, Right = 1 };
// Patches the element vtables of Sam2Game (main thread, at attach).
bool install(HMODULE game, HMODULE engine) noexcept;
// Restores them (rollback).
void uninstall() noexcept;
// During an element's render: its side, index and class name; otherwise Head/-1/null.
int currentSide() noexcept;
int currentIndex() noexcept;
const char *currentElement() noexcept;
constexpr unsigned MaxElements = 32;
// [HUD] Mode (true: wrists) and WristScale (metres per HUD canvas width).
void configure(bool wrists, float scale) noexcept;
bool wrists() noexcept;
float scale() noexcept;
// The element's 2D vertex bounds bound since the last take (its positions in
// the engine's 2D space, from _gfxBindVertexArray); false when none.
bool takeBounds(float (&minimum)[3], float (&maximum)[3]) noexcept;

} // namespace ss2vr::game::hud_wrist
