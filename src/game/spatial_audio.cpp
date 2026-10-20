// Spatial audio, game side (spatial_audio.hpp).
#include "spatial_audio.hpp"
#include "common/spatial_audio.hpp"
#include "game.hpp"
#include "common/win_settings.hpp"
#include <MinHook.h>
#include <windows.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>

namespace ss2vr::game::spatial_audio {
namespace {

using ALuint = unsigned;
using ALint = int;
using ALenum = int;
using ALfloat = float;
using ALsizei = int;
constexpr ALenum AlPosition = 0x1004, AlDirectFilter = 0x20005, AlFilterType = 0x8001, AlFilterLowpass = 1,
                 AlLowpassGain = 1, AlLowpassGainHF = 2;
constexpr int ApiOpenAl = 1, ApiDirectSound = 2;

Config config;
SRWLOCK lock = SRWLOCK_INIT; // Tracker and listener (OpenAL may call from the sound thread).
::ss2vr::spatial_audio::Tracker tracker;
Pose head{}, used{};
uint64_t headTick = 0;
bool usedValid = false;
HMODULE openal = nullptr;
bool prepared = false, preparedOk = false, hooked = false;
uint64_t lastFrame = 0;
std::atomic<unsigned> playCalls{0}, positionCalls{0}, listenerCalls{0}, listenerHead{0}, traceCalls{0}, blockedCalls{0};

void *(__cdecl *alcOpenDevice)(const char *) = nullptr;
int(__cdecl *alcCloseDevice)(void *) = nullptr;
void *(__cdecl *alcGetCurrentContext)() = nullptr;
void(__cdecl *alSourcei)(ALuint, ALenum, ALint) = nullptr;
void(__cdecl *alGenFilters)(ALsizei, ALuint *) = nullptr;
void(__cdecl *alFilteri)(ALuint, ALenum, ALint) = nullptr;
void(__cdecl *alFilterf)(ALuint, ALenum, ALfloat) = nullptr;
int(__cdecl *alIsSource)(ALuint) = nullptr;
void(__cdecl *originalSource3f)(ALuint, ALenum, ALfloat, ALfloat, ALfloat) = nullptr;
void(__cdecl *originalDeleteSources)(ALsizei, const ALuint *) = nullptr;
void(__cdecl *originalSourcePlay)(ALuint) = nullptr;

// Per source: its EFX lowpass and the values last applied.
struct Applied {
    ALuint source = 0, filter = 0;
    float gain = 1, gainHF = 1;
    bool used = false;
};
Applied applied[::ss2vr::spatial_audio::MaxSources];

void __cdecl source3f(ALuint source, ALenum parameter, ALfloat x, ALfloat y, ALfloat z) {
    originalSource3f(source, parameter, x, y, z);
    if (parameter != AlPosition) return;
    ++positionCalls;
    AcquireSRWLockExclusive(&lock);
    tracker.position(source, {x, y, z});
    ReleaseSRWLockExclusive(&lock);
}
void __cdecl deleteSources(ALsizei count, const ALuint *sources) {
    AcquireSRWLockExclusive(&lock);
    for (ALsizei i = 0; sources && i < count; ++i) {
        tracker.remove(sources[i]);
        for (auto &a : applied)
            if (a.used && a.source == sources[i]) a.used = false; // The filter object is kept for reuse.
    }
    ReleaseSRWLockExclusive(&lock);
    originalDeleteSources(count, sources);
}

void __cdecl sourcePlay(ALuint source) {
    ++playCalls;
    originalSourcePlay(source);
}
std::wstring binDirectory() {
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    std::wstring s(path, n);
    const auto slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : s.substr(0, slash + 1);
}

template <class F> bool resolve(F &out, const char *name) {
    out = reinterpret_cast<F>(GetProcAddress(openal, name));
    return out != nullptr;
}

bool hookOpenAl() {
    void *s3f = reinterpret_cast<void *>(GetProcAddress(openal, "alSource3f"));
    void *del = reinterpret_cast<void *>(GetProcAddress(openal, "alDeleteSources"));
    void *play = reinterpret_cast<void *>(GetProcAddress(openal, "alSourcePlay"));
    if (!s3f || !del || !play) return false;
    if (MH_CreateHook(play, reinterpret_cast<void *>(sourcePlay), reinterpret_cast<void **>(&originalSourcePlay)) != MH_OK ||
        MH_EnableHook(play) != MH_OK)
        return false;
    if (MH_CreateHook(s3f, reinterpret_cast<void *>(source3f), reinterpret_cast<void **>(&originalSource3f)) != MH_OK ||
        MH_CreateHook(del, reinterpret_cast<void *>(deleteSources), reinterpret_cast<void **>(&originalDeleteSources)) != MH_OK)
        return false;
    return MH_EnableHook(s3f) == MH_OK && MH_EnableHook(del) == MH_OK;
}

// Lab (SS2VR_LAB_CRASH_TRACE=1): the first access violations, by module and offset.
LONG CALLBACK crashTrace(EXCEPTION_POINTERS *e) {
    static std::atomic<unsigned> count{0};
    if (!e || !e->ExceptionRecord || e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || count++ >= 6)
        return EXCEPTION_CONTINUE_SEARCH;
    const auto *r = e->ExceptionRecord;
    HMODULE module = nullptr;
    char name[MAX_PATH]{};
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCSTR>(r->ExceptionAddress), &module);
    if (module) GetModuleFileNameA(module, name, MAX_PATH);
    const auto *c = e->ContextRecord;
    log("Lab access violation at %s+%lx (%s %08lx) eax=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx esp=%08lx",
        name[0] ? name : "?", static_cast<unsigned long>(reinterpret_cast<uintptr_t>(r->ExceptionAddress) - reinterpret_cast<uintptr_t>(module)),
        r->ExceptionInformation[0] ? "write" : "read", static_cast<unsigned long>(r->ExceptionInformation[1]),
        c->Eax, c->Ecx, c->Edx, c->Esi, c->Edi, c->Esp);
    const auto *stack = reinterpret_cast<const uint32_t *>(c->Esp);
    for (unsigned i = 0; i < 24; ++i) {
        MEMORY_BASIC_INFORMATION m{};
        if (!VirtualQuery(stack + i, &m, sizeof m) || m.State != MEM_COMMIT) break;
        HMODULE owner = nullptr;
        char ownerName[MAX_PATH]{};
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(uintptr_t(stack[i])), &owner) && owner) {
            GetModuleFileNameA(owner, ownerName, MAX_PATH);
            const char *base = std::strrchr(ownerName, '\\');
            log("  stack[%u] %s+%lx", i, base ? base + 1 : ownerName, static_cast<unsigned long>(stack[i] - reinterpret_cast<uintptr_t>(owner)));
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
void(__cdecl *originalStartup)(int, long) = nullptr;
void __cdecl startup(int api, long frequency) {
    // The engine's settings are not loaded yet: read [Audio] here.
    const std::wstring bin = binDirectory();
    if (!bin.empty()) {
        const VrSettings s = loadSettings(bin + L"SS2VR\\SS2VR.ini");
        config = {s.spatialAudio, s.audioOcclusion};
    }
    const int chosen = chooseApi(api);
    originalStartup(chosen, frequency);
    log("Spatial audio: sound started (api %d)", chosen);
}

} // namespace

void configure(const Config &c) { config = c; }

void installStartupHook() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    wchar_t crash[2]{};
    if (GetEnvironmentVariableW(L"SS2VR_LAB_CRASH_TRACE", crash, 2) == 1 && crash[0] == L'1') AddVectoredExceptionHandler(1, crashTrace);
    HMODULE engine = GetModuleHandleW(L"Engine.dll");
    void *target = engine ? reinterpret_cast<void *>(GetProcAddress(engine, "?sfxStartup_t@SeriousEngine@@YAXW4SfxAPI@1@J@Z")) : nullptr;
    if (!target || MH_CreateHook(target, reinterpret_cast<void *>(startup), reinterpret_cast<void **>(&originalStartup)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        log("Spatial audio: sound startup hook unavailable");
        return;
    }
    log("Spatial audio: sound startup hooked");
}

bool prepare() {
    if (prepared) return preparedOk;
    prepared = true;
    if (!config.enabled) return false;
    const std::wstring bin = binDirectory();
    if (bin.empty()) return false;
    const std::wstring dll = bin + L"SS2VR\\OpenAL32.dll", ini = bin + L"SS2VR\\alsoft.ini";
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW((bin + L"SfxOAL.dll").c_str()) == INVALID_FILE_ATTRIBUTES) {
        log("Spatial audio: OpenAL32.dll or SfxOAL.dll missing; the game's own sound stays");
        return false;
    }
    // OpenAL Soft reads ALSOFT_CONF through the shared UCRT environment.
    std::wstring conf = ini;
    wchar_t labNoEax[2]{}; // Lab: SS2VR_LAB_ALSOFT_NOEAX=1 runs with OpenAL Soft's EAX emulation off.
    if (GetEnvironmentVariableW(L"SS2VR_LAB_ALSOFT_NOEAX", labNoEax, 2) == 1 && labNoEax[0] == L'1') {
        const std::wstring lab = bin + L"..\\Temp\\SS2VR\\alsoft-lab.ini";
        if (CopyFileW(ini.c_str(), lab.c_str(), FALSE) &&
            WritePrivateProfileStringW(L"eax", L"enable", L"false", lab.c_str())) conf = lab;
    }
    if (GetFileAttributesW(conf.c_str()) != INVALID_FILE_ATTRIBUTES) {
        _wputenv_s(L"ALSOFT_CONF", conf.c_str());
        SetEnvironmentVariableW(L"ALSOFT_CONF", conf.c_str());
    }
    wchar_t labLog[2]{}; // Lab: SS2VR_LAB_ALSOFT_LOG=1 writes OpenAL Soft's own log beside it.
    if (GetEnvironmentVariableW(L"SS2VR_LAB_ALSOFT_LOG", labLog, 2) == 1 && labLog[0] == L'1') {
        const std::wstring file = bin + L"SS2VR\\alsoft.log";
        _wputenv_s(L"ALSOFT_LOGLEVEL", L"3");
        _wputenv_s(L"ALSOFT_LOGFILE", file.c_str());
    }
    openal = LoadLibraryW(dll.c_str());
    if (!openal || !resolve(alcOpenDevice, "alcOpenDevice") || !resolve(alcCloseDevice, "alcCloseDevice") ||
        !resolve(alcGetCurrentContext, "alcGetCurrentContext") || !resolve(alSourcei, "alSourcei") ||
        !resolve(alGenFilters, "alGenFilters") || !resolve(alFilteri, "alFilteri") ||
        !resolve(alFilterf, "alFilterf") || !resolve(alIsSource, "alIsSource")) {
        log("Spatial audio: OpenAL32.dll did not load (%lu)", GetLastError());
        return false;
    }
    // The game has no fallback once it picks OpenAL: open the device first.
    void *device = alcOpenDevice(nullptr);
    if (!device) {
        log("Spatial audio: no OpenAL output device; the game's own sound stays");
        return false;
    }
    alcCloseDevice(device);
    hooked = hookOpenAl();
    log("Spatial audio: OpenAL Soft ready (hooks=%d)", int(hooked));
    preparedOk = true;
    return true;
}

