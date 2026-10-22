#include "common/color.hpp"
#include "common/controls.hpp"
#include "common/foveation.hpp"
#include "common/winpath.hpp"
#include "common/frame_policy.hpp"
#include "common/single_pass_math.hpp"
#include "common/native_ui.hpp"
#include "common/native_ui_finish.hpp"
#include "common/world_markers.hpp"
#include "game.hpp"
#include "menu.hpp"
#include "native_finally.hpp"
#include "native_memory.hpp"
#include "eye_replay.hpp"
#include "hud_wrist.hpp"
#include "single_pass.hpp"
#include "env_probe.hpp"
#include "scope_gpu.hpp"
#include "scope_source_gpu.hpp"
#include "common/scope_capture_command.hpp"
#include "shaders/scope_image_opaque.hpp"
#include "scope_observer.hpp"
#include "remote_render.hpp"
#include <MinHook.h>
#include <atomic>
#include <bcrypt.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
namespace ss2vr::game {
Channel channel;
static SRWLOCK logLock = SRWLOCK_INIT;
static FILE *logfile = nullptr;
void log(const char *fmt, ...) {
    AcquireSRWLockExclusive(&logLock);
    if (!logfile) {
        try {
            std::wstring directory;
            if (moduleDirectory(nullptr,directory)) logfile=_wfopen((directory+L"SS2VR.log").c_str(),L"a");
        } catch (...) {} // Logging failure must not strand the lock or cross native callbacks.
    }
    if (logfile) {
        va_list a;
        va_start(a, fmt);
        vfprintf(logfile, fmt, a);
        va_end(a);
        fputc('\n', logfile);
        fflush(logfile);
    }
    ReleaseSRWLockExclusive(&logLock);
}
static bool matchesFile(const std::wstring &path,const char *hash) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = false;
    std::vector<unsigned char> obj;
    DWORD n = 0, size = 0;
    unsigned char digest[32];
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
        BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof(size), &n, 0) >=
            0) {
        obj.resize(size);
        if (BCryptCreateHash(alg, &h, obj.data(), size, nullptr, 0, 0) >= 0) {
            unsigned char buf[32768];
            ok = true;
            while (true) {
                if (!ReadFile(f, buf, sizeof(buf), &n, nullptr)) {
                    ok = false;
                    break;
                }
                if (!n)
                    break;
                if (BCryptHashData(h, buf, n, 0) < 0) {
                    ok = false;
                    break;
                }
            }
            if (BCryptFinishHash(h, digest, 32, 0) < 0)
                ok = false;
            char hex[65]{};
            for (int i = 0; i < 32; i++)
                sprintf(hex + 2 * i, "%02x", digest[i]);
            ok = ok && std::string(hex) == hash;
        }
    }
    if (h)
        BCryptDestroyHash(h);
    if (alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return ok;
}
static bool matches(HMODULE module, const char *hash) {
    std::wstring path;
    return modulePath(module,path) && matchesFile(path,hash);
}
bool nativeModuleFingerprint(HMODULE module,const char *hash) {return matches(module,hash);}
bool supported(bool headless) {
    struct Item {
        const wchar_t *file;
        const char *hash;
    };
    const Item items[] = {
        {nullptr, headless ? "bfa9d628483bb6e9cb39338e075848a6e02328ccb612f701d87dd909d4f87b57"
                           : "727901f161133ff653fcdc196858335b991b743e67deb448c03c808e5b33e28b"},
        {L"Engine.dll", "da6efc9f72637eb3b6f48eadca2107be89b09c00618b6e72d5d3632938a7d851"},
        {L"Sam2Game.dll", "5628b4ed30a966f10c8e8ea46bf0789257a35ea80127bacacebe28b1ce5303df"},
        {L"Core.dll", "7a1bd56b9bfa3edfbb23f4d3c96490e40a7c0b031b85e797e1af91b2ba3cf207"},
        {L"GfxD3D.dll", "88749b79be36f0c0dccb623c4f1712685b5af603b451e25ed3c5029bedcdf3ed"}};
    for (auto i : items) {
        if (headless && i.file && std::wstring(i.file) == L"GfxD3D.dll")
            continue;
        auto m = GetModuleHandleW(i.file);
        if (!m || !matches(m, i.hash)) {
            log("Build fingerprint rejected; desktop retained (%ls)", i.file ? i.file : L"Sam2.exe");
            return false;
        }
    }
    return true;
}
static bool labOnlineIsolated = false;
static std::atomic<uint32_t> labIsolationPhase{0}; // fresh, installing, complete
template<class T> static T loadLabProc(HMODULE module,const char *name) {
    return reinterpret_cast<T>(GetProcAddress(module,name));
}
static uintptr_t labEngineBase = 0;
static DWORD labStartupThread = 0;
static uintptr_t labInitializerCaller = 0;
static void *(__cdecl *labCurrentNet)() = nullptr;
static int(__cdecl *labNetIsLocal)() = nullptr;
static void(__cdecl *labOriginalUninitialize)() = nullptr;
static void(__thiscall *labOriginalOpen)(void *,const char *,const char *) = nullptr;
static int32_t(__thiscall *labStreamSize)(void *) = nullptr;
static int32_t(__thiscall *labStreamPosition)(void *) = nullptr;
static int(__cdecl *labIsLoadingThread)() = nullptr;
static void(__thiscall *labStreamSeek)(void *,int32_t) = nullptr;
static void(__thiscall *labStreamRead)(void *,void *,int32_t) = nullptr;
static char labSceneName[1024]{};
static bool labGripResourceProbe=false;
static thread_local bool labSceneReading = false;
static bool labNameMatches(const char *name,const char *expected) noexcept {
    if (!name) return false;
    for (unsigned i=0;i<sizeof(labSceneName);++i) {
        char a=name[i],b=expected[i];
        if (a=='\\') a='/';if (b=='\\') b='/';
        if (a>='A'&&a<='Z') a+=32;if (b>='A'&&b<='Z') b+=32;
        if (a!=b) return false;
        if (!a) return true;
    }
    return false;
}
static unsigned labGripResourceKey(const char *name) noexcept {
    if(!labGripResourceProbe)return 0;
    constexpr const char *names[]={
        "Content/SeriousSam2/Databases/EntityParams/ZapGunWeapon.ep",
        "Content/SeriousSam2/Models/Weapons/ZapGun/ZapGun_FP.mdl",
        "Content/SeriousSam2/Models/Weapons/ZapGun/Sources/Meshes/Zapgun.bmf",
        "Content/SeriousSam2/Models/Weapons/ZapGun/Sources/Zapgun.skl",
        "Content/SeriousSam2/Models/Weapons/ZapGun/Sources/Meshes/R_Hand.bmf"};
    for(unsigned i=0;i<5;++i)if(labNameMatches(name,names[i]))return i+1;
    return 0;
}
[[noreturn]] static void abortLab(const char *reason) {
    log("Lab isolation abort: %s",reason);
    // This opt-in fixture must never continue into real remote profile mutation
    // when its early isolation boundary cannot be established.
    TerminateProcess(GetCurrentProcess(),0x53533201);
    std::abort();
}
static void hashLabScene(void *stream,unsigned resource=0) {
    const int32_t size=labStreamSize(stream),position=labStreamPosition(stream);
    if (size<=0 || size>32*1024*1024 || position<0 || position>size)
        abortLab("native scene stream size/position outside probe contract");
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    void *object=nullptr;DWORD objectBytes=0,received=0;
    bool restore=false,complete=false;unsigned char digest[32]{};
    withNativeFinally([&] {
        if (BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0 ||
            BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectBytes),sizeof(objectBytes),&received,0)<0 ||
            !objectBytes) return;
        object=HeapAlloc(GetProcessHeap(),0,objectBytes);
        if (!object || BCryptCreateHash(algorithm,&hash,static_cast<PUCHAR>(object),objectBytes,nullptr,0,0)<0) return;
        restore=true; // Set before the first operation that can move native position.
        labStreamSeek(stream,0);
        unsigned char buffer[65536];
        int32_t remaining=size;
        while (remaining>0) {
            const int32_t bytes=std::min<int32_t>(remaining,sizeof(buffer));
            labStreamRead(stream,buffer,bytes);
            if (BCryptHashData(hash,buffer,static_cast<ULONG>(bytes),0)<0) return;
            remaining-=bytes;
        }
        complete=BCryptFinishHash(hash,digest,sizeof(digest),0)>=0;
    },[&](bool) noexcept {
        if (restore) {
            bool restored=false;
            withNativeFinally([&] {
                labStreamSeek(stream,position);
                restored=labStreamPosition(stream)==position;
            },[&](bool aborted) noexcept {
                if (aborted || !restored) abortLab("native scene stream restoration failed");
            });
        }
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm,0);
        if (object) HeapFree(GetProcessHeap(),0,object);
    });
    if (!complete) abortLab("native scene stream hash did not complete");
    char hex[65]{};for (unsigned i=0;i<32;++i) std::snprintf(hex+i*2,3,"%02x",digest[i]);
    if(resource)log("Lab native grip resource key=%u bytes=%ld positionRestored=%ld sha256=%s",
        resource,static_cast<long>(size),static_cast<long>(position),hex);
    else log("Lab native scene stream bytes=%ld positionRestored=%ld sha256=%s",static_cast<long>(size),static_cast<long>(position),hex);
}
static __attribute__((force_align_arg_pointer)) void __fastcall labOpenScene(void *stream,void *,const char *filename,const char *mode) {
    const bool readMode=mode&&mode[0]=='r'&&mode[1]==0;
    const bool candidate=readMode&&labNameMatches(filename,labSceneName);
    const unsigned resource=readMode?labGripResourceKey(filename):0;
    bool owns=false;
    withNativeFinally([&] {
        labOriginalOpen(stream,filename,mode); // Exactly once, including unrelated opens.
        if (!candidate && !resource) return;
        if(resource) {
            static std::atomic<unsigned> counts[5]{};
            const unsigned count=counts[resource-1].fetch_add(1);
            if(count>=2){if(count==2)log("Lab native grip resource saturated key=%u",resource);return;}
        }
        if(!matches(GetModuleHandleW(L"Sam2Game.dll"),"5628b4ed30a966f10c8e8ea46bf0789257a35ea80127bacacebe28b1ce5303df"))
            abortLab("loaded scene game-module fingerprint mismatch");
        if (labSceneReading || (GetCurrentThreadId()!=labStartupThread && !labIsLoadingThread()))
            abortLab("nested/foreign native scene open");
        labSceneReading=true;owns=true;
        hashLabScene(stream,resource); // Borrowed synchronously; never close or retain it.
    },[&](bool) noexcept { if (owns) labSceneReading=false; });
}
// Opt-in multiplayer lab client (SS2VR_LAB_MULTIPLAYER=1). This Steam edition
// carries every session, even a connect-by-IP one, through its online object
// (Steam networking, SteamID, message classes), so a multiplayer client builds
// it as the game does. What must not reach the user's Steam account is write
// access: SteamRemoteStorage() and SteamUserStats() return proxies that pass
// every call through except the writing ones, which are refused (Steam Cloud
// FileWrite, FileForget, FileDelete, FileShare, SetSyncPlatforms and the write
// streams; SetStat, UpdateAvgRateStat, SetAchievement, ClearAchievement and
// StoreStats). Interface versions: STEAMREMOTESTORAGE_INTERFACE_VERSION011 and
// STEAMUSERSTATS_INTERFACE_VERSION011 (this game's steam_api.dll).
static bool labMultiplayer() noexcept {
    static const bool on = [] {
        wchar_t v[2]{};
        return GetEnvironmentVariableW(L"SS2VR_LAB_MULTIPLAYER", v, 2) == 1 && v[0] == L'1';
    }();
    return on;
}
struct LabSteamRule { unsigned slot; uint32_t eax, edx; uint16_t argumentBytes; };
// A proxy object { vtable, real }: forwarding thunks keep the arguments and
// replace `this` (thiscall: ecx); refusing thunks return a fixed value and pop
// their arguments as the callee must.
static void *labSteamProxy(void *real, const LabSteamRule *rules, size_t count, const char *name) noexcept {
    constexpr unsigned Slots = 96, Thunk = 16;
    auto *memory = static_cast<uint8_t *>(VirtualAlloc(nullptr, Slots * Thunk + Slots * 4 + 8, MEM_COMMIT | MEM_RESERVE,
                                                       PAGE_EXECUTE_READWRITE));
    if (!real || !memory) return real;
    auto *table = reinterpret_cast<uint32_t *>(memory + Slots * Thunk);
    auto *object = reinterpret_cast<uint32_t *>(table + Slots);
    for (unsigned i = 0; i < Slots; ++i) {
        uint8_t *t = memory + i * Thunk;
        const LabSteamRule *rule = nullptr;
        for (size_t k = 0; k < count; ++k) if (rules[k].slot == i) rule = &rules[k];
        if (rule) {
            t[0] = 0xb8; std::memcpy(t + 1, &rule->eax, 4);              // mov eax, value
            t[5] = 0xba; std::memcpy(t + 6, &rule->edx, 4);              // mov edx, value
            t[10] = 0xc2; std::memcpy(t + 11, &rule->argumentBytes, 2);  // ret n
        } else {
            const uint32_t offset = i * 4;
            t[0] = 0x8b; t[1] = 0x49; t[2] = 0x04;                       // mov ecx, [ecx+4]
            t[3] = 0x8b; t[4] = 0x01;                                    // mov eax, [ecx]
            t[5] = 0xff; t[6] = 0xa0; std::memcpy(t + 7, &offset, 4);    // jmp [eax+offset]
        }
        table[i] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(t));
    }
    object[0] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(table));
    object[1] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(real));
    FlushInstructionCache(GetCurrentProcess(), memory, Slots * Thunk);
    log("Lab multiplayer: %s writes refused (proxy over %p)", name, real);
    return object;
}
static void *(__cdecl *labRealRemoteStorage)() = nullptr, *(__cdecl *labRealUserStats)() = nullptr;
static void *__cdecl labRemoteStorage() {
    static void *real = nullptr, *proxy = nullptr;
    void *current = labRealRemoteStorage();
    if (current != real) {
        static constexpr LabSteamRule rules[]{
            {0, 0, 0, 12},           // FileWrite -> false
            {2, 0, 0, 4},            // FileForget -> false
            {3, 0, 0, 4},            // FileDelete -> false
            {4, 0, 0, 4},            // FileShare -> k_uAPICallInvalid
            {5, 0, 0, 8},            // SetSyncPlatforms -> false
            {6, ~0u, ~0u, 4},        // FileWriteStreamOpen -> invalid handle
            {7, 0, 0, 16},           // FileWriteStreamWriteChunk -> false
            {8, 0, 0, 8},            // FileWriteStreamClose -> false
            {9, 0, 0, 8}};           // FileWriteStreamCancel -> false
        real = current;
        proxy = labSteamProxy(current, rules, std::size(rules), "Steam Cloud");
    }
    return proxy;
}
static void *__cdecl labUserStats() {
    static void *real = nullptr, *proxy = nullptr;
    void *current = labRealUserStats();
    if (current != real) {
        static constexpr LabSteamRule rules[]{
            {3, 1, 0, 8},            // SetStat (both overloads) -> true, not set
            {4, 1, 0, 8},
            {5, 1, 0, 16},           // UpdateAvgRateStat -> true, not set
            {7, 1, 0, 4},            // SetAchievement -> true, not set
            {8, 1, 0, 4},            // ClearAchievement -> true, not cleared
            {10, 1, 0, 0}};          // StoreStats -> true, nothing stored
        real = current;
        proxy = labSteamProxy(current, rules, std::size(rules), "Steam stats and achievements");
    }
    return proxy;
}
static void (__cdecl *labOriginalInitialize)() = nullptr;
static void labInstallSteamWriteBarrier() {
    HMODULE steam = GetModuleHandleW(L"steam_api.dll");
    void *storage = steam ? reinterpret_cast<void *>(GetProcAddress(steam, "SteamRemoteStorage")) : nullptr;
    void *stats = steam ? reinterpret_cast<void *>(GetProcAddress(steam, "SteamUserStats")) : nullptr;
    if (!storage || !stats ||
        MH_CreateHook(storage, reinterpret_cast<void *>(labRemoteStorage), reinterpret_cast<void **>(&labRealRemoteStorage)) != MH_OK ||
        MH_CreateHook(stats, reinterpret_cast<void *>(labUserStats), reinterpret_cast<void **>(&labRealUserStats)) != MH_OK ||
        MH_EnableHook(storage) != MH_OK || MH_EnableHook(stats) != MH_OK)
        abortLab("multiplayer Steam write barrier unavailable");
    log("Lab multiplayer: Steam write barrier installed (cloud, stats, achievements)");
}
static __attribute__((noinline)) void __cdecl suppressLabOnlineInitialization() {
    const auto caller=reinterpret_cast<uintptr_t>(__builtin_extract_return_addr(__builtin_return_address(0)));
    uint32_t pointer = 0;
    std::memcpy(&pointer,reinterpret_cast<const void *>(labEngineBase+0x2ec890),sizeof(pointer));
    if (labIsolationPhase.load(std::memory_order_acquire)!=2 || !labOnlineIsolated ||
        GetCurrentThreadId()!=labStartupThread || pointer || caller!=labInitializerCaller)
        abortLab("unexpected online initializer owner or existing interface");
    if (labMultiplayer()) {
        // The session needs the online object; the write barrier is in place.
        log("Lab multiplayer: native online initializer runs behind the Steam write barrier");
        labOriginalInitialize();
        return;
    }
    static bool reported = false;
    if (!reported) { reported=true; log("Lab native online initializer suppressed; interface=0; cloud/stats object not constructed"); }
}
static void __cdecl labOnlineUninitialize() {
    if(labIsolationPhase.load(std::memory_order_acquire)!=2) abortLab("incomplete isolation reached native shutdown");
    uint32_t pointer=0;
    std::memcpy(&pointer,reinterpret_cast<const void *>(labEngineBase+0x2ec890),sizeof(pointer));
    if (pointer && !labMultiplayer()) abortLab("online interface appeared before native shutdown");
    log("Lab native online shutdown wrapper entered; interface=%u; Steam synchronization %s", pointer ? 1u : 0u,
        pointer ? "behind the write barrier" : "body excluded");
    labOriginalUninitialize(); // Preserve the stock null-interface return.
}
void installLabOnlineIsolation() {
    wchar_t value[2]{};
    if (GetEnvironmentVariableW(L"SS2VR_LAB_ISOLATE_ONLINE",value,2)!=1 || value[0]!=L'1') return;
    if (labIsolationPhase.load(std::memory_order_acquire)==2) return;
    uint32_t fresh=0;
    if(!labIsolationPhase.compare_exchange_strong(fresh,1,std::memory_order_acq_rel))
        abortLab("reentered or concurrent partial lab isolation installation");
    std::wstring bin;
    if (!moduleDirectory(nullptr,bin) ||
        GetFileAttributesW((bin+L"..\\.ss2vr-runtime-lab.json").c_str())==INVALID_FILE_ATTRIBUTES)
        abortLab("private lab marker missing");
    const auto engine=GetModuleHandleW(L"Engine.dll");
    if (!engine || !matches(nullptr,"727901f161133ff653fcdc196858335b991b743e67deb448c03c808e5b33e28b") ||
        !matches(engine,"da6efc9f72637eb3b6f48eadca2107be89b09c00618b6e72d5d3632938a7d851") ||
        !matches(GetModuleHandleW(L"Core.dll"),"7a1bd56b9bfa3edfbb23f4d3c96490e40a7c0b031b85e797e1af91b2ba3cf207") ||
        !matches(GetModuleHandleW(L"GfxD3D.dll"),"88749b79be36f0c0dccb623c4f1712685b5af603b451e25ed3c5029bedcdf3ed") ||
        !matchesFile(bin+L"Sam2Game.dll","5628b4ed30a966f10c8e8ea46bf0789257a35ea80127bacacebe28b1ce5303df"))
        abortLab("startup binary fingerprint mismatch");
    labEngineBase=reinterpret_cast<uintptr_t>(engine);
    uint32_t pointer=0;
    std::memcpy(&pointer,reinterpret_cast<const void *>(labEngineBase+0x2ec890),sizeof(pointer));
    auto *entry=reinterpret_cast<void *>(GetProcAddress(engine,"?onlInitialize@SeriousEngine@@YAXXZ"));
    auto *uninitialize=reinterpret_cast<void *>(GetProcAddress(engine,"?onlUninitialize@SeriousEngine@@YAXXZ"));
    const uint8_t prefix[]{0x55,0x8b,0xec,0x6a,0xff};
    if (pointer || entry!=reinterpret_cast<void *>(labEngineBase+0x10abe0))
        abortLab("online initializer already entered or export mismatch");
    if (std::memcmp(entry,prefix,sizeof(prefix)) || uninitialize!=reinterpret_cast<void *>(labEngineBase+0x109010))
        abortLab("live online wrapper instruction/address mismatch");
    labCurrentNet=loadLabProc<void *(__cdecl *)()>(engine,"?netGetCurrent@SeriousEngine@@YAPAVCNetworkInterface@1@XZ");
    labNetIsLocal=loadLabProc<int(__cdecl *)()>(engine,"?netIsLocal@SeriousEngine@@YAHXZ");
    if (!labCurrentNet || reinterpret_cast<uintptr_t>(labNetIsLocal)!=labEngineBase+0xf81d0)
        abortLab("native local transport query missing");
    HMODULE pinned=nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(engine),&pinned)) abortLab("native online module pin failed");
    if (labMultiplayer()) labInstallSteamWriteBarrier();
    if (MH_CreateHook(entry,reinterpret_cast<void *>(suppressLabOnlineInitialization),
        reinterpret_cast<void **>(&labOriginalInitialize))!=MH_OK || !labOriginalInitialize)
        abortLab("online isolation hook creation failed");
    if (MH_CreateHook(uninitialize,reinterpret_cast<void *>(labOnlineUninitialize),
        reinterpret_cast<void **>(&labOriginalUninitialize))!=MH_OK || !labOriginalUninitialize)
        abortLab("online shutdown observation hook creation failed");
    labStartupThread=GetCurrentThreadId();
    labInitializerCaller=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))+0x1b1b;
    labOnlineIsolated=true; // Publish owner before the enabled detour can execute.
    if (MH_EnableHook(entry)!=MH_OK) abortLab("online isolation hook enable failed");
    if (MH_EnableHook(uninitialize)!=MH_OK) abortLab("online shutdown observation hook enable failed");
    const DWORD sceneBytes=GetEnvironmentVariableA("SS2VR_LAB_SCENE",labSceneName,sizeof(labSceneName));
    if (!sceneBytes || sceneBytes>=sizeof(labSceneName)) abortLab("native scene probe path missing/too long");
    wchar_t gripProbe[2]{};
    labGripResourceProbe=GetEnvironmentVariableW(L"SS2VR_LAB_GRIP_RESOURCES",gripProbe,2)==1 && gripProbe[0]==L'1';
    const auto core=GetModuleHandleW(L"Core.dll");
    auto *open=reinterpret_cast<void *>(GetProcAddress(core,"?OpenFile_t@CStream@SeriousEngine@@QAEXPBD0@Z"));
    labStreamSize=loadLabProc<decltype(labStreamSize)>(core,"?GetSize@CStream@SeriousEngine@@QAEJXZ");
    labStreamPosition=loadLabProc<decltype(labStreamPosition)>(core,"?GetPosition@CStream@SeriousEngine@@QAEJXZ");
    labStreamSeek=loadLabProc<decltype(labStreamSeek)>(core,"?SeekBeg_t@CStream@SeriousEngine@@QAEXJ@Z");
    labStreamRead=loadLabProc<decltype(labStreamRead)>(core,"?Read_t@CStream@SeriousEngine@@QAEXPAXJ@Z");
    const auto cb=reinterpret_cast<uintptr_t>(core);
    labIsLoadingThread=loadLabProc<decltype(labIsLoadingThread)>(core,"?mlIsThisLoadingThreadID@SeriousEngine@@YAHXZ");
    if (reinterpret_cast<uintptr_t>(labIsLoadingThread)!=cb+0x44860 ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(core),&pinned)) abortLab("native loading thread query/pin mismatch");
    if (reinterpret_cast<uintptr_t>(open)!=cb+0x4bd00 ||
        reinterpret_cast<uintptr_t>(labStreamSize)!=cb+0x4a0e0 ||
        reinterpret_cast<uintptr_t>(labStreamPosition)!=cb+0x4a0f0 ||
        reinterpret_cast<uintptr_t>(labStreamSeek)!=cb+0x4a160 ||
        reinterpret_cast<uintptr_t>(labStreamRead)!=cb+0x4a130)
        abortLab("native scene stream ABI exports mismatch");
    if (MH_CreateHook(open,reinterpret_cast<void *>(labOpenScene),reinterpret_cast<void **>(&labOriginalOpen))!=MH_OK ||
        !labOriginalOpen || MH_EnableHook(open)!=MH_OK) abortLab("native scene observation hook unavailable");
    labIsolationPhase.store(2,std::memory_order_release);
    log("Lab online isolation installed synchronously at D3D9 factory entry; interface=0");
}
bool labOnlineIsolationInstalled() noexcept {
    return labIsolationPhase.load(std::memory_order_acquire)==2 && labOnlineIsolated;
}
void validateLabOnlineIsolation(bool gameplay) {
    const auto phase=labIsolationPhase.load(std::memory_order_acquire);
    if (!phase) return;
    if(phase!=2 || !labOnlineIsolated) abortLab("partial lab isolation reached native gameplay");
    uint32_t pointer=0;
    std::memcpy(&pointer,reinterpret_cast<const void *>(labEngineBase+0x2ec890),sizeof(pointer));
    if (pointer && !labMultiplayer()) abortLab("online interface appeared during private fixture");
    const auto *net=labCurrentNet();
    if (!net) { if (gameplay) abortLab("gameplay has no native local transport"); return; }
    // Opt-in multiplayer client (SS2VR_LAB_MULTIPLAYER=1): the native UDP
    // transport to a server is allowed; Steam's online interface stays null.
    if (labMultiplayer()) {
        if (gameplay) {
            static bool reported=false;
            if (!reported) { reported=true; log("Lab multiplayer gameplay observed behind the Steam write barrier local=%d", labNetIsLocal()); }
        }
        return;
    }
    uintptr_t table=0;std::memcpy(&table,net,sizeof(table));
    const auto game=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"Sam2Game.dll"));
    if (!labNetIsLocal() || !game || table!=game+0x29d270)
        abortLab("private fixture escaped expected native local transport");
    static bool localReported=false;
    if (!localReported) { localReported=true; log("Lab native local transport established; interface=0"); }
    if (gameplay) {
        static bool reported=false;
        if (!reported) { reported=true; log("Lab local gameplay observed with online interface=0"); }
    }
}
static IDirect3DDevice9 *device = nullptr, *creationDevice = nullptr;
static DWORD creationThread = 0;
static IDirect3DSurface9 *eye[2]{}, *depth[2]{}, *readback[2]{};
// Single pass: one double-wide colour/depth pair (left eye | right eye).
static IDirect3DSurface9 *wideColor = nullptr, *wideDepth = nullptr;
// Zero-copy eye images (ABI14): one shared render target per IPC slot and eye.
// The host opens them by handle and copies straight into its swapchains, so a
// pair needs no readback. A slot's pair is published Ready at once and marked
// gpuReady when its event query shows the GPU finished (polled, never waited).
static IDirect3DTexture9 *sharedTexture[2][2]{};
static IDirect3DSurface9 *sharedSurface[2][2]{};
static IDirect3DQuery9 *sharedFence[2]{};
static uint32_t sharedGeneration = 0, sharedGenerations = 0;
static bool sharedPending[2]{}, sharedFailed = false;
static uint64_t sharedPendingSequence[2]{};
// Foveated readback per eye: [0] downsampled full view, [1] full-resolution inset.
static IDirect3DSurface9 *foveaTarget[2][2]{}, *foveaRead[2][2]{};
static uint32_t foveaWidth[2][2]{}, foveaHeight[2][2]{};
static uint32_t width = 0, height = 0;
// Eye/menu pixel sections for the current canvas extent (see PixelStore).
static PixelStore pixelStore;
static uint32_t pixelGenerations = 0;
// One admitted native output, beside the existing device/resource lifetime.
// Surfaces are reacquired per transaction; only the chain identity is retained.
static IDirect3DSwapChain9 *presentationChain = nullptr;
static HWND presentationWindow = nullptr;
static bool resetQuarantined = false;
static D3DFORMAT depthFormat = D3DFMT_UNKNOWN, colorFormat = D3DFMT_UNKNOWN;
static bool hasStencil = false;
static std::atomic<bool> stopping = false;
// Frame cost accounting, logged every five seconds. CPU times come from QPC;
// GPU times from D3D9 timestamp queries read back a few frames later without
// stalling. Marks: 0 pair start, 1 left eye, 2 right eye, 3 desktop pass,
// 4 end of the frame's native UI (just before readback).
struct PerfClock {
    double frame = 0, stereo = 0, mono = 0, eyeCpu[2]{}, desktopCpu = 0;
    double readTarget = 0, lock = 0, copy = 0, dim = 0, publish = 0, wait = 0, presentCall = 0, presentHook = 0;
    double preStereo = 0, postStereo = 0; // Present end -> stereo start, stereo end -> present start.
    double section[3]{};
    uint64_t eyeDraws = 0, desktopDraws = 0, hudDuplicates = 0, hudDraws = 0;
    double hudCpu = 0; // Whole HUD draw path (desktop draw, capture, both eye duplicates, restore).
    double hudFlushCpu = 0;
    unsigned hudLayerDraws = 0, hudFallbackDraws = 0, hudFlushes = 0;
    unsigned foveatedPairs = 0; // Pairs rendered through the foveated atlas.
    // Pairs rendered but not published, by the state they ended in.
    unsigned lostNoWorld = 0, lostFault = 0, lostNoOverlay = 0, lostIncomplete = 0, lostOther = 0;
    const char *lostReason = "none";
    uintptr_t lostOrigin = 0;
    // First failing stereo stage per lost pair: begin, scope, setup, world,
    // uiProjection, eyeInvalid, target, postlude, readback, restore.
    unsigned stage[10]{};
    unsigned scopeNeeded = 0, scopeCandidates = 0, scopeBegun = 0, scopeReady = 0, scopeHalted = 0;
    unsigned scopeCallbacks = 0, scopeCopied = 0, scopeFinished = 0;
    unsigned replayFallbacks = 0;
    double scopeSourceCpu = 0, scopeTailCpu = 0;
    unsigned scopeBeginFailures[4]{};
    float scopeLuma = -1;
    unsigned publishLock = 0, publishCancelled = 0, publishState = 0, publishRequest = 0, publishImages = 0,
        publishCommit = 0, finishNotCurrent = 0;
    bool lostWorldOk = false, lostUi = false, lostOverlaySeen = false, lostComplete = false;
    unsigned stereoCalls = 0;
    double gpu[4]{}; // left, right, desktop, ui/tail (ms sums)
    unsigned frames = 0, pairs = 0, monoFrames = 0, gpuSamples = 0, readbacks = 0, waits = 0;
    LARGE_INTEGER lastPresent{}, lastLog{};
    static double seconds(LARGE_INTEGER a, LARGE_INTEGER b) {
        static LARGE_INTEGER frequency{};
        if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
        return double(b.QuadPart - a.QuadPart) / double(frequency.QuadPart);
    }
    static LARGE_INTEGER now() { LARGE_INTEGER t{}; QueryPerformanceCounter(&t); return t; }
};
static PerfClock perf;
static ULONGLONG lastPairTick = 0; // Last published stereo pair (VR frame pacing).
extern "C" IMAGE_DOS_HEADER __ImageBase;
struct GpuProfile {
    static constexpr unsigned Sets = 6, Marks = 5;
    IDirect3DDevice9 *owner = nullptr;
    IDirect3DQuery9 *disjoint[Sets]{}, *frequency[Sets]{}, *stamp[Sets][Marks]{};
    unsigned marked[Sets]{}; // Bit per issued mark.
    unsigned age[Sets]{};    // Pairs begun since the set was ended.
    bool open[Sets]{}, pending[Sets]{}, failed = false;
    int current = -1;
    unsigned next = 0;
    void release() {
        for (unsigned i = 0; i < Sets; ++i) {
            for (auto *&q : stamp[i]) { if (q) q->Release(); q = nullptr; }
            if (disjoint[i]) disjoint[i]->Release();
            if (frequency[i]) frequency[i]->Release();
            disjoint[i] = frequency[i] = nullptr;
            marked[i] = 0; open[i] = pending[i] = false;
        }
        owner = nullptr; current = -1;
    }
    bool create(IDirect3DDevice9 *d) {
        if (owner == d) return true;
        release();
        // Timestamp queries cost DXVK work: lab/diagnostic runs only.
        static const bool enabled = [] { wchar_t value[2]{}; return GetEnvironmentVariableW(L"SS2VR_PERF_GPU", value, 2) == 1 && value[0] == L'1'; }();
        if (!enabled || failed || !d) return false;
        for (unsigned i = 0; i < Sets; ++i) {
            if (FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &disjoint[i])) ||
                FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &frequency[i]))) { failed = true; release(); return false; }
            for (auto *&q : stamp[i])
                if (FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q))) { failed = true; release(); return false; }
        }
        owner = d;
        return true;
    }
    void collect() {
        for (unsigned i = 0; i < Sets; ++i) {
            if (!pending[i]) continue;
            if (++age[i] > 2 * Sets) { pending[i] = false; continue; } // Never resolved: drop it.
            BOOL disjointed = TRUE;
            UINT64 hz = 0, t[Marks]{};
            if (disjoint[i]->GetData(&disjointed, sizeof(disjointed), 0) != S_OK ||
                frequency[i]->GetData(&hz, sizeof(hz), 0) != S_OK) continue;
            bool ready = true;
            for (unsigned k = 0; k < Marks && ready; ++k)
                if (marked[i] & (1u << k)) ready = stamp[i][k]->GetData(&t[k], sizeof(t[k]), 0) == S_OK;
            if (!ready) continue;
            pending[i] = false;
            if (disjointed || !hz || marked[i] != (1u << Marks) - 1) continue;
            for (unsigned k = 0; k + 1 < Marks; ++k) perf.gpu[k] += 1000. * double(t[k + 1] - t[k]) / double(hz);
            ++perf.gpuSamples;
        }
    }
    void begin(IDirect3DDevice9 *d) {
        current = -1;
        if (!create(d)) return;
        collect();
        const unsigned i = next;
        if (pending[i]) return; // Ring full: skip this pair.
        open[i] = false;        // A pair that never reached end() is abandoned.
        next = (next + 1) % Sets;
        marked[i] = 0;
        if (FAILED(disjoint[i]->Issue(D3DISSUE_BEGIN))) return;
        open[i] = true;
        current = int(i);
        mark(0);
    }
    void mark(unsigned k) {
        if (current < 0 || k >= Marks) return;
        if (SUCCEEDED(stamp[current][k]->Issue(D3DISSUE_END))) marked[current] |= 1u << k;
    }
    void end() {
        if (current < 0) return;
        const unsigned i = unsigned(current);
        disjoint[i]->Issue(D3DISSUE_END);
        frequency[i]->Issue(D3DISSUE_END);
        open[i] = false;
        pending[i] = true;
        age[i] = 0;
        current = -1;
    }
};
static GpuProfile gpuProfile;
static int deferredSlot = -1;
static uint64_t deferredSequence = 0;
static void retireDeferred() {
    if (deferredSlot < 0 || !channel.shared)
        return;
    Lock l(channel);
    if (l) {
        auto &slot = channel.shared->slot[deferredSlot];
        if (slot.state == SlotState::Rendering && slot.request.sequence == deferredSequence)
            slot.state = SlotState::Empty;
        deferredSlot = -1;
    }
}
thread_local int activeEye = -1;
thread_local bool eyeInvalid = false;
static thread_local bool scopeScratch = false;
static std::atomic<bool> scopeSourcesHalted{false};
thread_local IDirect3DSurface9 *desktopTarget = nullptr;
thread_local IDirect3DSurface9 *desktopDepth = nullptr;
thread_local Request renderRequest;
extern bool beginStereo(void *, const Request &, uint32_t &);
extern bool nativeWorldRenderActive() noexcept;
extern bool headFramePrepared(void *,const Request &) noexcept;
extern float frozenWorldVisibility();
extern bool commitStereo(void *, const Request &, Slot &, uint32_t);
extern void beginEye(void *, const Request &, int);
extern void *shadowReuseProbeTexture() noexcept;
extern void rootExecuteStats(double &left, double &right) noexcept;
extern void shadowReuseStats(unsigned &created, unsigned &reused, unsigned &missed, double &createdMs, double &reusedMs) noexcept;
extern void endEye();
static bool sameSurface(IDirect3DSurface9 *a, IDirect3DSurface9 *b) {
    if (a == b)
        return true;
    if (!a || !b)
        return false;
    IUnknown *x = nullptr, *y = nullptr;
    bool same = false;
    const bool contained = withNativeFinally([&] {
        same = SUCCEEDED(a->QueryInterface(IID_IUnknown, reinterpret_cast<void **>(&x))) &&
               SUCCEEDED(b->QueryInterface(IID_IUnknown, reinterpret_cast<void **>(&y))) && x == y;
    }, [&](bool) noexcept {
        if (x) { x->Release(); x = nullptr; }
        if (y) { y->Release(); y = nullptr; }
    });
    return contained && same;
}
using SetRT = HRESULT(WINAPI *)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *);
static SetRT originalSetRT = nullptr;
using SetDepth = HRESULT(WINAPI *)(IDirect3DDevice9 *, IDirect3DSurface9 *);
static SetDepth originalSetDepth = nullptr;
using Stretch = HRESULT(WINAPI *)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *,
                                  const RECT *, D3DTEXTUREFILTERTYPE);
