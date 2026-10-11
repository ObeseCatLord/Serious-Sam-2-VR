#define WIN32_LEAN_AND_MEAN
#include "common/winproc.hpp"
#include "common/winpath.hpp"
#include "game.hpp"
#include "native_finally.hpp"
#include <MinHook.h>
#include <atomic>
#include <d3d9.h>
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
        hr = chainHooks[Index].original(chain, src, dst, window, region, flags);
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
    const auto created = MH_CreateHook(address, reinterpret_cast<void *>(detour),
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
    const auto created = MH_CreateHook(address, detour, reinterpret_cast<void **>(&original));
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
        hr = originalPresent(d, src, dst, window, region);
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
    IDirect3D9 *api = create9 ? create9(version) : nullptr;
    static std::atomic<bool> apiOk{false}, apiFailed{false};
    auto &reported = api ? apiOk : apiFailed;
    if (!reported.exchange(true, std::memory_order_relaxed))
        ss2vr::game::log("Startup Direct3DCreate9 initialized=%d api=%p hooksEnabled=%d thread=%lu",
                        static_cast<int>(initialized), static_cast<void *>(api), vrEnabled, GetCurrentThreadId());
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
