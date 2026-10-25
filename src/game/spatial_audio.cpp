// Spatial audio, game side (spatial_audio.hpp).
#include "spatial_audio.hpp"
#include "common/spatial_audio.hpp"
#include "game.hpp"
#include "native_memory.hpp"
#include "common/win_settings.hpp"
#include <MinHook.h>
#include <windows.h>
#include <algorithm>
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
// Lab (SS2VR_LAB_AUDIO_TRACE=1): every 5 s, each playing source's settings.
void(__cdecl *alGetSourcei)(ALuint, ALenum, ALint *) = nullptr;
void(__cdecl *alGetSourcef)(ALuint, ALenum, ALfloat *) = nullptr;
void(__cdecl *alGetSource3f)(ALuint, ALenum, ALfloat *, ALfloat *, ALfloat *) = nullptr;
void(__cdecl *alGetBufferi)(ALuint, ALenum, ALint *) = nullptr;
void(__cdecl *alGetListener3f)(ALenum, ALfloat *, ALfloat *, ALfloat *) = nullptr;
int(__cdecl *alGetInteger)(ALenum) = nullptr;
void traceSources() {
    if (!alGetSourcei || !alGetSourcef || !alGetSource3f || !alGetBufferi || !alIsSource) return;
    float lx = 0, ly = 0, lz = 0;
    if (alGetListener3f) alGetListener3f(0x1004, &lx, &ly, &lz); // AL_POSITION
    log("Lab audio: listener %.1f,%.1f,%.1f distance model %x", lx, ly, lz, alGetInteger ? alGetInteger(0xD000) : -1);
    for (ALuint id = 1; id < 512; ++id) {
        if (!alIsSource(id)) continue;
        ALint state = 0, looping = 0, relative = 0, buffer = 0, size = 0, frequency = 0, channels = 0, bits = 0;
        alGetSourcei(id, 0x1010, &state); // AL_SOURCE_STATE
        if (state != 0x1012) continue;     // AL_PLAYING
        alGetSourcei(id, 0x1007, &looping);
        alGetSourcei(id, 0x202, &relative);
        alGetSourcei(id, 0x1009, &buffer);
        if (buffer) {
            alGetBufferi(ALuint(buffer), 0x2004, &size);
            alGetBufferi(ALuint(buffer), 0x2001, &frequency);
            alGetBufferi(ALuint(buffer), 0x2003, &channels);
            alGetBufferi(ALuint(buffer), 0x2002, &bits);
        }
        float gain = 0, reference = 0, rolloff = 0, maximum = 0, x = 0, y = 0, z = 0;
        alGetSourcef(id, 0x100A, &gain);
        alGetSourcef(id, 0x1020, &reference);
        alGetSourcef(id, 0x1021, &rolloff);
        alGetSourcef(id, 0x1023, &maximum);
        alGetSource3f(id, 0x1004, &x, &y, &z);
        const float seconds = frequency && channels && bits ? float(size) / float(frequency * channels * bits / 8) : 0.f;
        log("Lab audio: source %u loop=%d relative=%d gain=%.2f ref=%.1f rolloff=%.2f max=%.0f pos=%.1f,%.1f,%.1f "
            "buffer=%d %.2fs %dHz ch=%d", id, looping, relative, gain, reference, rolloff, maximum, x, y, z, buffer,
            seconds, frequency, channels);
    }
}
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

