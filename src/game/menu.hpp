#pragma once
#include <cstdint>
#include <windows.h>
namespace ss2vr::game::menus {
using HookInstaller = bool (*)(HMODULE, uint32_t, void *, void **);
bool initialize(HMODULE sam, HookInstaller install);
bool active();
uint32_t captureGeneration();
bool pointerAvailable();
// Queue one native menu key (engine key id) for the next menu poll; no OS input,
// so it works without window focus. Returns false when no native menu is open.
bool queueKey(int key);
} // namespace ss2vr::game::menus
