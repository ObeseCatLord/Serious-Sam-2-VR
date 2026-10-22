#include "common/winproc.hpp"
#include "game.hpp"
#include <MinHook.h>
#include <windows.h>
// Native Core module resource loader calls <name>_Startup_t(CResourceFile*)
// and <name>_Cleanup(CResourceFile*) using cdecl. It expects a default beacon
// resource. Construct and register that resource with Core's own allocator/API.
extern "C" __declspec(dllexport) void __cdecl SS2VRServer_Startup_t(void *resourceFile) {
    using namespace ss2vr;
    using namespace ss2vr::game;
    if (!resourceFile || !supported(true))
        return;
    auto core = GetModuleHandleW(L"Core.dll");
    using Allocate = void *(__cdecl *)(int, void *);
    using Construct = void *(__thiscall *)(void *);
    using Register = void(__thiscall *)(void *, int, const char *, void *);
    auto allocate = loadProc<Allocate>(core, "?memNewRC_internal@SeriousEngine@@YAPAXJPAVCDataType@1@@Z");
    auto construct = loadProc<Construct>(core, "??0CModuleBeacon@SeriousEngine@@QAE@XZ");
    auto registerResource = loadProc<Register>(
        core, "?RegisterModuleResource@CResourceFile@SeriousEngine@@QAEXJPBDPAVCResource@2@@Z");
    auto type = reinterpret_cast<void **>(
        GetProcAddress(core, "?md_pdtDataType@CModuleBeacon@SeriousEngine@@2PAVCDataType@2@A"));
    auto declareBegin =
        loadProc<void(__cdecl *)(const char *)>(core, "?mdModuleDeclareBeg@SeriousEngine@@YAXPBD@Z");
    auto declareEnd = loadProc<void(__cdecl *)()>(core, "?mdModuleDeclareEnd@SeriousEngine@@YAXXZ");
    if (!allocate || !construct || !registerResource || !type || !*type || !declareBegin || !declareEnd)
        return;
    // Sam2Game's native startup uses exactly 0x30 bytes for CModuleBeacon.
    void *beacon = allocate(0x30, *type);
    if (!beacon)
        return;
    construct(beacon);
    declareBegin("SS2VRServer");
    declareEnd();
    // As Sam2Game_Startup_t: the loader looks the beacon up by this reserved
    // ID and name ("No resource with name Beacon" otherwise).
    registerResource(resourceFile, 0x98967f, "Beacon", beacon);
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&SS2VRServer_Startup_t), &self))
        return;
    auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        log("Dedicated VR bootstrap failed: MinHook initialization=%d", int(status));
        return;
    }
    if (!attach(true)) {
        log("Dedicated VR bootstrap failed; VR capability unavailable");
        return;
    }
    log("Dedicated VR authority attached; no game/network verification");
}
extern "C" __declspec(dllexport) void __cdecl SS2VRServer_Cleanup(void *) {
    // Native Close/Disconnect/SetAvatar hooks retire capabilities. Pinning keeps
    // native detour code valid across resource-module reloads until process exit.
}