void traceSources();
void __cdecl sourcePlay(ALuint source) {
    ++playCalls;
    originalSourcePlay(source);
    // Lab trace from the game's own sound thread too (its context is current here).
    static const bool trace = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_AUDIO_TRACE", v, 2) == 1 && v[0] == L'1'; }();
    static std::atomic<uint64_t> traced{0};
    const uint64_t now = GetTickCount64();
    if (trace && now - traced.load() >= 5000) { traced = now; log("Lab audio (play thread %lu):", GetCurrentThreadId()); traceSources(); }
}
std::atomic<int> probeResult{0};
DWORD WINAPI probeDevice(void *) {
    void *device = alcOpenDevice(nullptr);
    if (device) alcCloseDevice(device);
    probeResult = device ? 1 : -1;
    return 0;
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
// Which sound files play: each distinct file once, with how it was started
// (CSoundSource for positioned sounds, CSoundChannel otherwise) and the play
// flags, to name looping sounds a player reports (a constant motor-like loop).
using SoundPlay = void(__fastcall *)(void *, void *, void *, unsigned long);
using ResourceFileOf = void *(__fastcall *)(void *, void *);
using FileNameOf = const char *(__fastcall *)(void *, void *);
SoundPlay originalSourceSoundPlay = nullptr, originalChannelSoundPlay = nullptr;
ResourceFileOf resourceFileOf = nullptr;
FileNameOf fileNameOf = nullptr;
SRWLOCK namesLock = SRWLOCK_INIT;
bool endsWith(const char *text, const char *tail) {
    const size_t n = std::strlen(text), m = std::strlen(tail);
    return n >= m && !_stricmp(text + n - m, tail);
}
bool noteSoundFile(void *sound, unsigned long flags, const char *via) {
    if (!sound || !resourceFileOf || !fileNameOf || !readableMemory(sound, 8)) return false;
    void *file = resourceFileOf(sound, nullptr);
    const char *name = file && readableMemory(file, 8) ? fileNameOf(file, nullptr) : nullptr;
    if (!name || !readableMemory(name, 1)) return false;
    char copy[160]{};
    for (size_t i = 0; i + 1 < sizeof copy && readableMemory(name + i, 1) && name[i]; ++i) copy[i] = name[i];
    uint64_t hash = 1469598103934665603ull;
    for (const char *c = copy; *c; ++c) hash = (hash ^ uint8_t(*c)) * 1099511628211ull;
    hash ^= uint64_t(flags) << 1 ^ (via[0] == 's' ? 1 : 0);
    static uint64_t seen[400]{};
    static unsigned count = 0;
    AcquireSRWLockExclusive(&namesLock);
    bool fresh = count < 400;
    for (unsigned i = 0; fresh && i < count; ++i) fresh = seen[i] != hash;
    if (fresh) seen[count++] = hash;
    ReleaseSRWLockExclusive(&namesLock);
    if (fresh) log("Sound file %s via %s flags=%lx", copy, via, flags);
    return endsWith(copy, "Weapons/CircularSaw/Saw_Engine.wav");
}
// The circular saw's engine loop left playing with no saw in hand (a constant
// motor-like hum at the player, restored by saves too): channels that start it
// are noted and stopped when the local player holds no saw (guardSawEngine).
using ChannelPlaying = int(__fastcall *)(void *, void *);
using ChannelSound = void *(__fastcall *)(void *, void *);
using ChannelStop = void(__fastcall *)(void *, void *);
ChannelPlaying channelPlaying = nullptr;
ChannelSound channelSound = nullptr;
ChannelStop channelStop = nullptr;
struct SawChannel { void *channel = nullptr, *sound = nullptr; uint64_t unheldSince = 0; };
SawChannel sawChannels[8];
void noteSawChannel(void *channel, void *sound) {
    AcquireSRWLockExclusive(&namesLock);
    SawChannel *slot = nullptr;
    for (auto &c : sawChannels) if (c.channel == channel) slot = &c;
    for (auto &c : sawChannels) if (!slot && !c.channel) slot = &c;
    if (slot) *slot = {channel, sound, 0};
    ReleaseSRWLockExclusive(&namesLock);
}
void __fastcall sourceSoundPlay(void *self, void *edx, void *sound, unsigned long flags) {
    noteSoundFile(sound, flags, "source");
    originalSourceSoundPlay(self, edx, sound, flags);
}
void __fastcall channelSoundPlay(void *self, void *edx, void *sound, unsigned long flags) {
    const bool saw = noteSoundFile(sound, flags, "channel");
    originalChannelSoundPlay(self, edx, sound, flags);
    if (saw && (flags & 1)) noteSawChannel(self, sound); // A looping saw engine.
}
void hookSoundNames(HMODULE engine) {
    const HMODULE core = GetModuleHandleW(L"Core.dll");
    resourceFileOf = core ? reinterpret_cast<ResourceFileOf>(GetProcAddress(core,
        "?GetResourceFile@CResource@SeriousEngine@@QAEPAVCResourceFile@2@XZ")) : nullptr;
    fileNameOf = core ? reinterpret_cast<FileNameOf>(GetProcAddress(core,
        "?GetFileName@CResourceFile@SeriousEngine@@QAEPBDXZ")) : nullptr;
    channelPlaying = reinterpret_cast<ChannelPlaying>(GetProcAddress(engine, "?IsPlaying@CSoundChannel@SeriousEngine@@QAEHXZ"));
    channelSound = reinterpret_cast<ChannelSound>(GetProcAddress(engine, "?GetPlayingSound@CSoundChannel@SeriousEngine@@QAEPAVCBaseSound@2@XZ"));
    channelStop = reinterpret_cast<ChannelStop>(GetProcAddress(engine, "?Stop@CSoundChannel@SeriousEngine@@QAEXXZ"));
    void *source = reinterpret_cast<void *>(GetProcAddress(engine, "?Play@CSoundSource@SeriousEngine@@QAEXPAVCBaseSound@2@K@Z"));
    void *channel = reinterpret_cast<void *>(GetProcAddress(engine, "?Play@CSoundChannel@SeriousEngine@@QAEXPAVCBaseSound@2@K@Z"));
    const bool ok = resourceFileOf && fileNameOf && source && channel &&
        MH_CreateHook(source, reinterpret_cast<void *>(sourceSoundPlay), reinterpret_cast<void **>(&originalSourceSoundPlay)) == MH_OK &&
        MH_CreateHook(channel, reinterpret_cast<void *>(channelSoundPlay), reinterpret_cast<void **>(&originalChannelSoundPlay)) == MH_OK &&
        MH_EnableHook(source) == MH_OK && MH_EnableHook(channel) == MH_OK;
    log("Sound file names %s", ok ? "logged" : "unavailable");
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

void guardSawEngine(bool active, bool sawHeld) {
    if (!channelPlaying || !channelSound || !channelStop) return;
    const uint64_t now = GetTickCount64();
    SawChannel pending[8];
    AcquireSRWLockExclusive(&namesLock);
    std::copy(std::begin(sawChannels), std::end(sawChannels), pending);
    ReleaseSRWLockExclusive(&namesLock);
    for (auto &c : pending) {
        if (!c.channel) continue;
        bool drop = !readableMemory(c.channel, 0x20) || !channelPlaying(c.channel, nullptr) ||
                    channelSound(c.channel, nullptr) != c.sound;
        if (!drop && active && !sawHeld) {
            if (!c.unheldSince) c.unheldSince = now;
            else if (now - c.unheldSince > 1000) {
                channelStop(c.channel, nullptr);
                static unsigned logged = 0;
                if (logged++ < 8) log("Saw engine loop stopped: no saw in hand");
                drop = true;
            }
        } else c.unheldSince = 0;
        AcquireSRWLockExclusive(&namesLock);
        for (auto &live : sawChannels)
            if (live.channel == c.channel && live.sound == c.sound) live = drop ? SawChannel{} : c;
        ReleaseSRWLockExclusive(&namesLock);
    }
}

void installStartupHook() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    wchar_t crash[2]{};
    if (GetEnvironmentVariableW(L"SS2VR_LAB_CRASH_TRACE", crash, 2) == 1 && crash[0] == L'1') AddVectoredExceptionHandler(1, crashTrace);
    HMODULE engine = GetModuleHandleW(L"Engine.dll");
    if (engine) hookSoundNames(engine);
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
        !resolve(alFilterf, "alFilterf") || !resolve(alIsSource, "alIsSource") || !resolve(alGetSourcei, "alGetSourcei") ||
        !resolve(alGetSourcef, "alGetSourcef") || !resolve(alGetSource3f, "alGetSource3f") ||
        !resolve(alGetBufferi, "alGetBufferi") || !resolve(alGetListener3f, "alGetListener3f") ||
        !resolve(alGetInteger, "alGetInteger")) {
        log("Spatial audio: OpenAL32.dll did not load (%lu)", GetLastError());
        return false;
    }
    // The game has no fallback once it picks OpenAL: open the device first, on
    // its own thread with a time limit (an audio service that does not answer
    // must not hang the game's start; lab V628 stalled here once).
    probeResult = 0;
    HANDLE thread = CreateThread(nullptr, 0, probeDevice, nullptr, 0, nullptr);
    const DWORD waited = thread ? WaitForSingleObject(thread, 4000) : WAIT_FAILED;
    if (thread) CloseHandle(thread);
    if (waited != WAIT_OBJECT_0 || probeResult != 1) {
        log(waited == WAIT_TIMEOUT ? "Spatial audio: the OpenAL output device did not open in 4 s; the game's own sound stays"
                                   : "Spatial audio: no OpenAL output device; the game's own sound stays");
        return false;
    }
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
    static const bool trace = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_AUDIO_TRACE", v, 2) == 1 && v[0] == L'1'; }();
    static uint64_t traced = 0;
    if (trace && now - traced >= 5000) { traced = now; traceSources(); }
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