using ReadTarget = HRESULT(WINAPI *)(IDirect3DDevice9 *, IDirect3DSurface9 *, IDirect3DSurface9 *);
using Fill = HRESULT(WINAPI *)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, D3DCOLOR);
static Stretch originalStretch = nullptr;
static ReadTarget originalReadTarget = nullptr;
static Fill originalFill = nullptr;
// One existing Rendering slot, held through its native once-only owner. These
// explicit owners precede foreign calls: native SEH does not unwind GNU RAII.
static std::atomic<uint64_t> resourceGeneration{1};
uint64_t graphicsResourceGeneration() noexcept { return resourceGeneration.load(std::memory_order_acquire); }
static std::atomic<bool> uiHalted{false};
static bool uiRoutingReady = false;
bool scopeGpuRoutingCurrent(IDirect3DDevice9 *d) noexcept {
    return uiRoutingReady && d == device && d == creationDevice && creationThread == GetCurrentThreadId();
}
static IDirect3DSurface9 *uiQuarantinedReadback = nullptr;
struct NativeUiFrame {
    IDirect3DDevice9 *device = nullptr;
    IDirect3DSwapChain9 *chain = nullptr;
    IDirect3DSurface9 *color = nullptr, *ds = nullptr, *backbuffer = nullptr, *extra = nullptr,
        *target[2]{}, *z[2]{}, *read[2]{}, *locked = nullptr, *probeColor = nullptr, *probeDepth = nullptr;
    D3DVIEWPORT9 viewport{};
    RECT scissor{};
    Request request;
    Matrix44 projection[2];
    Pose panel[2];
    uint8_t *pixels[2]{}; // The admitted slot's eye buffers in the current pixel sections.
    PixelRect insetRect[2]; // Foveated pairs: the crop actually read back per eye.
    size_t pixelBytes = 0;
    uint32_t pixelGeneration = 0;
    void *player = nullptr;
    uint64_t generation = 0;
    uint32_t remoteOwner = 0;
    int slot = -1;
    bool restore = false, ui = false, fault = false, world = false, zeroCopy = false, published = false;
    bool overlay = false, overlaySeen = false, complete = false, fade = false;
    bool hudLayerDirty = false, hudLayerCleared = false; // HUD layer content this frame / cleared this frame.
    bool hudWristCleared = false;                        // Wrist layer cleared since the last flush.
    // Wrist HUD: each tagged element's canvas rectangle since the last flush.
    struct WristRect { float x0 = 0, y0 = 0, x1 = 0, y1 = 0; int side = -1; bool used = false; };
    WristRect wristRects[hud_wrist::MaxElements];
    bool panelHidden[2]{}; // The HUD panel is behind this eye: the HUD is drawn in neither eye.
    uintptr_t faultOrigin = 0;
    const char *faultReason = "none";
    D3DVERTEXELEMENT9 diagnosticElements[8]{};
    UINT diagnosticElementCount = 0;
    DWORD diagnosticShaderVersion = 0, diagnosticShaderBytes = 0;
    DWORD diagnosticFirstOpcode = 0, diagnosticFirstLength = 0, diagnosticDclUsage = 0, diagnosticDclIndex = 0;
    DWORD diagnosticDclRegisterType = 0, diagnosticDclRegister = 0;
    bool diagnosticDcl = false;
    ScopeSourceView scopeView[2];
    IDirect3DTexture9 *scopeImage[2]{};
    IDirect3DPixelShader9 *scopeShader = nullptr;
    bool scopeReady[2]{};
};
static thread_local NativeUiFrame uiFrame;
// Publish only after normal-path TLS construction. Native unwind retirement
// must not evaluate uiFrame's TLS initializer (vectors/destructor registration).
// Same-thread, non-owning pointer; admission and retirement remain slot-owned.
static constinit thread_local NativeUiFrame *admittedUiFrame = nullptr;
static thread_local bool scopeTransaction = false, scopeInterference = false;
static thread_local uint64_t scopeTransactionGeneration = 0;
static void retireNativeFrameForReset() noexcept;
// Written once by the first deviceCreated diagnostic, then immutable. This
// snapshot correlates startup events; it never establishes current ownership.
static struct {
    IDirect3DDevice9 *device = nullptr;
    DWORD thread = 0;
} startupCreation;
static std::atomic<bool> startupCreationPublished{false};
// Startup observations only: no COM/native calls or shared-memory dereferences.
// Callback observations use arguments, immutable metadata, TLS and atomics.
// Local idle excludes cross-thread deferred state; unavailable fields are -1.
static void logStartup(const char *event, IDirect3DDevice9 *d, bool ready,
                       int routingReady = -1, int channelPresent = -1) {
    const DWORD currentThread = GetCurrentThreadId();
    const bool published = startupCreationPublished.load(std::memory_order_acquire);
    auto *firstDevice = published ? startupCreation.device : nullptr;
    const DWORD firstThread = published ? startupCreation.thread : 0;
    const int ownerMatch = published ? currentThread == firstThread && d == firstDevice : -1;
    const bool idleLocal = uiFrame.slot < 0 && activeEye < 0 && !scopeScratch && !scopeTransaction;
    log("Startup %s device=%p firstCreationDevice=%p firstCreationThread=%lu thread=%lu "
        "firstOwnerMatch=%d idleLocal=%d hooksReady=%d routingReady=%d channel=%d "
        "stopping=%d halted=%d eye=%d uiSlot=%d scratch=%d transaction=%d",
        event, static_cast<void *>(d), static_cast<void *>(firstDevice), firstThread, currentThread,
        ownerMatch, idleLocal, ready, routingReady, channelPresent, stopping.load(), uiHalted.load(),
        activeEye, uiFrame.slot, scopeScratch, scopeTransaction);
}
bool startupDeviceOwner(IDirect3DDevice9 *d) noexcept {
    return startupCreationPublished.load(std::memory_order_acquire) &&
        d == startupCreation.device && GetCurrentThreadId() == startupCreation.thread;
}
bool presentationDeviceOwner(IDirect3DDevice9 *d) noexcept {
    // Only the immutable first creation thread may read/change current graphics
    // ownership. Foreign concurrent factories remain desktop-only.
    return startupCreationPublished.load(std::memory_order_acquire) &&
        GetCurrentThreadId() == startupCreation.thread && d == device && d == creationDevice &&
        GetCurrentThreadId() == creationThread;
}
uint32_t traceChainPresent(IDirect3DSwapChain9 *chain, uintptr_t caller, HWND overrideWindow) noexcept {
    // Diagnostic only: immutable startup metadata is not current admission.
    // Foreign threads forward without COM introspection. References never span
    // the original Present, so reset/replacement cannot inherit probe ownership.
    if (!chain || !startupCreationPublished.load(std::memory_order_acquire) ||
        GetCurrentThreadId() != startupCreation.thread) return 0;
    if (resetQuarantined) return 0;
    const bool ready = hooksReady.load(std::memory_order_acquire);
    // Only the published creation thread writes these saturating counters.
    static unsigned observed[2]{}, tickets = 0;
    if (observed[ready] >= (ready ? 8u : 2u)) return 0;
    const unsigned ordinal = observed[ready]++;
    uint32_t ticket = 0;
    IDirect3DDevice9 *d = nullptr;
    IDirect3DSurface9 *bb = nullptr, *rt = nullptr, *z = nullptr;
    withNativeFinally([&] {
        const HRESULT deviceResult = chain->GetDevice(&d);
        if (FAILED(deviceResult) || d != startupCreation.device) return;
        ticket = ++tickets;
        D3DPRESENT_PARAMETERS params{};
        D3DSURFACE_DESC color{}, target{}, depthDesc{};
        const HRESULT paramsResult = chain->GetPresentParameters(&params);
        const HRESULT bbResult = chain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
        const HRESULT colorResult = bb ? bb->GetDesc(&color) : D3DERR_NOTFOUND;
        const HRESULT rtResult = d->GetRenderTarget(0, &rt);
        if (rt) rt->GetDesc(&target);
        const HRESULT zResult = d->GetDepthStencilSurface(&z);
        if (z) z->GetDesc(&depthDesc);
        const auto gfx = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"GfxD3D.dll"));
        const uintptr_t rva = gfx && caller >= gfx ? caller - gfx : 0;
        const HWND destination = overrideWindow ? overrideWindow : params.hDeviceWindow;
        log("Presentation probe ticket=%u ordinal=%u completed=0 hooksReady=%d thread=%lu "
            "chain=%p device=%p caller=%p gfxRva=%08lx window=%p foreground=%p "
            "paramsHr=%08lx bbHr=%08lx descHr=%08lx bb=%p size=%ux%u format=%u msaa=%u "
            "rtHr=%08lx rt=%p rtSize=%ux%u depthHr=%08lx depth=%p depthSize=%ux%u depthFormat=%u",
            ticket, ordinal, ready, GetCurrentThreadId(),
            static_cast<void *>(chain), static_cast<void *>(d), reinterpret_cast<void *>(caller),
            static_cast<unsigned long>(rva), static_cast<void *>(destination),
            static_cast<void *>(GetForegroundWindow()), static_cast<unsigned long>(paramsResult),
            static_cast<unsigned long>(bbResult), static_cast<unsigned long>(colorResult),
            static_cast<void *>(bb), color.Width, color.Height, static_cast<unsigned>(color.Format),
            static_cast<unsigned>(color.MultiSampleType), static_cast<unsigned long>(rtResult),
            static_cast<void *>(rt), target.Width, target.Height, static_cast<unsigned long>(zResult),
            static_cast<void *>(z), depthDesc.Width, depthDesc.Height, static_cast<unsigned>(depthDesc.Format));
    }, [&](bool) noexcept {
        for (auto *surface : {bb, rt, z}) if (surface) surface->Release();
        bb = rt = z = nullptr;
        if (d) { d->Release(); d = nullptr; }
    });
    return ticket;
}
void scopeGpuOutput(IDirect3DDevice9 *d) noexcept {
    if (scopeTransaction && d == uiFrame.device) scopeInterference = true;
}
bool scopeGpuMappingObservationCurrent(IDirect3DDevice9 *d) noexcept {
    return !uiHalted && !eyeInvalid && !uiFrame.fault && scopeGpuRoutingCurrent(d);
}
bool scopeGpuTransactionBegin(IDirect3DDevice9 *d) noexcept {
    if (scopeTransaction) { scopeInterference = true; return false; }
    if (!scopeGpuRoutingCurrent(d) || uiFrame.device != d || uiFrame.slot < 0 ||
        uiFrame.fault || uiHalted || stopping) return false;
    scopeTransactionGeneration = graphicsResourceGeneration();
    scopeInterference = false; scopeTransaction = true;
    return true;
}
bool scopeGpuTransactionCurrent(IDirect3DDevice9 *d) noexcept {
    return scopeTransaction && !scopeInterference && scopeGpuRoutingCurrent(d) && d == uiFrame.device &&
        uiFrame.slot >= 0 && !uiFrame.fault && !uiHalted && !stopping &&
        scopeTransactionGeneration == graphicsResourceGeneration() &&
        uiFrame.generation == graphicsResourceGeneration();
}
void scopeGpuTransactionEnd() noexcept { scopeTransaction = false; }
bool scopeGpuImage(IDirect3DDevice9 *d,unsigned hand,ScopeSourceView &view,
                   IDirect3DTexture9 *&texture,IDirect3DPixelShader9 *&shader) noexcept {
    if (texture || shader || hand >= 2 || scopeScratch || activeEye < 0 || activeEye > 1 ||
        !scopeGpuRoutingCurrent(d) || d != uiFrame.device || uiFrame.slot < 0 ||
        uiFrame.fault || uiHalted || stopping || scopeSourcesHalted ||
        uiFrame.generation != graphicsResourceGeneration() || !uiFrame.scopeReady[hand] ||
        !uiFrame.scopeImage[hand] || !uiFrame.scopeShader || uiFrame.overlay) return false;
    view = uiFrame.scopeView[hand];
    texture = uiFrame.scopeImage[hand]; texture->AddRef();
    shader = uiFrame.scopeShader; shader->AddRef();
    return true;
}
bool scopeGpuEyeOwner(IDirect3DDevice9 *d,IUnknown *&color,IUnknown *&z,UINT &w,UINT &h,
                      UINT &surfaceW,UINT &surfaceH,bool &stereo) noexcept {
    if(color || z || !scopeGpuRoutingCurrent(d) || activeEye<0 || activeEye>1 ||
        uiFrame.slot<0 || uiFrame.device!=d || uiFrame.fault || uiFrame.overlay || stopping || uiHalted ||
        uiFrame.generation!=graphicsResourceGeneration() ||
        renderRequest.sequence!=uiFrame.request.sequence ||
        renderRequest.input.sequence!=uiFrame.request.input.sequence ||
        !uiFrame.target[activeEye] || !uiFrame.z[activeEye]) return false;
    w=uiFrame.request.width; h=uiFrame.request.height;
    surfaceW=w; surfaceH=h; stereo=false;
    // Single pass: one draw for both eyes into the pass's wide (or atlas) target,
    // swapped in as eye 0's; viewport and scissor read back as the eye's own.
    UINT targetW=0,targetH=0,eyeW=0,eyeH=0;
    if(single_pass::passExtent(targetW,targetH,eyeW,eyeH)) {
        if(activeEye!=0 || eyeW!=w || eyeH!=h || !single_pass::drawing(d)) return false;
        surfaceW=targetW; surfaceH=targetH; stereo=true;
    }
    return w && h &&
        SUCCEEDED(uiFrame.target[activeEye]->QueryInterface(IID_IUnknown,reinterpret_cast<void **>(&color))) && color &&
        SUCCEEDED(uiFrame.z[activeEye]->QueryInterface(IID_IUnknown,reinterpret_cast<void **>(&z))) && z;
}
static thread_local bool uiBypass = false;
static void releaseUiFrame() noexcept;
static bool restoreUiFrame() noexcept;
__attribute__((noinline)) void nativeUiFault(const char *reason) noexcept {
    auto *frame = admittedUiFrame;
    if(frame && frame->slot>=0) {
        if(!frame->fault) {
            frame->faultOrigin=reinterpret_cast<uintptr_t>(__builtin_extract_return_addr(__builtin_return_address(0)));
            frame->faultReason=reason;
        }
        frame->fault=true;
    }
}
void scopeGpuFault() noexcept {
    eyeInvalid=true;
    uiFrame.fault=true; // Includes world-only pairs; nativeUiFault is UI-conditional.
    uiHalted=true;
}
static bool uiCurrent() {
    return uiFrame.slot >= 0 && uiFrame.ui && !uiFrame.fault && !stopping && !uiHalted &&
        uiFrame.generation == resourceGeneration.load() && nativeUiDeviceCurrent(uiFrame.device) &&
        nativeUiFrameCurrent(uiFrame.player, uiFrame.request);
}
static bool observingUi(IDirect3DDevice9 *d) {
    return !uiBypass && uiFrame.overlay && uiFrame.ui && d == uiFrame.device;
}
// No resource owner is left on an inner callback stack. Restoration is limited
// to state actually modified; native programs/textures/blends/streams stay bound.
struct NativeUiDrawState {
    IDirect3DSurface9 *color = nullptr, *extra = nullptr;
    IDirect3DSwapChain9 *chain = nullptr;
    IDirect3DVertexShader9 *vertex = nullptr;
    IDirect3DPixelShader9 *pixel = nullptr;
    IDirect3DVertexDeclaration9 *declaration = nullptr;
    IDirect3DVertexBuffer9 *positions = nullptr;
    D3DVIEWPORT9 viewport{};
    RECT scissor{};
    Matrix44 constants;
    float planes[6][4]{};
    DWORD state[4]{};
    DWORD fill = 0;
    DWORD layerState[5]{}; // Saved separate-alpha/colour-write states of a HUD layer draw.
    const char *captureStage = "not-started";
    bool busy = false, captured = false, changed = false, layered = false;
};
static thread_local NativeUiDrawState uiDraw;
static constexpr D3DRENDERSTATETYPE uiStates[]{D3DRS_ZENABLE, D3DRS_ZWRITEENABLE,
    D3DRS_SCISSORTESTENABLE, D3DRS_CLIPPLANEENABLE};
static constexpr D3DRENDERSTATETYPE hudLayerStates[]{D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SRCBLENDALPHA,
    D3DRS_DESTBLENDALPHA, D3DRS_BLENDOPALPHA, D3DRS_COLORWRITEENABLE};
