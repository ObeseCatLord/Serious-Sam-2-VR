// Spatial audio, game side (spatial_audio.hpp).
#include "hook_memory.hpp"
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
    if (createHook(play, reinterpret_cast<void *>(sourcePlay), reinterpret_cast<void **>(&originalSourcePlay)) != MH_OK ||
        MH_EnableHook(play) != MH_OK)
        return false;
    if (createHook(s3f, reinterpret_cast<void *>(source3f), reinterpret_cast<void **>(&originalSource3f)) != MH_OK ||
        createHook(del, reinterpret_cast<void *>(deleteSources), reinterpret_cast<void **>(&originalDeleteSources)) != MH_OK)
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
        createHook(source, reinterpret_cast<void *>(sourceSoundPlay), reinterpret_cast<void **>(&originalSourceSoundPlay)) == MH_OK &&
        createHook(channel, reinterpret_cast<void *>(channelSoundPlay), reinterpret_cast<void **>(&originalChannelSoundPlay)) == MH_OK &&
        MH_EnableHook(source) == MH_OK && MH_EnableHook(channel) == MH_OK;
    log("Sound file names %s", ok ? "logged" : "unavailable");
}
// Lab (SS2VR_LAB_SOUND_PERF=1): the engine reaches its sound backend (SfxOAL or
// SfxDSD) through exported function pointers; the per-frame ones are wrapped to
// count their calls and time, so the two backends can be compared in one table.
namespace backend_perf {
struct Counter { std::atomic<uint64_t> calls{0}, ticks{0}; };
Counter counters[5]; // isPlaying, hasFreePacket, loadSoundPacket, updateSource, applyChanges
struct Timed {
    Counter &counter; LARGE_INTEGER start;
    explicit Timed(Counter &c) : counter(c) { QueryPerformanceCounter(&start); }
    ~Timed() {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        counter.calls.fetch_add(1, std::memory_order_relaxed);
        counter.ticks.fetch_add(uint64_t(now.QuadPart - start.QuadPart), std::memory_order_relaxed);
    }
};
using SourceInt = int(__cdecl *)(void *);
using SourcePacket = int(__cdecl *)(void *, void *);
using SourceVoid = void(__cdecl *)(void *);
using Plain = void(__cdecl *)();
SourceInt oIsPlaying = nullptr, oHasFreePacket = nullptr;
SourcePacket oLoadSoundPacket = nullptr;
SourceVoid oUpdateSource = nullptr;
Plain oApplyChanges = nullptr;
int __cdecl isPlaying(void *source) { Timed t(counters[0]); return oIsPlaying(source); }
int __cdecl hasFreePacket(void *source) { Timed t(counters[1]); return oHasFreePacket(source); }
int __cdecl loadSoundPacket(void *source, void *data) { Timed t(counters[2]); return oLoadSoundPacket(source, data); }
void __cdecl updateSource(void *source) { Timed t(counters[3]); oUpdateSource(source); }
void __cdecl applyChanges() { Timed t(counters[4]); oApplyChanges(); }
bool enabled() {
    static const bool on = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SOUND_PERF", v, 2) == 1 && v[0] == L'1'; }();
    return on;
}
// The backend has just set its pointers (sound startup or an API change).
template <class F> void wrap(HMODULE engine, const char *name, F detour, F &original) {
    auto *slot = reinterpret_cast<F *>(GetProcAddress(engine, name));
    if (!slot || !*slot || *slot == detour) return;
    original = *slot;
    *slot = detour;
}
void install() {
    const HMODULE engine = enabled() ? GetModuleHandleW(L"Engine.dll") : nullptr;
    if (!engine) return;
    wrap(engine, "?_sfxIsPlaying@SeriousEngine@@3P6AHAAUSoundSource@1@@ZA", &isPlaying, oIsPlaying);
    wrap(engine, "?_sfxHasFreePacket@SeriousEngine@@3P6AHAAUSoundSource@1@@ZA", &hasFreePacket, oHasFreePacket);
    wrap(engine, "?_sfxLoadSoundPacket@SeriousEngine@@3P6AHAAUSoundSource@1@PAX@ZA", &loadSoundPacket, oLoadSoundPacket);
    wrap(engine, "?_sfxUpdateSource@SeriousEngine@@3P6AXAAUSoundSource@1@@ZA", &updateSource, oUpdateSource);
    wrap(engine, "?_sfxApplyChanges@SeriousEngine@@3P6AXXZA", &applyChanges, oApplyChanges);
    log("Sound backend calls timed: isPlaying=%d hasFreePacket=%d loadSoundPacket=%d updateSource=%d applyChanges=%d",
        int(oIsPlaying != nullptr), int(oHasFreePacket != nullptr), int(oLoadSoundPacket != nullptr), int(oUpdateSource != nullptr),
        int(oApplyChanges != nullptr));
}
void report(double seconds) {
    if (!enabled() || !(seconds > 0)) return;
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
    const double toMs = frequency.QuadPart ? 1000. / double(frequency.QuadPart) : 0.;
    double n[5], ms[5];
    for (unsigned i = 0; i < 5; ++i) {
        n[i] = double(counters[i].calls.exchange(0)) / seconds;
        ms[i] = double(counters[i].ticks.exchange(0)) * toMs / seconds;
    }
    log("Sound backend per s: isPlaying n=%.0f %.2fms | hasFreePacket n=%.0f %.2fms | loadSoundPacket n=%.0f %.2fms | "
        "updateSource n=%.0f %.2fms | applyChanges n=%.0f %.2fms", n[0], ms[0], n[1], ms[1], n[2], ms[2], n[3], ms[3], n[4], ms[4]);
}
} // namespace backend_perf
// Stream recovery. Measured on Windows: with the OpenAL backend (SfxOAL) the
// engine loads about eight more stream packets every frame than with DirectSound
// (the same 8 KB packets, queue of eight: about thirty times real time of OGG
// decoding, 0.9 ms of every frame on the render thread). The explanation this
// acts on, not yet confirmed by a run: a stream whose queue ran dry (a stall
// longer than its 0.37 s, e.g. a level load) is stopped by OpenAL; the backend
// never starts it again (its start waits for "not every buffer processed", and a
// stopped source reports every buffer processed), while its free-packet test
// stays true for the same reason, so the engine refills the whole queue every
// frame for a stream nobody hears. DirectSound's looping buffer has no such state.
// The free-packet test is wrapped: a source found stopped while the engine still
// feeds it gets one full queue of fresh packets and is started again (Windows
// A/B: packet traffic then matches DirectSound's: ~22 loads/s instead of ~700). The engine's own stop rewinds the source (initial state): left alone.
// A stream fed that fast in any other state is only reported (state, queue).
// Lab: SS2VR_LAB_STREAM_RECOVERY=0 leaves the backend as it is; =2 only holds a
// dry stream (no restart). Default: the refilled stream is started again, as
// DirectSound would still be playing it (the music stayed silent without it).
namespace stream_recovery {
constexpr ALenum AlSourceState = 0x1010, AlStopped = 0x1014, AlBuffersQueued = 0x1015;
using HasFreePacket = int(__cdecl *)(void *);
HasFreePacket original = nullptr;
struct Stream { ALuint source = 0; int fed = 0; unsigned restarts = 0; uint64_t restartedAt = 0, burstAt = 0; unsigned burst = 0; };
Stream streams[16];
std::atomic<unsigned> restarted{0}, held{0};
bool restart = false;
std::atomic<uint32_t> polled{0}; // Bit per stream slot asked about since the last report.
Stream &streamOf(ALuint source) {
    Stream *free = nullptr;
    for (auto &s : streams) {
        if (s.source == source) return s;
        if (!s.source && !free) free = &s;
    }
    if (!free) free = &streams[source % std::size(streams)];
    *free = {};
    free->source = source;
    return *free;
}
void(__cdecl *alSourcePlayRaw)(ALuint) = nullptr;
int __cdecl hasFreePacket(void *source) {
    if (!source || !alGetSourcei) return original(source);
    const ALuint id = *static_cast<const ALuint *>(source);
    ALint state = 0, queued = 0;
    alGetSourcei(id, AlSourceState, &state);
    Stream &stream = streamOf(id);
    polled.fetch_or(1u << unsigned(&stream - streams), std::memory_order_relaxed);
    if (state != AlStopped) {
        stream.fed = 0; stream.restarts = 0;
        const int free = original(source);
        // A stream fed far faster than it plays in any other state: reported, not changed.
        const uint64_t now = GetTickCount64();
        if (now - stream.burstAt > 100) { stream.burstAt = now; stream.burst = 0; }
        if (free && ++stream.burst == 17) {
            static unsigned logged = 0;
            ALint processed = 0;
            alGetSourcei(id, AlBuffersQueued, &queued);
            alGetSourcei(id, 0x1016, &processed); // AL_BUFFERS_PROCESSED
            if (logged++ < 6) log("Spatial audio: stream source %u fed %u packets in 0.1 s in state %x (queued %d processed %d)", id, stream.burst, state, queued, processed);
        }
        return free;
    }
    alGetSourcei(id, AlBuffersQueued, &queued);
    if (queued <= 0) return original(source); // Nothing queued: the backend's own first fill.
    if (stream.fed < queued) {
        // A full queue of fresh packets first (each "free" answer is followed by a load).
        const int free = original(source);
        if (free) ++stream.fed;
        return free;
    }
    const uint64_t now = GetTickCount64();
    if (restart && stream.restarts < 3 && (!stream.restartedAt || now - stream.restartedAt > 250)) {
        ++stream.restarts;
        stream.restartedAt = now;
        stream.fed = 0;
        if (originalSourcePlay) originalSourcePlay(id); else if (alSourcePlayRaw) alSourcePlayRaw(id);
        static unsigned logged = 0;
        if (logged++ < 12) log("Spatial audio: stream source %u had run dry (%d buffers); refilled and started again", id, queued);
        ++restarted;
        ALint after = 0;
        alGetSourcei(id, AlSourceState, &after);
        if (after != AlStopped) return original(source);
    }
    ++held; // Stopped and full: decoding more would only replace what nobody hears.
    return 0;
}
void install() {
    wchar_t v[2]{};
    const bool set = GetEnvironmentVariableW(L"SS2VR_LAB_STREAM_RECOVERY", v, 2) == 1;
    if (set && v[0] == L'0') return;
    restart = !(set && v[0] == L'2'); // Windows A/B: restarting matches DirectSound's packet traffic exactly.
    const HMODULE engine = GetModuleHandleW(L"Engine.dll");
    // Only the OpenAL the mod loaded, behind the game's own OpenAL backend.
    const HMODULE backend = GetModuleHandleW(L"SfxOAL.dll");
    auto *slot = engine ? reinterpret_cast<HasFreePacket *>(GetProcAddress(engine, "?_sfxHasFreePacket@SeriousEngine@@3P6AHAAUSoundSource@1@@ZA")) : nullptr;
    HMODULE owner = nullptr;
    if (!preparedOk || !openal || GetModuleHandleW(L"OpenAL32.dll") != openal || !alGetSourcei || !backend || !slot || !*slot ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(*slot), &owner) || owner != backend) return; // Not SfxOAL's own test.
    alSourcePlayRaw = reinterpret_cast<void(__cdecl *)(ALuint)>(GetProcAddress(openal, "alSourcePlay"));
    for (auto &s : streams) s = {};
    original = *slot;
    *slot = &hasFreePacket;
    log("Spatial audio: stream recovery on (a stream that ran dry is refilled once and %s)", restart ? "started again" : "fed no more");
}
} // namespace stream_recovery
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
    stream_recovery::install(); // Before the timing wrappers, which then time it too.
    backend_perf::install();
}

} // namespace

void configure(const Config &c) { config = c; }
void measureBackend() { stream_recovery::install(); backend_perf::install(); }

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
    if (!target || createHook(target, reinterpret_cast<void *>(startup), reinterpret_cast<void **>(&originalStartup)) != MH_OK ||
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
    {
        static uint64_t backendReported = 0;
        if (!backendReported) backendReported = now;
        else if (now - backendReported >= 5000) { backend_perf::report(double(now - backendReported) / 1000.); backendReported = now; }
    }
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
        log("Spatial audio: module=%d plays=%u positions=%u listener=%u head=%u rays=%u blocked=%u sources=%u occluded=%u streams=%u restarted=%u held=%u", int(GetModuleHandleW(L"OpenAL32.dll") == openal),
            playCalls.exchange(0), positionCalls.exchange(0),
            listenerCalls.exchange(0), listenerHead.exchange(0), traceCalls.exchange(0), blockedCalls.exchange(0), tracked, occluded,
            unsigned(__builtin_popcount(stream_recovery::polled.exchange(0))),
            stream_recovery::restarted.exchange(0), stream_recovery::held.exchange(0));
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
