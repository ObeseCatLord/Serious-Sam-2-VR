#define WIN32_LEAN_AND_MEAN
#include "hook_memory.hpp"
#include "common/winproc.hpp"
#include "common/winpath.hpp"
#include "game.hpp"
#include "spatial_audio.hpp"
#include "native_finally.hpp"
#include <MinHook.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <d3d9.h>
#include <new>
#include <unordered_map>
#include <vector>
#include <windows.h>
using Create9 = IDirect3D9 *(WINAPI *)(UINT);
using Create9Ex = HRESULT(WINAPI *)(UINT, IDirect3D9Ex **);
using CreateDevice = HRESULT(WINAPI *)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *,
                                       IDirect3DDevice9 **);
using Reset = HRESULT(WINAPI *)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
static HMODULE realDll = nullptr;
static Create9 create9 = nullptr;
static Create9Ex create9ex = nullptr;
static CreateDevice originalCreate = nullptr;
static Reset originalReset = nullptr;
using Present = HRESULT(WINAPI *)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
static Present originalPresent = nullptr;
static thread_local unsigned presentationDepth = 0;
using AdditionalChain = HRESULT(WINAPI *)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *, IDirect3DSwapChain9 **);
using ChainPresent = HRESULT(WINAPI *)(IDirect3DSwapChain9 *, const RECT *, const RECT *, HWND,
                                     const RGNDATA *, DWORD);
static AdditionalChain originalAdditional = nullptr;
// Windows itself (not Wine/Proton): a classic Direct3D 9 device cannot share its
// eye images with the headset program (D3D11), so every frame went through the
// CPU (single-digit frame rates). The game's device is created from Direct3D 9Ex
// instead, whose render targets can be shared. 9Ex refuses the managed pool, which
// the game uses for its textures and meshes: those are created in the default
// pool instead (textures dynamic, so the game can still lock and fill them).
// [Render] D3D9Ex: 1 on (Windows only), 0 off (default until confirmed on Windows).
using ss2vr::game::managedEmulation;
static UINT factoryVersion = D3D_SDK_VERSION; // The version the game asked for.
using CreateTextureFn = HRESULT(WINAPI *)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                          IDirect3DTexture9 **, HANDLE *);
using CreateVolumeFn = HRESULT(WINAPI *)(IDirect3DDevice9 *, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                         IDirect3DVolumeTexture9 **, HANDLE *);
using CreateCubeFn = HRESULT(WINAPI *)(IDirect3DDevice9 *, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                       IDirect3DCubeTexture9 **, HANDLE *);
using CreateVertexFn = HRESULT(WINAPI *)(IDirect3DDevice9 *, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9 **,
                                         HANDLE *);
using CreateIndexFn = HRESULT(WINAPI *)(IDirect3DDevice9 *, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9 **,
                                        HANDLE *);
