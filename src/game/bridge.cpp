#include "common/color.hpp"
#include "common/controls.hpp"
#include "common/foveation.hpp"
#include "common/winpath.hpp"
#include "common/frame_policy.hpp"
#include "common/native_ui.hpp"
#include "common/native_ui_finish.hpp"
#include "common/world_markers.hpp"
#include "game.hpp"
#include "menu.hpp"
#include "native_finally.hpp"
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
static __attribute__((noinline)) void __cdecl suppressLabOnlineInitialization() {
    const auto caller=reinterpret_cast<uintptr_t>(__builtin_extract_return_addr(__builtin_return_address(0)));
    uint32_t pointer = 0;
    std::memcpy(&pointer,reinterpret_cast<const void *>(labEngineBase+0x2ec890),sizeof(pointer));
    if (labIsolationPhase.load(std::memory_order_acquire)!=2 || !labOnlineIsolated ||
        GetCurrentThreadId()!=labStartupThread || pointer || caller!=labInitializerCaller)
        abortLab("unexpected online initializer owner or existing interface");
    static bool reported = false;
    if (!reported) { reported=true; log("Lab native online initializer suppressed; interface=0; cloud/stats object not constructed"); }
}
static void __cdecl labOnlineUninitialize() {
    if(labIsolationPhase.load(std::memory_order_acquire)!=2) abortLab("incomplete isolation reached native shutdown");
    uint32_t pointer=0;
    std::memcpy(&pointer,reinterpret_cast<const void *>(labEngineBase+0x2ec890),sizeof(pointer));
    if (pointer) abortLab("online interface appeared before native shutdown");
    log("Lab native online shutdown wrapper entered; interface=0; Steam synchronization body excluded");
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
    void *unusedOriginal=nullptr;
    if (MH_CreateHook(entry,reinterpret_cast<void *>(suppressLabOnlineInitialization),&unusedOriginal)!=MH_OK)
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
    if (pointer) abortLab("online interface appeared during private fixture");
    const auto *net=labCurrentNet();
    if (!net) { if (gameplay) abortLab("gameplay has no native local transport"); return; }
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
    bool restore = false, ui = false, fault = false, world = false;
    bool overlay = false, overlaySeen = false, complete = false, fade = false;
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
bool scopeGpuEyeOwner(IDirect3DDevice9 *d,IUnknown *&color,IUnknown *&z,UINT &w,UINT &h) noexcept {
    if(color || z || !scopeGpuRoutingCurrent(d) || activeEye<0 || activeEye>1 ||
        uiFrame.slot<0 || uiFrame.device!=d || uiFrame.fault || uiFrame.overlay || stopping || uiHalted ||
        uiFrame.generation!=graphicsResourceGeneration() ||
        renderRequest.sequence!=uiFrame.request.sequence ||
        renderRequest.input.sequence!=uiFrame.request.input.sequence ||
        !uiFrame.target[activeEye] || !uiFrame.z[activeEye]) return false;
    w=uiFrame.request.width; h=uiFrame.request.height;
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
    const char *captureStage = "not-started";
    bool busy = false, captured = false, changed = false;
};
static thread_local NativeUiDrawState uiDraw;
static constexpr D3DRENDERSTATETYPE uiStates[]{D3DRS_ZENABLE, D3DRS_ZWRITEENABLE,
    D3DRS_SCISSORTESTENABLE, D3DRS_CLIPPLANEENABLE};
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
template<class Call> static HRESULT uiDrawCall(IDirect3DDevice9 *d,D3DPRIMITIVETYPE topology,Call call) noexcept {
    const auto generation = graphicsResourceGeneration();
    if (!scopeGpuForwardingAllowed()) return D3DERR_INVALIDCALL;
    if (!observingUi(d)) return call();
    if (uiDraw.busy) { nativeUiFault(); return call(); }
    uiDraw.busy = true;
    HRESULT result = D3DERR_INVALIDCALL;
    const bool contained = withNativeFinally([&] {
        result = call(); // Desktop first, exactly once; preserve its HRESULT/state effects.
        if (generation != graphicsResourceGeneration()) return;
        if (FAILED(result)) { nativeUiFault("desktop-draw-hresult"); return; }
        if (!captureUiDraw(d)) {
            // Offscreen drawing stays native; only eventual desktop composition is adapted.
            if (!uiDraw.color || sameSurface(uiDraw.color,uiFrame.color)) nativeUiFault(uiDraw.captureStage);
            return;
        }
        if (!nativeUiTriangleTopology(topology,uiDraw.fill)) { nativeUiFault("topology-or-fill"); return; }
        NativeUiProjection projected[2];
        for (unsigned i=0;i<2;++i) {
            if (uiFrame.fade) {
                const auto &v=uiDraw.viewport;
                if (v.X || v.Y || v.Width!=uiFrame.request.width || v.Height!=uiFrame.request.height ||
                    v.MinZ!=0 || v.MaxZ!=1 || uiDraw.state[2]) { nativeUiFault(); return; }
                projected[i].constants = uiDraw.constants;
            } else {
                const auto &v=uiDraw.viewport;
                const NativeUiViewport vp{v.X,v.Y,v.Width,v.Height,v.MinZ,v.MaxZ};
                const auto &r=uiDraw.scissor;
                if (!nativeUiProjection(uiDraw.constants,uiFrame.projection[i],uiFrame.panel[i],
                    uiFrame.request.uiWidth,uiFrame.request.uiHeight,uiFrame.request.width,
                    uiFrame.request.height,vp,uiDraw.state[2]!=0,{r.left,r.top,r.right,r.bottom},projected[i])) {
                    nativeUiFault(); return;
                }
            }
        }
        uiBypass = true;
        uiDraw.changed = true; // Any partial mutation must attempt every restore.
        for (unsigned i=0;i<2;++i) {
            if (!uiCurrent()) { nativeUiFault(); break; }
            if (projected[i].empty) continue;
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
            if (!ok || FAILED(call()) || generation != graphicsResourceGeneration()) { nativeUiFault(); break; }
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
static void traceWorldDraw(IDirect3DDevice9 *d,UINT count) {
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
        traceWorldDraw(d,count);
        return originalDraw(d,type,start,count);
    });
}
static HRESULT forwardIndexed(IDirect3DDevice9 *d,D3DPRIMITIVETYPE type,INT base,UINT minimum,
                               UINT vertices,UINT start,UINT count) {
    return uiDrawCall(d,type,[&]{
        if(!scopeGpuForwardingAllowed()) return D3DERR_INVALIDCALL;
        traceWorldDraw(d,count);
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
using Clear = HRESULT(WINAPI *)(IDirect3DDevice9 *,DWORD,const D3DRECT *,DWORD,D3DCOLOR,float,DWORD);
using DrawUp = HRESULT(WINAPI *)(IDirect3DDevice9 *,D3DPRIMITIVETYPE,UINT,const void *,UINT);
using DrawIndexedUp = HRESULT(WINAPI *)(IDirect3DDevice9 *,D3DPRIMITIVETYPE,UINT,UINT,UINT,const void *,D3DFORMAT,const void *,UINT);
using DrawRect = HRESULT(WINAPI *)(IDirect3DDevice9 *,UINT,const float *,const D3DRECTPATCH_INFO *);
using DrawTri = HRESULT(WINAPI *)(IDirect3DDevice9 *,UINT,const float *,const D3DTRIPATCH_INFO *);
using Update = HRESULT(WINAPI *)(IDirect3DDevice9 *,IDirect3DSurface9 *,const RECT *,IDirect3DSurface9 *,const POINT *);
static Clear originalClear = nullptr;
static DrawUp originalDrawUp = nullptr;
static DrawIndexedUp originalDrawIndexedUp = nullptr;
static DrawRect originalDrawRect = nullptr;
static DrawTri originalDrawTri = nullptr;
static Update originalUpdate = nullptr;
static HRESULT WINAPI clear(IDirect3DDevice9 *d,DWORD count,const D3DRECT *rects,DWORD flags,D3DCOLOR color,float z,DWORD stencil) {
    scopeGpuOutput(d);
    const auto call=[&]{ return originalClear(d,count,rects,flags,color,z,stencil); };
    return flags&D3DCLEAR_TARGET ? observeUiOutput(d,call,nullptr,true) : call();
}
static HRESULT WINAPI drawUp(IDirect3DDevice9 *d,D3DPRIMITIVETYPE type,UINT count,const void *vertices,UINT stride) {
    scopeGpuOutput(d);
    return observeUiOutput(d,[&]{ traceWorldDraw(d,count);return originalDrawUp(d,type,count,vertices,stride); },nullptr,true);
}
static HRESULT WINAPI drawIndexedUp(IDirect3DDevice9 *d,D3DPRIMITIVETYPE type,UINT minimum,UINT vertices,UINT count,
                                    const void *indices,D3DFORMAT format,const void *data,UINT stride) {
    scopeGpuOutput(d);
    return observeUiOutput(d,[&]{ traceWorldDraw(d,count);return originalDrawIndexedUp(d,type,minimum,vertices,count,indices,format,data,stride); },nullptr,true);
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
        observeUiOutput(d, [&] { return originalStretch(d, eyeSurface(source), sourceRect, eyeSurface(target), targetRect, filter); }, target, false);
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
    HRESULT result = observeUiOutput(d, [&] { return originalFill(d, eyeSurface(target), rect, color); }, target, false);
    if ((activeEye >= 0 || scopeScratch) && FAILED(result))
        eyeInvalid = true;
    return result;
}
static HRESULT WINAPI setDepth(IDirect3DDevice9 *d, IDirect3DSurface9 *target) {
    scopeGpuOutput(d);
    if ((activeEye >= 0 || scopeScratch) && sameSurface(target, desktopDepth))
        target = uiFrame.slot >= 0 ? uiFrame.z[scopeScratch ? 0 : activeEye] : depth[scopeScratch ? 0 : activeEye];
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
    HRESULT result = originalSetRT(d, index, target);
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
static void releaseTargets() {
    ++resourceGeneration;
    nativeUiFault();
    if (uiQuarantinedReadback) uiQuarantinedReadback->Release();
    uiQuarantinedReadback=nullptr;
    invalidateRenderer();
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
    if (eye[0] && desc.Width == width && desc.Height == height && depthDesc.Format == depthFormat &&
        desc.Format == colorFormat) {
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
void present(IDirect3DDevice9 *d) {
    if (!presentationChain || resetQuarantined || !nativeUiDeviceCurrent(d) ||
        !nativePresentationThreadCurrent() || uiHalted || stopping) return;
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
        const uint32_t extras = valid ? ((input.buttons[0] | input.buttons[1]) & (Reload | Netricsa)) : 0;
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
    withNativeFinally([&] {
        if (FAILED(sourceBackbuffer(&backbuffer)) || !readback[0]) return;
        cpu = readback[0]; cpu->AddRef();
        if (FAILED(device->GetRenderTargetData(backbuffer, cpu)) ||
            generation != resourceGeneration.load(std::memory_order_acquire)) return;
        backbuffer->Release(); backbuffer = nullptr;
        D3DLOCKED_RECT pixels{};
        lockAttempted = true;
        const auto result = cpu->LockRect(&pixels, nullptr, D3DLOCK_READONLY);
        lockReturned = true; locked = SUCCEEDED(result);
        if (!locked || !pixels.pBits || pixels.Pitch < int(copyWidth * 4) ||
            generation != resourceGeneration.load(std::memory_order_acquire)) return;
        uint8_t *target = pixelStore.matches(pixelStore.generation, copyWidth, copyHeight) ?
            pixelStore.buffer(MenuPixelBuffer) : nullptr;
        if (!target) return;
        held = channel.lock();
        if (!held) return;
        auto &m = channel.shared->menu;
        for (uint32_t y = 0; y < copyHeight; ++y)
            memcpy(target + size_t(y) * copyWidth * 4,
                static_cast<uint8_t *>(pixels.pBits) + size_t(y) * pixels.Pitch, copyWidth * 4);
        for (size_t pixel = 3; pixel < size_t(copyWidth) * copyHeight * 4; pixel += 4) target[pixel] = 255;
        m.width = copyWidth; m.height = copyHeight; m.tickMs = now;
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
    uiFrame.player=nullptr;
    uiFrame.slot=-1;
    admittedUiFrame=nullptr;
    uiFrame.ui=uiFrame.fault=uiFrame.world=uiFrame.restore=false;
    uiFrame.overlay=uiFrame.overlaySeen=uiFrame.complete=uiFrame.fade=false;
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
    return true;
}
void nativeUiEndOverlay(bool completed) noexcept {
    if (uiFrame.slot<0 || !uiFrame.ui) return;
    if (!completed || !uiFrame.overlay || uiFrame.fade || uiDraw.busy) nativeUiFault();
    uiFrame.complete=completed && uiFrame.overlay && !uiFrame.fault;
    uiFrame.overlay=false;
    uiFrame.fade=false;
}
bool nativeUiBeginFade() {
    if (!uiFrame.overlay || !uiFrame.ui) return false;
    if (uiFrame.fade || !uiCurrent()) { nativeUiFault(); return false; }
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
    if (!dim && uiFrame.request.foveated) return readFoveatedEye(i);
    auto *d=uiFrame.device;
    auto *read=uiFrame.read[i];
    if (FAILED(d->GetRenderTargetData(uiFrame.target[i],read))) return false;
    D3DLOCKED_RECT locked{};
    if (FAILED(read->LockRect(&locked,nullptr,dim ? 0 : D3DLOCK_READONLY))) return false;
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
static bool publishUiFrame(uint32_t presentation) {
    if (!channel.shared || !uiFrame.world || uiFrame.fault ||
        uiFrame.generation!=resourceGeneration.load()) return false;
    // No native rendering while IPC is held. commitStereo retains the existing
    // native snapshot/remote binding admission through the Ready transition.
    if (!channel.lock(2)) return false;
    bool accepted=false;
    withNativeFinally([&] {
        auto &slot=channel.shared->slot[uiFrame.slot];
        if (!slot.cancelled && slot.state==SlotState::Rendering && sameRequest(slot.request,uiFrame.request) &&
            uiFrame.pixels[0] && uiFrame.pixels[1] && uiFrame.pixelGeneration==channel.shared->pixels.generation) {
            // Eyes were read back straight into this slot's own pixel sections.
            slot.pixelGeneration=uiFrame.pixelGeneration;
            for (unsigned i=0;i<2;++i) {
                const auto &r=uiFrame.insetRect[i];
                slot.insetRect[i][0]=r.x; slot.insetRect[i][1]=r.y; slot.insetRect[i][2]=r.w; slot.insetRect[i][3]=r.h;
            }
            slot.pixelReserved=0;
            slot.presentation=presentation;
            slot.presentationReserved=0;
            accepted=commitStereo(uiFrame.player,uiFrame.request,slot,uiFrame.remoteOwner);
            if (accepted) SetEvent(channel.ready);
            else slot.state=SlotState::Empty;
        }
    },[&](bool aborted) noexcept {
        if (aborted) { uiFrame.fault=true; uiHalted=true; }
        channel.unlock();
    });
    if (accepted) {
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
    if (!current) { nativeUiFault(); return; }
    // UI and original full-field fades have now finished in BOTH owned eye RTs.
    const bool left=readUiEye(0,false,1),right=left && readUiEye(1,false,1);
    bool ok=left && right;
    if(trace)log("Native UI readback left=%u right=%u",left,right);
    if (ok) {
        const auto opaque=makeOpaqueDimming(1);
        for (auto *image:uiFrame.pixels) dimOpaquePixels(image,uiFrame.pixelBytes/4,opaque);
    }
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
static __attribute__((force_align_arg_pointer)) void __cdecl captureNativeScope(void *opaque) noexcept {
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
static bool renderScopeSources(void *puppet,void(__thiscall *original)(void *),const Request &request) {
    const auto generation = graphicsResourceGeneration();
    if (scopeSourcesHalted || !scopePreviewNeeded(puppet,request)) return true;
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
    if (FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD *>(ScopeImageOpaqueProgram),&uiFrame.scopeShader)) ||
        !uiFrame.scopeShader) return true;
    activeEye = 0; eyeInvalid = false; renderRequest = request;
    beginScopePreview(puppet,request);
    bool ok = prepareScopeScratch();
    if (ok) original(puppet);
    if (generation != graphicsResourceGeneration()) return false;
    ok = ok && !eyeInvalid && eyeTargetCurrent(0);
    bool candidate[2]{};
    if (ok) for (unsigned hand=0;hand<2;++hand) candidate[hand] = copyScopeSourceView(hand,uiFrame.scopeView[hand]);
    endEye(); activeEye = -1;
    for (unsigned hand=0;hand<2 && ok;++hand) {
        if (!candidate[hand]) continue;
        if (FAILED(d->CreateTexture(request.width,request.height,1,D3DUSAGE_RENDERTARGET,colorFormat,
                                   D3DPOOL_DEFAULT,&uiFrame.scopeImage[hand],nullptr)) || !uiFrame.scopeImage[hand]) continue;
        scopeCaptureFrame = {hand,uiFrame.generation,true,false,{}};
        scopeScratch = true; eyeInvalid = false;
        const bool begun = beginScopeSource(puppet,request,uiFrame.scopeView[hand],captureNativeScope,
                                            scopeCaptureCurrent,&scopeCaptureFrame);
        if (begun) {
            ok = prepareScopeScratch();
            if (ok) original(puppet);
            if (generation != graphicsResourceGeneration()) return false;
            const bool finished = endScopeSource(ok && !eyeInvalid);
            uiFrame.scopeReady[hand] = scopeCaptureFrame.once.complete(finished,scopeCaptureFrame.copied) &&
                scopeCaptureCurrent(&scopeCaptureFrame) && nativeUiFrameCurrent(puppet,request);
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
void stereo(void *puppet,void(__thiscall *original)(void *),EyePostRender postRender) {
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
    {
        Lock l(channel);
        if(l) for(int i=0;i<2;++i) {
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
    for(unsigned i=0;i<2;++i) {
        uiFrame.target[i]=eye[i]; eye[i]->AddRef();
        uiFrame.z[i]=depth[i]; depth[i]->AddRef();
        uiFrame.read[i]=readback[i]; readback[i]->AddRef();
    }
    bool retained=false;
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
        if(ok) ok=beginStereo(puppet,request,uiFrame.remoteOwner);
        if (!ok) {
            static unsigned beginDiagnostics = 0;
            if (beginDiagnostics < 4) { ++beginDiagnostics; log("Native world pair admission rejected"); }
        }
        if (generation != graphicsResourceGeneration()) { original(puppet); return; }
        const float visibility=ok ? frozenWorldVisibility() : 1;
        if(traceDepth) traceStereoDepth(d,request,-1,"entry");
        if (ok) ok = renderScopeSources(puppet,original,request);
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
        for(unsigned pass=0;pass<2 && ok;++pass) {
            const int i=labWorldEyeForPass(pass,request.sequence,alternateEyes);
            activeEye=i; eyeInvalid=false; renderRequest=request;
            beginEye(puppet,request,i);
            D3DVIEWPORT9 ev{0,0,width,height,0,1};
            ok=preserveViewportDepth(d,ev) && SUCCEEDED(originalSetRT(d,0,uiFrame.target[i])) && SUCCEEDED(originalSetDepth(d,uiFrame.z[i]));
            ok=SUCCEEDED(d->SetViewport(&ev)) && ok;
            ok=SUCCEEDED(d->SetScissorRect(&uiFrame.scissor)) && ok;
            ok=ok && SUCCEEDED(d->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER|
                (hasStencil ? D3DCLEAR_STENCIL : 0),0xff000000,1,0));
            if(ok) {
                if(traceDepth) traceStereoDepth(d,request,i,"before-world");
                ok=observedWorldRender(d,puppet,original,request,i,traceDepth);
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
                ok=ok && nativeUiProjection(identity,uiFrame.projection[i],uiFrame.panel[i],
                    request.uiWidth,request.uiHeight,request.width,request.height,
                    {0,0,request.width,request.height,0,1},false,{},admission) && !admission.empty;
                if(traceEye) log("Native eye UI projection eye=%d admitted=%u empty=%u",i,ok,admission.empty);
            }
            ok=finishEyeRender(ok && !eyeInvalid,
                [&]{
                    const bool current=generation==graphicsResourceGeneration() && !eyeInvalid && eyeTargetCurrent(i);
                    if(traceEye) log("Native eye target eye=%d current=%u",i,current);
                    return current;
                },
                [&]{
                    const bool complete=generation==graphicsResourceGeneration() && (!postRender || postRender(puppet,request,i)) &&
                        generation==graphicsResourceGeneration();
                    if(traceDepth && generation==graphicsResourceGeneration()) traceStereoDepth(d,request,i,"after-postlude");
                    if(traceEye) log("Native eye postlude eye=%d completed=%u",i,complete);
                    return complete;
                });
            endEye(); activeEye=-1;
            if(ok && generation == graphicsResourceGeneration() && (!uiFrame.ui || visibility!=1)) ok=readUiEye(i,uiFrame.ui,visibility);
        }
        activeEye=-1; endEye(); desktopTarget=desktopDepth=nullptr;
        ok=restoreUiFrame() && ok;
        // Native desktop rebuild remains exactly once; never replay brain/overlay.
        if(traceDepth && generation==graphicsResourceGeneration()) traceStereoDepth(d,request,-1,"before-desktop");
        ok=observedWorldRender(d,puppet,original,request,-1,traceDepth) && ok;
        if (generation != graphicsResourceGeneration()) return;
        if(traceDepth) traceStereoDepth(d,request,-1,"after-desktop");
        uiFrame.world=ok && !uiFrame.fault && uiFrame.generation==resourceGeneration.load();
        static unsigned completionDiagnostics = 0;
        if (completionDiagnostics < 4) {
            ++completionDiagnostics;
            log("Native world render completion ok=%u fault=%u world=%u ui=%u",ok,uiFrame.fault,uiFrame.world,uiFrame.ui);
        }
        if(uiFrame.ui) {
            if(!uiCurrent()) nativeUiFault();
            retained=true; // Even a partial UI pair remains owned until enclosing cleanup.
        } else if(uiFrame.world) {
            const auto dimming=makeOpaqueDimming(visibility);
            for(auto *image:uiFrame.pixels) if(image) dimOpaquePixels(image,uiFrame.pixelBytes/4,dimming);
            publishUiFrame(0);
        }
    },[&](bool aborted) noexcept {
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