static void cleanupUiDraw(bool aborted) noexcept {
    if (aborted) { nativeUiFault(); uiHalted = true; }
    bool ok = true;
    if (uiDraw.changed && uiDraw.captured && uiFrame.device) {
        uiBypass = true;
        auto *d = uiFrame.device;
        ok = SUCCEEDED(originalSetRT(d, 0, uiDraw.color)) && ok;
        ok = SUCCEEDED(d->SetVertexShaderConstantF(1, uiDraw.constants.m, 4)) && ok;
        for (unsigned i=0;i<4;++i) ok = SUCCEEDED(d->SetRenderState(uiStates[i], uiDraw.state[i])) && ok;
        for (unsigned i=0;i<6;++i) ok = SUCCEEDED(d->SetClipPlane(i, uiDraw.planes[i])) && ok;
        if (uiDraw.layered)
            for (unsigned i=0;i<std::size(hudLayerStates);++i)
                ok = SUCCEEDED(d->SetRenderState(hudLayerStates[i], uiDraw.layerState[i])) && ok;
        // SetRenderTarget resets viewport. Both raster rectangles are restored last.
        ok = SUCCEEDED(d->SetViewport(&uiDraw.viewport)) && ok;
        ok = SUCCEEDED(d->SetScissorRect(&uiDraw.scissor)) && ok;
        uiBypass = false;
    }
    if (!ok) { nativeUiFault(); uiHalted = true; }
    uiBypass = false;
    if (uiDraw.chain) uiDraw.chain->Release();
    if (uiDraw.extra) uiDraw.extra->Release();
    if (uiDraw.color) uiDraw.color->Release();
    if (uiDraw.vertex) uiDraw.vertex->Release();
    if (uiDraw.pixel) uiDraw.pixel->Release();
    if (uiDraw.declaration) uiDraw.declaration->Release();
    if (uiDraw.positions) uiDraw.positions->Release();
    uiDraw = {};
}
static bool singleUiTarget(IDirect3DDevice9 *d,IDirect3DSurface9 *&extra) {
    D3DCAPS9 caps{};
    if (FAILED(d->GetDeviceCaps(&caps))) return false;
    for (DWORD i=1;i<caps.NumSimultaneousRTs;++i) {
        const HRESULT hr=d->GetRenderTarget(i,&extra);
        const bool active=extra!=nullptr;
        if (extra) extra->Release();
        extra=nullptr;
        if ((FAILED(hr) && hr!=D3DERR_NOTFOUND) || active) return false;
    }
    return true;
}
static bool uiOffscreen(IDirect3DSurface9 *target,IDirect3DSwapChain9 *&chain) {
    if (!target) return false;
    const HRESULT result=target->GetContainer(__uuidof(IDirect3DSwapChain9),reinterpret_cast<void **>(&chain));
    // Another backbuffer is another output, not an intermediate composition.
    if (chain) chain->Release();
    chain=nullptr;
    return result==E_NOINTERFACE;
}
static bool captureUiDraw(IDirect3DDevice9 *d) {
    // Scalar stage labels retain the first rejection without logging or querying
    // additional native state on callback/unwind paths. Query order is unchanged.
    uiDraw.captureStage="current-or-target";
    if (!uiCurrent() || FAILED(d->GetRenderTarget(0,&uiDraw.color))) return false;
    uiDraw.captureStage="single-target";
    if (!singleUiTarget(d,uiDraw.extra)) { nativeUiFault(uiDraw.captureStage); return false; }
    if (!sameSurface(uiDraw.color,uiFrame.color)) {
        uiDraw.captureStage="other-output";
        if (!uiOffscreen(uiDraw.color,uiDraw.chain)) nativeUiFault(uiDraw.captureStage);
        return false;
    }
    DWORD fog=0, stencil=0, clipping=0;
    uiDraw.captureStage="fog";
    if (FAILED(d->GetRenderState(D3DRS_FOGENABLE,&fog)) || fog) return false;
    uiDraw.captureStage="stencil";
    if (FAILED(d->GetRenderState(D3DRS_STENCILENABLE,&stencil)) || stencil) return false;
    uiDraw.captureStage="clipping";
    if (FAILED(d->GetRenderState(D3DRS_CLIPPING,&clipping)) || !clipping) return false;
    uiDraw.captureStage="fill-mode";
    if (FAILED(d->GetRenderState(D3DRS_FILLMODE,&uiDraw.fill))) return false;
    uiDraw.captureStage="vertex-shader";
    if (FAILED(d->GetVertexShader(&uiDraw.vertex))) return false;
    uiDraw.captureStage="pixel-shader";
    if (FAILED(d->GetPixelShader(&uiDraw.pixel))) return false;
    uiDraw.captureStage="native-programs";
    if (!nativeUiProgramsCurrent(uiDraw.vertex,uiDraw.pixel)) return false;
    uiDraw.captureStage="declaration";
    if (FAILED(d->GetVertexDeclaration(&uiDraw.declaration)) || !uiDraw.declaration) return false;
    D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH+1]{};
    UINT count=MAXD3DDECLLENGTH+1, positionBytes=0, positionOffset=0;
    uiDraw.captureStage="declaration-elements";
    if (FAILED(uiDraw.declaration->GetDeclaration(elements,&count)) || count>MAXD3DDECLLENGTH+1) return false;
    uiFrame.diagnosticElementCount=std::min<UINT>(count,8);
    for(UINT i=0;i<uiFrame.diagnosticElementCount;++i) uiFrame.diagnosticElements[i]=elements[i];
    for (unsigned i=0;i<count && elements[i].Stream!=0xff;++i) {
        const auto &e=elements[i];
        uiDraw.captureStage="transformed-position";
        if (e.Usage == D3DDECLUSAGE_POSITIONT) return false;
        if (e.Usage != D3DDECLUSAGE_TEXCOORD || e.UsageIndex != 0) continue;
        uiDraw.captureStage="position-layout";
        if (positionBytes || !nativeUiPositionInput(e.Stream,e.Offset,e.Type,e.Method,e.Usage,e.UsageIndex)) return false;
        positionBytes = 12;
        positionOffset = e.Offset;
    }
    // Fingerprinted backend injects dcl_texcoord0 v0 into the native builtin
    // VS1.1 program. POSITION0 legacy mapping is not this backend's contract.
    UINT offset=0,stride=0,frequency=0;
    uiDraw.captureStage="position-missing";
    if (!positionBytes) {
        // Opt-in lab observation only, never an admission predicate or shader
        // rewrite. Inspect the first instruction of the actual bound program;
        // retain scalar semantics, not shader code, for normal-owner logging.
        static const bool trace=[] { wchar_t value[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_TRACE",value,2)==1 && value[0]==L'1'; }();
        static bool observed=false;
        if(trace && !observed) {
            observed=true;
            DWORD words[256]{}; UINT bytes=0;
            if(SUCCEEDED(uiDraw.vertex->GetFunction(nullptr,&bytes)) && bytes>=4 && bytes<=sizeof(words) && !(bytes%4) &&
                SUCCEEDED(uiDraw.vertex->GetFunction(words,&bytes)) && bytes>=4 && bytes<=sizeof(words) && !(bytes%4)) {
                uiFrame.diagnosticShaderVersion=words[0];uiFrame.diagnosticShaderBytes=bytes;
                if(bytes>=8) {
                    uiFrame.diagnosticFirstOpcode=words[1]&D3DSI_OPCODE_MASK;
                    uiFrame.diagnosticFirstLength=(words[1]&D3DSI_INSTLENGTH_MASK)>>D3DSI_INSTLENGTH_SHIFT;
                }
                if(bytes>=16 && (words[0]==0xfffe0101 || words[0]==0xfffe0200 || words[0]==0xfffe0300) &&
                    uiFrame.diagnosticFirstOpcode==D3DSIO_DCL &&
                    (uiFrame.diagnosticFirstLength==2 || (words[0]==0xfffe0101 && uiFrame.diagnosticFirstLength==0))) {
                    uiFrame.diagnosticDcl=true;
                    uiFrame.diagnosticDclUsage=(words[2]&D3DSP_DCL_USAGE_MASK)>>D3DSP_DCL_USAGE_SHIFT;
                    uiFrame.diagnosticDclIndex=(words[2]&D3DSP_DCL_USAGEINDEX_MASK)>>D3DSP_DCL_USAGEINDEX_SHIFT;
                    uiFrame.diagnosticDclRegisterType=((words[3]&D3DSP_REGTYPE_MASK)>>D3DSP_REGTYPE_SHIFT)|
                        ((words[3]&D3DSP_REGTYPE_MASK2)>>D3DSP_REGTYPE_SHIFT2);
                    uiFrame.diagnosticDclRegister=words[3]&D3DSP_REGNUM_MASK;
                }
            }
        }
        return false;
    }
    uiDraw.captureStage="stream-query";
    if (FAILED(d->GetStreamSource(0,&uiDraw.positions,&offset,&stride))) return false;
    uiDraw.captureStage="stream-buffer";
    if (!uiDraw.positions) return false;
    uiDraw.captureStage="stream-stride";
    if (stride < positionOffset+positionBytes) return false;
    uiDraw.captureStage="stream-frequency";
    if (FAILED(d->GetStreamSourceFreq(0,&frequency)) || frequency!=1) return false;
    uiDraw.captureStage="projection-constants";
    if (FAILED(d->GetVertexShaderConstantF(1,uiDraw.constants.m,4))) return false;
    uiDraw.captureStage="viewport";
    if (FAILED(d->GetViewport(&uiDraw.viewport))) return false;
    uiDraw.captureStage="scissor";
    if (FAILED(d->GetScissorRect(&uiDraw.scissor))) return false;
    uiDraw.captureStage="render-state";
    for (unsigned i=0;i<4;++i) if (FAILED(d->GetRenderState(uiStates[i],&uiDraw.state[i]))) return false;
    uiDraw.captureStage="original-clip-planes";
    if (uiDraw.state[3]) return false; // Do not overwrite an original user-plane policy.
    uiDraw.captureStage="clip-plane-values";
    for (unsigned i=0;i<6;++i) if (FAILED(d->GetClipPlane(i,uiDraw.planes[i]))) return false;
    uiDraw.captureStage="complete";
    uiDraw.captured = true;
    return true;
}
using Draw = HRESULT(WINAPI *)(IDirect3DDevice9 *,D3DPRIMITIVETYPE,UINT,UINT);
using DrawIndexed = HRESULT(WINAPI *)(IDirect3DDevice9 *,D3DPRIMITIVETYPE,INT,UINT,UINT,UINT,UINT);
static Draw originalDraw = nullptr;
static DrawIndexed originalDrawIndexed = nullptr;
static_assert(D3DPT_TRIANGLELIST==4 && D3DPT_TRIANGLESTRIP==5 && D3DPT_TRIANGLEFAN==6);
static_assert(D3DFILL_POINT==1 && D3DFILL_WIREFRAME==2 && D3DFILL_SOLID==3);
static_assert(D3DDECLTYPE_FLOAT3==2 && D3DDECLMETHOD_DEFAULT==0 && D3DDECLUSAGE_TEXCOORD==5);
// VR frames render the native desktop view only with [Render] DesktopWorld=1;
// otherwise the window mirrors the left eye.
static bool desktopWorld = false;
void setDesktopWorld(bool render) noexcept { desktopWorld = render; }
// Right eye replayed from the left eye's recorded draw calls ([Render] EyeReplay).
static bool eyeReplay = false;
void setEyeReplay(bool enabled) noexcept {
    wchar_t value[2]{}; // Lab A/B: SS2VR_LAB_EYE_REPLAY=0 renders the right eye natively.
    eyeReplay = enabled && !(GetEnvironmentVariableW(L"SS2VR_LAB_EYE_REPLAY", value, 2) == 1 && value[0] == L'0');
}
// Both eyes from one world traversal ([Render] SinglePass).
static bool singlePassEnabled = false;
void setSinglePass(bool enabled) noexcept {
    wchar_t value[2]{}; // Lab A/B: SS2VR_LAB_SINGLE_PASS=0 renders each eye separately.
    singlePassEnabled = enabled && !(GetEnvironmentVariableW(L"SS2VR_LAB_SINGLE_PASS", value, 2) == 1 && value[0] == L'0');
}
using Clear = HRESULT(WINAPI *)(IDirect3DDevice9 *,DWORD,const D3DRECT *,DWORD,D3DCOLOR,float,DWORD);
using DrawUp = HRESULT(WINAPI *)(IDirect3DDevice9 *,D3DPRIMITIVETYPE,UINT,const void *,UINT);
static Clear originalClear = nullptr;
static DrawUp originalDrawUp = nullptr;
// HUD layer. Each admitted HUD draw renders once, unprojected, into a canvas-
// sized premultiplied layer (separate alpha blending keeps its coverage), and
// the layer is composited at the end of the overlay onto the desktop backbuffer
// (1:1) and onto each eye's panel (perspective-correct, trilinear). This
// replaces a desktop draw plus two projected eye draws, three render-target
// switches and six clip planes per HUD draw. Blend modes that cannot be
// expressed as a layer, and fades, flush the layer and keep the per-eye path.
static IDirect3DTexture9 *hudLayer = nullptr;
static IDirect3DSurface9 *hudLayerSurface = nullptr;
// Wrist HUD ([HUD] Mode=1): the wrist elements' layer, beside the head layer.
static IDirect3DTexture9 *hudWrist = nullptr;
static IDirect3DSurface9 *hudWristSurface = nullptr;
static IDirect3DDevice9 *hudLayerDevice = nullptr;
static bool hudLayerDisabled = false; // Composition failed: keep the per-eye path until resources change.
static uint64_t hudLayerDisabledGeneration = 0;
static IDirect3DStateBlock9 *hudState = nullptr; // Reused per flush (Capture/Apply), like foveaState.
static void releaseHudLayer() noexcept {
    if (hudLayerSurface) hudLayerSurface->Release();
    if (hudLayer) hudLayer->Release();
    if (hudWristSurface) hudWristSurface->Release();
    if (hudWrist) hudWrist->Release();
    hudWristSurface = nullptr; hudWrist = nullptr;
    if (hudState) hudState->Release();
    hudLayerSurface = nullptr; hudLayer = nullptr; hudLayerDevice = nullptr; hudState = nullptr;
}
static bool hudLayerReady(IDirect3DDevice9 *d) noexcept {
    if (hudLayerDisabled && hudLayerDisabledGeneration != resourceGeneration.load()) hudLayerDisabled = false; // Retry after a reset.
    if (hudLayerDisabled || uiFrame.fade || !uiFrame.color) return false;
    const UINT w = uiFrame.request.width, h = uiFrame.request.height;
    D3DSURFACE_DESC desc{};
    if (hudLayer && (hudLayerDevice != d || FAILED(hudLayerSurface->GetDesc(&desc)) || desc.Width != w || desc.Height != h))
        releaseHudLayer();
    if (!hudLayer) {
        static const bool autoMips = [d] {
            D3DCAPS9 c{}; IDirect3D9 *api = nullptr; D3DDEVICE_CREATION_PARAMETERS creation{}; D3DDISPLAYMODE mode{};
            bool ok = SUCCEEDED(d->GetDeviceCaps(&c)) && (c.Caps2 & D3DCAPS2_CANAUTOGENMIPMAP) &&
                (c.PrimitiveMiscCaps & D3DPMISCCAPS_SEPARATEALPHABLEND) &&
                SUCCEEDED(d->GetDirect3D(&api)) && api && SUCCEEDED(d->GetCreationParameters(&creation)) &&
                SUCCEEDED(d->GetDisplayMode(0,&mode)) &&
                api->CheckDeviceFormat(creation.AdapterOrdinal,creation.DeviceType,mode.Format,
                    D3DUSAGE_RENDERTARGET|D3DUSAGE_AUTOGENMIPMAP,D3DRTYPE_TEXTURE,D3DFMT_A8R8G8B8) == D3D_OK;
            if (api) api->Release();
            return ok;
        }();
        if (!autoMips || FAILED(d->CreateTexture(w,h,0,D3DUSAGE_RENDERTARGET|D3DUSAGE_AUTOGENMIPMAP,D3DFMT_A8R8G8B8,
                                                D3DPOOL_DEFAULT,&hudLayer,nullptr)) || !hudLayer ||
            FAILED(hudLayer->GetSurfaceLevel(0,&hudLayerSurface)) || !hudLayerSurface) {
            releaseHudLayer(); hudLayerDisabled = true; hudLayerDisabledGeneration = resourceGeneration.load();
            log("HUD layer unavailable (mips=%u); HUD stays per eye", unsigned(autoMips));
            return false;
        }
        hudLayerDevice = d;
        log("HUD layer %ux%u", w, h);
    }
    if (hud_wrist::wrists() && !hudWrist) {
        if (FAILED(d->CreateTexture(w,h,0,D3DUSAGE_RENDERTARGET|D3DUSAGE_AUTOGENMIPMAP,D3DFMT_A8R8G8B8,
                                    D3DPOOL_DEFAULT,&hudWrist,nullptr)) || !hudWrist ||
            FAILED(hudWrist->GetSurfaceLevel(0,&hudWristSurface)) || !hudWristSurface) {
            if (hudWristSurface) hudWristSurface->Release();
            if (hudWrist) hudWrist->Release();
            hudWristSurface = nullptr; hudWrist = nullptr;
            static bool logged = false;
            if (!logged) { logged = true; log("Wrist HUD layer unavailable; the HUD stays head-mounted"); }
        } else log("Wrist HUD layer %ux%u", w, h);
    }
    return true;
}
// Alpha blending that turns the engine's colour blend into premultiplied layer
// coverage: "over" blends accumulate alpha, additive ones leave it unchanged.
static bool hudLayerAlpha(IDirect3DDevice9 *d,DWORD &source,DWORD &destination) noexcept {
    DWORD enabled=0, src=0, dst=0, op=0;
    if (FAILED(d->GetRenderState(D3DRS_ALPHABLENDENABLE,&enabled)) || !enabled ||
        FAILED(d->GetRenderState(D3DRS_SRCBLEND,&src)) || FAILED(d->GetRenderState(D3DRS_DESTBLEND,&dst)) ||
        FAILED(d->GetRenderState(D3DRS_BLENDOP,&op)) || op != D3DBLENDOP_ADD) return false;
    if ((src == D3DBLEND_SRCALPHA || src == D3DBLEND_ONE) && dst == D3DBLEND_INVSRCALPHA) {
        source = D3DBLEND_ONE; destination = D3DBLEND_INVSRCALPHA; return true;
    }
    if ((src == D3DBLEND_SRCALPHA || src == D3DBLEND_ONE) && dst == D3DBLEND_ONE) {
        source = D3DBLEND_ZERO; destination = D3DBLEND_ONE; return true;
    }
    return false;
}
struct HudVertex { float x, y, z, rhw, u, v; };
// Composite the layer onto the desktop backbuffer and both eyes; restores all
// device state (state block) and the bound render target. Clears the layer flag.
static void labDumpHudLayer(IDirect3DDevice9 *d) noexcept;
static bool flushHudLayer(IDirect3DDevice9 *d) noexcept {
    if (!uiFrame.hudLayerDirty) return true;
    uiFrame.hudLayerDirty = false;
    if (!hudLayer || hudLayerDevice != d) return false;
    labDumpHudLayer(d);
    const LARGE_INTEGER start = PerfClock::now();
    ++perf.hudFlushes;
    hudLayer->GenerateMipSubLevels();
    const bool wristContent = hudWrist && hud_wrist::wrists() && uiFrame.hudWristCleared;
    if (wristContent) hudWrist->GenerateMipSubLevels();
    if (!hudState && FAILED(d->CreateStateBlock(D3DSBT_ALL,&hudState))) hudState = nullptr;
    IDirect3DStateBlock9 *saved = hudState;
    IDirect3DSurface9 *bound = nullptr;
    bool ok = saved && SUCCEEDED(saved->Capture()) &&
        SUCCEEDED(d->GetRenderTarget(0,&bound)) && bound;
    uiBypass = true;
    if (ok) {
        const float W = float(uiFrame.request.width), H = float(uiFrame.request.height);
        ok = SUCCEEDED(d->SetVertexShader(nullptr)) && SUCCEEDED(d->SetPixelShader(nullptr)) &&
            SUCCEEDED(d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1)) && SUCCEEDED(d->SetTexture(0,hudLayer)) &&
            SUCCEEDED(d->SetTexture(1,nullptr));
        const std::pair<D3DTEXTURESTAGESTATETYPE,DWORD> stages[]{
            {D3DTSS_COLOROP,D3DTOP_SELECTARG1},{D3DTSS_COLORARG1,D3DTA_TEXTURE},
            {D3DTSS_ALPHAOP,D3DTOP_SELECTARG1},{D3DTSS_ALPHAARG1,D3DTA_TEXTURE},
            {D3DTSS_TEXCOORDINDEX,0},{D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE}};
        for (const auto &[state,value] : stages) ok = SUCCEEDED(d->SetTextureStageState(0,state,value)) && ok;
        ok = SUCCEEDED(d->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE)) && ok;
        ok = SUCCEEDED(d->SetTextureStageState(1,D3DTSS_ALPHAOP,D3DTOP_DISABLE)) && ok;
        const std::pair<D3DRENDERSTATETYPE,DWORD> states[]{
            {D3DRS_ZENABLE,D3DZB_FALSE},{D3DRS_ZWRITEENABLE,FALSE},{D3DRS_ALPHATESTENABLE,FALSE},
            {D3DRS_ALPHABLENDENABLE,TRUE},{D3DRS_SRCBLEND,D3DBLEND_ONE},{D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA},
            {D3DRS_BLENDOP,D3DBLENDOP_ADD},{D3DRS_SEPARATEALPHABLENDENABLE,FALSE},{D3DRS_CULLMODE,D3DCULL_NONE},
            {D3DRS_FOGENABLE,FALSE},{D3DRS_STENCILENABLE,FALSE},{D3DRS_CLIPPLANEENABLE,0},
            {D3DRS_SCISSORTESTENABLE,FALSE},{D3DRS_LIGHTING,FALSE},{D3DRS_SRGBWRITEENABLE,FALSE},
            {D3DRS_FILLMODE,D3DFILL_SOLID},{D3DRS_SHADEMODE,D3DSHADE_GOURAUD},{D3DRS_SPECULARENABLE,FALSE},
            {D3DRS_TEXTUREFACTOR,0xffffffff},
            {D3DRS_COLORWRITEENABLE,D3DCOLORWRITEENABLE_RED|D3DCOLORWRITEENABLE_GREEN|D3DCOLORWRITEENABLE_BLUE}};
        for (const auto &[state,value] : states) ok = SUCCEEDED(d->SetRenderState(state,value)) && ok;
        const auto draw = [&](IDirect3DSurface9 *target,const HudVertex (&v)[4],bool filtered) {
            const D3DVIEWPORT9 full{0,0,uiFrame.request.width,uiFrame.request.height,0,1};
            bool drawn = SUCCEEDED(originalSetRT(d,0,target)) && SUCCEEDED(d->SetViewport(&full));
            const DWORD filter = filtered ? D3DTEXF_LINEAR : D3DTEXF_POINT;
            const std::pair<D3DSAMPLERSTATETYPE,DWORD> samplers[]{
                {D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP},{D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP},
                {D3DSAMP_MINFILTER,filter},{D3DSAMP_MAGFILTER,filter},
                {D3DSAMP_MIPFILTER,filtered ? D3DTEXF_LINEAR : D3DTEXF_NONE},{D3DSAMP_MAXMIPLEVEL,0},
                {D3DSAMP_MIPMAPLODBIAS,0},{D3DSAMP_SRGBTEXTURE,FALSE}};
            for (const auto &[state,value] : samplers) drawn = SUCCEEDED(d->SetSamplerState(0,state,value)) && drawn;
            ++perf.hudDuplicates;
            return drawn && SUCCEEDED(originalDrawUp(d,D3DPT_TRIANGLEFAN,2,v,sizeof(HudVertex)));
        };
        // Desktop: exactly one texel per pixel (D3D9 centres pre-transformed pixels on integers).
        const HudVertex desktop[4]{{-.5f,-.5f,.5f,1,0,0},{W-.5f,-.5f,.5f,1,1,0},
                                   {W-.5f,H-.5f,.5f,1,1,1},{-.5f,H-.5f,.5f,1,0,1}};
        // The head layer holds only what was drawn since the last flush (with
        // every element on the wrists it may hold nothing new: not drawn then).
        const bool headContent = uiFrame.hudLayerCleared;
        if (headContent) ok = ok && draw(uiFrame.color,desktop,false);
        // The desktop mirror shows the whole HUD, wrist elements in place.
        if (ok && wristContent) {
            ok = SUCCEEDED(d->SetTexture(0,hudWrist)) && draw(uiFrame.color,desktop,false) &&
                SUCCEEDED(d->SetTexture(0,hudLayer));
        }
        // Both eyes or neither: a panel partly behind one eye is hidden in both
        // (one eye only showed the HUD to the other eye during fast turns).
        HudVertex quad[2][4]{};
        bool visible = true;
        for (unsigned i=0;i<2 && visible;++i) {
            // Panel-local corners (top of the canvas is +Y) through this eye's projection.
            const Matrix34 panel = matrix(uiFrame.panel[i]);
            const Matrix44 &eye = uiFrame.projection[i];
            const float hw = uiFrame.request.uiWidth*.5f, hh = uiFrame.request.uiHeight*.5f;
            const float corner[4][4]{{-hw,hh,0,0},{hw,hh,1,0},{hw,-hh,1,1},{-hw,-hh,0,1}};
            HudVertex *v = quad[i];
            for (unsigned k=0;k<4 && visible;++k) {
                double local[3]{};
                for (unsigned r=0;r<3;++r)
                    local[r] = double(panel.m[4*r])*corner[k][0] + double(panel.m[4*r+1])*corner[k][1] + panel.m[4*r+3];
                double clip[4]{};
                for (unsigned r=0;r<4;++r)
                    clip[r] = double(eye.m[4*r])*local[0] + double(eye.m[4*r+1])*local[1] +
                              double(eye.m[4*r+2])*local[2] + eye.m[4*r+3];
                // A panel partly behind this eye is not drawn there (as the per-draw path).
                const double margin = std::max(1.,std::abs(clip[3]))*1e-5;
                if (!(clip[3] > margin) || !(clip[2] > margin) || !(clip[3]-clip[2] > margin)) { visible = false; break; }
                v[k] = {float((clip[0]/clip[3]+1)*.5*W), float((1-clip[1]/clip[3])*.5*H), .5f,
                        float(1/clip[3]), corner[k][2], corner[k][3]};
            }
        }
        for (unsigned i=0;i<2 && ok && visible && headContent;++i)
            ok = draw(uiFrame.target[i],quad[i],true);
        // Wrist watch faces: each wrist's elements stacked (in their screen order)
        // on the back of that wrist, turned with the controller.
        if (ok && wristContent) ok = SUCCEEDED(d->SetTexture(0,hudWrist));
        for (unsigned side=0; ok && wristContent && side<2; ++side) {
            const auto &input = uiFrame.request.input;
            if (!input.gripValid[side] || !finite(input.grip[side])) continue;
            unsigned order[hud_wrist::MaxElements], count = 0;
            for (unsigned e=0;e<hud_wrist::MaxElements;++e) {
                const auto &r = uiFrame.wristRects[e];
                if (r.used && r.side == int(side) && r.x1 > r.x0 && r.y1 > r.y0) order[count++] = e;
            }
            if (!count) continue;
            // Elements whose rectangles overlap (health beside the left hand's ammo)
            // are one piece: separately each showed a slice of the other.
            NativeUiFrame::WristRect pieces[hud_wrist::MaxElements];
            unsigned pieceCount = 0;
            for (unsigned k=0;k<count;++k) pieces[pieceCount++] = uiFrame.wristRects[order[k]];
            for (bool merged=true; merged;) {
                merged = false;
                for (unsigned i=0;i<pieceCount && !merged;++i)
                    for (unsigned j=i+1;j<pieceCount && !merged;++j) {
                        auto &p = pieces[i]; const auto &q = pieces[j];
                        const float pad = 6;
                        if (p.x0-pad < q.x1 && q.x0-pad < p.x1 && p.y0-pad < q.y1 && q.y0-pad < p.y1) {
                            p.x0 = std::min(p.x0,q.x0); p.y0 = std::min(p.y0,q.y0);
                            p.x1 = std::max(p.x1,q.x1); p.y1 = std::max(p.y1,q.y1);
                            pieces[j] = pieces[--pieceCount];
                            merged = true;
                        }
                    }
            }
            count = pieceCount;
            std::sort(pieces,pieces+count,[](const auto &a,const auto &b) { return a.y0 < b.y0; });
            // Metres per canvas pixel; pieces centred, top to bottom, with a gap.
            const float scale = hud_wrist::scale()/W, gap = .006f;
            float height = gap*float(count-1), width = 0;
            for (unsigned k=0;k<count;++k) {
                const auto &r = pieces[k];
                height += (std::min(r.y1,H)-std::max(r.y0,0.f))*scale;
                width = std::max(width,(std::min(r.x1,W)-std::max(r.x0,0.f))*scale);
            }
            // Placed as EDuke32 VR places its wrist HUD: on top of the wrist behind the
            // controller (aim pose: 3.5 cm up, 11 cm back toward the forearm), read like a
            // watch with the arm across the chest: text along the forearm, its top edge
            // away from you, the face tilted 35 degrees toward the eyes (mirrored per hand;
            // panel axes in the aim frame: left X=-Z Y=-X, right X=+Z Y=+X, normal +Y).
            if (!input.handValid[side] || !finite(input.hand[side])) continue;
            const Pose aim{normalize(input.hand[side].q),input.hand[side].p};
            const Quat face = side==0 ? Quat{-.5f,.5f,.5f,.5f} : Quat{-.5f,-.5f,-.5f,.5f};
            const float half = Pi*35.f/360.f;
            const Pose watch{normalize(multiply(multiply(aim.q,face),Quat{std::sin(half),0,0,std::cos(half)})),
                             aim.p + rotate(aim.q,Vec3{0,.035f,.11f})};
            const auto project = [&](const float (&corner)[4][4],HudVertex (&out)[2][4]) {
                for (unsigned i=0;i<2;++i) {
                    const Matrix34 panel = matrix(compose(inverse(uiFrame.request.eye[i]),watch));
                    const Matrix44 &eye = uiFrame.projection[i];
                    for (unsigned c=0;c<4;++c) {
                        double local[3]{};
                        for (unsigned row=0;row<3;++row)
                            local[row] = double(panel.m[4*row])*corner[c][0] + double(panel.m[4*row+1])*corner[c][1] + panel.m[4*row+3];
                        double clip[4]{};
                        for (unsigned row=0;row<4;++row)
                            clip[row] = double(eye.m[4*row])*local[0] + double(eye.m[4*row+1])*local[1] +
                                        double(eye.m[4*row+2])*local[2] + eye.m[4*row+3];
                        const double margin = std::max(1.,std::abs(clip[3]))*1e-5;
                        if (!(clip[3] > margin) || !(clip[2] > margin) || !(clip[3]-clip[2] > margin)) return false;
                        out[i][c] = {float((clip[0]/clip[3]+1)*.5*W), float((1-clip[1]/clip[3])*.5*H), .5f,
                                     float(1/clip[3]), corner[c][2], corner[c][3]};
                    }
                }
                return true;
            };
            float top = height*.5f;
            for (unsigned k=0;k<count && ok;++k) {
                const auto &r = pieces[k];
                const float x0 = std::max(r.x0,0.f), x1 = std::min(r.x1,W), y0 = std::max(r.y0,0.f), y1 = std::min(r.y1,H);
                if (!(x1 > x0) || !(y1 > y0)) continue;
                const float hw = (x1-x0)*scale*.5f, ph = (y1-y0)*scale;
                const float corner[4][4]{{-hw,top,x0/W,y0/H},{hw,top,x1/W,y0/H},{hw,top-ph,x1/W,y1/H},{-hw,top-ph,x0/W,y1/H}};
                top -= ph+gap;
                HudVertex piece[2][4]{};
                const bool shown = project(corner,piece);
                for (unsigned i=0;i<2 && ok && shown;++i) ok = draw(uiFrame.target[i],piece[i],true);
            }
        }
    }
    bool restored = bound && SUCCEEDED(originalSetRT(d,0,bound));
    restored = saved && SUCCEEDED(saved->Apply()) && restored;
    uiBypass = false;
    if (bound) bound->Release();
    perf.hudFlushCpu += PerfClock::seconds(start,PerfClock::now());
    if (!ok || !restored) {
        hudLayerDisabled = true;
        hudLayerDisabledGeneration = resourceGeneration.load();
        log("HUD layer composite failed (ok=%u restored=%u); HUD stays per eye", unsigned(ok), unsigned(restored));
        nativeUiFault("hud-layer-composite");
        return false;
    }
    uiFrame.hudLayerCleared = false; // The next layer draw starts from a clear layer.
    uiFrame.hudWristCleared = false;
    for (auto &r : uiFrame.wristRects) r = {};
    return true;
}
// One admitted HUD draw into the layer. Captured state (uiDraw) restores the
// render target, viewport, scissor and layer states in cleanupUiDraw.
template<class Call> static bool hudLayerDraw(IDirect3DDevice9 *d,Call call,DWORD alphaSource,DWORD alphaDestination,
                                              HRESULT &result) noexcept {
    for (unsigned i=0;i<std::size(hudLayerStates);++i)
        if (FAILED(d->GetRenderState(hudLayerStates[i],&uiDraw.layerState[i]))) return false;
    uiBypass = true;
    uiDraw.changed = true; uiDraw.layered = true;
    // A wrist element's draw goes to the wrist layer, and its rectangle (its 2D
    // vertices through this draw's projection) to its wrist's watch face.
    const int element = hud_wrist::currentIndex();
    const bool wrist = hudWrist && hud_wrist::wrists() && hud_wrist::currentSide() != hud_wrist::Head &&
        element >= 0 && element < int(hud_wrist::MaxElements);
    bool ok = SUCCEEDED(originalSetRT(d,0,wrist ? hudWristSurface : hudLayerSurface));
    if (ok && wrist && !uiFrame.hudWristCleared) {
        ok = SUCCEEDED(originalClear(d,0,nullptr,D3DCLEAR_TARGET,0,1,0));
        uiFrame.hudWristCleared = ok;
    }
    if (ok && !wrist && !uiFrame.hudLayerCleared) {
        ok = SUCCEEDED(originalClear(d,0,nullptr,D3DCLEAR_TARGET,0,1,0));
        uiFrame.hudLayerCleared = ok;
    }
    if (ok && wrist) {
        float lo[3], hi[3];
        if (hud_wrist::takeBounds(lo,hi)) {
            const auto &c = uiDraw.constants.m; // c1..c4: clip = (dot(c1,p), dot(c2,p), dot(c3,p), dot(c4,p))
            const auto &v = uiDraw.viewport;
            auto &r = uiFrame.wristRects[element];
            for (unsigned k=0;k<4;++k) {
                const float p[4]{k&1 ? hi[0] : lo[0], k&2 ? hi[1] : lo[1], lo[2], 1};
                float clip[4]{};
                for (unsigned row=0;row<4;++row)
                    clip[row] = c[4*row]*p[0]+c[4*row+1]*p[1]+c[4*row+2]*p[2]+c[4*row+3]*p[3];
                if (!(std::abs(clip[3]) > 1e-6f)) continue;
                const float x = v.X + (clip[0]/clip[3]+1)*.5f*v.Width, y = v.Y + (1-clip[1]/clip[3])*.5f*v.Height;
                if (!std::isfinite(x) || !std::isfinite(y)) continue;
                if (!r.used) { r.x0 = r.x1 = x; r.y0 = r.y1 = y; r.used = true; }
                else { r.x0 = std::min(r.x0,x); r.x1 = std::max(r.x1,x); r.y0 = std::min(r.y0,y); r.y1 = std::max(r.y1,y); }
            }
            r.side = hud_wrist::currentSide();
        }
    }
    ok = ok && SUCCEEDED(d->SetViewport(&uiDraw.viewport)) && SUCCEEDED(d->SetScissorRect(&uiDraw.scissor));
    {
        // Lab (SS2VR_LAB_HUD_TRACE=1): each HUD element's draw rectangle state.
        static const bool trace = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_HUD_TRACE", v, 2) == 1 && v[0] == L'1'; }();
        static unsigned traced = 0;
        if (trace && traced < 400) {
            ++traced;
            const auto &v = uiDraw.viewport; const auto &r = uiDraw.scissor;
            if (wrist && uiFrame.wristRects[element].used)
                log("Lab HUD wrist rect element=%s %.0f,%.0f-%.0f,%.0f", hud_wrist::currentElement(),
                    uiFrame.wristRects[element].x0, uiFrame.wristRects[element].y0, uiFrame.wristRects[element].x1,
                    uiFrame.wristRects[element].y1);
            log("Lab HUD draw element=%s side=%d viewport=%lu,%lu %lux%lu scissor=%ld,%ld-%ld,%ld scissorOn=%lu c1=%.4f,%.4f,%.4f,%.4f c2=%.4f,%.4f,%.4f,%.4f",
                hud_wrist::currentElement() ? hud_wrist::currentElement() : "-", hud_wrist::currentSide(), v.X, v.Y, v.Width, v.Height,
                r.left, r.top, r.right, r.bottom, uiDraw.layerState[0], uiDraw.constants.m[0], uiDraw.constants.m[1], uiDraw.constants.m[2],
                uiDraw.constants.m[3], uiDraw.constants.m[4], uiDraw.constants.m[5], uiDraw.constants.m[6], uiDraw.constants.m[7]);
        }
    }
    ok = ok && SUCCEEDED(d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,TRUE)) &&
        SUCCEEDED(d->SetRenderState(D3DRS_SRCBLENDALPHA,alphaSource)) &&
        SUCCEEDED(d->SetRenderState(D3DRS_DESTBLENDALPHA,alphaDestination)) &&
        SUCCEEDED(d->SetRenderState(D3DRS_BLENDOPALPHA,D3DBLENDOP_ADD));
    // Coverage must be written even if the engine masks alpha for the backbuffer.
    const DWORD colorWrite = uiDraw.layerState[4];
    ok = ok && SUCCEEDED(d->SetRenderState(D3DRS_COLORWRITEENABLE,
        colorWrite & 7 ? colorWrite | D3DCOLORWRITEENABLE_ALPHA : colorWrite));
    uiBypass = false;
    if (!ok) return false;
    result = call();
    if (SUCCEEDED(result)) { uiFrame.hudLayerDirty = true; ++perf.hudLayerDraws; }
    return true;
}
template<class Call> static HRESULT uiDrawCall(IDirect3DDevice9 *d,D3DPRIMITIVETYPE topology,Call call) noexcept {
    const auto generation = graphicsResourceGeneration();
    if (!scopeGpuForwardingAllowed()) return D3DERR_INVALIDCALL;
    if (!observingUi(d)) return call();
    if (uiDraw.busy) { nativeUiFault(); return call(); }
    uiDraw.busy = true;
    HRESULT result = D3DERR_INVALIDCALL;
    const LARGE_INTEGER hudStart = PerfClock::now();
    struct HudTimer { LARGE_INTEGER start; ~HudTimer() { perf.hudCpu += PerfClock::seconds(start, PerfClock::now()); ++perf.hudDraws; } } hudTimer{hudStart};
    const bool contained = withNativeFinally([&] {
        DWORD alphaSource = 0, alphaDestination = 0;
        if (hudLayerReady(d) && hudLayerAlpha(d,alphaSource,alphaDestination)) {
            // Capture first: the layer draw replaces the desktop draw (composited later).
            if (!captureUiDraw(d)) {
                result = call(); // Offscreen or unadmitted: native, exactly as before.
                if (generation != graphicsResourceGeneration()) return;
                if (FAILED(result)) { nativeUiFault("desktop-draw-hresult"); return; }
                if (!uiDraw.color || sameSurface(uiDraw.color,uiFrame.color)) nativeUiFault(uiDraw.captureStage);
                return;
            }
            if (!nativeUiTriangleTopology(topology,uiDraw.fill)) { result = call(); nativeUiFault("topology-or-fill"); return; }
            if (!hudLayerDraw(d,call,alphaSource,alphaDestination,result)) { nativeUiFault("hud-layer-draw"); return; }
            if (generation != graphicsResourceGeneration()) return;
            if (FAILED(result)) nativeUiFault("desktop-draw-hresult");
            return;
        }
        if (uiFrame.hudLayerDirty) {
            // A per-eye draw onto the backbuffer goes above the layer's earlier content.
            IDirect3DSurface9 *bound = nullptr;
            const bool desktop = SUCCEEDED(d->GetRenderTarget(0,&bound)) && sameSurface(bound,uiFrame.color);
            if (bound) bound->Release();
            if (desktop && !flushHudLayer(d)) return;
        }
        result = call(); // Desktop first, exactly once; preserve its HRESULT/state effects.
        if (generation != graphicsResourceGeneration()) return;
        if (FAILED(result)) { nativeUiFault("desktop-draw-hresult"); return; }
        if (uiFrame.ui) ++perf.hudFallbackDraws;
        if (!captureUiDraw(d)) {
            // Offscreen drawing stays native; only eventual desktop composition is adapted.
            if (!uiDraw.color || sameSurface(uiDraw.color,uiFrame.color)) nativeUiFault(uiDraw.captureStage);
            return;
        }
        if (!nativeUiTriangleTopology(topology,uiDraw.fill)) { nativeUiFault("topology-or-fill"); return; }
        NativeUiProjection projected[2];
        for (unsigned i=0;i<2;++i) {
            if (uiFrame.fade) {
                // A fade fills its whole viewport; in each eye it covers the whole
                // view (full viewport, no scissor, no depth below). A fade drawn in
                // a smaller viewport or with a scissor (the game's own drawport)
                // dropped every pair it touched, which showed as flicker.
                const auto &v=uiDraw.viewport;
                if (i==0 && (v.X || v.Y || v.Width!=uiFrame.request.width || v.Height!=uiFrame.request.height ||
                             uiDraw.state[2])) {
                    static unsigned logged=0;
                    if (logged++<8)
                        log("Native UI fade viewport=%lu,%lu %lux%lu scissor=%lu eye=%ux%u drawn full-field",
                            v.X,v.Y,v.Width,v.Height,uiDraw.state[2],uiFrame.request.width,uiFrame.request.height);
                }
                projected[i].constants = uiDraw.constants;
            } else {
                const auto &v=uiDraw.viewport;
                const NativeUiViewport vp{v.X,v.Y,v.Width,v.Height,v.MinZ,v.MaxZ};
                const auto &r=uiDraw.scissor;
                if (!nativeUiProjection(uiDraw.constants,uiFrame.projection[i],uiFrame.panel[i],
                    uiFrame.request.uiWidth,uiFrame.request.uiHeight,uiFrame.request.width,
                    uiFrame.request.height,vp,uiDraw.state[2]!=0,{r.left,r.top,r.right,r.bottom},projected[i])) {
                    nativeUiFault("ui-projection"); return;
                }
            }
        }
        uiBypass = true;
        uiDraw.changed = true; // Any partial mutation must attempt every restore.
        for (unsigned i=0;i<2;++i) {
            if (!uiCurrent()) { nativeUiFault(); break; }
            // A panel behind either eye is drawn in neither (one-eyed HUD otherwise).
            if (projected[i].empty || uiFrame.panelHidden[0] || uiFrame.panelHidden[1]) continue;
            bool ok = SUCCEEDED(originalSetRT(d,0,uiFrame.target[i]));
            const D3DVIEWPORT9 full{0,0,uiFrame.request.width,uiFrame.request.height,0,1};
            ok = SUCCEEDED(d->SetViewport(&full)) && ok;
            ok = SUCCEEDED(d->SetRenderState(D3DRS_ZENABLE,D3DZB_FALSE)) && ok;
            ok = SUCCEEDED(d->SetRenderState(D3DRS_ZWRITEENABLE,FALSE)) && ok;
            ok = SUCCEEDED(d->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE)) && ok;
            ok = SUCCEEDED(d->SetVertexShaderConstantF(1,projected[i].constants.m,4)) && ok;
            if (!uiFrame.fade)
                for (unsigned j=0;j<6;++j) ok = SUCCEEDED(d->SetClipPlane(j,projected[i].planes[j].data())) && ok;
            ok = SUCCEEDED(d->SetRenderState(D3DRS_CLIPPLANEENABLE,uiFrame.fade ? 0 : 0x3f)) && ok;
            if (!ok || FAILED(call()) || generation != graphicsResourceGeneration()) { nativeUiFault("ui-eye-draw"); break; }
        }
        uiBypass = false;
    }, [&](bool aborted) noexcept {
        const bool retired = generation != graphicsResourceGeneration();
        if (retired) uiDraw.changed = false;
        cleanupUiDraw(aborted && !retired);
    });
    if (!contained) nativeUiFault();
    return result;
}
struct LabWorldDraws {
    IDirect3DDevice9 *device=nullptr;
    uint64_t calls=0,primitives=0,readFailures=0;
    uint64_t stateCalls[16]{},statePrimitives[16]{};
    uint64_t fullRangeCalls[16]{},worldRangeCalls[16]{},otherRangeCalls[16]{};
};
static thread_local LabWorldDraws labWorldDraws{};
static void countDraw() {
    if (uiBypass) ++perf.hudDuplicates;
    else if (activeEye >= 0) ++perf.eyeDraws;
    else ++perf.desktopDraws;
}
// Lab only (SS2VR_LAB_SHADER_CENSUS=1): left-eye draws by bound vertex/pixel
// shader version (0 = fixed function), counted per perf window.
static struct ShaderCensus {
    IDirect3DVertexShader9 *seenVertex[256]{}; DWORD vertexVersion[256]{}; unsigned vertexCount = 0;
    IDirect3DPixelShader9 *seenPixel[256]{}; DWORD pixelVersion[256]{}; unsigned pixelCount = 0;
    unsigned draws[2][8]{}; // [vs/ps][version bucket: 0 ff, 1 x.1/1.x, 2 2.0, 3 2.x, 4 3.0, 5 other]
} census;
static unsigned censusBucket(DWORD version) noexcept {
    if (!version) return 0;
    const DWORD major = (version >> 8) & 0xff, minor = version & 0xff;
    if (major == 1) return 1;
    if (major == 2) return minor ? 3 : 2;
    if (major == 3) return 4;
    return 5;
}
// Private lab dumps (proprietary engine programs: never committed), one file per program.
template<class Shader> static void censusDump(Shader *shader, const char *kind, unsigned index) {
    UINT bytes = 0;
    if (FAILED(shader->GetFunction(nullptr, &bytes)) || !bytes) return;
    std::vector<uint8_t> code(bytes);
    if (FAILED(shader->GetFunction(code.data(), &bytes))) return;
    std::wstring directory;
    if (!moduleDirectory(nullptr, directory)) return;
    directory += L"SS2VR\\shaders";
    CreateDirectoryW(directory.c_str(), nullptr);
    uint32_t hash = 2166136261u; // FNV-1a over the bytecode: stable names across runs.
    for (uint8_t byte : code) hash = (hash ^ byte) * 16777619u;
    wchar_t name[64]; swprintf(name, 64, L"\\%hs_%08x.bin", kind, hash);
    (void)index;
    if (FILE *file = _wfopen((directory + name).c_str(), L"wb")) { fwrite(code.data(), 1, bytes, file); fclose(file); }
}
// Lab census of draw kinds and vertex layouts in the left eye (single-pass planning).
static struct DrawCensus { unsigned plain = 0, indexed = 0, stream15 = 0, maxStream = 0, instanced = 0; } drawCensus;
static void declarationCensus(IDirect3DDevice9 *d) {
    IDirect3DVertexDeclaration9 *declaration = nullptr;
    if (FAILED(d->GetVertexDeclaration(&declaration)) || !declaration) return;
    D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH + 1]{};
    UINT count = MAXD3DDECLLENGTH + 1;
    if (SUCCEEDED(declaration->GetDeclaration(elements, &count)))
        for (UINT e = 0; e < count && elements[e].Stream != 0xff; ++e) {
            drawCensus.maxStream = std::max<unsigned>(drawCensus.maxStream, elements[e].Stream);
            if (elements[e].Stream == 15 || elements[e].Usage == D3DDECLUSAGE_BLENDINDICES) ++drawCensus.stream15;
        }
    declaration->Release();
    UINT frequency = 1;
    if (SUCCEEDED(d->GetStreamSourceFreq(0, &frequency)) && frequency != 1) ++drawCensus.instanced;
}
static void shaderCensus(IDirect3DDevice9 *d) {
    static const bool enabled = [] { wchar_t value[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SHADER_CENSUS", value, 2) == 1 && value[0] == L'1'; }();
    if (!enabled || activeEye != 0 || uiBypass) return;
    declarationCensus(d);
    IDirect3DVertexShader9 *vs = nullptr; IDirect3DPixelShader9 *ps = nullptr;
    DWORD vv = 0, pv = 0;
    if (SUCCEEDED(d->GetVertexShader(&vs)) && vs) {
        unsigned i = 0;
        while (i < census.vertexCount && census.seenVertex[i] != vs) ++i;
        if (i == census.vertexCount && i < 256) {
            DWORD words[2]{}; UINT bytes = 0;
            if (SUCCEEDED(vs->GetFunction(nullptr, &bytes)) && bytes >= 4) {
                std::vector<DWORD> code(bytes / 4); bytes = UINT(code.size() * 4);
                if (SUCCEEDED(vs->GetFunction(code.data(), &bytes))) words[0] = code[0];
            }
            census.seenVertex[i] = vs; census.vertexVersion[i] = words[0] & 0xffff; ++census.vertexCount;
            censusDump(vs, "vs", i);
        }
        if (i < census.vertexCount) vv = census.vertexVersion[i];
        vs->Release();
    }
    if (SUCCEEDED(d->GetPixelShader(&ps)) && ps) {
        unsigned i = 0;
        while (i < census.pixelCount && census.seenPixel[i] != ps) ++i;
        if (i == census.pixelCount && i < 256) {
            UINT bytes = 0; DWORD first = 0;
            if (SUCCEEDED(ps->GetFunction(nullptr, &bytes)) && bytes >= 4) {
                std::vector<DWORD> code(bytes / 4); bytes = UINT(code.size() * 4);
                if (SUCCEEDED(ps->GetFunction(code.data(), &bytes))) first = code[0];
            }
            census.seenPixel[i] = ps; census.pixelVersion[i] = first & 0xffff; ++census.pixelCount;
            censusDump(ps, "ps", i);
        }
        if (i < census.pixelCount) pv = census.pixelVersion[i];
        ps->Release();
    }
    ++census.draws[0][censusBucket(vv)];
    ++census.draws[1][censusBucket(pv)];
}
static void traceWorldDraw(IDirect3DDevice9 *d,UINT count) {
    shaderCensus(d);
    auto &sample=labWorldDraws;
    if(sample.device!=d) return;
    ++sample.calls;sample.primitives+=count;
    DWORD z=0,write=0,alpha=0,blend=0;
    if(FAILED(d->GetRenderState(D3DRS_ZENABLE,&z)) || FAILED(d->GetRenderState(D3DRS_ZWRITEENABLE,&write)) ||
        FAILED(d->GetRenderState(D3DRS_ALPHATESTENABLE,&alpha)) || FAILED(d->GetRenderState(D3DRS_ALPHABLENDENABLE,&blend))) {
        ++sample.readFailures;return;
    }
    const unsigned key=(z?1:0)|(write?2:0)|(alpha?4:0)|(blend?8:0);
    ++sample.stateCalls[key];sample.statePrimitives[key]+=count;
    D3DVIEWPORT9 vp{};
    if(FAILED(d->GetViewport(&vp))) { ++sample.readFailures;return; }
    if(vp.MinZ==0 && vp.MaxZ==1) ++sample.fullRangeCalls[key];
    else if(vp.MinZ==0 && vp.MaxZ==0.89999997615814208984375f) ++sample.worldRangeCalls[key];
    else ++sample.otherRangeCalls[key];
}
static HRESULT WINAPI draw(IDirect3DDevice9 *d,D3DPRIMITIVETYPE type,UINT start,UINT count) {
    scopeGpuOutput(d);
    return uiDrawCall(d,type,[&]{
        if(!scopeGpuForwardingAllowed()) return D3DERR_INVALIDCALL;
        if(activeEye==0 && !uiBypass) ++drawCensus.plain;
        traceWorldDraw(d,count);
        countDraw();
        replay::recordDraw(type,start,count);
        if(single_pass::drawing(d)) return single_pass::draw(d,type,start,count);
        return originalDraw(d,type,start,count);
    });
}
static HRESULT forwardIndexed(IDirect3DDevice9 *d,D3DPRIMITIVETYPE type,INT base,UINT minimum,
                               UINT vertices,UINT start,UINT count) {
    return uiDrawCall(d,type,[&]{
        if(!scopeGpuForwardingAllowed()) return D3DERR_INVALIDCALL;
        if(activeEye==0 && !uiBypass) ++drawCensus.indexed;
        traceWorldDraw(d,count);
        countDraw();
        replay::recordDrawIndexed(type,base,minimum,vertices,start,count);
        if(single_pass::drawing(d)) return single_pass::drawIndexed(d,type,base,minimum,vertices,start,count);
        return originalDrawIndexed(d,type,base,minimum,vertices,start,count);
    });
}
static HRESULT WINAPI drawIndexed(IDirect3DDevice9 *d,D3DPRIMITIVETYPE type,INT base,UINT minimum,
                                  UINT vertices,UINT start,UINT count) {
#ifdef _MSC_VER
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
#else
    const auto caller=reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif
    return scopeGpuDraw(d,type,base,minimum,vertices,start,count,caller,forwardIndexed);
}
// Complete output domain for IDirect3DDevice9 color writes: only the two above
// draw families are duplicated. Other desktop writes reject the entire pair.
static thread_local IDirect3DSurface9 *uiObservedTarget = nullptr, *uiObservedExtra = nullptr;
static thread_local IDirect3DSwapChain9 *uiObservedChain = nullptr;
static thread_local bool uiObserverBusy = false;
template<class Call> static HRESULT observeUiOutput(IDirect3DDevice9 *d, Call call,
                                                    IDirect3DSurface9 *target, bool current) noexcept {
    const auto generation = graphicsResourceGeneration();
    if (!scopeGpuForwardingAllowed()) return D3DERR_INVALIDCALL;
    if (!observingUi(d)) return call();
    if (uiObserverBusy) { nativeUiFault(); return call(); }
    uiObserverBusy = true;
    HRESULT result = D3DERR_INVALIDCALL;
    withNativeFinally([&] {
        result = call();
        if (generation != graphicsResourceGeneration()) return;
        if (current) {
            if (!singleUiTarget(d,uiObservedExtra)) { nativeUiFault(); return; }
            if (FAILED(d->GetRenderTarget(0,&uiObservedTarget))) { nativeUiFault(); return; }
            target = uiObservedTarget;
        }
        if (!uiCurrent() || !target || sameSurface(target,uiFrame.color) ||
            !uiOffscreen(target,uiObservedChain)) nativeUiFault();
    },[&](bool aborted) noexcept {
        if (aborted && generation == graphicsResourceGeneration()) { nativeUiFault(); uiHalted = true; }
        if (uiObservedChain) uiObservedChain->Release();
        uiObservedChain=nullptr;
        if (uiObservedExtra) uiObservedExtra->Release();
        uiObservedExtra=nullptr;
        if (uiObservedTarget) uiObservedTarget->Release();
        uiObservedTarget = nullptr;
        uiObserverBusy = false;
    });
    return result;
}
using DrawIndexedUp = HRESULT(WINAPI *)(IDirect3DDevice9 *,D3DPRIMITIVETYPE,UINT,UINT,UINT,const void *,D3DFORMAT,const void *,UINT);
using DrawRect = HRESULT(WINAPI *)(IDirect3DDevice9 *,UINT,const float *,const D3DRECTPATCH_INFO *);
using DrawTri = HRESULT(WINAPI *)(IDirect3DDevice9 *,UINT,const float *,const D3DTRIPATCH_INFO *);
using Update = HRESULT(WINAPI *)(IDirect3DDevice9 *,IDirect3DSurface9 *,const RECT *,IDirect3DSurface9 *,const POINT *);
static DrawIndexedUp originalDrawIndexedUp = nullptr;
static DrawRect originalDrawRect = nullptr;
static DrawTri originalDrawTri = nullptr;
static Update originalUpdate = nullptr;
static HRESULT WINAPI clear(IDirect3DDevice9 *d,DWORD count,const D3DRECT *rects,DWORD flags,D3DCOLOR color,float z,DWORD stencil) {
    scopeGpuOutput(d);
    const auto call=[&]{
        replay::recordClear(count,rects,flags,color,z,stencil);
        if(single_pass::drawing(d)) return single_pass::clear(d,count,rects,flags,color,z,stencil);
        return originalClear(d,count,rects,flags,color,z,stencil);
    };
    return flags&D3DCLEAR_TARGET ? observeUiOutput(d,call,nullptr,true) : call();
}
static HRESULT WINAPI drawUp(IDirect3DDevice9 *d,D3DPRIMITIVETYPE type,UINT count,const void *vertices,UINT stride) {
    scopeGpuOutput(d);
    replay::invalidate("user-pointer draw");
    return observeUiOutput(d,[&]{ traceWorldDraw(d,count);
        if(single_pass::drawing(d)) return single_pass::drawUp(d,type,count,vertices,stride);
        return originalDrawUp(d,type,count,vertices,stride); },nullptr,true);
}
static HRESULT WINAPI drawIndexedUp(IDirect3DDevice9 *d,D3DPRIMITIVETYPE type,UINT minimum,UINT vertices,UINT count,
                                    const void *indices,D3DFORMAT format,const void *data,UINT stride) {
    scopeGpuOutput(d);
    replay::invalidate("user-pointer draw");
    return observeUiOutput(d,[&]{ traceWorldDraw(d,count);
        if(single_pass::drawing(d)) return single_pass::drawIndexedUp(d,type,minimum,vertices,count,indices,format,data,stride);
        return originalDrawIndexedUp(d,type,minimum,vertices,count,indices,format,data,stride); },nullptr,true);
}
static HRESULT WINAPI drawRect(IDirect3DDevice9 *d,UINT handle,const float *segments,const D3DRECTPATCH_INFO *info) {
    scopeGpuOutput(d);
    return observeUiOutput(d,[&]{ return originalDrawRect(d,handle,segments,info); },nullptr,true);
}
static HRESULT WINAPI drawTri(IDirect3DDevice9 *d,UINT handle,const float *segments,const D3DTRIPATCH_INFO *info) {
    scopeGpuOutput(d);
    return observeUiOutput(d,[&]{ return originalDrawTri(d,handle,segments,info); },nullptr,true);
}
static HRESULT WINAPI update(IDirect3DDevice9 *d,IDirect3DSurface9 *source,const RECT *rect,IDirect3DSurface9 *target,const POINT *point) {
    scopeGpuOutput(d);
    return observeUiOutput(d,[&]{ return originalUpdate(d,source,rect,target,point); },target,false);
}
static IDirect3DSurface9 *eyeSurface(IDirect3DSurface9 *surface) {
    if ((activeEye >= 0 || scopeScratch)) {
        if (sameSurface(surface, desktopTarget))
            return uiFrame.slot >= 0 ? uiFrame.target[scopeScratch ? 0 : activeEye] : eye[scopeScratch ? 0 : activeEye];
        if (sameSurface(surface, desktopDepth))
            return uiFrame.slot >= 0 ? uiFrame.z[scopeScratch ? 0 : activeEye] : depth[scopeScratch ? 0 : activeEye];
    }
    return surface;
}
static HRESULT WINAPI stretch(IDirect3DDevice9 *d, IDirect3DSurface9 *source, const RECT *sourceRect,
                              IDirect3DSurface9 *target, const RECT *targetRect,
                              D3DTEXTUREFILTERTYPE filter) {
    scopeGpuOutput(d);
    HRESULT result =
        observeUiOutput(d, [&] {
            replay::recordStretch(eyeSurface(source), sourceRect, eyeSurface(target), targetRect, filter);
            return originalStretch(d, eyeSurface(source), sourceRect, eyeSurface(target), targetRect, filter); }, target, false);
    if ((activeEye >= 0 || scopeScratch) && FAILED(result))
        eyeInvalid = true;
    return result;
}
static HRESULT WINAPI readTarget(IDirect3DDevice9 *d, IDirect3DSurface9 *source, IDirect3DSurface9 *target) {
    scopeGpuOutput(d);
    HRESULT result = originalReadTarget(d, eyeSurface(source), target);
    if ((activeEye >= 0 || scopeScratch) && FAILED(result))
        eyeInvalid = true;
    return result;
}
static HRESULT WINAPI fill(IDirect3DDevice9 *d, IDirect3DSurface9 *target, const RECT *rect, D3DCOLOR color) {
    scopeGpuOutput(d);
    HRESULT result = observeUiOutput(d, [&] { replay::recordFill(eyeSurface(target), rect, color); return originalFill(d, eyeSurface(target), rect, color); }, target, false);
    if ((activeEye >= 0 || scopeScratch) && FAILED(result))
        eyeInvalid = true;
    return result;
}
static HRESULT WINAPI setDepth(IDirect3DDevice9 *d, IDirect3DSurface9 *target) {
    scopeGpuOutput(d);
    if ((activeEye >= 0 || scopeScratch) && sameSurface(target, desktopDepth))
        target = uiFrame.slot >= 0 ? uiFrame.z[scopeScratch ? 0 : activeEye] : depth[scopeScratch ? 0 : activeEye];
    replay::recordDepth(target);
    HRESULT result = originalSetDepth(d, target);
    if ((activeEye >= 0 || scopeScratch) && FAILED(result))
        eyeInvalid = true;
    return result;
}
static HRESULT WINAPI setRT(IDirect3DDevice9 *d, DWORD index, IDirect3DSurface9 *target) {
    scopeGpuOutput(d);
    if ((activeEye >= 0 || scopeScratch) && index > 0 && target)
        eyeInvalid = true;
    if ((activeEye >= 0 || scopeScratch) && index == 0 && sameSurface(target, desktopTarget))
        target = uiFrame.slot >= 0 ? uiFrame.target[scopeScratch ? 0 : activeEye] : eye[scopeScratch ? 0 : activeEye];
    replay::recordRenderTarget(index, target);
    single_pass::beforeRenderTarget(d, index);
    HRESULT result = originalSetRT(d, index, target);
    if (SUCCEEDED(result)) single_pass::afterRenderTarget(d, index, target);
    if ((activeEye >= 0 || scopeScratch) && FAILED(result))
        eyeInvalid = true;
    if (index == 0 && target && activeEye < 0 && !scopeScratch && SUCCEEDED(result)) {
        static std::atomic<bool> first{false}, firstReady{false};
        const bool ready = hooksReady.load(std::memory_order_acquire);
        if (!first.load(std::memory_order_relaxed) && !first.exchange(true, std::memory_order_relaxed))
            logStartup("first ordinary RT0", d, ready);
        if (ready && !firstReady.load(std::memory_order_relaxed) &&
            !firstReady.exchange(true, std::memory_order_relaxed))
            logStartup("first ordinary RT0 after hooksReady", d, ready);
    }
    return result;
}
static void releaseSharedImages() {
    for (unsigned slot = 0; slot != 2; ++slot) {
        for (unsigned i = 0; i != 2; ++i) {
            if (sharedSurface[slot][i]) sharedSurface[slot][i]->Release();
            if (sharedTexture[slot][i]) sharedTexture[slot][i]->Release();
            sharedSurface[slot][i] = nullptr;
            sharedTexture[slot][i] = nullptr;
        }
        if (sharedFence[slot]) sharedFence[slot]->Release();
        sharedFence[slot] = nullptr;
        sharedPending[slot] = false;
    }
    if (sharedGeneration && channel.shared) {
        Lock l(channel, 50);
        if (l) channel.shared->images = {};
    }
    sharedGeneration = 0;
}
// Scope lens resources live across frames (a full-size render target per zoomed
// frame was allocation churn); released with the eye targets on loss/Reset.
static IDirect3DTexture9 *scopeImageCache[2]{};
static IDirect3DPixelShader9 *scopeShaderCache = nullptr;
static IDirect3DDevice9 *scopeCacheDevice = nullptr;
static void releaseScopeCache() noexcept {
    for (auto *&texture : scopeImageCache) { if (texture) texture->Release(); texture = nullptr; }
    if (scopeShaderCache) scopeShaderCache->Release();
    scopeShaderCache = nullptr; scopeCacheDevice = nullptr;
}
static void releaseMenuScaled();
// Foveated single pass: the pass draws into an atlas holding each eye's full
// view at reduced resolution and a full-resolution inset around the gaze; each
// eye image is then the full view stretched over the eye with the inset drawn
// over it (feathered border), so everything after the pass is unchanged.
static IDirect3DTexture9 *foveaTexture = nullptr;
static IDirect3DSurface9 *foveaColor = nullptr, *foveaDepth = nullptr;
static IDirect3DStateBlock9 *foveaState = nullptr;
static UINT atlasWidth = 0, atlasHeight = 0;
static void releaseFovea() {
    for (IUnknown *p : {static_cast<IUnknown *>(foveaColor), static_cast<IUnknown *>(foveaTexture),
                        static_cast<IUnknown *>(foveaDepth), static_cast<IUnknown *>(foveaState)})
        if (p) p->Release();
    foveaColor = foveaDepth = nullptr; foveaTexture = nullptr; foveaState = nullptr;
    atlasWidth = atlasHeight = 0;
}
struct FoveaFrame {
    single_pass::Layout layout;
    PixelRect inset[2];
};
// The layout for a request asking for foveated rendering (foveated == 2).
static bool foveaLayout(const Request &q, UINT width, UINT height, FoveaFrame &f) {
    const UINT lw = q.lowWidth, lh = q.lowHeight, iw = q.insetWidth, ih = q.insetHeight;
    if (q.foveated != 2 || !lw || !lh || !iw || !ih || lw > width || lh > height || iw > width || ih > height) return false;
    auto &l = f.layout;
    l.width = std::max(2 * lw, 2 * iw); l.height = lh + ih; l.count = 4;
    for (unsigned k = 0; k < 2; ++k) {
        l.region[k] = {RECT{LONG(k * lw), 0, LONG((k + 1) * lw), LONG(lh)}, k, 1, 0, 1, 0, true};
        const PixelRect r = insetCrop(q.fov[k], width, height, iw, ih, q.insetCentre[k][0], q.insetCentre[k][1]);
        f.inset[k] = r;
        // Eye NDC of the inset's centre and half extent: the inset's own NDC is (x - c) / e.
        const float cx = (2.f * r.x + r.w) / width - 1.f, ex = float(r.w) / width;
        const float cy = 1.f - (2.f * r.y + r.h) / height, ey = float(r.h) / height;
        l.region[2 + k] = {RECT{LONG(k * iw), LONG(lh), LONG((k + 1) * iw), LONG(lh + ih)}, k, 1.f / ex, -cx / ex, 1.f / ey, -cy / ey, false};
    }
    return true;
}
static bool ensureFovea(IDirect3DDevice9 *d, UINT width, UINT height, D3DFORMAT color, D3DFORMAT depth) {
    if (foveaColor && atlasWidth == width && atlasHeight == height) return true;
    releaseFovea();
    if (FAILED(d->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, color, D3DPOOL_DEFAULT, &foveaTexture, nullptr)) ||
        FAILED(foveaTexture->GetSurfaceLevel(0, &foveaColor)) ||
        FAILED(d->CreateDepthStencilSurface(width, height, depth, D3DMULTISAMPLE_NONE, 0, TRUE, &foveaDepth, nullptr)) ||
        FAILED(d->CreateStateBlock(D3DSBT_ALL, &foveaState))) {
        log("Foveated single pass: %ux%u atlas unavailable", width, height);
        releaseFovea();
        return false;
    }
    atlasWidth = width; atlasHeight = height;
    log("Foveated single pass: atlas %ux%u", width, height);
    return true;
}
// Eye k from the atlas into `target` (width x height).
static bool composeFovea(IDirect3DDevice9 *d, const FoveaFrame &f, unsigned k, IDirect3DSurface9 *target, UINT width, UINT height) {
    const RECT low = f.layout.region[k].rect, inset = f.layout.region[2 + k].rect;
    if (!foveaState || FAILED(foveaState->Capture())) return false;
    struct Vertex { float x, y, z, rhw; D3DCOLOR color; float u, v; };
    const PixelRect &r = f.inset[k];
    const float band = float(std::max(8, r.w / 16));
    const float xs[4]{float(r.x), r.x + band, r.x + r.w - band, float(r.x + r.w)};
    const float ys[4]{float(r.y), r.y + band, r.y + r.h - band, float(r.y + r.h)};
    Vertex v[16];
    for (unsigned j = 0; j < 4; ++j)
        for (unsigned i = 0; i < 4; ++i) {
            const bool inner = (i == 1 || i == 2) && (j == 1 || j == 2);
            v[j * 4 + i] = {xs[i] - .5f, ys[j] - .5f, 0, 1, D3DCOLOR_ARGB(inner ? 255 : 0, 255, 255, 255),
                            (inset.left + xs[i] - r.x) / float(atlasWidth), (inset.top + ys[j] - r.y) / float(atlasHeight)};
        }
    uint16_t indices[54];
    unsigned n = 0;
    for (uint16_t j = 0; j < 3; ++j)
        for (uint16_t i = 0; i < 3; ++i) {
            const uint16_t a = j * 4 + i;
            for (uint16_t e : {a, uint16_t(a + 1), uint16_t(a + 4), uint16_t(a + 1), uint16_t(a + 5), uint16_t(a + 4)}) indices[n++] = e;
        }
    const D3DVIEWPORT9 full{0, 0, width, height, 0, 1};
    bool ok = SUCCEEDED(originalSetRT(d, 0, target)) && SUCCEEDED(d->SetViewport(&full));
    d->SetVertexShader(nullptr); d->SetPixelShader(nullptr);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    d->SetTexture(0, foveaTexture);
    for (DWORD stage = 1; stage < 8; ++stage) d->SetTexture(stage, nullptr);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0); d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE); d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT); d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    const std::pair<D3DRENDERSTATETYPE, DWORD> states[]{
        {D3DRS_ZENABLE, FALSE}, {D3DRS_ZWRITEENABLE, FALSE}, {D3DRS_ALPHATESTENABLE, FALSE}, {D3DRS_ALPHABLENDENABLE, TRUE},
        {D3DRS_SRCBLEND, D3DBLEND_SRCALPHA}, {D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA}, {D3DRS_BLENDOP, D3DBLENDOP_ADD},
        {D3DRS_SEPARATEALPHABLENDENABLE, FALSE}, {D3DRS_CULLMODE, D3DCULL_NONE}, {D3DRS_FOGENABLE, FALSE},
        {D3DRS_LIGHTING, FALSE}, {D3DRS_STENCILENABLE, FALSE}, {D3DRS_SCISSORTESTENABLE, FALSE},
        {D3DRS_COLORWRITEENABLE, 0x7}, {D3DRS_SRGBWRITEENABLE, FALSE}, {D3DRS_CLIPPLANEENABLE, 0},
        {D3DRS_FILLMODE, D3DFILL_SOLID}, {D3DRS_SHADEMODE, D3DSHADE_GOURAUD}};
    for (const auto &[state, value] : states) d->SetRenderState(state, value);
    // The full view, filtered over the eye: texel centres at the region's edges
    // map to the eye's edge pixels, so filtering never reads the next region.
    {
        const float rw = float(low.right - low.left), rh = float(low.bottom - low.top);
        const float sx = (rw - 1) / float(std::max(1u, width - 1)), sy = (rh - 1) / float(std::max(1u, height - 1));
        const float u0 = (low.left + .5f - .5f * sx) / float(atlasWidth), u1 = (low.left + .5f + (width - .5f) * sx) / float(atlasWidth);
        const float v0 = (low.top + .5f - .5f * sy) / float(atlasHeight), v1 = (low.top + .5f + (height - .5f) * sy) / float(atlasHeight);
        const Vertex quad[4]{{-.5f, -.5f, 0, 1, 0xffffffffu, u0, v0}, {width - .5f, -.5f, 0, 1, 0xffffffffu, u1, v0},
                             {width - .5f, height - .5f, 0, 1, 0xffffffffu, u1, v1}, {-.5f, height - .5f, 0, 1, 0xffffffffu, u0, v1}};
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        ok = ok && SUCCEEDED(originalDrawUp(d, D3DPT_TRIANGLEFAN, 2, quad, sizeof(Vertex)));
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT); d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    }
    ok = ok && SUCCEEDED(originalDrawIndexedUp(d, D3DPT_TRIANGLELIST, 0, 16, 18, indices, D3DFMT_INDEX16, v, sizeof(Vertex)));
    foveaState->Apply();
    return ok;
}
static void releaseTargets() {
    ++resourceGeneration;
    env_probe::release();
    releaseScopeCache();
    releaseHudLayer();
    releaseMenuScaled();
    gpuProfile.release();
    releaseSharedImages();
    sharedFailed = false;
    nativeUiFault();
    if (uiQuarantinedReadback) uiQuarantinedReadback->Release();
    uiQuarantinedReadback=nullptr;
    invalidateRenderer();
    if (wideColor) wideColor->Release();
    if (wideDepth) wideDepth->Release();
    wideColor = wideDepth = nullptr;
    releaseFovea();
    for (int i = 0; i < 2; i++) {
        for (auto p : {eye[i], depth[i], readback[i]})
            if (p)
                p->Release();
        eye[i] = depth[i] = readback[i] = nullptr;
        for (unsigned k = 0; k < 2; ++k) {
            if (foveaTarget[i][k]) foveaTarget[i][k]->Release();
            if (foveaRead[i][k]) foveaRead[i][k]->Release();
            foveaTarget[i][k] = foveaRead[i][k] = nullptr;
            foveaWidth[i][k] = foveaHeight[i][k] = 0;
        }
    }
    width = height = 0;
    if (channel.shared) {
        Lock l(channel);
        if (l) {
            channel.shared->rendererReady = 0;
            channel.shared->menu.visible = 0;
            channel.shared->menu.tickMs = 0;
            for (auto &s : channel.shared->slot) {
                if (s.state == SlotState::Requested || s.state == SlotState::Ready)
                    s.state = SlotState::Empty;
                else if (s.state == SlotState::Rendering)
                    s.cancelled = 1;
            }
        }
    }
}
static void retirePresentationOwner() noexcept {
    auto *old = presentationChain;
    presentationChain = nullptr;
    presentationWindow = nullptr;
    if (old) old->Release();
}
void deviceLost() {
    resetQuarantined = true;
    ++resourceGeneration; // Retire immutable entry epochs before clearing TLS.
    retireNativeFrameForReset();
    releaseTargets();
    single_pass::releaseDeviceResources();
    retirePresentationOwner();
}
static HRESULT sourceBackbuffer(IDirect3DSurface9 **out) {
    if (!out) return D3DERR_INVALIDCALL;
    *out = nullptr;
    if (!presentationChain || resetQuarantined || !nativeUiDeviceCurrent(device) ||
        !nativePresentationThreadCurrent() || device->TestCooperativeLevel() != D3D_OK) return D3DERR_INVALIDCALL;
    return presentationChain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, out);
}
// Native eye canvas. The engine sizes its window canvas, and so every eye
// target, from Core's cached client rectangle (wndGetWindowClientRect), and its
// shared windowed depth buffer from the desktop size (GfxD3D GetSystemMetrics).
// For the game's main window report the runtime's recommended eye size instead;
// Present scales the larger backbuffer into the desktop window. The size is
// cached so the next launch creates its canvas and depth at it from the start.
static std::atomic<uint32_t> canvasWidth{0}, canvasHeight{0};
static std::atomic<bool> canvasResizePending{false};
static uintptr_t canvasExe = 0, canvasGfx = 0, canvasGfxEnd = 0;
static int32_t *(__cdecl *originalClientRect)(int32_t *, const void *) = nullptr;
static int(WINAPI *originalSystemMetrics)(int) = nullptr;
static int(__cdecl *nativeResizeCanvas)(void *) = nullptr;
static std::wstring canvasCache() {
    std::wstring directory;
    return moduleDirectory(nullptr, directory) ? directory + L"SS2VR\\headset-view.ini" : std::wstring();
}
static void *mainCanvasWindow() noexcept {
    void *window = nullptr;
    if (canvasExe) memcpy(&window, reinterpret_cast<void *>(canvasExe + 0x8494), sizeof(window));
    return window;
}
static int32_t *__cdecl eyeClientRect(int32_t *out, const void *window) {
    int32_t *rect = originalClientRect(out, window);
    const uint32_t w = canvasWidth.load(std::memory_order_relaxed), h = canvasHeight.load(std::memory_order_relaxed);
    if (rect && w && h && window && window == mainCanvasWindow()) {
        rect[0] = rect[1] = 0;
        rect[2] = int32_t(w);
        rect[3] = int32_t(h);
    }
    return rect;
}
static int WINAPI eyeSystemMetrics(int index) {
    const int value = originalSystemMetrics(index);
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_extract_return_addr(__builtin_return_address(0)));
    if ((index != SM_CXSCREEN && index != SM_CYSCREEN) || caller < canvasGfx || caller >= canvasGfxEnd) return value;
    // Windowed depth is shared by every canvas; size it for any eye extent up to
    // 4096 so a later recommendation can still be rendered without a restart.
    const uint32_t eye = index == SM_CXSCREEN ? canvasWidth.load() : canvasHeight.load();
    return std::max(value, int(std::max<uint32_t>(eye, 4096)));
}
void installEyeCanvas() noexcept {
    static bool installed = false;
    if (installed) return;
    installed = true;
    const HMODULE core = GetModuleHandleW(L"Core.dll"), gfx = GetModuleHandleW(L"GfxD3D.dll"),
                  user = GetModuleHandleW(L"user32.dll"), exe = GetModuleHandleW(nullptr);
    if (!core || !gfx || !user || !exe) { log("Eye canvas unavailable: modules missing"); return; }
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(gfx);
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(reinterpret_cast<const uint8_t *>(gfx) + dos->e_lfanew);
    canvasGfx = reinterpret_cast<uintptr_t>(gfx);
    canvasGfxEnd = canvasGfx + nt->OptionalHeader.SizeOfImage;
    canvasExe = reinterpret_cast<uintptr_t>(exe);
    const auto cache = canvasCache();
    if (!cache.empty()) {
        const UINT w = GetPrivateProfileIntW(L"View", L"Width", 0, cache.c_str());
        const UINT h = GetPrivateProfileIntW(L"View", L"Height", 0, cache.c_str());
        if (w && h && w <= MaxDimension && h <= MaxDimension) { canvasWidth = w; canvasHeight = h; }
    }
    void *clientRect = reinterpret_cast<void *>(GetProcAddress(core,
        "?wndGetWindowClientRect@SeriousEngine@@YA?AVCTRect@1@ABVCWindow@1@@Z"));
    void *metrics = reinterpret_cast<void *>(GetProcAddress(user, "GetSystemMetrics"));
    bool ok = clientRect && metrics &&
        MH_CreateHook(clientRect, reinterpret_cast<void *>(eyeClientRect), reinterpret_cast<void **>(&originalClientRect)) == MH_OK &&
        MH_CreateHook(metrics, reinterpret_cast<void *>(eyeSystemMetrics), reinterpret_cast<void **>(&originalSystemMetrics)) == MH_OK &&
        MH_EnableHook(clientRect) == MH_OK && MH_EnableHook(metrics) == MH_OK;
    log("Eye canvas hooks=%d cached=%ux%u", ok, canvasWidth.load(), canvasHeight.load());
}
// Adopt the runtime's recommended eye size: remember it for the next launch and
// let the main thread resize the native canvas at its next input poll.
static void requestEyeCanvas(uint32_t w, uint32_t h) {
    if (!w || !h || w > MaxDimension || h > MaxDimension || !originalClientRect ||
        (w == canvasWidth.load() && h == canvasHeight.load())) return;
    canvasWidth = w;
    canvasHeight = h;
    const auto cache = canvasCache();
    if (!cache.empty()) {
        WritePrivateProfileStringW(L"View", L"Width", std::to_wstring(w).c_str(), cache.c_str());
        WritePrivateProfileStringW(L"View", L"Height", std::to_wstring(h).c_str(), cache.c_str());
    }
    canvasResizePending = true;
    log("Eye canvas requested %ux%u (OpenXR recommended)", w, h);
}
void applyEyeCanvas() noexcept {
    if (!canvasResizePending.exchange(false) || !canvasExe) return;
    if (!nativeResizeCanvas)
        if (const HMODULE engine = GetModuleHandleW(L"Engine.dll"))
            nativeResizeCanvas = reinterpret_cast<int(__cdecl *)(void *)>(
                GetProcAddress(engine, "?gfxResizeWindowCanvas@SeriousEngine@@YAHPAVCWindowCanvas@1@@Z"));
    void *canvas = nullptr;
    memcpy(&canvas, reinterpret_cast<void *>(canvasExe + 0x8498), sizeof(canvas));
    if (!nativeResizeCanvas || !canvas) { canvasResizePending = true; return; }
    int32_t before[2]{}, after[2]{};
    memcpy(before, static_cast<uint8_t *>(canvas) + 0xc, sizeof(before));
    const int result = nativeResizeCanvas(canvas); // The same call Sam2.exe makes for a window resize.
    memcpy(after, static_cast<uint8_t *>(canvas) + 0xc, sizeof(after));
    // A fullscreen display mode has no window canvas to resize: run the game windowed in VR.
    log("Eye canvas native resize result=%d canvas %dx%d -> %dx%d", result, before[0], before[1], after[0], after[1]);
}
// Pixel sections follow the canvas extent. A new generation is published before
// the old one closes; readers keep their own mapping of an in-flight frame.
static bool ensurePixelStore() {
    if (pixelStore.width == width && pixelStore.height == height && pixelStore.generation) return true;
    if (!channel.shared) return false;
    PixelStore replacement;
    const uint32_t generation = pixelGenerations + 1;
    if (!replacement.open(channel.token, generation, width, height, true)) {
        static unsigned failures = 0;
        if (failures++ < 4) log("Pixel sections %ux%u unavailable error=%lu", width, height, GetLastError());
        return false;
    }
    {
        Lock l(channel, 50);
        if (!l) return false;
        channel.shared->pixels = {generation, width, height, 0};
        for (auto &slot : channel.shared->slot)
            if (slot.state == SlotState::Ready) slot.state = SlotState::Empty;
            else if (slot.state == SlotState::Rendering) slot.cancelled = 1;
        channel.shared->menu.visible = 0;
    }
    pixelGenerations = generation;
    std::swap(pixelStore.mapping, replacement.mapping);
    std::swap(pixelStore.view, replacement.view);
    pixelStore.generation = generation; pixelStore.width = width; pixelStore.height = height;
    log("Pixel sections generation=%u %ux%u (%llu MB each)", generation, width, height,
        static_cast<unsigned long long>(pixelStore.bytes() >> 20));
    return true;
}
// Shared eye images are always A8R8G8B8 so the host sees B8G8R8A8_UNORM; the
// alpha channel is ignored by the opaque projection layer.
static void createSharedImages() {
    if (sharedGeneration || sharedFailed || !device || !channel.shared || !width || !height) return;
    uint32_t handles[2][2]{};
    bool ok = true;
    for (unsigned slot = 0; slot != 2 && ok; ++slot) {
        for (unsigned i = 0; i != 2 && ok; ++i) {
            HANDLE handle = nullptr;
            ok = SUCCEEDED(device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                                 D3DPOOL_DEFAULT, &sharedTexture[slot][i], &handle)) &&
                handle && handle != INVALID_HANDLE_VALUE &&
                SUCCEEDED(sharedTexture[slot][i]->GetSurfaceLevel(0, &sharedSurface[slot][i]));
            handles[slot][i] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(handle));
        }
        ok = ok && SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_EVENT, &sharedFence[slot]));
    }
    if (!ok) {
        log("Shared eye images unavailable %ux%u; pairs use pixel sections", width, height);
        releaseSharedImages();
        sharedFailed = true;
        return;
    }
    Lock l(channel, 50);
    if (!l) { releaseSharedImages(); return; } // Retried on the next allocation.
    sharedGeneration = ++sharedGenerations;
    auto &images = channel.shared->images;
    images = {};
    images.generation = sharedGeneration;
    images.width = width;
    images.height = height;
    for (unsigned slot = 0; slot != 2; ++slot)
        for (unsigned i = 0; i != 2; ++i) images.handle[slot][i] = handles[slot][i];
    log("Shared eye images generation=%u %ux%u handles=%08x,%08x,%08x,%08x", sharedGeneration, width, height,
        handles[0][0], handles[0][1], handles[1][0], handles[1][1]);
}
// Marks pairs whose GPU work has finished; the host copies only those.
static void pollSharedFences() {
    for (unsigned slot = 0; slot != 2; ++slot) {
        if (!sharedPending[slot]) continue;
        BOOL done = FALSE;
        const HRESULT result = sharedFence[slot] ? sharedFence[slot]->GetData(&done, sizeof(done), 0) : E_FAIL;
        if (result == S_FALSE) continue;
        Lock l(channel, 5);
        if (!l) continue;
        sharedPending[slot] = false;
        auto &target = channel.shared->slot[slot];
        if (result == S_OK && target.state == SlotState::Ready && target.sharedImage == sharedGeneration &&
            target.request.sequence == sharedPendingSequence[slot] && !target.gpuReady) {
            target.gpuReady = 1;
            SetEvent(channel.ready);
        }
    }
}
static bool allocateBody(bool stereo, IDirect3DSurface9 *&bb, IDirect3DSurface9 *&nativeDepth) {
    if (!device || !presentationChain || resetQuarantined)
        return false;
    if (FAILED(sourceBackbuffer(&bb)))
        return false;
    D3DSURFACE_DESC desc{};
    const HRESULT descriptionResult = bb->GetDesc(&desc);
    bb->Release();
    bb = nullptr;
    if (FAILED(descriptionResult) || desc.Width > MaxDimension || desc.Height > MaxDimension || !desc.Width || !desc.Height ||
        desc.MultiSampleType != D3DMULTISAMPLE_NONE ||
        (desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8))
        return false;
    if (desc.Width != width || desc.Height != height || desc.Format != colorFormat) {
        releaseTargets(); // Preserve the admitted chain while replacing targets.
        width = desc.Width; height = desc.Height; colorFormat = desc.Format;
    }
    if (!readback[0] && FAILED(device->CreateOffscreenPlainSurface(width, height, colorFormat,
        D3DPOOL_SYSTEMMEM, &readback[0], nullptr))) return false;
    if (!ensurePixelStore()) return false;
    if (!stereo) return readback[0] != nullptr;
    D3DSURFACE_DESC depthDesc{};
    if (FAILED(device->GetDepthStencilSurface(&nativeDepth)) || !nativeDepth)
        return false;
    const auto depthResult = nativeDepth->GetDesc(&depthDesc);
    nativeDepth->Release();
    nativeDepth = nullptr;
    D3DVIEWPORT9 viewport{};
    if (FAILED(depthResult) || depthDesc.MultiSampleType != D3DMULTISAMPLE_NONE ||
        depthDesc.Width < width || depthDesc.Height < height || FAILED(device->GetViewport(&viewport)) ||
        viewport.X || viewport.Y || viewport.Width != width || viewport.Height != height)
        return false;
    const auto ensureWide = [&] {
        if (!singlePassEnabled || (wideColor && wideDepth)) return;
        if (FAILED(device->CreateRenderTarget(width * 2, height, colorFormat, D3DMULTISAMPLE_NONE, 0, FALSE, &wideColor, nullptr)) ||
            FAILED(device->CreateDepthStencilSurface(width * 2, height, depthFormat, D3DMULTISAMPLE_NONE, 0, TRUE, &wideDepth, nullptr))) {
            if (wideColor) wideColor->Release();
            if (wideDepth) wideDepth->Release();
            wideColor = wideDepth = nullptr;
            log("Single pass: %ux%u targets unavailable; eyes render separately", width * 2, height);
        }
    };
    if (eye[0] && desc.Width == width && desc.Height == height && depthDesc.Format == depthFormat &&
        desc.Format == colorFormat) {
        ensureWide();
        createSharedImages();
        Lock l(channel);
        if (l)
            channel.shared->rendererReady = 1;
        return true;
    }
    // Stereo resources can change while menu color readback remains usable.
    for (unsigned i = 0; i != 2; ++i) {
        if (eye[i]) eye[i]->Release();
        if (depth[i]) depth[i]->Release();
        eye[i] = depth[i] = nullptr;
    }
    depthFormat = depthDesc.Format;
    colorFormat = desc.Format;
    hasStencil = depthFormat == D3DFMT_D24S8 || depthFormat == D3DFMT_D15S1 || depthFormat == D3DFMT_D24X4S4;
    for (int i = 0; i < 2; i++) {
        if (FAILED(device->CreateRenderTarget(width, height, colorFormat, D3DMULTISAMPLE_NONE, 0, FALSE,
                                              &eye[i], nullptr)) ||
            FAILED(device->CreateDepthStencilSurface(width, height, depthFormat, D3DMULTISAMPLE_NONE, 0, TRUE,
                                                     &depth[i], nullptr)) ||
            (!readback[i] && FAILED(device->CreateOffscreenPlainSurface(width, height, colorFormat, D3DPOOL_SYSTEMMEM,
                                                       &readback[i], nullptr)))) {
            releaseTargets();
            return false;
        }
    }
    ensureWide();
    createSharedImages();
    Lock l(channel);
    if (l) {
        channel.shared->width = width;
        channel.shared->height = height;
        channel.shared->rendererReady = 1;
    }
    log("CPU stereo targets %ux%u; performance unverified", width, height);
    return true;
}
static bool allocate(bool stereo = true) noexcept {
    IDirect3DSurface9 *bb = nullptr, *nativeDepth = nullptr;
    bool result = false;
    const bool contained = withNativeFinally([&] { result = allocateBody(stereo, bb, nativeDepth); },
        [&](bool aborted) noexcept {
            if (bb) { bb->Release(); bb = nullptr; }
            if (nativeDepth) { nativeDepth->Release(); nativeDepth = nullptr; }
            if (aborted) { uiHalted = true; result = false; }
        });
    return contained && result;
}
template <class T> static bool deviceHook(void *address, void *detour, T &original, const char *name) {
    static void *boundAddress = nullptr;
    if (original)
        return address == boundAddress;
    static std::atomic<bool> createOk{false}, createFailed{false}, enableOk{false}, enableFailed{false};
    const auto created = MH_CreateHook(address, detour, reinterpret_cast<void **>(&original));
    auto &createReported = created == MH_OK ? createOk : createFailed;
    if (!createReported.exchange(true, std::memory_order_relaxed))
        log("Startup routing hook=%s operation=create status=%d thread=%lu", name,
            static_cast<int>(created), GetCurrentThreadId());
    if (created == MH_OK) {
        const auto enabled = MH_EnableHook(address);
        auto &enableReported = enabled == MH_OK ? enableOk : enableFailed;
        if (!enableReported.exchange(true, std::memory_order_relaxed))
            log("Startup routing hook=%s operation=enable status=%d thread=%lu", name,
                static_cast<int>(enabled), GetCurrentThreadId());
        if (enabled == MH_OK) {
            boundAddress = address;
            return true;
        }
        MH_RemoveHook(address);
        original = nullptr;
    }
    return false;
}
// Component admission only: the native main-thread/owner/resource-epoch checks
// remain required. A Reset reuses creation ownership; deviceReady cannot invent
// that ownership. MULTITHREADED offers per-call safety, not a whole transaction.
bool nativePresentationIdleForBodyMove() noexcept {
    // uiFrame spans both eyes, their gap, scope previews and deferred native UI.
    // eyeIndex alone would incorrectly admit a body move between two eyes.
    return creationThread && GetCurrentThreadId()==creationThread &&
        device && device==creationDevice && eye[0] && eye[1] && !stopping.load() &&
        uiFrame.slot<0 && deferredSlot<0 && activeEye<0 && !scopeScratch && !scopeTransaction;
}
bool nativeUiDeviceCurrent(IDirect3DDevice9 *d) {
    if (!d || d != device || d != creationDevice || !creationThread ||
        GetCurrentThreadId() != creationThread) return false;
    D3DDEVICE_CREATION_PARAMETERS creation{};
    if (FAILED(d->GetCreationParameters(&creation)) ||
        (creation.BehaviorFlags & (D3DCREATE_PUREDEVICE | D3DCREATE_MULTITHREADED))) return false;
    D3DCAPS9 caps{};
    return SUCCEEDED(d->GetDeviceCaps(&caps)) && caps.MaxUserClipPlanes >= 6;
}
void deviceCreated(IDirect3DDevice9 *d) {
    static std::atomic<bool> claimed{false};
    const bool first = !claimed.exchange(true, std::memory_order_acq_rel);
    if (first) {
        startupCreation.device = d;
        startupCreation.thread = GetCurrentThreadId();
        startupCreationPublished.store(true, std::memory_order_release);
    } else if (!startupCreationPublished.load(std::memory_order_acquire) ||
               GetCurrentThreadId() != startupCreation.thread) return;
    creationDevice = d;
    creationThread = GetCurrentThreadId();
    uiHalted = false; // A newly created device is a new admitted resource lifetime.
    if (first) {
        logStartup("deviceCreated", d, hooksReady.load(std::memory_order_acquire));
    }
    deviceReady(d);
    resetQuarantined = false;
}
void deviceReady(IDirect3DDevice9 *d) {
    if (device && device != d) {
        deviceLost();
        device->Release();
    }
    if (device != d) {
        device = d;
        device->AddRef();
    }
    auto v = *reinterpret_cast<void ***>(d);
    uiRoutingReady = true;
    uiRoutingReady = deviceHook(v[37], reinterpret_cast<void *>(setRT), originalSetRT, "SetRenderTarget") && uiRoutingReady;
    uiRoutingReady = deviceHook(v[39], reinterpret_cast<void *>(setDepth), originalSetDepth, "Depth") && uiRoutingReady;
    uiRoutingReady = deviceHook(v[34], reinterpret_cast<void *>(stretch), originalStretch, "Canvas copy") && uiRoutingReady;
    uiRoutingReady = deviceHook(v[32], reinterpret_cast<void *>(readTarget), originalReadTarget, "Canvas read") && uiRoutingReady;
    uiRoutingReady = deviceHook(v[35], reinterpret_cast<void *>(fill), originalFill, "Canvas fill") && uiRoutingReady;
    uiRoutingReady = deviceHook(v[81], reinterpret_cast<void *>(draw), originalDraw, "Native UI draw") && uiRoutingReady;
    uiRoutingReady = deviceHook(v[82], reinterpret_cast<void *>(drawIndexed), originalDrawIndexed, "Native UI indexed draw") && uiRoutingReady;
    uiRoutingReady = deviceHook(v[43], reinterpret_cast<void *>(clear), originalClear, "Native UI clear observer") && uiRoutingReady;
    uiRoutingReady = deviceHook(v[83], reinterpret_cast<void *>(drawUp), originalDrawUp, "Native UI UP observer") && uiRoutingReady;
    uiRoutingReady = deviceHook(v[84], reinterpret_cast<void *>(drawIndexedUp), originalDrawIndexedUp, "Native UI indexed UP observer") && uiRoutingReady;
    uiRoutingReady = deviceHook(v[115], reinterpret_cast<void *>(drawRect), originalDrawRect, "Native UI rectangle observer") && uiRoutingReady;
    uiRoutingReady = deviceHook(v[116], reinterpret_cast<void *>(drawTri), originalDrawTri, "Native UI triangle observer") && uiRoutingReady;
    uiRoutingReady = deviceHook(v[30], reinterpret_cast<void *>(update), originalUpdate, "Native UI update observer") && uiRoutingReady;
    const bool ready = hooksReady.load();
    static std::atomic<bool> firstWaiting{false}, firstReady{false};
    auto &reported = ready ? firstReady : firstWaiting;
    if (!reported.exchange(true, std::memory_order_relaxed))
        logStartup("deviceReady", d, ready, uiRoutingReady);
}
static void startChannel() {
    auto *d = device;
    const bool ready = hooksReady.load(std::memory_order_acquire);
    if (!ready || !presentationChain || resetQuarantined || !nativeUiDeviceCurrent(d) ||
        !nativePresentationThreadCurrent()) return;
    if (!channel.shared) {
        wchar_t token[64];
        swprintf(token, 64, L"%lu-%llu", GetCurrentProcessId(), GetTickCount64());
        if (!channel.open(token, true)) {
            static std::atomic<bool> failed{false};
            if (!failed.exchange(true, std::memory_order_relaxed))
                logStartup("IPC create success=0", d, ready);
            return;
        }
        static std::atomic<bool> opened{false};
        if (!opened.exchange(true, std::memory_order_relaxed))
            logStartup("IPC create success=1", d, ready, -1, 1);
        std::wstring root;
        if (!moduleDirectory(nullptr,root)) {
            log("Startup OpenXR host creation skipped: directory unavailable");
            return;
        }
        const auto executable=root+L"SS2VR\\ss2vr_host.exe";
        auto cmd=L"\""+executable+L"\" --channel "+token;
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> arg(cmd.begin(), cmd.end());
        arg.push_back(0);
        // Explicit image path prevents whitespace-based executable search.
        // Null environment preserves Steam/Proton's prefix and runtime context.
        if (CreateProcessW(executable.c_str(), arg.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                           root.c_str(), &si, &pi)) {
            log("Startup OpenXR host created=1 process=%lu thread=%lu", pi.dwProcessId, GetCurrentThreadId());
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else {
            const DWORD error = GetLastError();
            log("Startup OpenXR host created=0 error=%lu thread=%lu", error, GetCurrentThreadId());
        }
    }
}
static uint64_t lastMenuCopy = 0, menuSequence = 0;
// The flat screen is a ~2 m panel: a 1920-wide image is all a headset resolves on
// it. Its band is scaled on the GPU before readback instead of copying the whole
// eye-sized canvas (50 MB at 3561^2) through the CPU every copy.
static IDirect3DSurface9 *menuScaled = nullptr, *menuScaledRead = nullptr;
static uint32_t menuScaledWidth = 0, menuScaledHeight = 0;
static void releaseMenuScaled() {
    if (menuScaled) menuScaled->Release();
    if (menuScaledRead) menuScaledRead->Release();
    menuScaled = menuScaledRead = nullptr;
    menuScaledWidth = menuScaledHeight = 0;
}
// The region of the canvas the game draws its 2D view into: ForceAspectRatio
// letterboxes the drawport to gfx_fAspectRatio (Sam2Game RVA 3F9DE0).
struct UiBand { uint32_t x = 0, y = 0, width = 0, height = 0; };
static UiBand uiBand(uint32_t width, uint32_t height) {
    UiBand band{0, 0, width, height};
    static const auto sam = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"Sam2Game.dll"));
    float aspect = 0;
    if (sam) memcpy(&aspect, reinterpret_cast<void *>(sam + 0x3f9de0), sizeof(aspect));
    if (!std::isfinite(aspect) || aspect < .25f || aspect > 4 || !width || !height ||
        std::abs(float(width) / float(height) - aspect) <= .001f) return band;
    if (float(width) / float(height) > aspect) {
        band.width = std::min(width, uint32_t(float(height) * aspect));
        band.x = (width - band.width) / 2;
    } else {
        band.height = std::min(height, uint32_t(float(width) / aspect));
        band.y = (height - band.height) / 2;
    }
    return band;
}
static MenuNavigation menuNavigation;
static WORD pendingKey = 0;
static uint64_t releaseKeyAt = 0;
// Core reads the keyboard from its own window messages (no DirectInput), so a
// posted key reaches the game without focus and never leaks into another
// application the way desktop-wide SendInput would.
static void keyEvent(WORD key, bool up) {
    D3DDEVICE_CREATION_PARAMETERS params{};
    if (!device || FAILED(device->GetCreationParameters(&params)) || !params.hFocusWindow)
        return;
    uint32_t bits = 1u | ((MapVirtualKeyW(key, MAPVK_VK_TO_VSC) & 0xffu) << 16);
    if (key == VK_UP || key == VK_DOWN || key == VK_LEFT || key == VK_RIGHT)
        bits |= 1u << 24; // Extended key.
    if (up)
        bits |= 3u << 30; // Previous state down, transition up.
    PostMessageW(params.hFocusWindow, up ? WM_KEYUP : WM_KEYDOWN, key, static_cast<LPARAM>(bits));
}
static bool labAutoContinue = false;
void setLabAutoContinue(bool enabled) noexcept { labAutoContinue = enabled; }
static void keyTap(WORD key);
// Measurement only ([Lab] AutoContinue): with no controller to press, confirm
// loading prompts and menus every 2 s until gameplay starts, as the lab does.
static void autoContinue(bool nativeMenu, bool menuVisible, uint64_t now) {
    static uint64_t last = 0;
    if (!labAutoContinue || !menuVisible || now - last < 2000) return;
    last = now;
    if (!(nativeMenu && menus::queueCommand(0x0b))) keyTap(VK_RETURN);
}
static void keyTap(WORD key) {
    if (pendingKey)
        keyEvent(pendingKey, true);
    keyEvent(key, false);
    pendingKey = key;
    releaseKeyAt = GetTickCount64() + 75;
}
std::atomic<uint64_t> vrInputTick{0};
// While the headset is actively driving the game, the engine treats itself as
// the foreground application (full frame rate, live input) regardless of focus.
bool vrInputLive() noexcept {
    const uint64_t tick = vrInputTick.load(std::memory_order_relaxed), now = GetTickCount64();
    return hooksReady.load(std::memory_order_acquire) && tick && now >= tick && now - tick < 1000;
}
bool foregroundGame() {
    if (!device)
        return false;
    D3DDEVICE_CREATION_PARAMETERS params{};
    if (FAILED(device->GetCreationParameters(&params)) || !params.hFocusWindow)
        return false;
    HWND foreground = GetForegroundWindow();
    return foreground && GetAncestor(foreground, GA_ROOT) == GetAncestor(params.hFocusWindow, GA_ROOT);
}
bool nativePresentationCaller(uintptr_t caller, bool chain) noexcept {
    if (!hooksReady.load(std::memory_order_acquire)) return false;
    const auto module = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"GfxD3D.dll"));
    return module && caller == module + (chain ? 0x1c68u : 0x1c75u);
}
void nestedNativePresentation() noexcept {
    if (!startupCreationPublished.load(std::memory_order_acquire) ||
        GetCurrentThreadId() != startupCreation.thread) return;
    ++resourceGeneration;
    nativeUiFault();
    if (channel.shared && channel.lock()) {
        channel.shared->rendererReady = 0;
        channel.shared->menu.tickMs = 0;
        channel.shared->menu.visible = 0;
        channel.unlock();
    }
}
void nativePresentationFailed(IDirect3DDevice9 *d, IDirect3DSwapChain9 *chain) noexcept {
    if (!startupCreationPublished.load(std::memory_order_acquire) ||
        GetCurrentThreadId() != startupCreation.thread ||
        (d && d != device) || (chain && chain != presentationChain) || (!d && !chain)) return;
    resetQuarantined = true;
    nestedNativePresentation(); // Scalar invalidation only; no post-Present COM queries.
}
static void admitPresentation(IDirect3DSwapChain9 *chain, uintptr_t caller, bool chainRoute,
    const RECT *src, const RECT *dst, HWND overrideWindow, const RGNDATA *region, DWORD flags) noexcept {
    if (!chain || src || dst || region || flags || !nativePresentationCaller(caller, chainRoute) ||
        !startupCreationPublished.load(std::memory_order_acquire) ||
        GetCurrentThreadId() != startupCreation.thread || resetQuarantined || stopping || uiHalted ||
        activeEye >= 0 || uiFrame.slot >= 0 || scopeScratch || scopeTransaction) return;
    IDirect3DDevice9 *d = nullptr;
    IDirect3DSurface9 *bb = nullptr, *rt = nullptr;
    withNativeFinally([&] {
        if (FAILED(chain->GetDevice(&d)) || !nativeUiDeviceCurrent(d) || !uiRoutingReady ||
            !nativePresentationThreadCurrent()) return;
        D3DPRESENT_PARAMETERS params{};
        D3DDEVICE_CREATION_PARAMETERS creation{};
        D3DSURFACE_DESC desc{};
        D3DVIEWPORT9 viewport{};
        if (FAILED(chain->GetPresentParameters(&params)) || FAILED(d->GetCreationParameters(&creation))) return;
        const HWND window = overrideWindow ? overrideWindow : params.hDeviceWindow;
        if (!window || window != creation.hFocusWindow) return;
        DWORD process = 0;
        if (!GetWindowThreadProcessId(window, &process) || process != GetCurrentProcessId()) return;
        if (FAILED(chain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb ||
            FAILED(bb->GetDesc(&desc)) || FAILED(d->GetRenderTarget(0, &rt)) || !rt ||
            !sameSurface(bb, rt) || FAILED(d->GetViewport(&viewport)) ||
            !desc.Width || !desc.Height || desc.Width > MaxDimension || desc.Height > MaxDimension ||
            desc.MultiSampleType != D3DMULTISAMPLE_NONE ||
            (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8) ||
            viewport.X || viewport.Y || viewport.Width != desc.Width || viewport.Height != desc.Height) return;
        if (chain != presentationChain || window != presentationWindow) {
            releaseTargets();
            retirePresentationOwner();
            presentationChain = chain;
            chain->AddRef();
            presentationWindow = window;
            log("Native presentation owner admitted %ux%u; capture/lifecycle acceptance pending", desc.Width, desc.Height);
        }
        // Do not retain admission surfaces through input, allocation or readback.
        bb->Release(); bb = nullptr;
        rt->Release(); rt = nullptr;
        present(d);
    }, [&](bool aborted) noexcept {
        if (bb) { bb->Release(); bb = nullptr; }
        if (rt) { rt->Release(); rt = nullptr; }
        if (d) { d->Release(); d = nullptr; }
        if (aborted) { uiHalted = true; nestedNativePresentation(); }
    });
}
void presentChain(IDirect3DSwapChain9 *chain, uintptr_t caller, const RECT *src, const RECT *dst,
                  HWND window, const RGNDATA *region, DWORD flags) noexcept {
    admitPresentation(chain, caller, true, src, dst, window, region, flags);
}
void presentDevice(IDirect3DDevice9 *d, uintptr_t caller, const RECT *src, const RECT *dst,
                   HWND window, const RGNDATA *region) noexcept {
    if (!nativePresentationCaller(caller, false) || !presentationDeviceOwner(d) || resetQuarantined) return;
    IDirect3DSwapChain9 *chain = nullptr;
    withNativeFinally([&] {
        if (SUCCEEDED(d->GetSwapChain(0, &chain))) admitPresentation(chain, caller, false, src, dst, window, region, 0);
    }, [&](bool aborted) noexcept {
        if (chain) { chain->Release(); chain = nullptr; }
        if (aborted) { uiHalted = true; nestedNativePresentation(); }
    });
}
static LARGE_INTEGER lastPresentEnd{}, lastStereoEnd{};
void notePerfSection(unsigned section, LARGE_INTEGER start) noexcept {
    if (section < 3) perf.section[section] += PerfClock::seconds(start, PerfClock::now());
}
// VR frame pacing, at the frame boundary: while the headset streams pairs, the
// next frame starts only once the host has a request waiting, so its simulation
// uses the same input sample as the request and its render becomes a pair
// instead of a flat frame nobody sees (a full-resolution render competing with
// the real pairs). Bounded, and inactive outside stereo streaming.
static bool requestWaiting() {
    Lock l(channel, 2);
    if (!l) return true; // Never stall on contention.
    for (const auto &slot : channel.shared->slot)
        if (slot.state == SlotState::Requested && !slot.cancelled) return true;
    return false;
}
static void paceToHeadset() {
    if (!lastPairTick || GetTickCount64() - lastPairTick > 250 || !channel.shared || stopping) return;
    if (requestWaiting()) return;
    const LARGE_INTEGER start = PerfClock::now();
    const ULONGLONG deadline = GetTickCount64() + 25;
    do {
        Sleep(1);
        pollSharedFences(); // Finished pairs must keep freeing slots while we wait.
    } while (!requestWaiting() && GetTickCount64() < deadline && !stopping);
    perf.wait += PerfClock::seconds(start, PerfClock::now());
    ++perf.waits;
}
void notePresentCall(LARGE_INTEGER start, LARGE_INTEGER end) noexcept {
    perf.presentCall += PerfClock::seconds(start, end);
    if (lastStereoEnd.QuadPart) perf.postStereo += PerfClock::seconds(lastStereoEnd, start);
    lastStereoEnd = {};
    pollSharedFences(); // Present flushed the frame: its pair is often complete already.
    paceToHeadset();
    lastPresentEnd = PerfClock::now();
}
static void presentBody(IDirect3DDevice9 *d);
void present(IDirect3DDevice9 *d) {
    const LARGE_INTEGER hookStart = PerfClock::now();
    presentBody(d);
    perf.presentHook += PerfClock::seconds(hookStart, PerfClock::now());
}
static void presentBody(IDirect3DDevice9 *d) {
    {
        const LARGE_INTEGER t = PerfClock::now();
        if (perf.lastPresent.QuadPart) { perf.frame += PerfClock::seconds(perf.lastPresent, t); ++perf.frames; }
        perf.lastPresent = t;
        if (!perf.lastLog.QuadPart) perf.lastLog = t;
        if (PerfClock::seconds(perf.lastLog, t) >= 5 && perf.frames) {
            const double pairs = perf.pairs ? perf.pairs : 1, reads = perf.readbacks ? perf.readbacks : 1,
                gpus = perf.gpuSamples ? perf.gpuSamples : 1;
            if (perf.lostNoWorld + perf.lostFault + perf.lostNoOverlay + perf.lostIncomplete + perf.lostOther)
                log("Perf unpublished pairs: noWorld=%u fault=%u noOverlay=%u incomplete=%u other=%u lastFault=%s origin=+%lx world=%u ui=%u overlay=%u complete=%u",
                    perf.lostNoWorld, perf.lostFault, perf.lostNoOverlay, perf.lostIncomplete, perf.lostOther, perf.lostReason,
                    static_cast<unsigned long>(perf.lostOrigin), perf.lostWorldOk, perf.lostUi, perf.lostOverlaySeen, perf.lostComplete);
            unsigned lostStages = 0;
            for (unsigned k : perf.stage) lostStages += k;
            if (lostStages)
                log("Perf lost stereo stages: begin=%u scope=%u setup=%u world=%u uiProjection=%u eyeInvalid=%u target=%u postlude=%u readback=%u restore=%u",
                    perf.stage[0],perf.stage[1],perf.stage[2],perf.stage[3],perf.stage[4],perf.stage[5],perf.stage[6],
                    perf.stage[7],perf.stage[8],perf.stage[9]);
            if (perf.publishLock + perf.publishCancelled + perf.publishState + perf.publishRequest + perf.publishImages +
                perf.publishCommit + perf.finishNotCurrent)
                log("Perf publish rejected: lock=%u cancelled=%u state=%u request=%u images=%u commit=%u notCurrent=%u",
                    perf.publishLock, perf.publishCancelled, perf.publishState, perf.publishRequest, perf.publishImages,
                    perf.publishCommit, perf.finishNotCurrent);
            if (perf.scopeNeeded || scopeSourcesHalted)
                log("Perf scope: needed=%u candidates=%u begun=%u ready=%u halted=%u callbacks=%u copied=%u finished=%u "
                "drawn=%u luma=%.1f eyeInOptic=%.3f,%.3f,%.3f sourceCpu=%.2fms tail=%.2fms beginFail=%u/%u/%u",
                perf.scopeNeeded, perf.scopeCandidates,
                perf.scopeBegun, perf.scopeReady, unsigned(scopeSourcesHalted), perf.scopeCallbacks, perf.scopeCopied,
                perf.scopeFinished, scopeImageDraws(), double(perf.scopeLuma), double(scopeEyeInOptic()[0]),
                double(scopeEyeInOptic()[1]), double(scopeEyeInOptic()[2]),
                perf.scopeBegun ? perf.scopeSourceCpu*1000/perf.scopeBegun : 0.,
                perf.scopeCallbacks ? perf.scopeTailCpu*1000/perf.scopeCallbacks : 0., perf.scopeBeginFailures[1],
                perf.scopeBeginFailures[2], perf.scopeBeginFailures[3]);
            log("Perf draws per pair: eyes=%.0f hud duplicates=%.0f; other per frame=%.0f; hud draws per frame=%.1f "
                "hud cpu per frame=%.3fms layer=%u fallback=%u flushes=%u flush=%.3fms",
                perf.pairs ? double(perf.eyeDraws) / perf.pairs : 0., perf.pairs ? double(perf.hudDuplicates) / perf.pairs : 0.,
                double(perf.desktopDraws) / perf.frames, double(perf.hudDraws) / perf.frames,
                1000 * perf.hudCpu / perf.frames, perf.hudLayerDraws, perf.hudFallbackDraws, perf.hudFlushes,
                perf.hudFlushes ? 1000 * perf.hudFlushCpu / perf.hudFlushes : 0.);
            {
                unsigned created = 0, reused = 0, missed = 0;
                double createdMs = 0, reusedMs = 0;
                shadowReuseStats(created, reused, missed, createdMs, reusedMs);
                // Lab only (SS2VR_SHADOW_PROBE=1): dump one pooled shadow image once.
                static const bool shadowProbe = [] { wchar_t value[2]{}; return GetEnvironmentVariableW(L"SS2VR_SHADOW_PROBE", value, 2) == 1 && value[0] == L'1'; }();
                static bool shadowProbed = false;
                if (shadowProbe && !shadowProbed && device) {
                    if (auto *texture = static_cast<IDirect3DTexture9 *>(shadowReuseProbeTexture())) {
                        shadowProbed = true;
                        IDirect3DSurface9 *level = nullptr, *read = nullptr;
                        D3DSURFACE_DESC desc{};
                        D3DLOCKED_RECT locked{};
                        if (SUCCEEDED(texture->GetSurfaceLevel(0, &level)) && SUCCEEDED(level->GetDesc(&desc)) &&
                            SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &read, nullptr)) &&
                            SUCCEEDED(device->GetRenderTargetData(level, read)) && SUCCEEDED(read->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
                            std::wstring directory;
                            FILE *file = moduleDirectory(nullptr, directory) ? _wfopen((directory + L"SS2VR\\shadow-probe.ppm").c_str(), L"wb") : nullptr;
                            unsigned dark = 0;
                            if (file) fprintf(file, "P6\n%u %u\n255\n", desc.Width, desc.Height);
                            for (UINT y = 0; y < desc.Height; ++y) for (UINT x = 0; x < desc.Width; ++x) {
                                uint8_t rgb[3]{};
                                const auto *row = static_cast<const uint8_t *>(locked.pBits) + size_t(y) * locked.Pitch;
                                if (desc.Format == D3DFMT_R5G6B5) {
                                    const uint16_t v = uint16_t(row[x*2] | row[x*2+1] << 8);
                                    rgb[0] = uint8_t((v >> 11) << 3); rgb[1] = uint8_t(((v >> 5) & 63) << 2); rgb[2] = uint8_t((v & 31) << 3);
                                } else { rgb[0] = row[x*4+2]; rgb[1] = row[x*4+1]; rgb[2] = row[x*4]; }
                                dark += rgb[0] + rgb[1] + rgb[2] < 384;
                                if (file) fwrite(rgb, 1, 3, file);
                            }
                            if (file) fclose(file);
                            read->UnlockRect();
                            log("Shadow probe %ux%u format=%u dark=%u of %u", desc.Width, desc.Height, unsigned(desc.Format), dark, desc.Width * desc.Height);
                        }
                        if (read) read->Release();
                        if (level) level->Release();
                    }
                }
                if (created || reused || missed)
                    log("Perf shadows per frame: created=%.1f reused=%.1f missed=%.1f | per shadow: render+project=%.3fms "
                        "project only=%.3fms", double(created) / perf.frames, double(reused) / perf.frames,
                        double(missed) / perf.frames, created ? createdMs / created : 0., reused ? reusedMs / reused : 0.);
            }
            {
                double left = 0, right = 0;
                rootExecuteStats(left, right);
                if (perf.pairs)
                    log("Perf eye split per pair: execute=%.2f,%.2fms of eyes=%.2f,%.2fms (rest: collect/sort/setup)",
                        left / perf.pairs, right / perf.pairs, 1000 * perf.eyeCpu[0] / perf.pairs, 1000 * perf.eyeCpu[1] / perf.pairs);
            }
            if (census.vertexCount || census.draws[0][0]) {
                log("Perf shader census left-eye draws: vs ff=%u 1.x=%u 2.0=%u 2.x=%u 3.0=%u other=%u (programs=%u) | "
                    "ps ff=%u 1.x=%u 2.0=%u 2.x=%u 3.0=%u other=%u (programs=%u)",
                    census.draws[0][0], census.draws[0][1], census.draws[0][2], census.draws[0][3], census.draws[0][4],
                    census.draws[0][5], census.vertexCount, census.draws[1][0], census.draws[1][1], census.draws[1][2],
                    census.draws[1][3], census.draws[1][4], census.draws[1][5], census.pixelCount);
                for (auto &row : census.draws) for (auto &count : row) count = 0;
                log("Perf draw census left eye: plain=%u indexed=%u stream15OrBlendIndices=%u maxStream=%u instanced=%u",
                    drawCensus.plain, drawCensus.indexed, drawCensus.stream15, drawCensus.maxStream, drawCensus.instanced);
                drawCensus = {};
            }
            {
                const auto sp = single_pass::takeStats();
                if (singlePassEnabled)
                    log("Perf single pass: passes=%u draws/pass=%.0f instanced/pass=%.0f unpatched/pass=%.0f matrix states/pass=%.1f target switches/pass=%.1f up/pass=%.1f locked/pass=%.1f variants=%u failed=%u foveated=%u atlas=%ux%u",
                        sp.passes, sp.passes ? double(sp.draws) / sp.passes : 0., sp.passes ? double(sp.instanced) / sp.passes : 0.,
                        sp.passes ? double(sp.unpatched) / sp.passes : 0.,
                        sp.passes ? double(sp.stateChanges) / sp.passes : 0., sp.passes ? double(sp.suspensions) / sp.passes : 0., sp.passes ? double(sp.userPointer) / sp.passes : 0., sp.passes ? double(sp.screenLocked) / sp.passes : 0.,
                        sp.variants, sp.failedVariants, perf.foveatedPairs, atlasWidth, atlasHeight);
            }
            {
                const auto r = replay::takeStats();
                if (eyeReplay)
                    log("Perf eye replay: recorded=%u replayed=%u invalidated=%u fallbacks=%u commands/replay=%.0f draws/record=%.0f "
                        "tagged/record=%.0f replay=%.2fms last=%s", r.recorded, r.replayed, r.invalidated, perf.replayFallbacks,
                        r.replayed ? double(r.commands) / r.replayed : 0., r.recorded ? double(r.draws) / r.recorded : 0.,
                        r.recorded ? double(r.tagged) / r.recorded : 0., r.replayed ? r.replayMs / r.replayed : 0., r.lastReason);
            }
            log("Perf mod sections per frame: update=%.2fms lasers=%.2fms wheels=%.2fms", 1000 * perf.section[0] / perf.frames,
                1000 * perf.section[1] / perf.frames, 1000 * perf.section[2] / perf.frames);
            log("Perf pacing: waits=%u wait=%.2fms per frame", perf.waits, 1000 * perf.wait / perf.frames);
            log("Perf per frame: present call=%.2fms hook=%.2fms | before stereo=%.2fms after stereo=%.2fms (n=%u)",
                1000 * perf.presentCall / perf.frames, 1000 * perf.presentHook / perf.frames,
                perf.stereoCalls ? 1000 * perf.preStereo / perf.stereoCalls : 0.,
                perf.stereoCalls ? 1000 * perf.postStereo / perf.stereoCalls : 0., perf.stereoCalls);
            {
                // Render-thread CPU time (excludes preemption by other processes,
                // unlike the wall-clock figures below).
                static uint64_t lastThreadCpu = 0;
                FILETIME created{}, exited{}, kernel{}, user{};
                if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
                    const uint64_t cpu = ((uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
                                         ((uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime);
                    if (lastThreadCpu && perf.frames)
                        log("Perf render thread cpu per frame=%.2fms", double(cpu - lastThreadCpu) / 1e4 / perf.frames);
                    lastThreadCpu = cpu;
                }
            }
            log("Perf frames=%u pairs=%u mono=%u size=%ux%u frame=%.1fms | per pair cpu: stereo=%.1f eyes=%.1f,%.1f "
                "desktop=%.1f wait=%.1f | per eye read: getRT=%.2f lock=%.2f copy=%.2f | per pair dim=%.2f publish=%.2f | "
                "mono=%.1fms | gpu(n=%u): eyes=%.2f,%.2f desktop=%.2f tail=%.2f",
                perf.frames, perf.pairs, perf.monoFrames, width, height, 1000 * perf.frame / perf.frames,
                1000 * perf.stereo / pairs, 1000 * perf.eyeCpu[0] / pairs, 1000 * perf.eyeCpu[1] / pairs,
                1000 * perf.desktopCpu / pairs, 1000 * perf.wait / pairs, 1000 * perf.readTarget / reads,
                1000 * perf.lock / reads, 1000 * perf.copy / reads, 1000 * perf.dim / pairs, 1000 * perf.publish / pairs,
                perf.monoFrames ? 1000 * perf.mono / perf.monoFrames : 0., perf.gpuSamples, perf.gpu[0] / gpus,
                perf.gpu[1] / gpus, perf.gpu[2] / gpus, perf.gpu[3] / gpus);
            perf = PerfClock{};
            perf.lastPresent = perf.lastLog = t;
        }
    }
    if (!presentationChain || resetQuarantined || !nativeUiDeviceCurrent(d) ||
        !nativePresentationThreadCurrent() || uiHalted || stopping) return;
    pollSharedFences();
    static std::atomic<bool> first{false};
    if (!first.load(std::memory_order_relaxed) && !first.exchange(true, std::memory_order_relaxed))
        logStartup("first device Present", d, hooksReady.load(std::memory_order_acquire));
    nativeUiFault(); // A nested presentation cannot reuse the retained eye resources.
    if (pendingKey && GetTickCount64() >= releaseKeyAt) {
        keyEvent(pendingKey, true);
        pendingKey = 0;
    }
    retireDeferred();
    if (!device || !hooksReady || uiHalted)
        return;
    if (!channel.shared)
        startChannel();
    if (!channel.shared || !allocate(false))
        return;
    // Readiness precedes tracking admission in engine::update. Attempt stereo
    // capability independently; menu readback remains usable without depth.
    const bool stereoReady = allocate();
    Input input{};
    Ui ui{};
    uint32_t recommendedWidth = 0, recommendedHeight = 0;
    {
        Lock l(channel);
        if (!l)
            return;
        input = channel.shared->latest;
        ui = channel.shared->ui;
        recommendedWidth = channel.shared->recommendedWidth;
        recommendedHeight = channel.shared->recommendedHeight;
    }
    requestEyeCanvas(recommendedWidth, recommendedHeight);
    uint64_t now = GetTickCount64();
    bool gameplay = ui.gameplay && now >= ui.tickMs && now - ui.tickMs < 250;
    const bool nativeMenu = menus::active();
    // Every frame the stereo world path cannot own is presented on the flat
    // screen: menus, loading, pre-rendered movies, cinematic cameras, Netricsa,
    // death views, and gameplay without stereo resources. Only native menus
    // accept pointer interaction; Confirm/Back keys work for all of them.
    const bool menuVisible = nativeMenu || !gameplay || !stereoReady;
    autoContinue(nativeMenu, nativeMenu || !gameplay, now);
    static unsigned diagnostics = 0, previousClassification = ~0u;
    const unsigned classification = unsigned(nativeMenu) | (unsigned(gameplay) << 1) | (unsigned(stereoReady) << 2);
    if (diagnostics < 16 && classification != previousClassification) {
        ++diagnostics;
        previousClassification = classification;
        log("Native frame classification menu=%d gameplayEnabled=%d stereoReady=%d uiAge=%llu",
            nativeMenu, gameplay, stereoReady, static_cast<unsigned long long>(now >= ui.tickMs ? now-ui.tickMs : 0));
    }
    bool valid = input.focused && input.headValid && now >= input.tickMs && now - input.tickMs < 200;
    if (valid) vrInputTick.store(now, std::memory_order_relaxed);
    MenuAction action = menuNavigation.sample(input, menuVisible, valid, menus::pointerAvailable());
    // The weapon wheel's menu bubble opens the pause menu exactly as Menu does.
    if (wheelMenuRequested(now) && action == MenuAction::None && valid && !menuVisible)
        action = MenuAction::Back;
    WORD key = 0;
    int command = 0; // Sam2Game menu system commands (syscmdOk, syscmdBack, ...).
    switch (action) {
    case MenuAction::Back:
        key = VK_ESCAPE; command = 0x0c;
        break;
    case MenuAction::Confirm:
        key = VK_RETURN; command = 0x0b;
        break;
    case MenuAction::Up:
        key = VK_UP; command = 0x0f;
        break;
    case MenuAction::Down:
        key = VK_DOWN; command = 0x10;
        break;
    case MenuAction::Left:
        key = VK_LEFT; command = 0x0d;
        break;
    case MenuAction::Right:
        key = VK_RIGHT; command = 0x0e;
        break;
    default:
        break;
    }
    // Controller-prompt X/Y menu functions are syscmdF1/F2 in the menus:
    // Touch X/Y, Index left A / left trackpad.
    {
        static uint32_t previousExtras = 0;
        // The wheel-open button took over Touch Y / Index left trackpad: still F2 here.
        const uint32_t extras = valid ? ((input.buttons[0] | input.buttons[1]) & (Reload | Netricsa | WheelOpen)) : 0;
        const uint32_t pressed = extras & ~previousExtras;
        previousExtras = extras;
        if (nativeMenu && !key && pressed)
            menus::queueCommand((pressed & Reload) ? 0x15 : 0x16);
    }
    // Native menus take commands through their own dispatcher. Other screens
    // (gameplay pause, loading prompts, movies) get a key posted to the game
    // window; neither path needs window focus.
    if (key && !(nativeMenu && menus::queueCommand(command)))
        keyTap(key);
    if (!valid && pendingKey) {
        keyEvent(pendingKey, true);
        pendingKey = 0;
    }
    {
        Lock l(channel);
        if (l)
            channel.shared->menu.visible = menuVisible ? 1 : 0;
    }
    if (!menuVisible || now - lastMenuCopy < 33)
        return;
    uint32_t menuGeneration = nativeMenu ? menus::captureGeneration() : 0;
    lastMenuCopy = now;
    const auto generation = resourceGeneration.load(std::memory_order_acquire);
    const uint32_t copyWidth = width, copyHeight = height;
    IDirect3DSurface9 *backbuffer = nullptr, *cpu = nullptr;
    bool locked = false, lockAttempted = false, lockReturned = false, held = false;
    // The game letterboxes its 2D view to gfx_fAspectRatio inside the (eye-
    // sized) canvas; the flat screen shows only that band, at its own aspect.
    // Its menus are drawn over the whole canvas (a 4:3 box filling the square
    // eye canvas): the band cut off their top and bottom, so a native menu shows
    // the whole canvas (the pointer maps image -> canvas either way).
    // Native menus lay out a 640x480 (4:3) space stretched over the whole canvas
    // (V481/V482: the cursor spans its full width and height), so on the square
    // eye canvas they looked squeezed: their image is the canvas scaled to 4:3.
    const UiBand source = nativeMenu ? UiBand{0, 0, copyWidth, copyHeight} : uiBand(copyWidth, copyHeight);
    const auto scaled = [&](float sourceWidth, uint32_t &w, uint32_t &h) {
        const float fit = std::min({1.f, 1920.f / std::max(1.f, sourceWidth), 1080.f / float(std::max(1u, source.height))});
        w = std::max(2u, uint32_t(sourceWidth * fit)) & ~1u;
        h = std::max(2u, uint32_t(float(source.height) * fit)) & ~1u;
    };
    uint32_t outWidth = 0, outHeight = 0;
    scaled(nativeMenu ? float(source.height) * 4.f / 3.f : float(source.width), outWidth, outHeight);
    // The scaled image must fit the canvas-sized pixel buffer. (Bounding the
    // unscaled 4:3 width instead left every eye canvas, all narrower than 4:3,
    // at the canvas aspect: 1344x1512 was still published as 960x1080.)
    if (uint64_t(outWidth) * outHeight > uint64_t(copyWidth) * copyHeight) scaled(float(source.width), outWidth, outHeight);
    withNativeFinally([&] {
        if (FAILED(sourceBackbuffer(&backbuffer))) return;
        if (menuScaledWidth != outWidth || menuScaledHeight != outHeight || !menuScaled || !menuScaledRead) {
            releaseMenuScaled();
            if (FAILED(device->CreateRenderTarget(outWidth, outHeight, colorFormat, D3DMULTISAMPLE_NONE, 0, FALSE,
                                                  &menuScaled, nullptr)) ||
                FAILED(device->CreateOffscreenPlainSurface(outWidth, outHeight, colorFormat, D3DPOOL_SYSTEMMEM,
                                                           &menuScaledRead, nullptr))) { releaseMenuScaled(); return; }
            menuScaledWidth = outWidth; menuScaledHeight = outHeight;
        }
        const RECT bandRect{LONG(source.x), LONG(source.y), LONG(source.x + source.width), LONG(source.y + source.height)};
        if (FAILED(originalStretch(device, backbuffer, &bandRect, menuScaled, nullptr, D3DTEXF_LINEAR))) return;
        cpu = menuScaledRead; cpu->AddRef();
        if (FAILED(device->GetRenderTargetData(menuScaled, cpu)) ||
            generation != resourceGeneration.load(std::memory_order_acquire)) return;
        backbuffer->Release(); backbuffer = nullptr;
        D3DLOCKED_RECT pixels{};
        lockAttempted = true;
        const auto result = cpu->LockRect(&pixels, nullptr, D3DLOCK_READONLY);
        lockReturned = true; locked = SUCCEEDED(result);
        if (!locked || !pixels.pBits || pixels.Pitch < int(outWidth * 4) ||
            generation != resourceGeneration.load(std::memory_order_acquire)) return;
        uint8_t *target = pixelStore.matches(pixelStore.generation, copyWidth, copyHeight) ?
            pixelStore.buffer(MenuPixelBuffer) : nullptr;
        if (!target) return;
        const UiBand band{0, 0, outWidth, outHeight};
        held = channel.lock(5);
        if (!held) return;
        auto &m = channel.shared->menu;
        for (uint32_t y = 0; y < band.height; ++y)
            memcpy(target + size_t(y) * band.width * 4,
                static_cast<uint8_t *>(pixels.pBits) + size_t(y + band.y) * pixels.Pitch + size_t(band.x) * 4,
                band.width * 4);
        for (size_t pixel = 3; pixel < size_t(band.width) * band.height * 4; pixel += 4) target[pixel] = 255;
        m.width = band.width; m.height = band.height; m.tickMs = now;
        m.pixelGeneration = pixelStore.generation; m.pixelReserved = 0;
        m.sequence = ++menuSequence; m.interactionGeneration = menuGeneration; m.visible = 1;
    }, [&](bool aborted) noexcept {
        if (held) { channel.unlock(); held = false; }
        const bool uncertain = aborted && lockAttempted && !lockReturned;
        const bool unlockFailed = locked && cpu && FAILED(cpu->UnlockRect());
        locked = false;
        if (cpu && (uncertain || unlockFailed)) {
            uiHalted = true;
            uiQuarantinedReadback = cpu; cpu = nullptr;
        }
        if (cpu) { cpu->Release(); cpu = nullptr; }
        if (backbuffer) { backbuffer->Release(); backbuffer = nullptr; }
        if (aborted) { uiHalted = true; nestedNativePresentation(); }
    });
}
// The replay and single-pass modules hook the same device methods; the first
// stereo frame (after the settings are known) installs the one in use.
static void ensureStereoHooks(IDirect3DDevice9 *d) noexcept {
    static bool done = false;
    if (done || !originalDraw || !originalDrawIndexed || !originalClear) return;
    done = true;
    if (singlePassEnabled) {
        single_pass::Originals originals;
        originals.draw = originalDraw; originals.drawIndexed = originalDrawIndexed; originals.clear = originalClear;
        originals.drawUp = originalDrawUp; originals.drawIndexedUp = originalDrawIndexedUp;
        single_pass::install(d, originals);
    } else if (eyeReplay) {
        replay::Originals originals;
        originals.setRT = originalSetRT; originals.setDepth = originalSetDepth; originals.clear = originalClear;
        originals.stretch = originalStretch; originals.fill = originalFill; originals.draw = originalDraw;
        originals.drawIndexed = originalDrawIndexed;
        replay::install(d, originals);
    }
}
// Lab (SS2VR_LAB_REPLAY_VERIFY=1): on sampled frames the right eye is replayed,
// copied aside, then rendered natively over it; the two images are compared and,
// when they differ, written beside the game as PPMs (replay-N.ppm, native-N.ppm).
namespace replay_compare {
IDirect3DSurface9 *copy = nullptr;
UINT copyWidth = 0, copyHeight = 0;
unsigned frames = 0, written = 0;
bool due() noexcept { return ++frames % 45 == 0; }
// SS2VR_LAB_REPLAY_BISECT=x0,y0,x1,y1: find the draw whose replay puts wrong
// pixels in that rectangle (skip half of [lo,hi) per compared frame).
struct Bisect { bool on = false; int x0 = 0, y0 = 0, x1 = 0, y1 = 0; unsigned lo = 0, hi = 0, baseline = 0, step = 0; bool done = false; };
Bisect bisect = [] {
    Bisect b; wchar_t v[64]{};
    if (GetEnvironmentVariableW(L"SS2VR_LAB_REPLAY_BISECT", v, 64)) b.on = swscanf(v, L"%d,%d,%d,%d", &b.x0, &b.y0, &b.x1, &b.y1) == 4;
    return b;
}();
void prepare() noexcept {
    if (!bisect.on || bisect.done) return;
    if (!bisect.hi) { replay::labSkipDraws(0, 0, ~0u); return; } // Baseline frame.
    const unsigned mid = (bisect.lo + bisect.hi) / 2;
    replay::labSkipDraws(bisect.lo, mid, bisect.hi - bisect.lo == 1 ? bisect.lo : ~0u);
}
bool keep(IDirect3DDevice9 *d, IDirect3DSurface9 *target) noexcept {
    D3DSURFACE_DESC desc{};
    if (!target || FAILED(target->GetDesc(&desc))) return false;
    if (!copy || copyWidth != desc.Width || copyHeight != desc.Height) {
        if (copy) copy->Release();
        copy = nullptr;
        if (FAILED(d->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &copy, nullptr)))
            return false;
        copyWidth = desc.Width; copyHeight = desc.Height;
    }
    return SUCCEEDED(originalStretch(d, target, nullptr, copy, nullptr, D3DTEXF_NONE));
}
static bool read(IDirect3DDevice9 *d, IDirect3DSurface9 *rt, std::vector<uint8_t> &out) {
    D3DSURFACE_DESC desc{};
    IDirect3DSurface9 *cpu = nullptr;
    if (FAILED(rt->GetDesc(&desc)) ||
        FAILED(d->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &cpu, nullptr)))
        return false;
    bool ok = SUCCEEDED(d->GetRenderTargetData(rt, cpu));
    D3DLOCKED_RECT r{};
    if (ok && SUCCEEDED(cpu->LockRect(&r, nullptr, D3DLOCK_READONLY))) {
        out.resize(size_t(desc.Width) * desc.Height * 4);
        for (UINT y = 0; y < desc.Height; ++y)
            memcpy(out.data() + size_t(y) * desc.Width * 4, static_cast<uint8_t *>(r.pBits) + size_t(y) * r.Pitch, desc.Width * 4);
        cpu->UnlockRect();
    } else ok = false;
    cpu->Release();
    return ok;
}
static void write(const wchar_t *name, const std::vector<uint8_t> &bgra, UINT w, UINT h) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t *slash = wcsrchr(path, L'\\')) slash[1] = 0;
    wcscat_s(path, name);
    if (FILE *f = _wfopen(path, L"wb")) {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        std::vector<uint8_t> row(size_t(w) * 3);
        for (UINT y = 0; y < h; ++y) {
            for (UINT x = 0; x < w; ++x) {
                const uint8_t *p = bgra.data() + (size_t(y) * w + x) * 4;
                row[x*3] = p[2]; row[x*3+1] = p[1]; row[x*3+2] = p[0];
            }
            fwrite(row.data(), 1, row.size(), f);
        }
        fclose(f);
    }
}
size_t differingBetween(IDirect3DDevice9 *d, IDirect3DSurface9 *a, IDirect3DSurface9 *b) noexcept {
    std::vector<uint8_t> pa, pb;
    D3DSURFACE_DESC desc{};
    if (!a || !b || FAILED(a->GetDesc(&desc)) || !read(d, a, pa) || !read(d, b, pb) || pa.size() != pb.size()) return 0;
    size_t count = 0;
    for (size_t i = 0; i < pa.size(); i += 4)
        if (std::abs(int(pa[i]) - pb[i]) + std::abs(int(pa[i+1]) - pb[i+1]) + std::abs(int(pa[i+2]) - pb[i+2]) > 30) ++count;
    static unsigned saved = 0;
    if (count > 4 && saved < 12) {
        wchar_t name[64];
        swprintf(name, 64, L"mirror-a-%u.ppm", saved); write(name, pa, desc.Width, desc.Height);
        swprintf(name, 64, L"mirror-b-%u.ppm", saved); write(name, pb, desc.Width, desc.Height);
        ++saved;
    }
    return count;
}
double regionLuma(IDirect3DDevice9 *d, IDirect3DSurface9 *target, int x0, int y0, int x1, int y1) noexcept {
    std::vector<uint8_t> pixels;
    D3DSURFACE_DESC desc{};
    if (!target || FAILED(target->GetDesc(&desc)) || !read(d, target, pixels)) return -1;
    double sum = 0; size_t n = 0;
    static const wchar_t kind = [] { wchar_t v[4]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SP_METRIC", v, 4) ? v[0] : L'l'; }();
    if (kind == L's') { // Seam: median luma step between columns x0 and x1 over rows y0..y1.
        std::vector<double> steps;
        auto luma = [&](int x, int y) { const uint8_t *p = pixels.data() + (size_t(y) * desc.Width + x) * 4; return .2126 * p[2] + .7152 * p[1] + .0722 * p[0]; };
        for (int y = std::max(0, y0); y < std::min<int>(y1, desc.Height); ++y)
            if (x0 >= 0 && x1 < int(desc.Width)) steps.push_back(std::abs(luma(x1, y) - luma(x0, y)));
        if (steps.empty()) return -1;
        std::nth_element(steps.begin(), steps.begin() + steps.size() / 2, steps.end());
        return steps[steps.size() / 2];
    }
    for (int y = std::max(0, y0); y < std::min<int>(y1, desc.Height); ++y)
        for (int x = std::max(0, x0); x < std::min<int>(x1, desc.Width); ++x) {
            const uint8_t *p = pixels.data() + (size_t(y) * desc.Width + x) * 4;
            static const wchar_t metric = [] { wchar_t v[4]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SP_METRIC", v, 4) ? v[0] : L'l'; }();
            if (metric == L'r') sum += (p[2] > 170 && p[1] < 90 && p[0] < 90) ? 255. : 0.;
            else if (metric == L'd') sum += (.2126 * p[2] + .7152 * p[1] + .0722 * p[0]) < 90 ? 255. : 0.;
            else sum += .2126 * p[2] + .7152 * p[1] + .0722 * p[0];
            ++n;
        }
    return n ? sum / n : -1;
}
size_t differing(IDirect3DDevice9 *d, IDirect3DSurface9 *target) noexcept {
    std::vector<uint8_t> now, kept;
    if (!copy || !read(d, target, now) || !read(d, copy, kept) || now.size() != kept.size()) return 0;
    size_t count = 0;
    for (size_t i = 0; i < now.size(); i += 4)
        if (std::abs(int(now[i]) - kept[i]) + std::abs(int(now[i+1]) - kept[i+1]) + std::abs(int(now[i+2]) - kept[i+2]) > 60) ++count;
    if (count > 200 && written < 4) {
        wchar_t name[64];
        swprintf(name, 64, L"sp-kept-%u.ppm", written); write(name, kept, copyWidth, copyHeight);
        swprintf(name, 64, L"sp-now-%u.ppm", written); write(name, now, copyWidth, copyHeight);
        ++written;
    }
    return count;
}
void compare(IDirect3DDevice9 *d, IDirect3DSurface9 *target) noexcept {
    std::vector<uint8_t> native, replayed;
    if (!copy || !read(d, target, native) || !read(d, copy, replayed) || native.size() != replayed.size()) return;
    size_t differing = 0;
    for (size_t i = 0; i < native.size(); i += 4) {
        const int diff = std::abs(int(native[i]) - replayed[i]) + std::abs(int(native[i+1]) - replayed[i+1]) +
                         std::abs(int(native[i+2]) - replayed[i+2]);
        if (diff > 90) ++differing;
    }
    log("Eye replay compare frame=%u differing=%zu of %zu pixels", frames, differing, native.size() / 4);
    if (bisect.on && !bisect.done) {
        unsigned region = 0;
        for (int y = std::max(0, bisect.y0); y < std::min<int>(bisect.y1, copyHeight); ++y)
            for (int x = std::max(0, bisect.x0); x < std::min<int>(bisect.x1, copyWidth); ++x) {
                const size_t i = (size_t(y) * copyWidth + x) * 4;
                // Whitish replay pixels where the native image has none (the misplaced object).
                const int lo = std::min({int(replayed[i]), int(replayed[i+1]), int(replayed[i+2])});
                const int nlo = std::min({int(native[i]), int(native[i+1]), int(native[i+2])});
                if (lo > 185 && nlo < 150) ++region;
            }
        if (!bisect.hi) { bisect.baseline = region; bisect.lo = 0; bisect.hi = replay::labDrawCount(); }
        else {
            const unsigned mid = (bisect.lo + bisect.hi) / 2;
            const bool inLower = region * 2 < bisect.baseline; // Skipping [lo,mid) removed the artifact.
            if (bisect.hi - bisect.lo == 1) bisect.done = true;
            else if (inLower) bisect.hi = mid; else bisect.lo = mid;
        }
        log("Eye replay bisect step=%u region=%u baseline=%u range=[%u,%u)", bisect.step++, region, bisect.baseline, bisect.lo, bisect.hi);
    }
    if (written < 8 && differing > native.size() / 4 / 1000) {
        wchar_t name[64];
        swprintf(name, 64, L"replay-%u.ppm", written); write(name, replayed, copyWidth, copyHeight);
        swprintf(name, 64, L"native-%u.ppm", written); write(name, native, copyWidth, copyHeight);
        ++written;
    }
}
} // namespace replay_compare
// Lab (SS2VR_LAB_STAGE_DUMP=1): the right eye image at stages of the 1500th stereo pair.
static unsigned labStagePair = 0;
static void labStageDump(IDirect3DDevice9 *d, IDirect3DSurface9 *target, const wchar_t *name) noexcept {
    static const bool dump = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_STAGE_DUMP", v, 2) == 1 && v[0] == L'1'; }();
    std::vector<uint8_t> px; D3DSURFACE_DESC desc{};
    if (dump && labStagePair == 1500 && target && SUCCEEDED(target->GetDesc(&desc)) && replay_compare::read(d, target, px))
        replay_compare::write(name, px, desc.Width, desc.Height);
}
// Lab (SS2VR_LAB_HUD_DUMP=1): the HUD layer's colour and alpha at its 1500th flush.
static void labDumpHudLayer(IDirect3DDevice9 *d) noexcept {
    static const bool dump = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_HUD_DUMP", v, 2) == 1 && v[0] == L'1'; }();
    static unsigned flushes = 0;
    std::vector<uint8_t> px; D3DSURFACE_DESC desc{};
    if (!dump || ++flushes != 1500 || !hudLayerSurface || FAILED(hudLayerSurface->GetDesc(&desc)) || !replay_compare::read(d, hudLayerSurface, px))
        return;
    replay_compare::write(L"hud-color.ppm", px, desc.Width, desc.Height);
    for (size_t i = 0; i < px.size(); i += 4) px[i] = px[i + 1] = px[i + 2] = px[i + 3];
    replay_compare::write(L"hud-alpha.ppm", px, desc.Width, desc.Height);
}
static bool eyeTargetCurrent(int index) {
    D3DVIEWPORT9 viewport{};
    const bool matches=index>=0 && index<2 && activeEye==index && uiFrame.device &&
        SUCCEEDED(uiFrame.device->GetRenderTarget(0,&uiFrame.probeColor)) &&
        SUCCEEDED(uiFrame.device->GetDepthStencilSurface(&uiFrame.probeDepth)) &&
        SUCCEEDED(uiFrame.device->GetViewport(&viewport)) &&
        sameSurface(uiFrame.probeColor,uiFrame.target[index]) && sameSurface(uiFrame.probeDepth,uiFrame.z[index]) &&
        viewport.X==0 && viewport.Y==0 && viewport.Width==uiFrame.request.width && viewport.Height==uiFrame.request.height;
    if(uiFrame.probeColor) uiFrame.probeColor->Release();
    if(uiFrame.probeDepth) uiFrame.probeDepth->Release();
    uiFrame.probeColor=uiFrame.probeDepth=nullptr;
    return matches;
}
// SetRenderTarget resets the D3D viewport, but does not update the native
// renderer's cached depth endpoints. Carry the pre-bind device endpoints into
// the reshaped viewport so an equal-range native call may safely be elided.
static bool preserveViewportDepth(IDirect3DDevice9 *d,D3DVIEWPORT9 &out) {
    D3DVIEWPORT9 current{};
    if(FAILED(d->GetViewport(&current)) || !std::isfinite(current.MinZ) || !std::isfinite(current.MaxZ) ||
        current.MinZ<0 || current.MaxZ>1 || current.MinZ>current.MaxZ) return false;
    out.MinZ=current.MinZ;out.MaxZ=current.MaxZ;
    return true;
}
static bool restoreUiFrame() noexcept {
    if (!uiFrame.restore) return true;
    if (uiFrame.generation != graphicsResourceGeneration()) { uiFrame.restore = false; return false; }
    bool ok = true;
    uiBypass = true;
    auto *d=uiFrame.device;
    auto viewport=uiFrame.viewport;
    ok=preserveViewportDepth(d,viewport) && ok;
    ok = SUCCEEDED(originalSetRT(d,0,uiFrame.color)) && ok;
    ok = SUCCEEDED(originalSetDepth(d,uiFrame.ds)) && ok;
    ok = SUCCEEDED(d->SetViewport(&viewport)) && ok;
    ok = SUCCEEDED(d->SetScissorRect(&uiFrame.scissor)) && ok;
    uiBypass = false;
    uiFrame.restore = false;
    if (!ok) { uiFrame.fault = true; uiHalted = true; }
    return ok;
}
static void releaseUiFrame() noexcept {
    remote_render::retirePresentation(uiFrame.remoteOwner);
    uiFrame.remoteOwner=0;
    retireNativeUiLock(uiFrame.locked,
        [](IDirect3DSurface9 *surface) {
            if (FAILED(surface->UnlockRect())) return false;
            surface->Release(); // Explicit lock reference, separate from frame/global refs.
            return true;
        },[] { uiFrame.fault=true; uiHalted=true; });
    if (uiFrame.locked) {
        // Halt already prevents world AND menu readback reuse. Transfer the lock
        // reference for retirement at deviceLost, never assume it was unlocked.
        uiQuarantinedReadback=uiFrame.locked;
        uiFrame.locked=nullptr;
    }
    if (uiFrame.probeColor) uiFrame.probeColor->Release();
    if (uiFrame.probeDepth) uiFrame.probeDepth->Release();
    uiFrame.probeColor=uiFrame.probeDepth=nullptr;
    if (uiFrame.backbuffer) uiFrame.backbuffer->Release();
    if (uiFrame.extra) uiFrame.extra->Release();
    if (uiFrame.chain) uiFrame.chain->Release();
    uiFrame.chain=nullptr;
    uiFrame.backbuffer=uiFrame.extra=nullptr;
    if (uiFrame.color) uiFrame.color->Release();
    if (uiFrame.ds) uiFrame.ds->Release();
    for (unsigned i=0;i<2;++i) {
        if (uiFrame.target[i]) uiFrame.target[i]->Release();
        if (uiFrame.z[i]) uiFrame.z[i]->Release();
        if (uiFrame.read[i]) uiFrame.read[i]->Release();
        uiFrame.target[i]=uiFrame.z[i]=uiFrame.read[i]=nullptr;
        uiFrame.pixels[i] = nullptr;
        uiFrame.insetRect[i] = {};
    }
    uiFrame.pixelBytes = 0;
    uiFrame.pixelGeneration = 0;
    for (unsigned i=0;i<2;++i) {
        auto *texture = uiFrame.scopeImage[i]; uiFrame.scopeImage[i] = nullptr;
        if (texture) texture->Release();
        uiFrame.scopeReady[i] = false; uiFrame.scopeView[i] = {};
    }
    auto *shader = uiFrame.scopeShader; uiFrame.scopeShader = nullptr;
    if (shader) shader->Release();
    if (uiFrame.device) uiFrame.device->Release();
    uiFrame.device=nullptr;
    uiFrame.color=uiFrame.ds=nullptr;
    if (uiFrame.slot>=0 && !uiFrame.published) {
        if (!uiFrame.world) ++perf.lostNoWorld;
        else if (uiFrame.fault) ++perf.lostFault;
        else if (uiFrame.ui && !uiFrame.overlaySeen) ++perf.lostNoOverlay;
        else if (uiFrame.ui && !uiFrame.complete) ++perf.lostIncomplete;
        else ++perf.lostOther;
        if (uiFrame.fault) {
            perf.lostReason = uiFrame.faultReason;
            perf.lostOrigin = uiFrame.faultOrigin ? uiFrame.faultOrigin - reinterpret_cast<uintptr_t>(&__ImageBase) : 0;
            perf.lostWorldOk = uiFrame.world; perf.lostUi = uiFrame.ui;
            perf.lostOverlaySeen = uiFrame.overlaySeen; perf.lostComplete = uiFrame.complete;
        }
    }
    uiFrame.player=nullptr;
    uiFrame.slot=-1;
    admittedUiFrame=nullptr;
    uiFrame.ui=uiFrame.fault=uiFrame.world=uiFrame.restore=uiFrame.zeroCopy=uiFrame.published=false;
    uiFrame.overlay=uiFrame.overlaySeen=uiFrame.complete=uiFrame.fade=false;
    uiFrame.panelHidden[0]=uiFrame.panelHidden[1]=false;
    uiFrame.faultOrigin=0;
    uiFrame.faultReason="none";
    uiFrame.diagnosticElementCount=0;
    uiFrame.diagnosticShaderVersion=uiFrame.diagnosticShaderBytes=0;
    uiFrame.diagnosticDcl=false;
}
void nativeUiEndOwner(bool aborted) noexcept {
    if (uiFrame.slot<0) return;
    if (aborted) { uiFrame.fault=true; uiHalted=true; }
    if (scopeScratch) scopeSourcesHalted = true; // Any early owner retirement leaves native cleanup unproved.
    scopeScratch = false;
    scopeGpuTransactionEnd();
    cleanupUiDraw(aborted);
    activeEye=-1;
    eyeInvalid=false;
    endEye();
    desktopTarget=desktopDepth=nullptr;
    restoreUiFrame();
    deferredSlot=uiFrame.slot;
    deferredSequence=uiFrame.request.sequence;
    releaseUiFrame();
    retireDeferred();
}
bool nativeUiBeginOverlay(void *player,bool admitted) {
    if (uiFrame.slot<0 || !uiFrame.ui) return false;
    const bool structure=admitted && player==uiFrame.player && uiFrame.world && !uiFrame.overlaySeen;
    const bool current=structure && uiCurrent();
    if (!current) {
        static unsigned diagnostics=0;
        if(diagnostics<4) {
            ++diagnostics;
            log("Native UI overlay rejected admitted=%u player=%u world=%u seen=%u fault=%u current=%u",
                admitted,player==uiFrame.player,uiFrame.world,uiFrame.overlaySeen,uiFrame.fault,current);
        }
        nativeUiFault(); return false;
    }
    uiFrame.overlaySeen=true;
    uiFrame.overlay=true;
    uiFrame.hudLayerDirty=uiFrame.hudLayerCleared=false;
    return true;
}
void nativeUiEndOverlay(bool completed) noexcept {
    if (uiFrame.slot<0 || !uiFrame.ui) return;
    if (completed && uiFrame.overlay && !uiFrame.fade && !uiDraw.busy && !uiFrame.fault && uiFrame.device)
        flushHudLayer(uiFrame.device);
    if (uiFrame.device) labStageDump(uiFrame.device, uiFrame.target[1], L"stage-d-overlay.ppm");
    uiFrame.hudLayerDirty=false;
    if (!completed || !uiFrame.overlay || uiFrame.fade || uiDraw.busy) nativeUiFault();
    uiFrame.complete=completed && uiFrame.overlay && !uiFrame.fault;
    uiFrame.overlay=false;
    uiFrame.fade=false;
}
bool nativeUiBeginFade() {
    if (!uiFrame.overlay || !uiFrame.ui) return false;
    if (uiFrame.fade || !uiCurrent()) { nativeUiFault(); return false; }
    // Fades cover the whole field of view above the HUD drawn so far.
    if (uiFrame.hudLayerDirty && !flushHudLayer(uiFrame.device)) return false;
    uiFrame.fade=true;
    return true;
}
void nativeUiEndFade(bool completed) noexcept {
    if (!completed) nativeUiFault();
    uiFrame.fade=false;
}
// Copy one locked readback surface into the eye's pixel buffer at `offset`.
static bool copyLockedRows(IDirect3DSurface9 *read, uint8_t *target, uint32_t width, uint32_t height) {
    D3DLOCKED_RECT locked{};
    if (FAILED(read->LockRect(&locked,nullptr,D3DLOCK_READONLY))) return false;
    uiFrame.locked=read;
    read->AddRef();
    const size_t rowBytes=size_t(width)*4;
    bool ok=locked.pBits && locked.Pitch>=int(rowBytes);
    if (ok)
        for (uint32_t row=0;row<height;++row)
            memcpy(target+size_t(row)*rowBytes,static_cast<uint8_t *>(locked.pBits)+size_t(row)*locked.Pitch,rowBytes);
    return retireNativeUiLock(uiFrame.locked,
        [](IDirect3DSurface9 *surface) {
            if (FAILED(surface->UnlockRect())) return false;
            surface->Release();
            return true;
        },[] { uiFrame.fault=true; uiHalted=true; }) && ok;
}
// Foveated readback: a filtered downsample of the whole eye and an unscaled
// crop around the requested gaze, packed into the eye's pixel buffer.
static bool readFoveatedEye(unsigned i) {
    auto *d=uiFrame.device;
    const auto &q=uiFrame.request;
    const uint32_t width[2]{q.lowWidth,q.insetWidth}, height[2]{q.lowHeight,q.insetHeight};
    if (!foveationFits(q.width,q.height,q.lowWidth,q.lowHeight,q.insetWidth,q.insetHeight) || !uiFrame.pixels[i] ||
        pixelStore.generation!=uiFrame.pixelGeneration) return false;
    for (unsigned k=0;k<2;++k) {
        if (foveaTarget[i][k] && foveaWidth[i][k]==width[k] && foveaHeight[i][k]==height[k]) continue;
        if (foveaTarget[i][k]) foveaTarget[i][k]->Release();
        if (foveaRead[i][k]) foveaRead[i][k]->Release();
        foveaTarget[i][k]=foveaRead[i][k]=nullptr;
        foveaWidth[i][k]=foveaHeight[i][k]=0;
        if (FAILED(d->CreateRenderTarget(width[k],height[k],colorFormat,D3DMULTISAMPLE_NONE,0,FALSE,&foveaTarget[i][k],nullptr)) ||
            FAILED(d->CreateOffscreenPlainSurface(width[k],height[k],colorFormat,D3DPOOL_SYSTEMMEM,&foveaRead[i][k],nullptr)))
            return false;
        foveaWidth[i][k]=width[k]; foveaHeight[i][k]=height[k];
    }
    const PixelRect rect=insetCrop(q.fov[i],q.width,q.height,q.insetWidth,q.insetHeight,q.insetCentre[i][0],q.insetCentre[i][1]);
    const RECT crop{rect.x,rect.y,rect.x+rect.w,rect.y+rect.h};
    if (FAILED(originalStretch(d,uiFrame.target[i],nullptr,foveaTarget[i][0],nullptr,D3DTEXF_LINEAR)) ||
        FAILED(originalStretch(d,uiFrame.target[i],&crop,foveaTarget[i][1],nullptr,D3DTEXF_NONE)))
        return false;
    uint8_t *target=uiFrame.pixels[i];
    for (unsigned k=0;k<2;++k) {
        if (FAILED(d->GetRenderTargetData(foveaTarget[i][k],foveaRead[i][k])) ||
            !copyLockedRows(foveaRead[i][k],target,width[k],height[k])) return false;
        target+=size_t(width[k])*height[k]*4;
    }
    uiFrame.insetRect[i]=rect;
    static unsigned traced=0;
    if (traced<2) {
        ++traced;
        log("Foveated readback eye=%u full=%ux%u low=%ux%u inset=%ux%u at %d,%d",i,q.width,q.height,
            q.lowWidth,q.lowHeight,q.insetWidth,q.insetHeight,rect.x,rect.y);
    }
    return true;
}
static bool readUiEye(unsigned i,bool dim,float visibility) {
    if (!dim && uiFrame.request.foveated == 1) return readFoveatedEye(i);
    auto *d=uiFrame.device;
    auto *read=uiFrame.read[i];
    const LARGE_INTEGER t0=PerfClock::now();
    if (FAILED(d->GetRenderTargetData(uiFrame.target[i],read))) return false;
    const LARGE_INTEGER t1=PerfClock::now();
    D3DLOCKED_RECT locked{};
    if (FAILED(read->LockRect(&locked,nullptr,dim ? 0 : D3DLOCK_READONLY))) return false;
    const LARGE_INTEGER t2=PerfClock::now();
    perf.readTarget+=PerfClock::seconds(t0,t1); perf.lock+=PerfClock::seconds(t1,t2); ++perf.readbacks;
    uiFrame.locked=read;
    read->AddRef();
    bool ok=locked.pBits && locked.Pitch>=int(uiFrame.request.width*4);
    if (ok) {
        const size_t rowBytes=size_t(uiFrame.request.width)*4;
        ok=dim || (uiFrame.pixels[i] && uiFrame.pixelBytes==rowBytes*uiFrame.request.height &&
            pixelStore.generation==uiFrame.pixelGeneration);
        const auto dimming=makeOpaqueDimming(visibility);
        for (uint32_t row=0;row<uiFrame.request.height;++row) {
            auto *source=static_cast<uint8_t *>(locked.pBits)+size_t(row)*locked.Pitch;
            if (dim) dimRgbPixels(source,uiFrame.request.width,dimming);
            else if (ok) memcpy(uiFrame.pixels[i]+size_t(row)*rowBytes,source,rowBytes);
        }
        perf.copy+=PerfClock::seconds(t2,PerfClock::now());
    }
    ok = retireNativeUiLock(uiFrame.locked,
        [](IDirect3DSurface9 *surface) {
            if (FAILED(surface->UnlockRect())) return false;
            surface->Release();
            return true;
        },[] { uiFrame.fault=true; uiHalted=true; }) && ok;
    if (dim && ok) ok=SUCCEEDED(originalUpdate(d,read,nullptr,uiFrame.target[i],nullptr));
    return ok;
}
// End-of-pair marker for a zero-copy slot; flushed so it completes promptly.
static void issueSharedFence() {
    auto *fence = uiFrame.slot >= 0 ? sharedFence[uiFrame.slot] : nullptr;
    if (!fence || FAILED(fence->Issue(D3DISSUE_END))) { nativeUiFault(); return; }
    BOOL done = FALSE;
    fence->GetData(&done, sizeof(done), D3DGETDATA_FLUSH);
}
static bool publishUiFrameBody(uint32_t presentation);
static bool publishUiFrame(uint32_t presentation) {
    const LARGE_INTEGER start=PerfClock::now();
    const bool accepted=publishUiFrameBody(presentation);
    perf.publish+=PerfClock::seconds(start,PerfClock::now());
    if (accepted) { ++perf.pairs; uiFrame.published=true; lastPairTick=GetTickCount64(); }
    return accepted;
}
static bool publishUiFrameBody(uint32_t presentation) {
    if (!channel.shared || !uiFrame.world || uiFrame.fault ||
        uiFrame.generation!=resourceGeneration.load()) return false;
    // No native rendering while IPC is held. commitStereo retains the existing
    // native snapshot/remote binding admission through the Ready transition.
    // A finished pair is worth a short wait: Wine's mutex wake-up can exceed a
    // couple of milliseconds even when the host holds the lock only briefly.
    if (!channel.lock(25)) { ++perf.publishLock; return false; }
    bool accepted=false;
    withNativeFinally([&] {
        auto &slot=channel.shared->slot[uiFrame.slot];
        const bool imagesCurrent=uiFrame.zeroCopy ? sharedGeneration && channel.shared->images.opened==sharedGeneration :
            uiFrame.pixels[0] && uiFrame.pixels[1] && uiFrame.pixelGeneration==channel.shared->pixels.generation;
        if (slot.cancelled) ++perf.publishCancelled;
        else if (slot.state!=SlotState::Rendering) ++perf.publishState;
        else if (!sameRequest(slot.request,uiFrame.request)) ++perf.publishRequest;
        else if (!imagesCurrent) ++perf.publishImages;
        if (!slot.cancelled && slot.state==SlotState::Rendering && sameRequest(slot.request,uiFrame.request) && imagesCurrent) {
            // Eyes were rendered into this slot's shared images, or read back
            // straight into its own pixel sections.
            slot.sharedImage=uiFrame.zeroCopy ? sharedGeneration : 0;
            slot.gpuReady=uiFrame.zeroCopy ? 0 : 1;
            slot.pixelGeneration=uiFrame.pixelGeneration;
            for (unsigned i=0;i<2;++i) {
                const auto &r=uiFrame.insetRect[i];
                slot.insetRect[i][0]=r.x; slot.insetRect[i][1]=r.y; slot.insetRect[i][2]=r.w; slot.insetRect[i][3]=r.h;
            }
            slot.pixelReserved=0;
            slot.presentation=presentation;
            slot.presentationReserved=0;
            accepted=commitStereo(uiFrame.player,uiFrame.request,slot,uiFrame.remoteOwner);
            if (!accepted) ++perf.publishCommit;
            if (accepted) SetEvent(channel.ready);
            else slot.state=SlotState::Empty;
        }
    },[&](bool aborted) noexcept {
        if (aborted) { uiFrame.fault=true; uiHalted=true; }
        channel.unlock();
    });
    if (accepted && uiFrame.zeroCopy) {
        sharedPending[uiFrame.slot]=true;
        sharedPendingSequence[uiFrame.slot]=uiFrame.request.sequence;
        static unsigned zeroCopyDiagnostics = 0;
        if (zeroCopyDiagnostics < 2) {
            ++zeroCopyDiagnostics;
            log("Zero-copy pair published sequence=%llu slot=%d images=%u",
                static_cast<unsigned long long>(uiFrame.request.sequence),uiFrame.slot,sharedGeneration);
        }
    }
    if (accepted && !uiFrame.zeroCopy) {
        static unsigned pairDiagnostics = 0;
        if (pairDiagnostics < 4) {
            ++pairDiagnostics;
            uint64_t hash[2]{14695981039346656037ull,14695981039346656037ull};
            size_t different = 0;
            const auto bytes = size_t(uiFrame.request.width) * uiFrame.request.height * 4;
            for (size_t i=0;i<bytes;++i) {
                for (unsigned eye=0;eye<2;++eye) hash[eye]=(hash[eye]^uiFrame.pixels[eye][i])*1099511628211ull;
                if (uiFrame.pixels[0][i]!=uiFrame.pixels[1][i]) ++different;
            }
            log("Native eye pair accepted sequence=%llu pixels=%ux%u left=%016llx right=%016llx differingBytes=%llu presentation=%u",
                static_cast<unsigned long long>(uiFrame.request.sequence),uiFrame.request.width,uiFrame.request.height,
                static_cast<unsigned long long>(hash[0]),static_cast<unsigned long long>(hash[1]),
                static_cast<unsigned long long>(different),presentation);
        }
    }
    if(accepted) {
        static const bool labTrace=[] { wchar_t value[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_TRACE",value,2)==1 && value[0]==L'1'; }();
        static unsigned receipts=0;
        if(labTrace && receipts<8192) {
            ++receipts;
            log("Lab native pair request=%llu session=%u reference=%u tracking=%u presentation=%u",
                static_cast<unsigned long long>(uiFrame.request.sequence),uiFrame.request.session,
                uiFrame.request.reference,uiFrame.request.trackingGeneration,presentation);
        }
    }
    return accepted;
}
void nativeUiFinishOwner() {
    if (uiFrame.slot<0 || !uiFrame.ui) return;
    const bool structure=uiFrame.complete && !uiFrame.overlay && !uiFrame.fade;
    const bool current=structure && uiCurrent();
    static unsigned diagnostics=0;
    const bool trace=diagnostics<4;
    if(trace) {
        ++diagnostics;
        log("Native UI finish sequence=%llu complete=%u overlay=%u fade=%u seen=%u fault=%u world=%u current=%u origin=%p reason=%s",
            static_cast<unsigned long long>(uiFrame.request.sequence),uiFrame.complete,uiFrame.overlay,uiFrame.fade,
            uiFrame.overlaySeen,uiFrame.fault,uiFrame.world,current,reinterpret_cast<void *>(uiFrame.faultOrigin),uiFrame.faultReason);
        if(uiFrame.diagnosticShaderVersion) log("Native UI device program version=%08lx bytes=%lu firstOpcode=%lu firstLength=%lu dcl=%u usage=%lu index=%lu registerType=%lu register=%lu",
            uiFrame.diagnosticShaderVersion,uiFrame.diagnosticShaderBytes,uiFrame.diagnosticFirstOpcode,uiFrame.diagnosticFirstLength,
            uiFrame.diagnosticDcl,uiFrame.diagnosticDclUsage,uiFrame.diagnosticDclIndex,
            uiFrame.diagnosticDclRegisterType,uiFrame.diagnosticDclRegister);
        for(UINT i=0;i<uiFrame.diagnosticElementCount;++i) {
            const auto &e=uiFrame.diagnosticElements[i];
            log("Native UI declaration element=%u stream=%u offset=%u type=%u method=%u usage=%u index=%u",
                i,e.Stream,e.Offset,e.Type,e.Method,e.Usage,e.UsageIndex);
        }
    }
    if (!current) { ++perf.finishNotCurrent; nativeUiFault(); return; }
    // UI and original full-field fades have now finished in BOTH owned eye RTs.
    gpuProfile.mark(4);
    gpuProfile.end();
    bool left=true,right=true;
    if(uiFrame.zeroCopy) issueSharedFence();
    else { left=readUiEye(0,false,1); right=left && readUiEye(1,false,1); }
    bool ok=left && right;
    if(trace)log("Native UI readback left=%u right=%u",left,right);
    // No alpha pass: the base projection layer is submitted without
    // source-alpha blending, so the compositor ignores the eye images' alpha.
    const bool valid=ok && uiCurrent();
    const bool published=valid && publishUiFrame(NativeUiComplete);
    if(trace) log("Native UI publish readback=%u current=%u published=%u",ok,valid,published);
    if(!published) nativeUiFault();
}
struct ScopeCaptureFrame {
    unsigned hand = 2;
    uint64_t generation = 0;
    bool active = false, copied = false;
    ScopeCaptureOnce once;
};
// Never recycled after an abnormal source invocation. A stale native callback
// first checks the process-lifetime halt and cannot reach released resources.
static thread_local ScopeCaptureFrame scopeCaptureFrame;
static void retireNativeFrameForReset() noexcept {
    remote_render::invalidatePresentationForReset(nativeWorldRenderActive());
    // Release-only retirement; suspended callbacks must not restore old state.
    uiHalted = true;
    uiFrame.restore = false;
    uiDraw.changed = false;
    cleanupUiDraw(false);
    for (auto **owned : {&uiObservedTarget, &uiObservedExtra}) {
        auto *old = *owned; *owned = nullptr;
        if (old) old->Release();
    }
    auto *observed = uiObservedChain; uiObservedChain = nullptr;
    if (observed) observed->Release();
    uiObserverBusy = false;
    if (scopeScratch || scopeTransaction || scopeCaptureFrame.active) scopeSourcesHalted = true;
    scopeCaptureFrame.active = false;
    scopeCaptureFrame.copied = false;
    scopeCaptureFrame.once.rejected = true;
    scopeScratch = false;
    scopeGpuTransactionEnd();
    activeEye = -1; eyeInvalid = true;
    endEye(); desktopTarget = desktopDepth = nullptr;
    if (uiFrame.slot >= 0) {
        deferredSlot = uiFrame.slot;
        deferredSequence = uiFrame.request.sequence;
        releaseUiFrame();
    }
    retireDeferred(); // On contention the existing tuple remains for later retirement.
}
static bool scopeCaptureCurrent(void *opaque) noexcept {
    const auto *capture = static_cast<ScopeCaptureFrame *>(opaque);
    return !scopeSourcesHalted && capture == &scopeCaptureFrame && capture->active && !capture->once.rejected &&
        capture->hand < 2 && scopeScratch && activeEye == -1 && uiFrame.slot >= 0 &&
        !uiFrame.fault && !eyeInvalid && !uiHalted && !stopping &&
        capture->generation == graphicsResourceGeneration() &&
        uiFrame.generation == capture->generation;
}
static HRESULT copyScopePixels(IDirect3DDevice9 *d,IDirect3DSurface9 *from,IDirect3DSurface9 *to) noexcept {
    return originalStretch(d,from,nullptr,to,nullptr,D3DTEXF_NONE);
}
static bool scopeCaptureTransferCurrent(void *opaque) noexcept {
    return scopeCaptureCurrent(opaque) && scopeSourceExecuting() && scopeGpuTransactionCurrent(uiFrame.device);
}
static LARGE_INTEGER scopeCaptureTime{};
static __attribute__((force_align_arg_pointer)) void __cdecl captureNativeScope(void *opaque) noexcept {
    ++perf.scopeCallbacks;
    scopeCaptureTime = PerfClock::now();
    if (!scopeCaptureCurrent(opaque) || !scopeSourceExecuting()) return;
    if (!scopeCaptureFrame.once.enter()) { scopeCaptureFrame.copied = false; return; }
    if (!scopeGpuTransactionBegin(uiFrame.device)) return;
    IDirect3DDevice9 *retainedDevice = nullptr;
    IDirect3DSurface9 *retainedSource = nullptr;
    IDirect3DTexture9 *retainedDestination = nullptr;
    const UINT width = uiFrame.request.width, height = uiFrame.request.height;
    // Independent invocation references survive even a reentrant early frame
    // retirement. All outputs live above the native finally which releases them.
    withNativeFinally([&] {
        retainedDevice = uiFrame.device; retainedDevice->AddRef();
        if (!scopeCaptureTransferCurrent(opaque)) return;
        retainedSource = uiFrame.target[0]; retainedSource->AddRef();
        if (!scopeCaptureTransferCurrent(opaque)) return;
        retainedDestination = uiFrame.scopeImage[scopeCaptureFrame.hand]; retainedDestination->AddRef();
        if (!scopeCaptureTransferCurrent(opaque)) return;
        scopeCaptureFrame.copied = captureScopeSource(retainedDevice,retainedSource,
            retainedDestination,width,height,copyScopePixels,scopeCaptureTransferCurrent,opaque);
    },[&](bool aborted) noexcept {
        if (aborted) { scopeSourcesHalted = true; scopeCaptureFrame.copied = false; }
        if (retainedDestination) { auto *p=retainedDestination; retainedDestination=nullptr; p->Release(); }
        if (retainedSource) { auto *p=retainedSource; retainedSource=nullptr; p->Release(); }
        if (retainedDevice) { auto *p=retainedDevice; retainedDevice=nullptr; p->Release(); }
        if (!scopeCaptureTransferCurrent(opaque)) scopeCaptureFrame.copied = false;
        scopeGpuTransactionEnd();
    });
}
static bool prepareScopeScratch() {
    auto *d = uiFrame.device;
    D3DVIEWPORT9 vp{0,0,uiFrame.request.width,uiFrame.request.height,0,1};
    return preserveViewportDepth(d,vp) && SUCCEEDED(originalSetRT(d,0,uiFrame.target[0])) && SUCCEEDED(originalSetDepth(d,uiFrame.z[0])) &&
        SUCCEEDED(d->SetViewport(&vp)) && SUCCEEDED(d->SetScissorRect(&uiFrame.scissor)) &&
        SUCCEEDED(d->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER|
            (hasStencil ? D3DCLEAR_STENCIL : 0),0xff000000,1,0));
}
// Diagnostic only (SS2VR_SCOPE_PROBE=1): mean luma of the central quarter of the
// scope source image, read back once per perf window (stalls the GPU).
static void probeScopeImage(IDirect3DDevice9 *d,IDirect3DTexture9 *image,const Request &request) noexcept {
    static const bool enabled = [] { wchar_t value[2]{}; return GetEnvironmentVariableW(L"SS2VR_SCOPE_PROBE", value, 2) == 1 && value[0] == L'1'; }();
    if (!enabled || perf.scopeLuma >= 0 || !image) return;
    IDirect3DSurface9 *level = nullptr, *read = nullptr;
    D3DLOCKED_RECT locked{};
    D3DSURFACE_DESC desc{};
    if (SUCCEEDED(image->GetSurfaceLevel(0,&level)) && SUCCEEDED(level->GetDesc(&desc)) &&
        SUCCEEDED(d->CreateOffscreenPlainSurface(desc.Width,desc.Height,desc.Format,D3DPOOL_SYSTEMMEM,&read,nullptr)) &&
        SUCCEEDED(d->GetRenderTargetData(level,read)) && SUCCEEDED(read->LockRect(&locked,nullptr,D3DLOCK_READONLY))) {
        double sum = 0; unsigned n = 0;
        for (UINT y = request.height*3/8; y < request.height*5/8; ++y) {
            const auto *row = static_cast<const uint8_t *>(locked.pBits) + size_t(y)*locked.Pitch;
            for (UINT x = request.width*3/8; x < request.width*5/8; ++x, ++n)
                sum += .114*row[x*4] + .587*row[x*4+1] + .299*row[x*4+2];
        }
        read->UnlockRect();
        perf.scopeLuma = n ? float(sum/n) : 0.f;
    }
    if (read) read->Release();
    if (level) level->Release();
}
// Scope views observed in the previous frame's left-eye render. Replaces a
// dedicated preview render of the world (a whole extra pass while zoomed).
static ScopeSourceView observedScopeView[2];
static bool observedScopeValid[2]{};
static uint64_t observedScopeGeneration = 0;
static void noteScopeViews(void *puppet,const Request &request) noexcept {
    const bool needed = !scopeSourcesHalted && scopePreviewNeeded(puppet,request);
    for (unsigned hand=0;hand<2;++hand)
        observedScopeValid[hand] = needed && copyScopeSourceView(hand,observedScopeView[hand]);
    observedScopeGeneration = graphicsResourceGeneration();
}
static bool renderScopeSources(void *puppet,void(__thiscall *original)(void *),const Request &request) {
    const auto generation = graphicsResourceGeneration();
    if (scopeSourcesHalted || !scopePreviewNeeded(puppet,request)) {
        observedScopeValid[0] = observedScopeValid[1] = false;
        return true;
    }
    ++perf.scopeNeeded;
    auto *d = uiFrame.device;
    D3DCAPS9 caps{};
    constexpr DWORD required = D3DPS20CAPS_ARBITRARYSWIZZLE | D3DPS20CAPS_GRADIENTINSTRUCTIONS |
        D3DPS20CAPS_PREDICATION | D3DPS20CAPS_NODEPENDENTREADLIMIT | D3DPS20CAPS_NOTEXINSTRUCTIONLIMIT;
    if (FAILED(d->GetDeviceCaps(&caps)) || caps.PixelShaderVersion < D3DPS_VERSION(2,0) ||
        !(caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFLINEAR) ||
        !(caps.TextureFilterCaps & D3DPTFILTERCAPS_MAGFLINEAR) ||
        caps.PS20Caps.NumTemps < 22 || caps.PS20Caps.NumInstructionSlots < 512 ||
        (caps.PS20Caps.Caps & required) != required ||
        (colorFormat != D3DFMT_A8R8G8B8 && colorFormat != D3DFMT_X8R8G8B8)) return true;
    if (scopeCacheDevice != d) releaseScopeCache();
    scopeCacheDevice = d;
    if (!scopeShaderCache &&
        (FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD *>(ScopeImageOpaqueProgram),&scopeShaderCache)) ||
         !scopeShaderCache)) return true;
    if (!uiFrame.scopeShader) { uiFrame.scopeShader = scopeShaderCache; scopeShaderCache->AddRef(); }
    activeEye = -1; eyeInvalid = false; renderRequest = request;
    bool ok = true;
    for (unsigned hand=0;hand<2 && ok;++hand) {
        if (!observedScopeValid[hand] || observedScopeGeneration != generation) continue;
        observedScopeValid[hand] = false; // One source render per observation.
        uiFrame.scopeView[hand] = observedScopeView[hand];
        ++perf.scopeCandidates;
        // The source copy needs the eye target's exact format (zero-copy shared
        // targets are A8R8G8B8; the pixel fallback uses the backbuffer format).
        D3DSURFACE_DESC eyeDesc{}, cached{};
        if (!uiFrame.target[0] || FAILED(uiFrame.target[0]->GetDesc(&eyeDesc))) continue;
        auto *&image = scopeImageCache[hand];
        if (image && (FAILED(image->GetLevelDesc(0,&cached)) || cached.Width != request.width ||
                      cached.Height != request.height || cached.Format != eyeDesc.Format)) {
            image->Release(); image = nullptr;
        }
        // Automatic mipmaps when the device can: the lens shows a small, heavily
        // minified view of this full-size image and would otherwise sparkle.
        static const bool autoMips = [d,format = eyeDesc.Format] {
            D3DCAPS9 c{}; IDirect3D9 *api = nullptr; D3DDEVICE_CREATION_PARAMETERS creation{}; D3DDISPLAYMODE mode{};
            bool ok = SUCCEEDED(d->GetDeviceCaps(&c)) && (c.Caps2 & D3DCAPS2_CANAUTOGENMIPMAP) &&
                SUCCEEDED(d->GetDirect3D(&api)) && api && SUCCEEDED(d->GetCreationParameters(&creation)) &&
                SUCCEEDED(d->GetDisplayMode(0,&mode)) &&
                api->CheckDeviceFormat(creation.AdapterOrdinal,creation.DeviceType,mode.Format,
                    D3DUSAGE_RENDERTARGET|D3DUSAGE_AUTOGENMIPMAP,D3DRTYPE_TEXTURE,format) == D3D_OK;
            if (api) api->Release();
            log("Scope lens mipmaps=%u", unsigned(ok));
            return ok;
        }();
        if (!image && (FAILED(d->CreateTexture(request.width,request.height,autoMips ? 0 : 1,
                                              D3DUSAGE_RENDERTARGET|(autoMips ? D3DUSAGE_AUTOGENMIPMAP : 0),
                                              eyeDesc.Format,D3DPOOL_DEFAULT,&image,nullptr)) || !image)) {
            image = nullptr; continue;
        }
        if (!uiFrame.scopeImage[hand]) { uiFrame.scopeImage[hand] = image; image->AddRef(); }
        scopeCaptureFrame = {hand,uiFrame.generation,true,false,{}};
        scopeScratch = true; eyeInvalid = false;
        const bool begun = beginScopeSource(puppet,request,uiFrame.scopeView[hand],captureNativeScope,
                                            scopeCaptureCurrent,&scopeCaptureFrame);
        if (!begun) perf.scopeBeginFailures[std::min(scopeSourceBeginFailure(),3u)]++;
        if (begun) {
            ++perf.scopeBegun;
            const LARGE_INTEGER sourceStart = PerfClock::now();
            scopeCaptureTime = {};
            ok = prepareScopeScratch();
            if (ok) original(puppet);
            const LARGE_INTEGER sourceEnd = PerfClock::now();
            perf.scopeSourceCpu += PerfClock::seconds(sourceStart,sourceEnd);
            if (scopeCaptureTime.QuadPart) perf.scopeTailCpu += PerfClock::seconds(scopeCaptureTime,sourceEnd);
            if (generation != graphicsResourceGeneration()) return false;
            const bool finished = endScopeSource(ok && !eyeInvalid);
            perf.scopeFinished += finished; perf.scopeCopied += scopeCaptureFrame.copied;
            uiFrame.scopeReady[hand] = scopeCaptureFrame.once.complete(finished,scopeCaptureFrame.copied) &&
                scopeCaptureCurrent(&scopeCaptureFrame) && nativeUiFrameCurrent(puppet,request);
            if (uiFrame.scopeReady[hand]) {
                ++perf.scopeReady;
                if (uiFrame.scopeImage[hand]->GetLevelCount() > 1) uiFrame.scopeImage[hand]->GenerateMipSubLevels();
                probeScopeImage(d,uiFrame.scopeImage[hand],request);
            }
        }
        scopeCaptureFrame.active = false;
        scopeScratch = false;
        ok = ok && !eyeInvalid && !uiFrame.fault && !scopeSourcesHalted;
    }
    return ok;
}
// Lab-only observation: inspect the existing device state without changing
// native renderer policy or retaining an attachment beyond this invocation.
static void traceStereoDepth(IDirect3DDevice9 *d,const Request &request,int index,const char *stage) {
    DWORD z=0,write=0,func=0,alpha=0,blend=0,stencil=0;
    D3DVIEWPORT9 vp{};
    IDirect3DSurface9 *color=nullptr,*ds=nullptr;
    D3DSURFACE_DESC cd{},zd{};
    bool ok=true;
    withNativeFinally([&] {
        ok=SUCCEEDED(d->GetRenderState(D3DRS_ZENABLE,&z)) && ok;
        ok=SUCCEEDED(d->GetRenderState(D3DRS_ZWRITEENABLE,&write)) && ok;
        ok=SUCCEEDED(d->GetRenderState(D3DRS_ZFUNC,&func)) && ok;
        ok=SUCCEEDED(d->GetRenderState(D3DRS_ALPHATESTENABLE,&alpha)) && ok;
        ok=SUCCEEDED(d->GetRenderState(D3DRS_ALPHABLENDENABLE,&blend)) && ok;
        ok=SUCCEEDED(d->GetRenderState(D3DRS_STENCILENABLE,&stencil)) && ok;
        ok=SUCCEEDED(d->GetViewport(&vp)) && ok;
        ok=SUCCEEDED(d->GetRenderTarget(0,&color)) && color && ok;
        ok=SUCCEEDED(d->GetDepthStencilSurface(&ds)) && ds && ok;
        if(color) ok=SUCCEEDED(color->GetDesc(&cd)) && ok;
        if(ds) ok=SUCCEEDED(ds->GetDesc(&zd)) && ok;
        log("Lab depth stage request=%llu eye=%d stage=%s read=%u z=%lu write=%lu func=%lu alpha=%lu blend=%lu stencil=%lu viewport=%ux%u range=%.9g,%.9g color=%ux%u,%u,%u depth=%ux%u,%u,%u",
            static_cast<unsigned long long>(request.sequence),index,stage,ok,z,write,func,alpha,blend,stencil,
            vp.Width,vp.Height,vp.MinZ,vp.MaxZ,cd.Width,cd.Height,unsigned(cd.Format),unsigned(cd.MultiSampleType),
            zd.Width,zd.Height,unsigned(zd.Format),unsigned(zd.MultiSampleType));
    },[&](bool) noexcept {
        if(ds) { auto *p=ds;ds=nullptr;p->Release(); }
        if(color) { auto *p=color;color=nullptr;p->Release(); }
    });
}
static bool observedWorldRender(IDirect3DDevice9 *d,void *puppet,void(__thiscall *original)(void *),
                                const Request &request,int index,bool trace) {
    if(!trace || labWorldDraws.device) { original(puppet);return true; }
    labWorldDraws={};labWorldDraws.device=d;
    bool aborted=false;
    const bool contained=withNativeFinally([&] { original(puppet); },[&](bool unwind) noexcept {
        labWorldDraws.device=nullptr;
        aborted=unwind;
    });
    // Foreign unwind propagates without logging. Only a returned observation
    // reaches file I/O; native-finally cleanup remains scalar and allocation-free.
    log("Lab world draw attempts request=%llu eye=%d aborted=%u calls=%llu primitives=%llu readFailures=%llu",
            static_cast<unsigned long long>(request.sequence),index,aborted,
            static_cast<unsigned long long>(labWorldDraws.calls),static_cast<unsigned long long>(labWorldDraws.primitives),
            static_cast<unsigned long long>(labWorldDraws.readFailures));
    for(unsigned key=0;key<16;++key) if(labWorldDraws.stateCalls[key])
            log("Lab world draw state request=%llu eye=%d z=%u write=%u alpha=%u blend=%u calls=%llu primitives=%llu fullRange=%llu worldRange=%llu otherRange=%llu",
                static_cast<unsigned long long>(request.sequence),index,key&1,(key>>1)&1,(key>>2)&1,(key>>3)&1,
                static_cast<unsigned long long>(labWorldDraws.stateCalls[key]),
                static_cast<unsigned long long>(labWorldDraws.statePrimitives[key]),
                static_cast<unsigned long long>(labWorldDraws.fullRangeCalls[key]),
                static_cast<unsigned long long>(labWorldDraws.worldRangeCalls[key]),
                static_cast<unsigned long long>(labWorldDraws.otherRangeCalls[key]));
    if(!contained || aborted) { nativeUiFault();uiHalted=true;return false; }
    return true;
}
// Lab only (SS2VR_LAB_SAMPLER=1): statistical profile of the render thread.
// A sampler thread suspends it ~1000 times a second, reads EIP and buckets it by
// module (and 256-byte RVA bucket inside Engine/GfxD3D); logged every 10 s.
namespace sampler {
static HANDLE target = nullptr;
static std::atomic<bool> running{false};
static std::atomic<bool> inEyes{false}; // Sample only while an eye renders.
struct Module { uintptr_t base = 0, end = 0; char name[48]{}; uint32_t samples = 0; };
static Module modules[96];
static unsigned moduleCount = 0;
static uint32_t unknown = 0, total = 0;
struct Bucket { uint32_t key = 0, samples = 0; };
static Bucket buckets[4096]; // key = module index << 24 | rva >> 8
static SRWLOCK lock = SRWLOCK_INIT;
static void refreshModules() {
    moduleCount = 0;
    HMODULE handle = nullptr;
    // Walk the address space for mapped images (no psapi dependency).
    MEMORY_BASIC_INFORMATION info{};
    for (uintptr_t address = 0x10000; address < 0x7fff0000 && moduleCount < 96;) {
        if (!VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof(info))) break;
        if (info.Type == MEM_IMAGE && info.AllocationBase == info.BaseAddress &&
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCWSTR>(info.BaseAddress), &handle)) {
            auto &m = modules[moduleCount++];
            m.base = reinterpret_cast<uintptr_t>(info.BaseAddress);
            const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(m.base);
            const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(m.base + dos->e_lfanew);
            m.end = m.base + nt->OptionalHeader.SizeOfImage;
            wchar_t path[MAX_PATH]{};
            GetModuleFileNameW(handle, path, MAX_PATH);
            const wchar_t *file = wcsrchr(path, L'\\');
            file = file ? file + 1 : path;
            WideCharToMultiByte(CP_UTF8, 0, file, -1, m.name, sizeof(m.name) - 1, nullptr, nullptr);
            // Distinguish the mod's proxy d3d9 from DXVK's d3d9 by directory.
            if (!_stricmp(m.name, "d3d9.dll") && wcsstr(path, L"system32")) strcpy(m.name, "d3d9.dll(dxvk)");
            address = m.end;
            continue;
        }
        address = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    }
}
static Bucket callers[1024]; // CRT samples attributed to the first caller outside the CRT.
static unsigned moduleOf(uintptr_t address) {
    unsigned i = 0;
    while (i < moduleCount && !(address >= modules[i].base && address < modules[i].end)) ++i;
    return i;
}
static bool crt(unsigned i) { return i < moduleCount && (!_stricmp(modules[i].name, "ucrtbase.dll") || !_stricmp(modules[i].name, "ntdll.dll") || !_stricmp(modules[i].name, "msvcrt.dll")); }
static void recordCaller(uintptr_t esp) {
    for (unsigned slot = 0; slot < 48; ++slot) {
        uintptr_t value = 0;
        if (!readableMemory(reinterpret_cast<void *>(esp + slot * 4), 4)) return;
        std::memcpy(&value, reinterpret_cast<void *>(esp + slot * 4), 4);
        const unsigned m = moduleOf(value);
        if (m == moduleCount || crt(m) || !readableMemory(reinterpret_cast<void *>(value - 6), 6)) continue;
        const auto *code = reinterpret_cast<const uint8_t *>(value);
        if (code[-5] != 0xe8 && code[-6] != 0xff && code[-2] != 0xff && code[-3] != 0xff) continue;
        const uint32_t key = (m << 24) | uint32_t((value - modules[m].base) >> 4);
        uint32_t index = (key * 2654435761u) & 1023;
        for (unsigned probe = 0; probe < 1024; ++probe, index = (index + 1) & 1023)
            if (!callers[index].samples || callers[index].key == key) { callers[index].key = key; ++callers[index].samples; break; }
        return;
    }
}
static void record(uintptr_t eip) {
    AcquireSRWLockExclusive(&lock);
    ++total;
    unsigned i = 0;
    while (i < moduleCount && !(eip >= modules[i].base && eip < modules[i].end)) ++i;
    if (i == moduleCount) { ++unknown; ReleaseSRWLockExclusive(&lock); return; }
    ++modules[i].samples;
    const uint32_t key = (i << 24) | uint32_t((eip - modules[i].base) >> 8);
    uint32_t slot = (key * 2654435761u) & 4095;
    for (unsigned probe = 0; probe < 4096; ++probe, slot = (slot + 1) & 4095)
        if (!buckets[slot].samples || buckets[slot].key == key) { buckets[slot].key = key; ++buckets[slot].samples; break; }
    ReleaseSRWLockExclusive(&lock);
}
static DWORD WINAPI run(void *) {
    refreshModules();
    uint64_t last = GetTickCount64();
    while (running.load()) {
        Sleep(1);
        if (inEyes.load(std::memory_order_relaxed) && SuspendThread(target) != DWORD(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            const bool ok = GetThreadContext(target, &context) && inEyes.load(std::memory_order_relaxed);
            ResumeThread(target);
            if (ok) {
                record(context.Eip);
                AcquireSRWLockExclusive(&lock);
                if (crt(moduleOf(context.Eip))) recordCaller(context.Esp);
                ReleaseSRWLockExclusive(&lock);
            }
        }
        if (GetTickCount64() - last >= 10000) {
            last = GetTickCount64();
            AcquireSRWLockExclusive(&lock);
            char line[1024]; int used = snprintf(line, sizeof(line), "Lab sampler total=%u unknown=%u |", total, unknown);
            for (unsigned pick = 0; pick < 12; ++pick) {
                unsigned best = moduleCount; uint32_t most = 0;
                for (unsigned i = 0; i < moduleCount; ++i) if (modules[i].samples > most) { most = modules[i].samples; best = i; }
                if (best == moduleCount) break;
                used += snprintf(line + used, sizeof(line) - used, " %s=%.1f%%", modules[best].name, 100.0 * most / std::max(1u, total));
                modules[best].samples = 0;
                if (used >= int(sizeof(line)) - 64) break;
            }
            log("%s", line);
            for (unsigned pick = 0; pick < 24; ++pick) {
                unsigned best = 4096; uint32_t most = 0;
                for (unsigned i = 0; i < 4096; ++i) if (buckets[i].samples > most) { most = buckets[i].samples; best = i; }
                if (best == 4096) break;
                const auto &m = modules[buckets[best].key >> 24];
                log("Lab sampler hot %s+%05x %.1f%%", m.name, (buckets[best].key & 0xffffff) << 8, 100.0 * most / std::max(1u, total));
                buckets[best].samples = 0;
            }
            for (unsigned pick = 0; pick < 12; ++pick) {
                unsigned best = 1024; uint32_t most = 0;
                for (unsigned i = 0; i < 1024; ++i) if (callers[i].samples > most) { most = callers[i].samples; best = i; }
                if (best == 1024) break;
                const auto &m = modules[callers[best].key >> 24];
                log("Lab sampler crt caller %s+%05x %.1f%%", m.name, (callers[best].key & 0xffffff) << 4, 100.0 * most / std::max(1u, total));
                callers[best].samples = 0;
            }
            for (auto &b : callers) b = {};
            for (auto &b : buckets) b = {};
            for (unsigned i = 0; i < moduleCount; ++i) modules[i].samples = 0;
            total = unknown = 0;
            ReleaseSRWLockExclusive(&lock);
        }
    }
    return 0;
}
static void start() {
    static const bool enabled = [] { wchar_t value[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SAMPLER", value, 2) == 1 && value[0] == L'1'; }();
    if (!enabled || running.exchange(true)) return;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0)) return;
    if (HANDLE thread = CreateThread(nullptr, 0, run, nullptr, 0, nullptr)) CloseHandle(thread);
    log("Lab sampler started on render thread %lu", GetCurrentThreadId());
}
} // namespace sampler
void stereo(void *puppet,void(__thiscall *original)(void *),EyePostRender postRender) {
    sampler::start();
    const LARGE_INTEGER stereoStart = PerfClock::now();
    struct StereoTimer {
        LARGE_INTEGER start;
        bool pair = false;
        ~StereoTimer() {
            lastStereoEnd = PerfClock::now();
            const double spent = PerfClock::seconds(start, lastStereoEnd);
            if (pair) perf.stereo += spent;
            else { perf.mono += spent; ++perf.monoFrames; }
        }
    } stereoTimer{stereoStart};
    if (lastPresentEnd.QuadPart) { perf.preStereo += PerfClock::seconds(lastPresentEnd, stereoStart); ++perf.stereoCalls; }
    pollSharedFences();
    const auto generation = graphicsResourceGeneration();
    if (uiFrame.slot>=0) {
        nativeUiFault();
        withNativeFinally([&]{ original(puppet); },[&](bool aborted) noexcept { if(aborted) uiHalted=true; });
        return;
    }
    retireDeferred();
    if (!nativeRenderExtentCurrent() || deferredSlot>=0 || activeEye>=0 || stopping || uiHalted || !device || !channel.shared || !originalSetRT ||
        !originalSetDepth || !originalStretch || !originalReadTarget || !originalFill || !allocate() ||
        generation != graphicsResourceGeneration()) {
        withNativeFinally([&]{ original(puppet); },[&](bool aborted) noexcept { if(aborted) uiHalted=true; });
        return;
    }
    Request request{};
    int chosen=-1;
    bool zeroCopy=false;
    const auto acquireRequest=[&] {
        Lock l(channel,5); // A lost race would otherwise turn this frame flat.
        if(l) {
            const auto &images=channel.shared->images;
            const uint32_t sinceCapture=static_cast<uint32_t>(GetTickCount())-images.pixelRequestTick;
            zeroCopy=sharedGeneration && images.generation==sharedGeneration && images.opened==sharedGeneration &&
                images.width==width && images.height==height && (!images.pixelRequestTick || sinceCapture>3000);
        }
        if(l) {
            // Newest request first: with two waiting, the older one holds the
            // older prediction (the host late-latches the other).
            const auto &slots=channel.shared->slot;
            const int order[2]{slots[1].request.sequence>slots[0].request.sequence ? 1 : 0,
                               slots[1].request.sequence>slots[0].request.sequence ? 0 : 1};
            for(int i : order) {
                auto &slot=channel.shared->slot[i];
                const auto now=GetTickCount64();
                if(slot.state==SlotState::Requested && !slot.cancelled && slot.request.width==width &&
                    slot.request.height==height && slot.request.input.focused && slot.request.input.headValid &&
                    now>=slot.request.input.tickMs && now-slot.request.input.tickMs<200 &&
                    headFramePrepared(puppet,slot.request)) {
                    chosen=i; request=slot.request; slot.state=SlotState::Rendering; break;
                }
            }
        }
    };
    acquireRequest();
    static unsigned requestDiagnostics = 0;
    if (chosen >= 0 && requestDiagnostics < 4) {
        ++requestDiagnostics;
        log("Native world request acquired sequence=%llu size=%ux%u",
            static_cast<unsigned long long>(request.sequence),request.width,request.height);
    }
    if(chosen<0) {
        withNativeFinally([&]{ original(puppet); },[&](bool aborted) noexcept { if(aborted) uiHalted=true; });
        return;
    }
    static const bool labDepthTrace=[] { wchar_t value[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_TRACE",value,2)==1 && value[0]==L'1'; }();
    static unsigned depthTraceRequests=0;
    const bool traceDepth=labDepthTrace && depthTraceRequests<8;
    if(traceDepth) ++depthTraceRequests;
    stereoTimer.pair=true;
    gpuProfile.begin(device);
    uiFrame.slot=chosen;
    uiFrame.pixelGeneration=pixelStore.generation;
    uiFrame.pixelBytes=size_t(pixelStore.bytes());
    for(unsigned i=0;i<2;++i)
        uiFrame.pixels[i]=pixelStore.matches(pixelStore.generation,request.width,request.height) ?
            pixelStore.buffer(slotPixelBuffer(unsigned(chosen),i)) : nullptr;
    admittedUiFrame=&uiFrame;
    uiFrame.request=request;
    uiFrame.player=puppet;
    uiFrame.generation=resourceGeneration.load();
    uiFrame.device=device;
    device->AddRef();
    uiFrame.zeroCopy=zeroCopy && request.foveated != 1 && sharedSurface[chosen][0] && sharedSurface[chosen][1];
    for(unsigned i=0;i<2;++i) {
        uiFrame.target[i]=uiFrame.zeroCopy ? sharedSurface[chosen][i] : eye[i]; uiFrame.target[i]->AddRef();
        uiFrame.z[i]=depth[i]; depth[i]->AddRef();
        uiFrame.read[i]=readback[i]; readback[i]->AddRef();
    }
    bool retained=false;
    // The single-pass target swapped into eye 0 for the centre render; swapped
    // back by the cleanup if native code unwinds before the normal swap-back
    // (else the frame release and releaseTargets released the wrong surfaces).
    IDirect3DSurface9 **swappedColor=nullptr, **swappedDepth=nullptr;
    withNativeFinally([&] {
        auto *d=uiFrame.device;
        bool ok=SUCCEEDED(d->GetRenderTarget(0,&uiFrame.color)) &&
            SUCCEEDED(d->GetDepthStencilSurface(&uiFrame.ds)) &&
            SUCCEEDED(d->GetViewport(&uiFrame.viewport)) && SUCCEEDED(d->GetScissorRect(&uiFrame.scissor));
        // Backbuffer proof uses the retained known original desktop target rather
        // than retaining any native intermediate target after its invocation.
        ok=SUCCEEDED(sourceBackbuffer(&uiFrame.backbuffer)) && ok;
        ok=ok && sameSurface(uiFrame.color,uiFrame.backbuffer) && uiFrame.ds;
        if(uiFrame.backbuffer) uiFrame.backbuffer->Release();
        uiFrame.backbuffer=nullptr;
        const auto &vp=uiFrame.viewport;
        ok=ok && vp.X==0 && vp.Y==0 && vp.Width==width && vp.Height==height;
        D3DSURFACE_DESC cd{},zd{};
        if(uiFrame.color) ok=SUCCEEDED(uiFrame.color->GetDesc(&cd)) && ok;
        if(uiFrame.ds) ok=SUCCEEDED(uiFrame.ds->GetDesc(&zd)) && ok;
        ok=ok && cd.Format==colorFormat && cd.MultiSampleType==D3DMULTISAMPLE_NONE &&
            zd.Format==depthFormat && zd.MultiSampleType==D3DMULTISAMPLE_NONE;
        ok=singleUiTarget(d,uiFrame.extra) && ok;
        uiFrame.restore=ok;
        D3DPRESENT_PARAMETERS presentation{};
        const bool directWritesExcluded=ok &&
            SUCCEEDED(uiFrame.color->GetContainer(__uuidof(IDirect3DSwapChain9),reinterpret_cast<void **>(&uiFrame.chain))) &&
            uiFrame.chain && SUCCEEDED(uiFrame.chain->GetPresentParameters(&presentation)) &&
            !(presentation.Flags&D3DPRESENTFLAG_LOCKABLE_BACKBUFFER);
        uiFrame.ui=ok && postRender && request.uiRequested==1 && !uiHalted && uiRoutingReady &&
            originalDraw && originalDrawIndexed && originalClear && originalDrawUp && originalDrawIndexedUp &&
            originalDrawRect && originalDrawTri && originalUpdate && directWritesExcluded &&
            nativeUiDeviceCurrent(d) && nativeUiOwnerCurrent(puppet);
        if(uiFrame.ui) for(unsigned i=0;i<2;++i)
            uiFrame.panel[i]=compose(inverse(request.eye[i]),request.uiPanel);
        desktopTarget=uiFrame.color;
        desktopDepth=uiFrame.ds;
        static unsigned admissionDiagnostics = 0;
        if (admissionDiagnostics < 4) {
            ++admissionDiagnostics;
            log("Native world target admission ok=%u ui=%u source=%ux%u viewport=%ux%u",
                ok,uiFrame.ui,cd.Width,cd.Height,vp.Width,vp.Height);
        }
        bool stageNoted=false;
        const auto noteStage=[&](unsigned which) { if(!ok && !stageNoted) { stageNoted=true; ++perf.stage[which]; } };
        const bool admitted=ok;
        if(ok) ok=beginStereo(puppet,request,uiFrame.remoteOwner);
        if(admitted) noteStage(0);
        if (!ok) {
            static unsigned beginDiagnostics = 0;
            if (beginDiagnostics < 4) { ++beginDiagnostics; log("Native world pair admission rejected"); }
        }
        if (generation != graphicsResourceGeneration()) { original(puppet); return; }
        const float visibility=ok ? frozenWorldVisibility() : 1;
        if(uiFrame.zeroCopy && visibility!=1) {
            // Fades dim the eye images on the CPU: use the pixel path for this pair.
            for(unsigned i=0;i<2;++i) { uiFrame.target[i]->Release(); uiFrame.target[i]=eye[i]; eye[i]->AddRef(); }
            uiFrame.zeroCopy=false;
        }
        if(traceDepth) traceStereoDepth(d,request,-1,"entry");
        if (ok) ok = renderScopeSources(puppet,original,request);
        if(admitted) noteStage(1);
        if (generation != graphicsResourceGeneration()) { original(puppet); return; }
        static const bool labAlternateEyes=[] {
            wchar_t enabled[2]{},privateDisplay[2]{},weapon[3]{};
            return GetEnvironmentVariableW(L"SS2VR_LAB_ALTERNATE_EYES",enabled,2)==1 && enabled[0]==L'1' &&
                GetEnvironmentVariableW(L"SS2VR_LAB_PRIVATE_DISPLAY",privateDisplay,2)==1 && privateDisplay[0]==L'1' &&
                GetEnvironmentVariableW(L"SS2VR_LAB_IDLE_WEAPON",weapon,3)==2 && weapon[0]==L'1' && weapon[1]==L'3';
        }();
        const bool alternateEyes=labAlternateEyes && labOnlineIsolationInstalled();
        static unsigned orderReceipts=0;
        if(alternateEyes && orderReceipts<32) {
            ++orderReceipts;
            log("Lab world eye order request=%llu first=%d second=%d",
                static_cast<unsigned long long>(request.sequence),
                labWorldEyeForPass(0,request.sequence,true),labWorldEyeForPass(1,request.sequence,true));
        }
        // Right eye replayed from the left eye's recorded calls when possible.
        static const bool labReplayVerify=[] { wchar_t value[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_REPLAY_VERIFY",value,2)==1 && value[0]==L'1'; }();
        ensureStereoHooks(d);
        // Single pass: one world traversal from a centre camera into the double-wide
        // target, issued per eye half; each eye then adopts its derived camera.
        const Request centre=ss2vr::single_pass::centreRequest(request);
        ss2vr::replay::EyeChange singlePassChange[2];
        const bool singlePassFrame=singlePassEnabled && !alternateEyes && wideColor && wideDepth &&
            single_pass::installed() && singlePassChanges(request,centre,singlePassChange);
        {
            // Mode flips change culling/LOD and the overlay path: say when and why.
            static int previousMode=-1;
            const int mode=singlePassFrame ? 1 : 0;
            if(singlePassEnabled && mode!=previousMode) {
                static unsigned flips=0;
                if(++flips<=40 || flips%200==0)
                    log("Stereo mode %s (wide=%u installed=%u alternate=%u changes=%u) flips=%u",
                        mode ? "single pass" : "two pass",unsigned(wideColor && wideDepth),unsigned(single_pass::installed()),
                        unsigned(alternateEyes),unsigned(mode ? 1u : unsigned(singlePassChanges(request,centre,singlePassChange))),flips);
                previousMode=mode;
            }
        }
        bool centreReady=false;
        // Foveated: the pass draws into the atlas instead of the double-wide target.
        FoveaFrame fovea;
        bool foveated=false;
        if(singlePassFrame && foveaLayout(request,width,height,fovea)) {
            D3DSURFACE_DESC cd{}, zd{};
            foveated=SUCCEEDED(wideColor->GetDesc(&cd)) && SUCCEEDED(wideDepth->GetDesc(&zd)) &&
                ensureFovea(d,fovea.layout.width,fovea.layout.height,cd.Format,zd.Format);
        }
        IDirect3DSurface9 *&passColor=foveated ? foveaColor : wideColor, *&passDepth=foveated ? foveaDepth : wideDepth;
        const UINT passWidth=foveated ? fovea.layout.width : width*2, passHeight=foveated ? fovea.layout.height : height;
        const bool useReplay=eyeReplay && !alternateEyes && !singlePassFrame;
        bool replayReady=false;
        ss2vr::replay::EyeChange replayChange;
        for(unsigned pass=0;pass<2 && ok;++pass) {
            const int i=labWorldEyeForPass(pass,request.sequence,alternateEyes);
            activeEye=i; eyeInvalid=false; renderRequest=request;
            if(singlePassFrame) {
                if(pass==0) {
                    // The engine's target redirection (setRT/setDepth) follows uiFrame eye 0.
                    std::swap(uiFrame.target[0],passColor); std::swap(uiFrame.z[0],passDepth);
                    swappedColor=&passColor; swappedDepth=&passDepth;
                    beginEye(puppet,centre,0);
                    const RECT wideScissor{0,0,LONG(passWidth),LONG(passHeight)};
                    D3DVIEWPORT9 wv{0,0,passWidth,passHeight,0,1};
                    ok=preserveViewportDepth(d,wv) && SUCCEEDED(originalSetRT(d,0,uiFrame.target[0])) &&
                       SUCCEEDED(originalSetDepth(d,uiFrame.z[0])) && SUCCEEDED(d->SetViewport(&wv)) &&
                       SUCCEEDED(d->SetScissorRect(&wideScissor)) &&
                       SUCCEEDED(d->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER|(hasStencil ? D3DCLEAR_STENCIL : 0),0xff000000,1,0));
                    // The engine sets its own eye-sized viewport and scissor; restore them as one eye.
                    D3DVIEWPORT9 ev{0,0,width,height,wv.MinZ,wv.MaxZ};
                    ok=ok && SUCCEEDED(d->SetViewport(&ev)) && SUCCEEDED(d->SetScissorRect(&uiFrame.scissor));
                    noteStage(2);
                    const LARGE_INTEGER eyeStart=PerfClock::now();
                    sampler::inEyes.store(true, std::memory_order_relaxed);
                    // Lab (SS2VR_LAB_SP_BISECT=1): on sampled frames render the pass without
                    // instancing, keep it, render again instancing a range of eligible draws,
                    // and binary-search the first draw whose instancing changes the image.
                    static const bool spBisect=[] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SP_BISECT",v,2)==1 && v[0]==L'1'; }();
                    static unsigned spFrames=0, spLo=0, spHi=0; static bool spDone=false;
                    const bool spCompare=spBisect && !foveated && !spDone && ++spFrames%30==0;
                    if(ok && spCompare && single_pass::begin(d,uiFrame.target[0],width,height,singlePassChange)) {
                        single_pass::labInstanceRange(0,0,~0u);
                        setSinglePassWorld(true,&request);
                        ok=observedWorldRender(d,puppet,original,centre,0,false);
                        setSinglePassWorld(false);
                        single_pass::end(d);
                        ok=ok && replay_compare::keep(d,uiFrame.target[0]);
                        D3DVIEWPORT9 wv2{0,0,width*2,height,0,1};
                        const RECT wideScissor2{0,0,LONG(width*2),LONG(height)};
                        ok=ok && SUCCEEDED(d->SetViewport(&wv2)) && SUCCEEDED(d->SetScissorRect(&wideScissor2)) &&
                           SUCCEEDED(d->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER|(hasStencil ? D3DCLEAR_STENCIL : 0),0xff000000,1,0));
                        D3DVIEWPORT9 ev2{0,0,width,height,0,1};
                        ok=ok && SUCCEEDED(d->SetViewport(&ev2)) && SUCCEEDED(d->SetScissorRect(&uiFrame.scissor));
                        if(!spHi) spHi=1000000;
                        const unsigned mid=spLo+(spHi-spLo)/2;
                        if(ok && single_pass::begin(d,uiFrame.target[0],width,height,singlePassChange)) {
                            single_pass::labInstanceRange(0,mid,spHi-spLo<=1 ? spLo : ~0u);
                            setSinglePassWorld(true,&request);
                            ok=observedWorldRender(d,puppet,original,centre,0,traceDepth);
                            setSinglePassWorld(false);
                            const unsigned eligible=single_pass::labEligibleCount();
                            single_pass::end(d);
                            const size_t differing=replay_compare::differing(d,uiFrame.target[0]);
                            if(spHi==1000000) { spHi=eligible; spDone=differing<=200; } // First: everything instanced.
                            else if(spHi-spLo<=1) spDone=true;
                            else if(differing>200) spHi=mid; else spLo=mid;
                            log("Single pass bisect allowed=[0,%u) differing=%zu eligible=%u -> range [%u,%u)",mid,differing,eligible,spLo,spHi);
                        } else ok=false;
                    } else if(ok && single_pass::begin(d,uiFrame.target[0],width,height,singlePassChange,foveated ? &fovea.layout : nullptr)) {
                        // Lab (SS2VR_LAB_SP_REGION=x0,y0,x1,y1 of the left eye): across frames,
                        // binary-search the first eligible draw whose instancing changes the
                        // mean luminance of that region (e.g. where the moon must be).
                        static struct { bool on=false; int x0=0,y0=0,x1=0,y1=0; unsigned frame=0, step=0, lo=0, hi=0; double good=-1, bad=-1; bool done=false, logged=false; } rb = [] {
                            decltype(rb) b; wchar_t v[64]{};
                            if(GetEnvironmentVariableW(L"SS2VR_LAB_SP_REGION",v,64)) b.on=swscanf(v,L"%d,%d,%d,%d",&b.x0,&b.y0,&b.x1,&b.y1)==4;
                            return b; }();
                        static const unsigned sweepDelay=[]{ wchar_t v[16]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SP_SWEEP_DELAY",v,16) ? unsigned(wcstoul(v,nullptr,10)) : 0u; }();
                        const bool measure=rb.on && !rb.done && ++rb.frame>sweepDelay && rb.frame%20==0;
                        static const bool sweep=[]{ wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SP_SWEEP",v,2)==1 && v[0]==L'1'; }();
                        // Bisection over the instanced pixel programs (stable across frames).
                        static std::vector<uint32_t> psList;
                        if(rb.on && !rb.done) {
                            if(rb.step==0) single_pass::labAllowPixelShaders({},true);       // none instanced
                            else if(rb.step==1) single_pass::labAllowPixelShaders({},false); // all
                            else if(single_pass::labMirrorTarget() || sweep) single_pass::labAllowPixelShaders(rb.step-2<psList.size() ? std::vector<uint32_t>{psList[rb.step-2]} : psList,true);
                            else single_pass::labAllowPixelShaders(std::vector<uint32_t>(psList.begin()+rb.lo,psList.begin()+(rb.lo+rb.hi)/2),true);
                        } else if(rb.on) single_pass::labAllowPixelShaders({},false);
                        setSinglePassWorld(true,&request);
                        ok=observedWorldRender(d,puppet,original,centre,0,traceDepth);
                        if(measure && ok) {
                            IDirect3DSurface9 *mirror=single_pass::labMirrorTarget();
                            const double l=mirror ? double(replay_compare::differingBetween(d,uiFrame.target[0],mirror)) :
                                replay_compare::regionLuma(d,uiFrame.target[0],rb.x0,rb.y0,rb.x1,rb.y1);
                            const unsigned eligible=single_pass::labEligibleCount();
                            if(rb.step==0) rb.good=l;
                            else if(rb.step==1) { rb.bad=l; psList=single_pass::labInstancedPixelShaders(); rb.lo=0; rb.hi=unsigned(psList.size()); }
                            else if(single_pass::labMirrorTarget() || sweep) {
                                // Mirror: one program instanced at a time (same-frame, exact).
                                if(rb.step-2<psList.size()) log("Single pass mirror program %08x differing=%.0f",psList[rb.step-2],l);
                                else rb.done=true;
                            } else {
                                const unsigned mid=(rb.lo+rb.hi)/2;
                                const bool broken=std::abs(l-rb.bad)<std::abs(l-rb.good);
                                if(broken) rb.hi=mid; else rb.lo=mid;
                                if(rb.hi-rb.lo<=1) {
                                    rb.done=true;
                                    log("Single pass region bisect culprit pixel program %08x",rb.lo<psList.size()?psList[rb.lo]:0u);
                                }
                            }
                            (void)eligible;
                            log("Single pass region bisect step=%u luma=%.2f good=%.2f bad=%.2f eligible=%u range=[%u,%u) leftSeam=%.2f",rb.step,l,rb.good,rb.bad,eligible,rb.lo,rb.hi,
                                replay_compare::regionLuma(d,uiFrame.target[0],140,100,142,500));
                            ++rb.step;
                        }
                        setSinglePassWorld(false);
                        single_pass::end(d);
                    } else ok=false;
                    sampler::inEyes.store(false, std::memory_order_relaxed);
                    perf.eyeCpu[0]+=PerfClock::seconds(eyeStart,PerfClock::now());
                    if(generation==graphicsResourceGeneration()) noteScopeViews(puppet,centre);
                    centreReady=ok && generation==graphicsResourceGeneration() && singlePassCaptureCentre(centre);
                    std::swap(uiFrame.target[0],passColor); std::swap(uiFrame.z[0],passDepth);
                    swappedColor=swappedDepth=nullptr;
                    for(unsigned k=0;k<2 && centreReady;++k) {
                        const RECT half{LONG(k*width),0,LONG((k+1)*width),LONG(height)};
                        centreReady=foveated ? composeFovea(d,fovea,k,uiFrame.target[k],width,height) :
                            SUCCEEDED(originalStretch(d,wideColor,&half,uiFrame.target[k],nullptr,D3DTEXF_NONE));
                    }
                    if(foveated) ++perf.foveatedPairs;
                    if(centreReady && env_probe::wanted()) {
                        // Water reflections: this frame's left eye (world only) into the environment cube.
                        float eyeView[12], eyeProjection[16];
                        if(singlePassEyeView(request,0,eyeView,eyeProjection)) {
                            const env_probe::Device device{originalSetRT,originalDrawUp,originalStretch};
                            env_probe::capture(d,device,uiFrame.target[0],width,height,eyeView,eyeProjection);
                        }
                    }
                    ++labStagePair;
                    labStageDump(d,uiFrame.target[1],L"stage-a-pass.ppm");
                    {
                        // Lab (SS2VR_LAB_FOVEA_DUMP=1): the atlas and both composed eyes of the 300th foveated pair.
                        static const bool dump=[]{ wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_FOVEA_DUMP",v,2)==1 && v[0]==L'1'; }();
                        static unsigned dumped=0, pixelDumps=0;
                        const bool pixelPair=foveated && !uiFrame.zeroCopy && pixelDumps<1;
                        if(dump && foveated && (++dumped==300 || pixelPair)) {
                            if(pixelPair) ++pixelDumps;
                            log("Foveated lab dump zeroCopy=%u slot=%d",unsigned(uiFrame.zeroCopy),uiFrame.slot);
                            std::vector<uint8_t> px; D3DSURFACE_DESC desc{};
                            const std::pair<IDirect3DSurface9 *,const wchar_t *> shots[]{{foveaColor,pixelPair ? L"fovea-atlas-px.ppm" : L"fovea-atlas.ppm"},{uiFrame.target[0],pixelPair ? L"fovea-eye0-px.ppm" : L"fovea-eye0.ppm"},
                                {uiFrame.target[1],pixelPair ? L"fovea-eye1-px.ppm" : L"fovea-eye1.ppm"}};
                            for(const auto &[surface,name]:shots)
                                if(surface && SUCCEEDED(surface->GetDesc(&desc)) && replay_compare::read(d,surface,px)) replay_compare::write(name,px,desc.Width,desc.Height);
                            for(unsigned k=0;k<4;++k) {
                                const auto &g=fovea.layout.region[k];
                                log("Foveated lab region %u rect=%ld,%ld,%ld,%ld eye=%u s=%.3f,%.3f o=%.3f,%.3f",k,g.rect.left,g.rect.top,g.rect.right,g.rect.bottom,g.eye,g.sx,g.sy,g.ox,g.oy);
                            }
                            for(unsigned k=0;k<2;++k) log("Foveated lab inset eye=%u rect=%d,%d %dx%d",k,fovea.inset[k].x,fovea.inset[k].y,fovea.inset[k].w,fovea.inset[k].h);
                        }
                    }
                    noteStage(3);
                    endEye();
                }
                D3DVIEWPORT9 ev{0,0,width,height,0,1};
                ok=ok && centreReady && preserveViewportDepth(d,ev) && SUCCEEDED(originalSetRT(d,0,uiFrame.target[i])) &&
                   SUCCEEDED(originalSetDepth(d,uiFrame.z[i])) && SUCCEEDED(d->SetViewport(&ev)) &&
                   SUCCEEDED(d->SetScissorRect(&uiFrame.scissor)) && singlePassAdoptEye(puppet,request,i);
            } else {
            beginEye(puppet,request,i);
            D3DVIEWPORT9 ev{0,0,width,height,0,1};
            ok=preserveViewportDepth(d,ev) && SUCCEEDED(originalSetRT(d,0,uiFrame.target[i])) && SUCCEEDED(originalSetDepth(d,uiFrame.z[i]));
            ok=SUCCEEDED(d->SetViewport(&ev)) && ok;
            ok=SUCCEEDED(d->SetScissorRect(&uiFrame.scissor)) && ok;
            ok=ok && SUCCEEDED(d->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER|
                (hasStencil ? D3DCLEAR_STENCIL : 0),0xff000000,1,0));
            noteStage(2);
            }
            if(ok && !singlePassFrame) {
                if(traceDepth) traceStereoDepth(d,request,i,"before-world");
                const LARGE_INTEGER eyeStart=PerfClock::now();
                sampler::inEyes.store(true, std::memory_order_relaxed);
                bool replayed=false;
                bool compareShot=false;
                if(useReplay && pass==1 && replayReady && labReplayVerify) {
                    // Lab verification: native right eye, then compare the derived matrices
                    // (and, on sampled frames, the replayed image against the native one).
                    if(replay_compare::due() && (replay_compare::prepare(), true) && replay::play(d,uiFrame.target[i],uiFrame.z[i],replayChange,replayRootState()) &&
                       replay_compare::keep(d,uiFrame.target[i])) {
                        compareShot=true;
                        D3DVIEWPORT9 again{0,0,width,height,0,1};
                        ok=preserveViewportDepth(d,again) && SUCCEEDED(originalSetRT(d,0,uiFrame.target[i])) &&
                           SUCCEEDED(originalSetDepth(d,uiFrame.z[i])) && SUCCEEDED(d->SetViewport(&again)) &&
                           SUCCEEDED(d->SetScissorRect(&uiFrame.scissor)) &&
                           SUCCEEDED(d->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER|(hasStencil ? D3DCLEAR_STENCIL : 0),0xff000000,1,0));
                    }
                } else if(useReplay && pass==1 && replayReady && generation==graphicsResourceGeneration()) {
                    replayed=replay::play(d,uiFrame.target[i],uiFrame.z[i],replayChange,replayRootState()) && replayAdoptEye(replayChange);
                    if(!replayed) {
                        // Native right eye after an unusable replay: start again from a clean target.
                        ++perf.replayFallbacks;
                        D3DVIEWPORT9 again{0,0,width,height,0,1};
                        ok=preserveViewportDepth(d,again) && SUCCEEDED(originalSetRT(d,0,uiFrame.target[i])) &&
                           SUCCEEDED(originalSetDepth(d,uiFrame.z[i])) && SUCCEEDED(d->SetViewport(&again)) &&
                           SUCCEEDED(d->SetScissorRect(&uiFrame.scissor)) &&
                           SUCCEEDED(d->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER|(hasStencil ? D3DCLEAR_STENCIL : 0),0xff000000,1,0));
                    }
                } else if(useReplay && pass==1) ++perf.replayFallbacks;
                const bool record=useReplay && pass==0 && replay::begin(d,uiFrame.target[i],uiFrame.z[i]);
                if(!replayed && ok) ok=observedWorldRender(d,puppet,original,request,i,traceDepth);
                if(useReplay && pass==1 && replayReady && labReplayVerify && ok) replayVerifyEye(replayChange);
                if(compareShot && ok) replay_compare::compare(d,uiFrame.target[i]);
                if(record) {
                    replay::end();
                    replayReady=ok && generation==graphicsResourceGeneration() && replay::usable() &&
                                replayCaptureEyeGeometry(replayChange);
                }
                sampler::inEyes.store(false, std::memory_order_relaxed);
                perf.eyeCpu[pass]+=PerfClock::seconds(eyeStart,PerfClock::now());
                if(i==0 && generation==graphicsResourceGeneration()) noteScopeViews(puppet,request);
                noteStage(3);
                if(traceDepth && generation==graphicsResourceGeneration()) traceStereoDepth(d,request,i,"after-world");
            }
            if (generation != graphicsResourceGeneration()) { ok = false; break; }
            static unsigned eyeDiagnostics=0;
            const bool traceEye=eyeDiagnostics<4;
            if(traceEye) {
                ++eyeDiagnostics;
                log("Native eye render returned eye=%d setup=%u invalid=%u",i,ok,eyeInvalid);
            }
            if(uiFrame.ui && ok) {
                ok=copyExecutedUiProjection(puppet,request,i,uiFrame.projection[i]);
                Matrix44 identity{};
                identity.m[0]=identity.m[5]=identity.m[10]=identity.m[15]=1;
                NativeUiProjection admission;
                // An empty projection means the HUD panel is not in front of this
                // eye (a head turn it has not followed yet); HUD draws skip the eye.
                ok=ok && nativeUiProjection(identity,uiFrame.projection[i],uiFrame.panel[i],
                    request.uiWidth,request.uiHeight,request.width,request.height,
                    {0,0,request.width,request.height,0,1},false,{},admission);
                uiFrame.panelHidden[i]=ok && admission.empty;
                if(traceEye) log("Native eye UI projection eye=%d admitted=%u empty=%u",i,ok,admission.empty);
                noteStage(4);
            }
            if(ok && eyeInvalid) { ok=false; noteStage(5); }
            bool targetCurrent=true;
            ok=finishEyeRender(ok && !eyeInvalid,
                [&]{
                    const bool current=generation==graphicsResourceGeneration() && !eyeInvalid && eyeTargetCurrent(i);
                    if(traceEye) log("Native eye target eye=%d current=%u",i,current);
                    targetCurrent=current;
                    return current;
                },
                [&]{
                    const bool complete=generation==graphicsResourceGeneration() && (!postRender || postRender(puppet,request,i)) &&
                        generation==graphicsResourceGeneration();
                    if(traceDepth && generation==graphicsResourceGeneration()) traceStereoDepth(d,request,i,"after-postlude");
                    if(traceEye) log("Native eye postlude eye=%d completed=%u",i,complete);
                    if(i==1) labStageDump(d,uiFrame.target[1],L"stage-b-postlude.ppm");
                    return complete;
                });
            if(!ok && !stageNoted) noteStage(targetCurrent ? 7 : 6);
            endEye(); activeEye=-1;
            gpuProfile.mark(pass+1);
            if(ok && generation == graphicsResourceGeneration() && !uiFrame.zeroCopy && (!uiFrame.ui || visibility!=1)) ok=readUiEye(i,uiFrame.ui,visibility);
            noteStage(8);
            if(i==1) labStageDump(d,uiFrame.target[1],L"stage-c-eye.ppm");
        }
        replay::reset();
        activeEye=-1; endEye(); desktopTarget=desktopDepth=nullptr;
        ok=restoreUiFrame() && ok;
        noteStage(9);
        // Native desktop rebuild remains exactly once; never replay brain/overlay.
        if(traceDepth && generation==graphicsResourceGeneration()) traceStereoDepth(d,request,-1,"before-desktop");
        const LARGE_INTEGER desktopStart=PerfClock::now();
        if(desktopWorld || traceDepth) {
            ok=observedWorldRender(d,puppet,original,request,-1,traceDepth) && ok;
        } else {
            // The desktop window mirrors the left eye; the native mono render is
            // skipped (a third full scene traversal), keeping its bookkeeping.
            monoPresentationWithoutRender(puppet);
            if (generation == graphicsResourceGeneration() && uiFrame.color)
                originalStretch(d,uiFrame.target[0],nullptr,uiFrame.color,nullptr,D3DTEXF_NONE);
        }
        perf.desktopCpu+=PerfClock::seconds(desktopStart,PerfClock::now());
        if (generation != graphicsResourceGeneration()) return;
        if(traceDepth) traceStereoDepth(d,request,-1,"after-desktop");
        uiFrame.world=ok && !uiFrame.fault && uiFrame.generation==resourceGeneration.load();
        static unsigned completionDiagnostics = 0;
        if (completionDiagnostics < 4) {
            ++completionDiagnostics;
            log("Native world render completion ok=%u fault=%u world=%u ui=%u",ok,uiFrame.fault,uiFrame.world,uiFrame.ui);
        }
        gpuProfile.mark(3);
        if(!uiFrame.ui) { gpuProfile.mark(4); gpuProfile.end(); }
        if(uiFrame.ui) {
            if(!uiCurrent()) nativeUiFault();
            retained=true; // Even a partial UI pair remains owned until enclosing cleanup.
        } else if(uiFrame.world) {
            const LARGE_INTEGER dimStart=PerfClock::now();
            const auto dimming=makeOpaqueDimming(visibility);
            if(uiFrame.zeroCopy) issueSharedFence();
            else if(!dimming.passthrough) for(auto *image:uiFrame.pixels) if(image) dimOpaquePixels(image,uiFrame.pixelBytes/4,dimming);
            perf.dim+=PerfClock::seconds(dimStart,PerfClock::now());
            publishUiFrame(0);
        }
    },[&](bool aborted) noexcept {
        if(swappedColor && swappedDepth) {
            std::swap(uiFrame.target[0],*swappedColor); std::swap(uiFrame.z[0],*swappedDepth);
            swappedColor=swappedDepth=nullptr;
        }
        if(aborted || !retained) nativeUiEndOwner(aborted && generation == graphicsResourceGeneration());
    });
}
void deviceResetSucceeded(IDirect3DDevice9 *d) {
    if (d==creationDevice && GetCurrentThreadId()==creationThread) {
        uiHalted=false;
        resetQuarantined=false;
    }
    // Native additional chains are recreated after Reset returns. No eager
    // allocation/channel startup against the implicit buffer or retired owner.
}
void shutdown() {
    stopping = true;
    if (channel.shared) {
        Lock l(channel);
        if (l)
            channel.shared->shutdown = 1;
    }
    deviceLost();
    if (device)
        device->Release();
    device = nullptr;
}
} // namespace ss2vr::game