static CreateTextureFn originalCreateTexture = nullptr;
static CreateVolumeFn originalCreateVolume = nullptr;
static CreateCubeFn originalCreateCube = nullptr;
static CreateVertexFn originalCreateVertex = nullptr;
static CreateIndexFn originalCreateIndex = nullptr;
static void noteManaged(const char *kind, HRESULT hr) {
    static std::atomic<unsigned> converted{0}, failed{0};
    if (SUCCEEDED(hr)) {
        if (converted.fetch_add(1, std::memory_order_relaxed) == 0)
            ss2vr::game::log("Direct3D 9Ex: managed %s created in the default pool", kind);
    } else if (failed.fetch_add(1, std::memory_order_relaxed) < 8)
        ss2vr::game::log("Direct3D 9Ex: default-pool %s refused hr=%08lx", kind, static_cast<unsigned long>(hr));
}
// Textures: dynamic so they stay lockable (default-pool textures can be locked only
// when dynamic). A format or usage the driver refuses as dynamic is created static:
// it cannot be locked, so it is logged (format/usage) for a Windows report.
static void noteStatic(const char *kind, D3DFORMAT format, DWORD usage) {
    static std::atomic<unsigned> traced{0};
    if (traced.fetch_add(1, std::memory_order_relaxed) < 16)
        ss2vr::game::log("Direct3D 9Ex: %s format=%lu usage=%08lx not available dynamic; created static (not lockable)",
                         kind, static_cast<unsigned long>(format), static_cast<unsigned long>(usage));
}
static HRESULT WINAPI createTexture(IDirect3DDevice9 *d, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT format,
                                    D3DPOOL pool, IDirect3DTexture9 **out, HANDLE *share) {
    if (pool != D3DPOOL_MANAGED || !managedEmulation.load(std::memory_order_relaxed))
        return originalCreateTexture(d, w, h, levels, usage, format, pool, out, share);
    HRESULT hr = originalCreateTexture(d, w, h, levels, usage | D3DUSAGE_DYNAMIC, format, D3DPOOL_DEFAULT, out, share);
    if (FAILED(hr)) {
        hr = originalCreateTexture(d, w, h, levels, usage, format, D3DPOOL_DEFAULT, out, share);
        if (SUCCEEDED(hr)) noteStatic("texture", format, usage);
    }
    noteManaged("texture", hr);
    return hr;
}
static HRESULT WINAPI createVolume(IDirect3DDevice9 *d, UINT w, UINT h, UINT depth, UINT levels, DWORD usage,
                                   D3DFORMAT format, D3DPOOL pool, IDirect3DVolumeTexture9 **out, HANDLE *share) {
    if (pool != D3DPOOL_MANAGED || !managedEmulation.load(std::memory_order_relaxed))
        return originalCreateVolume(d, w, h, depth, levels, usage, format, pool, out, share);
    HRESULT hr = originalCreateVolume(d, w, h, depth, levels, usage | D3DUSAGE_DYNAMIC, format, D3DPOOL_DEFAULT, out, share);
    if (FAILED(hr)) {
        hr = originalCreateVolume(d, w, h, depth, levels, usage, format, D3DPOOL_DEFAULT, out, share);
        if (SUCCEEDED(hr)) noteStatic("volume texture", format, usage);
    }
    noteManaged("volume texture", hr);
    return hr;
}
static HRESULT WINAPI createCube(IDirect3DDevice9 *d, UINT edge, UINT levels, DWORD usage, D3DFORMAT format,
                                 D3DPOOL pool, IDirect3DCubeTexture9 **out, HANDLE *share) {
    if (pool != D3DPOOL_MANAGED || !managedEmulation.load(std::memory_order_relaxed))
        return originalCreateCube(d, edge, levels, usage, format, pool, out, share);
    HRESULT hr = originalCreateCube(d, edge, levels, usage | D3DUSAGE_DYNAMIC, format, D3DPOOL_DEFAULT, out, share);
    if (FAILED(hr)) {
        hr = originalCreateCube(d, edge, levels, usage, format, D3DPOOL_DEFAULT, out, share);
        if (SUCCEEDED(hr)) noteStatic("cube texture", format, usage);
    }
    noteManaged("cube texture", hr);
    return hr;
}
// One line per start for a Windows report: what this driver allows of the
// textures the game's managed ones became. False when a format the game's
// textures use cannot be created dynamic and locked: the classic device then.
static bool selfTest(IDirect3DDevice9 *d) {
    bool usable = true;
    char line[256]{};
    int used = std::snprintf(line, sizeof line, "Direct3D 9Ex self-test:");
    const struct { const char *name; D3DFORMAT format; } formats[]{
        {"A8R8G8B8", D3DFMT_A8R8G8B8}, {"DXT1", D3DFMT_DXT1}, {"DXT3", D3DFMT_DXT3}, {"DXT5", D3DFMT_DXT5}};
    for (const auto &f : formats) {
        IDirect3DTexture9 *t = nullptr;
        HRESULT created = originalCreateTexture(d, 64, 64, 0, D3DUSAGE_DYNAMIC, f.format, D3DPOOL_DEFAULT, &t, nullptr);
        bool locked = false;
        if (SUCCEEDED(created) && t) {
            D3DLOCKED_RECT r{};
            locked = SUCCEEDED(t->LockRect(1, &r, nullptr, 0)) && r.pBits;
            if (locked) t->UnlockRect(1);
            t->Release();
        }
        IDirect3DCubeTexture9 *c = nullptr;
        const bool cube = SUCCEEDED(originalCreateCube(d, 64, 0, D3DUSAGE_DYNAMIC, f.format, D3DPOOL_DEFAULT, &c, nullptr)) && c;
        if (c) c->Release();
        IDirect3DTexture9 *a = nullptr;
        const bool autogen = SUCCEEDED(originalCreateTexture(d, 64, 64, 0, D3DUSAGE_DYNAMIC | D3DUSAGE_AUTOGENMIPMAP, f.format,
                                                             D3DPOOL_DEFAULT, &a, nullptr)) && a;
        if (a) a->Release();
        if (used > 0 && used < int(sizeof line))
            used += std::snprintf(line + used, sizeof line - used, " %s dynamic=%d lock=%d cube=%d autogen=%d", f.name,
                                  int(SUCCEEDED(created)), int(locked), int(cube), int(autogen));
        usable = usable && SUCCEEDED(created) && locked;
    }
    ss2vr::game::log("%s", line);
    return usable;
}
// Direct3D 9Ex buffer copies: the game's managed vertex and index buffers keep a
// system-memory copy, as the managed pool did. The game reads its meshes back
// through READONLY locks every frame (collision hulls, CPU-side mesh work); in the
// default pool those reads came straight from driver memory. A lock returns the
// copy; the unlock of a writing lock uploads the written range to the real buffer.
// [Render] D3D9ExBufferCopies=0 turns this off (locks reach the driver again).
namespace buffer_copies {
using LockFn = HRESULT(WINAPI *)(void *, UINT, UINT, void **, DWORD);
using UnlockFn = HRESULT(WINAPI *)(void *);
LockFn oLock[2]{};     // [vertex, index]
UnlockFn oUnlock[2]{};
void *hookedLock[2]{}, *hookedUnlock[2]{}; // The implementations those detours cover.
std::atomic<uint64_t> foreign{0};          // Buffers of another implementation: locked directly.
std::atomic<bool> enabled{true};
std::atomic<uint64_t> buffers{0}, bytes{0}, uploads{0}, uploadBytes{0}, uploadTicks{0}, uploadFailures{0};
SRWLOCK guard = SRWLOCK_INIT;
// Most locks (tens of thousands a second) are of the game's dynamic buffers, which
// have no copy: the buffers last found to have none, so those locks skip the table
// and its lock. A slot is cleared when a buffer at that address gets a copy.
std::atomic<void *> uncopied[256]{};
inline std::atomic<void *> &uncopiedSlot(void *b) noexcept {
    return uncopied[(uint32_t(reinterpret_cast<uintptr_t>(b) >> 4) * 2654435761u) >> 24];
}
inline bool knownUncopied(void *b) noexcept { return uncopiedSlot(b).load(std::memory_order_relaxed) == b; }
struct Copy;
std::unordered_map<void *, Copy *> *copies = nullptr; // Never destroyed: buffers may outlive static teardown.
static const GUID tag{0x5b0a6c5e, 0x4d21, 0x4f0b, {0x9a, 0x3e, 0x53, 0x53, 0x32, 0x56, 0x52, 0x01}};
static const GUID unknownId{0x00000000, 0x0000, 0x0000, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
// Owned by the buffer (private data): destroyed with it.
struct Copy final : IUnknown {
    std::atomic<ULONG> references{1};
    void *buffer = nullptr;
    uint8_t *data = nullptr;
    UINT length = 0, locks = 0, dirtyLow = ~0u, dirtyHigh = 0;
    unsigned locker = 0, unlocker = 0; // Which originals reach this buffer's driver methods.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        if (!out) return E_POINTER;
        if (std::memcmp(&id, &unknownId, sizeof(GUID))) { *out = nullptr; return E_NOINTERFACE; }
        *out = static_cast<IUnknown *>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return references.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = references.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (!left) {
            AcquireSRWLockExclusive(&guard);
            if (copies) {
                const auto found = copies->find(buffer);
                if (found != copies->end() && found->second == this) copies->erase(found);
            }
            ReleaseSRWLockExclusive(&guard);
            buffers.fetch_sub(1, std::memory_order_relaxed);
            bytes.fetch_sub(length, std::memory_order_relaxed);
            std::free(data);
            delete this;
        }
        return left;
    }
    virtual ~Copy() = default;
};
// A buffer the game asked for in the managed pool, just created in the default one.
template <class Buffer> void adopt(Buffer *b, UINT length, unsigned kind) noexcept {
    if (!enabled.load(std::memory_order_relaxed) || !b || !length) return;
    // Only a buffer whose own Lock and Unlock are both detoured: with one of them
    // missed, writes would stay in the copy and never reach the driver.
    void **methods = *reinterpret_cast<void ***>(b);
    int locker = -1, unlocker = -1;
    for (int k = 0; k < 2; ++k) {
        if (locker < 0 && hookedLock[k] && oLock[k] && methods[11] == hookedLock[k]) locker = k;
        if (unlocker < 0 && hookedUnlock[k] && oUnlock[k] && methods[12] == hookedUnlock[k]) unlocker = k;
    }
    if (locker < 0 || unlocker < 0) {
        if (foreign.fetch_add(1, std::memory_order_relaxed) < 4)
            ss2vr::game::log("Direct3D 9Ex: %s buffer of another implementation; locked directly", kind ? "index" : "vertex");
        return;
    }
    auto *data = static_cast<uint8_t *>(std::calloc(1, length));
    auto *copy = data ? new (std::nothrow) Copy : nullptr;
    if (!copy) { std::free(data); return; }
    copy->buffer = b; copy->data = data; copy->length = length; copy->locker = unsigned(locker); copy->unlocker = unsigned(unlocker);
    buffers.fetch_add(1, std::memory_order_relaxed);
    bytes.fetch_add(length, std::memory_order_relaxed);
    AcquireSRWLockExclusive(&guard);
    if (!copies) copies = new (std::nothrow) std::unordered_map<void *, Copy *>;
    bool listed = false;
    if (copies) {
        try { (*copies)[b] = copy; listed = true; } catch (...) {}
    }
    uncopiedSlot(b).store(nullptr, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&guard);
    // The buffer holds the copy from here; a refusal leaves the buffer as it was.
    if (listed && FAILED(b->SetPrivateData(tag, static_cast<IUnknown *>(copy), sizeof(IUnknown *), D3DSPD_IUNKNOWN))) {
        static std::atomic<unsigned> refused{0};
        if (refused.fetch_add(1, std::memory_order_relaxed) < 4) ss2vr::game::log("Direct3D 9Ex: buffer copy not attached; buffer locked directly");
    }
    copy->Release();
}
// True when the lock was answered from the copy (hr set).
bool lock(void *b, UINT offset, UINT size, void **out, DWORD flags, HRESULT &hr) noexcept {
    AcquireSRWLockExclusive(&guard);
    Copy *copy = nullptr;
    if (copies) {
        const auto found = copies->find(b);
        if (found != copies->end()) copy = found->second;
    }
    if (copy) {
        if (!out || offset > copy->length || size > copy->length - offset) hr = D3DERR_INVALIDCALL;
        else {
            const UINT count = size ? size : copy->length - offset; // Size 0: to the end.
            ++copy->locks;
            if (!(flags & D3DLOCK_READONLY) && count) {
                if (offset < copy->dirtyLow) copy->dirtyLow = offset;
                if (offset + count > copy->dirtyHigh) copy->dirtyHigh = offset + count;
            }
            *out = copy->data + offset;
            hr = D3D_OK;
        }
    }
    ReleaseSRWLockExclusive(&guard);
    return copy != nullptr;
}
// True when the buffer has a copy (hr set); the last unlock uploads what was written.
bool unlock(void *b, HRESULT &hr) noexcept {
    AcquireSRWLockExclusive(&guard);
    Copy *copy = nullptr;
    if (copies) {
        const auto found = copies->find(b);
        if (found != copies->end()) copy = found->second;
    }
    UINT low = 0, high = 0;
    if (copy) {
        if (copy->locks) --copy->locks;
        if (!copy->locks && copy->dirtyHigh > copy->dirtyLow) {
            low = copy->dirtyLow; high = copy->dirtyHigh;
            copy->dirtyLow = ~0u; copy->dirtyHigh = 0;
        }
    }
    ReleaseSRWLockExclusive(&guard);
    if (!copy) return false;
    hr = D3D_OK;
    if (high > low) { // The caller holds the buffer: the copy outlives this call.
        LARGE_INTEGER a, c;
        QueryPerformanceCounter(&a);
        void *mapped = nullptr;
        const HRESULT locked = oLock[copy->locker](b, low, high - low, &mapped, 0);
        if (SUCCEEDED(locked) && mapped) {
            std::memcpy(mapped, copy->data + low, high - low);
            oUnlock[copy->unlocker](b);
            QueryPerformanceCounter(&c);
            uploads.fetch_add(1, std::memory_order_relaxed);
            uploadBytes.fetch_add(high - low, std::memory_order_relaxed);
            uploadTicks.fetch_add(uint64_t(c.QuadPart - a.QuadPart), std::memory_order_relaxed);
        } else {
            if (SUCCEEDED(locked)) oUnlock[copy->unlocker](b);
            AcquireSRWLockExclusive(&guard); // Kept for the next unlock.
            if (low < copy->dirtyLow) copy->dirtyLow = low;
            if (high > copy->dirtyHigh) copy->dirtyHigh = high;
            ReleaseSRWLockExclusive(&guard);
            if (uploadFailures.fetch_add(1, std::memory_order_relaxed) < 8)
                ss2vr::game::log("Direct3D 9Ex: buffer upload refused hr=%08lx bytes=%u", static_cast<unsigned long>(locked), high - low);
        }
    }
    return true;
}
} // namespace buffer_copies
// Direct3D 9Ex lock timing (Windows reports): how long the game's locks of the
// resources its managed ones became take, per kind and flags, every 5 s. A
// default-pool lock waits for the GPU where a managed one wrote a system copy.
namespace lock_timing {
enum Kind { Texture, Cube, Vertex, Index, Kinds };
struct Totals { std::atomic<uint64_t> ticks{0}, count{0}, plain{0}, discard{0}, noOverwrite{0}, readOnly{0}, heldRead{0}, heldWrite{0}; };
// Lock to unlock (the game reading or filling the memory), per buffer kind.
struct Held { void *buffer = nullptr; LARGE_INTEGER start{}; bool readOnly = false; };
thread_local Held held[2];
Totals totals[Kinds];
std::atomic<uint64_t> reportedAt{0};
LARGE_INTEGER frequency{};
using LockTexture = HRESULT(WINAPI *)(IDirect3DTexture9 *, UINT, D3DLOCKED_RECT *, const RECT *, DWORD);
using LockCube = HRESULT(WINAPI *)(IDirect3DCubeTexture9 *, D3DCUBEMAP_FACES, UINT, D3DLOCKED_RECT *, const RECT *, DWORD);
using LockVertex = HRESULT(WINAPI *)(IDirect3DVertexBuffer9 *, UINT, UINT, void **, DWORD);
using LockIndex = HRESULT(WINAPI *)(IDirect3DIndexBuffer9 *, UINT, UINT, void **, DWORD);
LockTexture oTexture = nullptr; LockCube oCube = nullptr; LockVertex oVertex = nullptr; LockIndex oIndex = nullptr;
void report() noexcept {
    const uint64_t now = GetTickCount64();
    uint64_t last = reportedAt.load(std::memory_order_relaxed);
    if (now - last < 5000 || !reportedAt.compare_exchange_strong(last, now)) return;
    if (!last) return; // The first call starts the window.
    const double seconds = double(now - last) / 1000., toMs = frequency.QuadPart ? 1000. / double(frequency.QuadPart) : 0.;
    static const char *names[Kinds]{"texture", "cube", "vertex", "index"};
    char line[512]; int used = std::snprintf(line, sizeof line, "Direct3D 9Ex locks per s:");
    for (unsigned k = 0; k < Kinds && used > 0 && used < int(sizeof line); ++k) {
        auto &t = totals[k];
        used += std::snprintf(line + used, sizeof line - used, " %s n=%.0f ms=%.2f (plain=%.0f discard=%.0f nooverwrite=%.0f readonly=%.0f)", names[k],
                              double(t.count.exchange(0)) / seconds, double(t.ticks.exchange(0)) * toMs / seconds, double(t.plain.exchange(0)) / seconds,
                              double(t.discard.exchange(0)) / seconds, double(t.noOverwrite.exchange(0)) / seconds, double(t.readOnly.exchange(0)) / seconds);
    }
    static std::atomic<unsigned> lines{0};
    if (lines.fetch_add(1, std::memory_order_relaxed) >= 200) return;
    ss2vr::game::log("%s", line);
    // Held: lock to unlock, the game's own reads and writes of the memory.
    using namespace buffer_copies;
    const double uploaded = double(uploads.exchange(0));
    ss2vr::game::log("Direct3D 9Ex buffers per s: vertex held read=%.2fms write=%.2fms | index held read=%.2fms write=%.2fms | "
                     "copies=%llu (%.1f MB) direct=%llu uploads=%.0f %.2f MB %.2fms failed=%llu",
                     double(totals[Vertex].heldRead.exchange(0)) * toMs / seconds, double(totals[Vertex].heldWrite.exchange(0)) * toMs / seconds,
                     double(totals[Index].heldRead.exchange(0)) * toMs / seconds, double(totals[Index].heldWrite.exchange(0)) * toMs / seconds,
                     static_cast<unsigned long long>(buffers.load()), double(bytes.load()) / 1048576.,
                     static_cast<unsigned long long>(foreign.load()), uploaded / seconds,
                     double(uploadBytes.exchange(0)) / 1048576. / seconds, double(uploadTicks.exchange(0)) * toMs / seconds,
                     static_cast<unsigned long long>(uploadFailures.load()));
}
// SS2VR_LAB_LOCK_TIMING=1: the counters and times above (four clock reads and a
// table lookup per lock otherwise spent on every one of the game's locks).
static const bool timing = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_LOCK_TIMING", v, 2) == 1 && v[0] == L'1'; }();
template <class F> HRESULT timed(Kind kind, DWORD flags, F &&call) {
    if (!timing) return call();
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    const HRESULT hr = call();
    QueryPerformanceCounter(&b);
    auto &t = totals[kind];
    t.ticks.fetch_add(uint64_t(b.QuadPart - a.QuadPart), std::memory_order_relaxed);
    t.count.fetch_add(1, std::memory_order_relaxed);
    (flags & D3DLOCK_READONLY ? t.readOnly : flags & D3DLOCK_DISCARD ? t.discard : flags & D3DLOCK_NOOVERWRITE ? t.noOverwrite : t.plain)
        .fetch_add(1, std::memory_order_relaxed);
    report();
    return hr;
}
HRESULT WINAPI texture(IDirect3DTexture9 *t, UINT level, D3DLOCKED_RECT *r, const RECT *rect, DWORD flags) {
    return timed(Texture, flags, [&] { return oTexture(t, level, r, rect, flags); });
}
HRESULT WINAPI cube(IDirect3DCubeTexture9 *t, D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT *r, const RECT *rect, DWORD flags) {
    return timed(Cube, flags, [&] { return oCube(t, face, level, r, rect, flags); });
}
template <class F> HRESULT buffer(Kind kind, void *b, UINT offset, UINT size, void **data, DWORD flags, F &&original) {
    if (!timing) {
        if (buffer_copies::knownUncopied(b)) return original();
        HRESULT copied = D3D_OK;
        if (buffer_copies::lock(b, offset, size, data, flags, copied)) return copied;
        buffer_copies::uncopiedSlot(b).store(b, std::memory_order_relaxed);
        return original();
    }
    const HRESULT hr = timed(kind, flags, [&] {
        HRESULT copied = D3D_OK;
        return buffer_copies::lock(b, offset, size, data, flags, copied) ? copied : original();
    });
    if (SUCCEEDED(hr)) {
        auto &h = held[kind - Vertex];
        h.buffer = b; h.readOnly = (flags & D3DLOCK_READONLY) != 0;
        QueryPerformanceCounter(&h.start);
    }
    return hr;
}
template <class F> HRESULT release(Kind kind, void *b, F &&original) {
    if (!timing && buffer_copies::knownUncopied(b)) return original();
    auto &h = held[kind - Vertex];
    if (h.buffer == b) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        (h.readOnly ? totals[kind].heldRead : totals[kind].heldWrite).fetch_add(uint64_t(now.QuadPart - h.start.QuadPart), std::memory_order_relaxed);
        h.buffer = nullptr;
    }
    HRESULT hr = D3D_OK;
    return buffer_copies::unlock(b, hr) ? hr : original();
}
HRESULT WINAPI vertex(IDirect3DVertexBuffer9 *b, UINT offset, UINT size, void **data, DWORD flags) {
    return buffer(Vertex, b, offset, size, data, flags, [&] { return oVertex(b, offset, size, data, flags); });
}
HRESULT WINAPI index(IDirect3DIndexBuffer9 *b, UINT offset, UINT size, void **data, DWORD flags) {
    return buffer(Index, b, offset, size, data, flags, [&] { return oIndex(b, offset, size, data, flags); });
}
using UnlockVertex = HRESULT(WINAPI *)(IDirect3DVertexBuffer9 *);
using UnlockIndex = HRESULT(WINAPI *)(IDirect3DIndexBuffer9 *);
UnlockVertex oVertexUnlock = nullptr; UnlockIndex oIndexUnlock = nullptr;
HRESULT WINAPI vertexUnlock(IDirect3DVertexBuffer9 *b) {
    return release(Vertex, b, [&] { return oVertexUnlock(b); });
}
HRESULT WINAPI indexUnlock(IDirect3DIndexBuffer9 *b) {
    return release(Index, b, [&] { return oIndexUnlock(b); });
}
} // namespace lock_timing
// Buffers in the default pool stay lockable (static, as the game made them).
static HRESULT WINAPI createVertex(IDirect3DDevice9 *d, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
                                   IDirect3DVertexBuffer9 **out, HANDLE *share) {
    if (pool != D3DPOOL_MANAGED || !managedEmulation.load(std::memory_order_relaxed))
        return originalCreateVertex(d, length, usage, fvf, pool, out, share);
    const HRESULT hr = originalCreateVertex(d, length, usage, fvf, D3DPOOL_DEFAULT, out, share);
    noteManaged("vertex buffer", hr);
    if (SUCCEEDED(hr) && out && *out && !(usage & D3DUSAGE_DYNAMIC)) buffer_copies::adopt(*out, length, 0);
    return hr;
}
static HRESULT WINAPI createIndex(IDirect3DDevice9 *d, UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                  IDirect3DIndexBuffer9 **out, HANDLE *share) {
    if (pool != D3DPOOL_MANAGED || !managedEmulation.load(std::memory_order_relaxed))
        return originalCreateIndex(d, length, usage, format, pool, out, share);
    const HRESULT hr = originalCreateIndex(d, length, usage, format, D3DPOOL_DEFAULT, out, share);
    noteManaged("index buffer", hr);
    if (SUCCEEDED(hr) && out && *out && !(usage & D3DUSAGE_DYNAMIC)) buffer_copies::adopt(*out, length, 1);
    return hr;
}
// [Render] D3D9ExBufferCopies: 1 (default) keeps system-memory copies of the game's
// managed vertex and index buffers on the 9Ex device; 0 locks the driver's memory.
static bool wantBufferCopies() {
    wchar_t lab[2]{};
    if (GetEnvironmentVariableW(L"SS2VR_LAB_BUFFER_COPIES", lab, 2) == 1) return lab[0] != L'0';
    std::wstring bin;
    if (!ss2vr::moduleDirectory(nullptr, bin)) return true;
    for (const wchar_t *name : {L"SS2VR\\SS2VR.user.ini", L"SS2VR\\SS2VR.ini"}) {
        wchar_t buffer[16]{};
        if (GetPrivateProfileStringW(L"Render", L"D3D9ExBufferCopies", L"", buffer, 16, (bin + name).c_str()))
            return _wtoi(buffer) != 0;
    }
    return true;
}
static bool wantD3D9Ex() {
    // Lab: SS2VR_LAB_D3D9EX=1 takes this route under Wine too (DXVK), to exercise it.
    wchar_t lab[2]{};
    if (GetEnvironmentVariableW(L"SS2VR_LAB_D3D9EX", lab, 2) == 1 && lab[0] == L'1') return true;
    if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) return false; // DXVK shares already.
    std::wstring bin;
    if (!ss2vr::moduleDirectory(nullptr, bin)) return true;
    int value = -1;
    for (const wchar_t *name : {L"SS2VR\\SS2VR.user.ini", L"SS2VR\\SS2VR.ini"}) {
        wchar_t buffer[16]{};
        if (GetPrivateProfileStringW(L"Render", L"D3D9Ex", L"", buffer, 16, (bin + name).c_str())) {
            value = _wtoi(buffer);
            break;
        }
    }
    return value != 0; // Default on (Windows); D3D9Ex=0 keeps the classic device.
}
// Two method implementations suffice for this bounded discovery probe. Unknown
// implementations are logged and left untouched, never mapped to a wrong ABI.
static struct ChainHook { void *address = nullptr; ChainPresent original = nullptr; bool enabled = false; }
    chainHooks[2];
