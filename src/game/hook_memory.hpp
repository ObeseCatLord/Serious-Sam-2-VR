#pragma once
// MinHook refuses a target whose page is not reported executable
// (MH_ERROR_NOT_EXECUTABLE). Under Proton's x86 emulation on ARM64 (FEX,
// Steam Frame) the game's own code pages are reported read-only, so every
// engine hook failed there. A committed, readable page about to be patched is
// given its executable protection first; elsewhere it already has it.
#include <cstdint>
#include <initializer_list>
#include <windows.h>
#include <MinHook.h>
namespace ss2vr {
inline void ensureExecutable(const void *target) noexcept {
    const auto *p = static_cast<const uint8_t *>(target);
    for (const uint8_t *at : {p, p + 31}) { // A patch and its relocated bytes may cross a page.
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(at, &info, sizeof info) || info.State != MEM_COMMIT) continue;
        const DWORD base = info.Protect & 0xff, flags = info.Protect & ~DWORD(0xff);
        DWORD executable = 0;
        if (base == PAGE_READONLY) executable = PAGE_EXECUTE_READ;
        else if (base == PAGE_READWRITE) executable = PAGE_EXECUTE_READWRITE;
        else if (base == PAGE_WRITECOPY) executable = PAGE_EXECUTE_WRITECOPY;
        if (!executable) continue;
        DWORD old = 0;
        VirtualProtect(const_cast<uint8_t *>(at), 1, executable | flags, &old);
    }
}
inline MH_STATUS createHook(void *target, void *detour, void **original) noexcept {
    if (target) ensureExecutable(target);
    return MH_CreateHook(target, detour, original);
}
} // namespace ss2vr
