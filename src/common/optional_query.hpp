#pragma once
#include <cstdint>
namespace ss2vr {
// Lexical cancellation only. No native resource, traversal, movement, or
// successful-query ownership is supplied by this decision function.
enum class OptionalQueryDecision : unsigned { original, cancel, clearBranch };
constexpr OptionalQueryDecision optionalQueryDecision(bool *unavailable,
                                                      bool replacementPending,
                                                      unsigned gateKind) noexcept {
    if (!unavailable) return OptionalQueryDecision::original;
    if (*unavailable) return OptionalQueryDecision::cancel;
    if (gateKind>1 || (gateKind==0 && replacementPending)) {
        *unavailable=true;
        return OptionalQueryDecision::cancel;
    }
    return gateKind==0?OptionalQueryDecision::clearBranch:OptionalQueryDecision::original;
}
// Pinned gfxLockVertexBuffer system-memory route only. Unknown GPU providers
// and an already-held buffer cancel the optional query before any lock occurs.
constexpr bool optionalQueryBufferReadable(uint32_t data, int32_t bytes, int16_t locks,
                                          uint8_t format, int32_t requested, int32_t offset) noexcept {
    if (!data || bytes <= 0 || locks != 0 || (format & 0x3fu) == 0x1fu || offset < 0)
        return false;
    const uint32_t count = requested > 0 ? uint32_t(requested) : uint32_t(bytes);
    return uint64_t(uint32_t(offset)) + count <= uint32_t(bytes) &&
           uint64_t(data) + uint32_t(bytes) <= (uint64_t(1) << 32);
}
// Provider-owned static buffers (format 0x1f) lock through the engine's
// _gfxLockStaticVertexBuffer slot. Admit only the fingerprinted GfxD3D pair,
// whose lock is IDirect3DVertexBuffer9::Lock(D3DLOCK_READONLY) for read type
// 0x93, on a buffer not already locked and within its recorded size. Any other
// provider, access type or held buffer still cancels before the lock.
constexpr bool optionalQueryProviderBufferLockable(uint32_t buffer, int32_t bytes, int16_t locks,
                                                  uint8_t format, int32_t type, int32_t requested,
                                                  int32_t offset, uint32_t lockSlot, uint32_t unlockSlot,
                                                  uint32_t pinnedLock, uint32_t pinnedUnlock) noexcept {
    if (!buffer || bytes <= 0 || locks != 0 || (format & 0x3fu) != 0x1fu || type != 0x93 || offset < 0 ||
        !pinnedLock || !pinnedUnlock || lockSlot != pinnedLock || unlockSlot != pinnedUnlock)
        return false;
    const uint32_t count = requested > 0 ? uint32_t(requested) : uint32_t(bytes);
    return uint64_t(uint32_t(offset)) + count <= uint32_t(bytes);
}
// Admit only a pinned, inspected virtual target inside an optional query.
// Unknown shapes cancel the query; they are never treated as empty space.
constexpr OptionalQueryDecision optionalQueryTargetDecision(bool* unavailable,
                                                            bool supported) noexcept {
    if (!unavailable) return OptionalQueryDecision::original;
    if (*unavailable || !supported) {
        *unavailable=true;
        return OptionalQueryDecision::cancel;
    }
    return OptionalQueryDecision::original;
}
} // namespace ss2vr