static bool attachAtPresentation(IDirect3DDevice9 *d);
// Native frames reach either the device Present (GfxD3D+1C75) or the swap-chain
// Present (GfxD3D+1C68), depending on display mode. Attach from whichever runs.
static bool attachAtChainPresentation(IDirect3DSwapChain9 *chain, uintptr_t caller) {
    const auto gfx = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"GfxD3D.dll"));
    if (!gfx || caller != gfx + 0x1c68u || !chain) return false;
    IDirect3DDevice9 *device = nullptr;
    if (FAILED(chain->GetDevice(&device)) || !device) return false;
    const bool attached = attachAtPresentation(device);
    device->Release();
    return attached;
}
template<unsigned Index>
static __attribute__((noinline)) HRESULT WINAPI chainPresent(IDirect3DSwapChain9 *chain,
    const RECT *src, const RECT *dst, HWND window, const RGNDATA *region, DWORD flags) {
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_extract_return_addr(__builtin_return_address(0)));
    const auto ticket = presentationDepth == 0 ? ss2vr::game::traceChainPresent(chain, caller, window) : 0;
    HRESULT hr = D3DERR_INVALIDCALL;
    const bool outer = presentationDepth == 0;
    ++presentationDepth;
    ss2vr::game::withNativeFinally([&] {
        // The frame that attaches presents natively; VR presentation starts next frame.
        const bool attachedNow = outer && attachAtChainPresentation(chain, caller);
        if (outer && !attachedNow) ss2vr::game::presentChain(chain, caller, src, dst, window, region, flags);
        else if (ss2vr::game::nativePresentationCaller(caller, true)) ss2vr::game::nestedNativePresentation();
        LARGE_INTEGER start{}, end{};
        QueryPerformanceCounter(&start);
        hr = chainHooks[Index].original(chain, src, dst, window, region, flags);
        QueryPerformanceCounter(&end);
        if (outer) ss2vr::game::notePresentCall(start, end);
    }, [&](bool) noexcept { --presentationDepth; });
    if (FAILED(hr)) ss2vr::game::nativePresentationFailed(nullptr, chain);
    // Completion is scalar-only, including failed Present: no COM introspection
    // or retained resource crosses the original callback.
    if (ticket) ss2vr::game::log("Presentation probe ticket=%u completed=1 chain=%p caller=%p hr=%08lx",
        ticket, static_cast<void *>(chain), reinterpret_cast<void *>(caller), static_cast<unsigned long>(hr));
    return hr;
}
static void discoverChain(IDirect3DDevice9 *device, IDirect3DSwapChain9 *chain) {
    // This startup probe covers only the first creation owner. Hook records are
    // written on that thread and immutable once enabled; no cross-thread list.
    if (!chain || !ss2vr::game::presentationDeviceOwner(device)) return;
    void *address = (*reinterpret_cast<void ***>(chain))[3];
    for (auto &hook : chainHooks)
        if (hook.address == address) return;
    unsigned index = 0;
    while (index < 2 && chainHooks[index].address) ++index;
    if (index == 2) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            ss2vr::game::log("Presentation probe unknown chain method=%p; untouched", address);
        }
        return;
    }
    auto &hook = chainHooks[index];
    const auto detour = index == 0 ? chainPresent<0> : chainPresent<1>;
    const auto created = ss2vr::createHook(address, reinterpret_cast<void *>(detour),
                                       reinterpret_cast<void **>(&hook.original));
    MH_STATUS enabled = MH_UNKNOWN;
    if (created == MH_OK) {
        // Publish the trampoline/address before enabling its typed callback.
        hook.address = address;
        enabled = MH_EnableHook(address);
        hook.enabled = enabled == MH_OK;
        if (!hook.enabled) {
            MH_RemoveHook(address);
            hook = {};
        }
    }
    static bool failedCreate = false, failedEnable = false;
    bool report = hook.enabled;
    if (created != MH_OK && !failedCreate) { failedCreate = true; report = true; }
    if (created == MH_OK && enabled != MH_OK && !failedEnable) { failedEnable = true; report = true; }
    if (report) ss2vr::game::log("Presentation probe chain method=%p create=%d enable=%d covered=%d",
        address, static_cast<int>(created), static_cast<int>(enabled), hook.enabled);
}
static void seedChain(IDirect3DDevice9 *device) noexcept {
    if (!ss2vr::game::presentationDeviceOwner(device)) return;
    IDirect3DSwapChain9 *chain = nullptr;
    ss2vr::game::withNativeFinally([&] {
        const HRESULT hr = device->GetSwapChain(0, &chain);
        if (SUCCEEDED(hr) && chain) discoverChain(device, chain);
        else {
            static bool reported = false;
            if (!reported) {
                reported = true;
                ss2vr::game::log("Presentation probe implicit seed failed hr=%08lx",static_cast<unsigned long>(hr));
            }
        }
    }, [&](bool) noexcept { if (chain) { chain->Release(); chain = nullptr; } });
}
static HRESULT WINAPI additionalChain(IDirect3DDevice9 *device, D3DPRESENT_PARAMETERS *params,
                                      IDirect3DSwapChain9 **out) {
    const HRESULT hr = originalAdditional(device, params, out);
    if (SUCCEEDED(hr) && out && *out) {
        // Probe installation owns no chain references and changes no parameters.
        try { discoverChain(device, *out); } catch (...) {}
    }
    return hr;
}
// Diagnostic latches never gate hook installation or forwarding. Keep both
// first success and first failure visible if an existing caller retries.
static void logHook(const char *name, const char *operation, MH_STATUS status,
                    std::atomic<bool> &succeeded, std::atomic<bool> &failed) {
    auto &reported = status == MH_OK ? succeeded : failed;
    if (!reported.exchange(true, std::memory_order_relaxed))
        ss2vr::game::log("Startup hook=%s operation=%s status=%d thread=%lu", name, operation,
                        static_cast<int>(status), GetCurrentThreadId());
}
template <class T> static void proxyHook(void *address, void *detour, T &original, const char *name) {
    static std::atomic<bool> createOk{false}, createFailed{false}, enableOk{false}, enableFailed{false};
    const auto created = ss2vr::createHook(address, detour, reinterpret_cast<void **>(&original));
    logHook(name, "create", created, createOk, createFailed);
    if (created == MH_OK) {
        const auto enabled = MH_EnableHook(address);
        logHook(name, "enable", enabled, enableOk, enableFailed);
    }
}
static bool attachAtPresentation(IDirect3DDevice9 *d);
static __attribute__((noinline)) HRESULT WINAPI present(IDirect3DDevice9 *d, const RECT *src, const RECT *dst, HWND window,
                              const RGNDATA *region) {
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_extract_return_addr(__builtin_return_address(0)));
    HRESULT hr = D3DERR_INVALIDCALL;
    const bool outer = presentationDepth == 0;
    ++presentationDepth;
    ss2vr::game::withNativeFinally([&] {
        // The fingerprinted GfxD3D device Present site (the same one
        // nativePresentationCaller admits once hooks are ready). The frame
        // that attaches presents natively; VR presentation starts next frame.
        const auto gfx = outer ? reinterpret_cast<uintptr_t>(GetModuleHandleW(L"GfxD3D.dll")) : 0;
        const bool attachedNow = gfx && caller == gfx + 0x1c75u && attachAtPresentation(d);
        if (outer && !attachedNow) ss2vr::game::presentDevice(d, caller, src, dst, window, region);
        else if (ss2vr::game::nativePresentationCaller(caller, false)) ss2vr::game::nestedNativePresentation();
        LARGE_INTEGER start{}, end{};
        QueryPerformanceCounter(&start);
        hr = originalPresent(d, src, dst, window, region);
        QueryPerformanceCounter(&end);
        if (outer) ss2vr::game::notePresentCall(start, end);
    }, [&](bool) noexcept { --presentationDepth; });
    if (FAILED(hr)) ss2vr::game::nativePresentationFailed(d, nullptr);
    return hr;
}
static INIT_ONCE initialize = INIT_ONCE_STATIC_INIT;
static bool vrEnabled = false, minHookReady = false, stockRenderer = false, labIsolationRequested = false;
// Native attach runs on the device-owning main thread between frames. A
// background worker raced world loading: it called engine string/allocator
// functions from a foreign thread and suspended the loading thread to patch,
// which intermittently deadlocked startup on Serious Engine mutexes.
static std::atomic<bool> attachPending{false};
static bool attachAtPresentation(IDirect3DDevice9 *d) {
    static unsigned waiting = 0;
    if (!attachPending.load(std::memory_order_relaxed) || !ss2vr::game::presentationDeviceOwner(d))
        return false;
    if (!GetModuleHandleW(L"Sam2Game.dll") || !GetModuleHandleW(L"Engine.dll") || !GetModuleHandleW(L"Core.dll")) {
        if (++waiting == 600) ss2vr::game::log("Native modules unavailable after 600 presents; attach still pending");
        return false;
    }
    attachPending.store(false, std::memory_order_relaxed);
    ss2vr::game::log("Startup native attach on presentation thread=%lu", GetCurrentThreadId());
    ss2vr::game::attach();
    return true;
}
static BOOL CALLBACK init(PINIT_ONCE, PVOID, PVOID *) {
    // Detours outlive graphics backend reloads; their owning DLL must stay mapped.
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&init), &self))
        return FALSE;
    std::wstring directory;
    if (!ss2vr::systemDirectory(directory)) return FALSE;
    try {
        const auto path=directory+L"\\d3d9.dll";
        realDll=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    } catch (...) { return FALSE; }
    if (!realDll) return FALSE;
    if (realDll==self) {
        FreeLibrary(realDll); realDll=nullptr;
        return FALSE; // Never recurse into this proxy after a loader/override alias.
    }
    create9 = ss2vr::loadProc<Create9>(realDll, "Direct3DCreate9");
    create9ex = ss2vr::loadProc<Create9Ex>(realDll, "Direct3DCreate9Ex");
    if (!create9) {
        FreeLibrary(realDll); realDll=nullptr; create9ex=nullptr;
        return FALSE;
    }
    // Latch the private comparator before even D3DPERF forwarding can create a
    // VR worker. Later environment changes cannot switch an installed renderer.
    wchar_t mode[2]{},isolation[2]{};
    stockRenderer=GetEnvironmentVariableW(L"SS2VR_LAB_STOCK_RENDER",mode,2)==1 && mode[0]==L'1';
    labIsolationRequested=GetEnvironmentVariableW(L"SS2VR_LAB_ISOLATE_ONLINE",isolation,2)==1 && isolation[0]==L'1';
    if(stockRenderer && !labIsolationRequested) return FALSE;
    const auto hookStatus = MH_Initialize();
    minHookReady = hookStatus == MH_OK;
    vrEnabled = minHookReady && !stockRenderer;
    ss2vr::game::log("Startup MinHook initialize status=%d", static_cast<int>(hookStatus));
    if(stockRenderer) ss2vr::game::log("Lab stock renderer selected before VR worker creation; graphics hooks disabled");
    if (vrEnabled) attachPending.store(true, std::memory_order_relaxed);
    return TRUE;
}
static HRESULT WINAPI reset(IDirect3DDevice9 *d, D3DPRESENT_PARAMETERS *p) {
    const bool owner = ss2vr::game::presentationDeviceOwner(d);
    if (owner) ss2vr::game::deviceLost();
    HRESULT hr = originalReset(d, p);
    if (owner && SUCCEEDED(hr))
        ss2vr::game::deviceResetSucceeded(d);
    if (owner && SUCCEEDED(hr)) seedChain(d);
    return hr;
}
static HRESULT WINAPI createDevice(IDirect3D9 *api, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
                                   D3DPRESENT_PARAMETERS *p, IDirect3DDevice9 **out) {
    HRESULT hr = originalCreate(api, adapter, type, window, flags, p, out);
    if (FAILED(hr) && managedEmulation.load(std::memory_order_relaxed) && create9) {
        // The 9Ex device was refused: a classic device from a classic factory instead.
        ss2vr::game::log("Direct3D 9Ex CreateDevice failed hr=%08lx; classic device", static_cast<unsigned long>(hr));
        managedEmulation.store(false, std::memory_order_relaxed);
        if (IDirect3D9 *classic = create9(factoryVersion)) {
            hr = originalCreate(classic, adapter, type, window, flags, p, out);
            classic->Release(); // The device keeps its factory alive.
        }
    }
    static std::atomic<bool> createOk{false}, createFailed{false};
    auto &reported = SUCCEEDED(hr) ? createOk : createFailed;
    if (!reported.exchange(true, std::memory_order_relaxed))
        ss2vr::game::log("Startup CreateDevice hr=%08lx device=%p type=%u flags=%08lx thread=%lu",
                        static_cast<unsigned long>(hr),
                        SUCCEEDED(hr) && out ? static_cast<void *>(*out) : nullptr,
                        static_cast<unsigned>(type), flags, GetCurrentThreadId());
    if (SUCCEEDED(hr) && out && *out && type == D3DDEVTYPE_HAL) {
        auto table = *reinterpret_cast<void ***>(*out);
        if (!originalReset)
            proxyHook(table[16], reinterpret_cast<void *>(reset), originalReset, "Reset");
        if (!originalPresent)
            proxyHook(table[17], reinterpret_cast<void *>(present), originalPresent, "Present");
        if (!originalAdditional)
            proxyHook(table[13], reinterpret_cast<void *>(additionalChain), originalAdditional, "AdditionalSwapChain");
        if (managedEmulation.load(std::memory_order_relaxed) && !originalCreateTexture) {
            proxyHook(table[23], reinterpret_cast<void *>(createTexture), originalCreateTexture, "CreateTexture");
            proxyHook(table[24], reinterpret_cast<void *>(createVolume), originalCreateVolume, "CreateVolumeTexture");
            proxyHook(table[25], reinterpret_cast<void *>(createCube), originalCreateCube, "CreateCubeTexture");
            proxyHook(table[26], reinterpret_cast<void *>(createVertex), originalCreateVertex, "CreateVertexBuffer");
            proxyHook(table[27], reinterpret_cast<void *>(createIndex), originalCreateIndex, "CreateIndexBuffer");
            if (managedEmulation.load(std::memory_order_relaxed) && originalCreateTexture && originalCreateCube &&
                originalCreateVertex && originalCreateIndex) {
                // Lock timing on the 9Ex device's resources (its vtables), for Windows reports.
                IDirect3DTexture9 *t = nullptr; IDirect3DCubeTexture9 *c = nullptr;
                IDirect3DVertexBuffer9 *v = nullptr; IDirect3DIndexBuffer9 *i = nullptr;
                QueryPerformanceFrequency(&lock_timing::frequency);
                if (SUCCEEDED(originalCreateTexture(*out, 4, 4, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t, nullptr)) && t)
                    proxyHook((*reinterpret_cast<void ***>(t))[19], reinterpret_cast<void *>(lock_timing::texture), lock_timing::oTexture, "Texture LockRect");
                if (SUCCEEDED(originalCreateCube(*out, 4, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &c, nullptr)) && c)
                    proxyHook((*reinterpret_cast<void ***>(c))[19], reinterpret_cast<void *>(lock_timing::cube), lock_timing::oCube, "Cube LockRect");
                if (SUCCEEDED(originalCreateVertex(*out, 64, 0, 0, D3DPOOL_DEFAULT, &v, nullptr)) && v)
                    proxyHook((*reinterpret_cast<void ***>(v))[11], reinterpret_cast<void *>(lock_timing::vertex), lock_timing::oVertex, "Vertex Lock");
                if (v)
                    proxyHook((*reinterpret_cast<void ***>(v))[12], reinterpret_cast<void *>(lock_timing::vertexUnlock), lock_timing::oVertexUnlock, "Vertex Unlock");
                if (SUCCEEDED(originalCreateIndex(*out, 64, 0, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &i, nullptr)) && i)
                    proxyHook((*reinterpret_cast<void ***>(i))[11], reinterpret_cast<void *>(lock_timing::index), lock_timing::oIndex, "Index Lock");
                if (i)
                    proxyHook((*reinterpret_cast<void ***>(i))[12], reinterpret_cast<void *>(lock_timing::indexUnlock), lock_timing::oIndexUnlock, "Index Unlock");
                // Copies need all four originals of a kind; one implementation may serve both kinds.
                {
                    using namespace buffer_copies;
                    oLock[0] = reinterpret_cast<LockFn>(lock_timing::oVertex);
                    oUnlock[0] = reinterpret_cast<UnlockFn>(lock_timing::oVertexUnlock);
                    oLock[1] = reinterpret_cast<LockFn>(lock_timing::oIndex);
                    oUnlock[1] = reinterpret_cast<UnlockFn>(lock_timing::oIndexUnlock);
                    // The same code behind both tables is hooked once, by the vertex detour:
                    // adopt() matches each buffer's own methods against these addresses.
                    if (v && oLock[0]) hookedLock[0] = (*reinterpret_cast<void ***>(v))[11];
                    if (v && oUnlock[0]) hookedUnlock[0] = (*reinterpret_cast<void ***>(v))[12];
                    if (i && oLock[1]) hookedLock[1] = (*reinterpret_cast<void ***>(i))[11];
                    if (i && oUnlock[1]) hookedUnlock[1] = (*reinterpret_cast<void ***>(i))[12];
                    enabled.store(wantBufferCopies(), std::memory_order_relaxed);
                    ss2vr::game::log("Direct3D 9Ex buffer copies=%d (system-memory copies of the game's managed vertex and index buffers)",
                                     int(enabled.load() && hookedLock[0] && hookedUnlock[0] && hookedLock[1] && hookedUnlock[1]));
                }
                if (t) t->Release(); if (c) c->Release(); if (v) v->Release(); if (i) i->Release();
            }
            if (originalCreateTexture && originalCreateCube && !selfTest(*out) && create9) {
                // Nothing else holds the device yet: a classic one replaces it.
                managedEmulation.store(false, std::memory_order_relaxed);
                IDirect3DDevice9 *classicDevice = nullptr;
                HRESULT classicHr = D3DERR_NOTAVAILABLE;
                if (IDirect3D9 *classic = create9(factoryVersion)) {
                    (*out)->Release();
                    *out = nullptr;
                    classicHr = originalCreate(classic, adapter, type, window, flags, p, &classicDevice);
                    classic->Release();
                }
                ss2vr::game::log("Direct3D 9Ex self-test failed; classic device hr=%08lx", static_cast<unsigned long>(classicHr));
                if (FAILED(classicHr)) return classicHr;
                *out = classicDevice;
            }
        }
        ss2vr::game::deviceCreated(*out);
        seedChain(*out);
    }
    return hr;
}
extern "C" __declspec(dllexport) IDirect3D9 *WINAPI Direct3DCreate9(UINT version) {
    const BOOL initialized = InitOnceExecuteOnce(&initialize, init, nullptr, nullptr);
    if(stockRenderer && (!initialized || !minHookReady)) return nullptr;
    // On the fingerprinted native startup route this synchronous boundary
    // precedes onlInitialize. The lab adapter fails closed if unavailable.
    ss2vr::game::installLabOnlineIsolation();
    IDirect3D9 *api = nullptr;
    factoryVersion = version;
    if (vrEnabled && create9ex && wantD3D9Ex()) {
        IDirect3D9Ex *ex = nullptr;
        const HRESULT hr = create9ex(version, &ex);
        if (SUCCEEDED(hr) && ex) {
            api = ex; // IDirect3D9Ex extends IDirect3D9: the game keeps its classic calls.
            managedEmulation.store(true, std::memory_order_relaxed);
            ss2vr::game::log("Startup Direct3D 9Ex: eye images can be shared with the headset program");
        } else
            ss2vr::game::log("Startup Direct3D 9Ex unavailable hr=%08lx; classic device", static_cast<unsigned long>(hr));
    }
    if (!api) api = create9 ? create9(version) : nullptr;
    static std::atomic<bool> apiOk{false}, apiFailed{false};
    auto &reported = api ? apiOk : apiFailed;
    if (!reported.exchange(true, std::memory_order_relaxed))
        ss2vr::game::log("Startup Direct3DCreate9 initialized=%d api=%p hooksEnabled=%d thread=%lu",
                        static_cast<int>(initialized), static_cast<void *>(api), vrEnabled, GetCurrentThreadId());
    if (api && vrEnabled) ss2vr::game::installEyeCanvas(); // Before the engine creates its canvas.
    if (api && vrEnabled) ss2vr::game::spatial_audio::installStartupHook(); // Before the engine starts its sound.
    if (api && vrEnabled && !originalCreate) {
        auto v = *reinterpret_cast<void ***>(api);
        proxyHook(v[16], reinterpret_cast<void *>(createDevice), originalCreate, "CreateDevice");
    }
    return api;
}
extern "C" __declspec(dllexport) HRESULT WINAPI Direct3DCreate9Ex(UINT version, IDirect3D9Ex **out) {
    InitOnceExecuteOnce(&initialize, init, nullptr, nullptr);
    // The inspected fixture startup/online barrier is the classic factory route.
    // Do not silently run an unvalidated Ex path with profile isolation assumed.
    if(stockRenderer || labIsolationRequested) return D3DERR_NOTAVAILABLE;
    return create9ex ? create9ex(version, out) : D3DERR_NOTAVAILABLE;
}
#define FORWARD(name, ret, args, callargs, fallback)                                                         \
    extern "C" __declspec(dllexport) ret WINAPI name args {                                                  \
        InitOnceExecuteOnce(&initialize, init, nullptr, nullptr);                                            \
        auto f = realDll ? ss2vr::loadProc<ret(WINAPI *) args>(realDll, #name) : nullptr;                    \
        if (f)                                                                                               \
            return f callargs;                                                                               \
        return fallback;                                                                                     \
    }
FORWARD(D3DPERF_BeginEvent, int, (D3DCOLOR c, LPCWSTR s), (c, s), -1)
FORWARD(D3DPERF_EndEvent, int, (), (), -1)
FORWARD(D3DPERF_GetStatus, DWORD, (), (), 0)
FORWARD(D3DPERF_QueryRepeatFrame, BOOL, (), (), FALSE)
extern "C" __declspec(dllexport) void WINAPI D3DPERF_SetMarker(D3DCOLOR c, LPCWSTR s) {
    InitOnceExecuteOnce(&initialize, init, nullptr, nullptr);
    auto f = ss2vr::loadProc<void(WINAPI *)(D3DCOLOR, LPCWSTR)>(realDll, "D3DPERF_SetMarker");
    if (f)
        f(c, s);
}
extern "C" __declspec(dllexport) void WINAPI D3DPERF_SetRegion(D3DCOLOR c, LPCWSTR s) {
    InitOnceExecuteOnce(&initialize, init, nullptr, nullptr);
    auto f = ss2vr::loadProc<void(WINAPI *)(D3DCOLOR, LPCWSTR)>(realDll, "D3DPERF_SetRegion");
    if (f)
        f(c, s);
}
extern "C" __declspec(dllexport) void WINAPI D3DPERF_SetOptions(DWORD o) {
    InitOnceExecuteOnce(&initialize, init, nullptr, nullptr);
    auto f = ss2vr::loadProc<void(WINAPI *)(DWORD)>(realDll, "D3DPERF_SetOptions");
    if (f)
        f(o);
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(module);
    return TRUE;
}