int chooseApi(int api) {
    if (api != ApiDirectSound || !config.enabled || !prepare()) return api;
    log("Spatial audio: sound starts on OpenAL instead of DirectSound");
    return ApiOpenAl;
}

void noteHead(const Pose &pose, uint64_t tickMs) {
    AcquireSRWLockExclusive(&lock);
    head = pose;
    headTick = tickMs;
    ReleaseSRWLockExclusive(&lock);
}

bool listener(const Pose &native, Pose &out) {
    if (!config.enabled) return false;
    ++listenerCalls;
    AcquireSRWLockExclusive(&lock);
    const uint64_t now = GetTickCount64();
    const Vec3 d = head.p - native.p;
    const bool ok = headTick && now - headTick < 250 && dot(d, d) < 3.f * 3.f &&
                    std::isfinite(head.p.x + head.p.y + head.p.z + head.q.x + head.q.y + head.q.z + head.q.w);
    used = ok ? head : native;
    usedValid = true;
    if (ok) ++listenerHead;
    if (ok) out = head;
    ReleaseSRWLockExclusive(&lock);
    return ok;
}

void frame() {
    const uint64_t now = GetTickCount64();
    const float seconds = lastFrame && now > lastFrame ? std::min(.25f, float(now - lastFrame) / 1000.f) : 0.f;
    lastFrame = now;
    static uint64_t reported = 0;
    static unsigned reports = 0;
    if (preparedOk && now - reported >= 5000 && reports < 60) {
        reported = now;
        ++reports;
        unsigned tracked = 0, occluded = 0;
        AcquireSRWLockShared(&lock);
        for (const auto &s : tracker.sources)
            if (s.used) { ++tracked; if (s.occlusion > .5f) ++occluded; }
        ReleaseSRWLockShared(&lock);
        log("Spatial audio: module=%d plays=%u positions=%u listener=%u head=%u rays=%u blocked=%u sources=%u occluded=%u", int(GetModuleHandleW(L"OpenAL32.dll") == openal),
            playCalls.exchange(0), positionCalls.exchange(0),
            listenerCalls.exchange(0), listenerHead.exchange(0), traceCalls.exchange(0), blockedCalls.exchange(0), tracked, occluded);
    }
    if (!preparedOk || !hooked || !alcGetCurrentContext || !alcGetCurrentContext()) return;
    struct Pending { ALuint source; float gain, gainHF; };
    Pending pending[::ss2vr::spatial_audio::MaxSources];
    unsigned count = 0;
    AcquireSRWLockExclusive(&lock);
    tracker.step(seconds);
    for (const auto &s : tracker.sources) {
        if (!s.used) continue;
        const auto f = ::ss2vr::spatial_audio::occlusionFilter(config.occlusion ? s.occlusion : 0.f);
        pending[count++] = {s.id, f.gain, f.gainHF};
    }
    ReleaseSRWLockExclusive(&lock);
    static unsigned logged = 0;
    for (unsigned i = 0; i < count; ++i) {
        const auto &p = pending[i];
        Applied *a = nullptr;
        for (auto &candidate : applied)
            if (candidate.used && candidate.source == p.source) { a = &candidate; break; }
        if (!a) {
            if (p.gain > .995f && p.gainHF > .995f) continue; // Clear and never filtered: leave it alone.
            for (auto &candidate : applied)
                if (!candidate.used) { a = &candidate; break; }
            if (!a || !alIsSource(p.source)) continue;
            if (!a->filter) {
                alGenFilters(1, &a->filter);
                if (!a->filter) continue;
                alFilteri(a->filter, AlFilterType, AlFilterLowpass);
            }
            a->used = true;
            a->source = p.source;
            a->gain = a->gainHF = -1;
        }
        if (std::abs(a->gain - p.gain) < .01f && std::abs(a->gainHF - p.gainHF) < .01f) continue;
        alFilterf(a->filter, AlLowpassGain, p.gain);
        alFilterf(a->filter, AlLowpassGainHF, p.gainHF);
        alSourcei(p.source, AlDirectFilter, ALint(a->filter));
        a->gain = p.gain;
        a->gainHF = p.gainHF;
        if (logged < 40 && (p.gainHF < .3f || (p.gainHF > .99f && a->gainHF < .3f))) {
            ++logged;
            log("Spatial audio: source %u occlusion filter gain=%.2f hf=%.2f", p.source, p.gain, p.gainHF);
        }
    }
}

unsigned dueTraces(Vec3 &from, uint32_t *ids, Vec3 *ends, unsigned max) {
    if (!preparedOk || !config.occlusion) return 0;
    AcquireSRWLockExclusive(&lock);
    unsigned n = 0;
    if (usedValid) {
        from = used.p;
        n = tracker.due(used, GetTickCount64(), ids, ends, max);
    }
    ReleaseSRWLockExclusive(&lock);
    return n;
}

void traced(uint32_t id, bool hit, float hitDistance, float distance) {
    ++traceCalls;
    if (hit && hitDistance < distance - ::ss2vr::spatial_audio::NearMiss) ++blockedCalls;
    AcquireSRWLockExclusive(&lock);
    tracker.traced(id, hit, hitDistance, distance, GetTickCount64());
    ReleaseSRWLockExclusive(&lock);
}

} // namespace ss2vr::game::spatial_audio
