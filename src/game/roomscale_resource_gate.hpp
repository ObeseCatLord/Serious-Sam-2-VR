#pragma once
#include <windows.h>
#include <cstdint>

namespace ss2vr::game {
using RoomscaleInternalHook = bool (*)(HMODULE, uint32_t, void *, void **);
using RoomscaleResourceBody = void (__cdecl *)(void *);
// Experimental optional-query boundary. These functions do not activate body
// movement or prove collision fractions. The owner must fingerprint Engine,
// successfully enable every queued gate before running a scope, exclude foreign
// query workers, provide fresh native query cleanup, and make
// the entire result unusable if unavailable becomes true.
bool queueRoomscaleResourceGates(HMODULE engine, RoomscaleInternalHook registrar);
void resetRoomscaleResourceGatesAfterRemoval() noexcept;
bool runRoomscaleResourceScope(bool &unavailable, DWORD simulationThread,
                               RoomscaleResourceBody body, void *context) noexcept;
// Only the checked-placement adapter may end filtering immediately before its
// first approved native write. Subsequent native commit callbacks stay native.
bool roomscaleResourceGatesUsable() noexcept;
bool roomscaleResourceScopeUsable() noexcept;
bool finishRoomscaleResourceScopeForCommit(bool &unavailable) noexcept;
// Lab diagnostic only: the first gate that cancelled the current optional query
// since reset. gate is the source line of its RESOURCE_GATE declaration; table is
// the receiver/vtable pointer (not dereferenced when unrecognized).
struct RoomscaleResourceCancel {
    uint32_t gate=0,kind=0,resource=0,engineBase=0,flags=0,handle=0,type=0;
};
void resetRoomscaleResourceCancel() noexcept;
RoomscaleResourceCancel roomscaleResourceCancel() noexcept;
} // namespace ss2vr::game
