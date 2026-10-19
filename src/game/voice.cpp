// Multiplayer voice chat: capture, Opus, playout and mixing on one audio thread
// (see voice.hpp and common/voice.hpp). Windows waveIn/waveOut: 48 kHz mono
// capture and 48 kHz stereo playback in 20 ms buffers, which Wine maps onto
// PulseAudio/PipeWire. Nothing here calls into the game.
#include "voice.hpp"
#include "common/voice.hpp"
#include "game.hpp"
#include <opus.h>
#include <windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace ss2vr::game::voice {
namespace {
using ss2vr::voice::Activity;
using ss2vr::voice::Jitter;
using ss2vr::voice::Packer;
constexpr unsigned Frame = ss2vr::voice::FrameSamples, Rate = ss2vr::voice::SampleRate;
constexpr unsigned CaptureBuffers = 8, PlaybackBuffers = 6, PlaybackAhead = 3, OutboundQueue = 4;

std::mutex lock; // Scene, settings, queues and indicator state.
VrSettings::Voice config;
bool configured = false;
Scene scene;
uint64_t sceneTick = 0;
std::deque<network::VoicePacket> outbound;
struct Inbound { uint32_t avatar, incarnation; network::VoicePacket packet; uint64_t tick; };
std::deque<Inbound> inbound;
std::atomic<bool> sendingNow{false};
struct Talker { uint32_t avatar; uint64_t until; };
std::array<Talker, 18> talkers{};

std::atomic<bool> running{false};
// Lab (SS2VR_LAB_VOICE=1): a session without peers, a synthetic talker (1 s of
// voice-like tone every 2 s) instead of the microphone, every packet looped back
// as a speaker 2 m to the right, the mix written to Bin/voice-lab.wav (12 s)
// and the pipeline counted in the log.
const bool labLoopback = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_VOICE", v, 2) == 1 && v[0] == L'1'; }();
constexpr uint32_t LabSpeaker = 0x7ab0u;
struct LabCounters { unsigned captured = 0, synthetic = 0, packets = 0, received = 0, played = 0, concealed = 0, mixed = 0; } labCount;
HANDLE thread = nullptr, wake = nullptr;

// One remote speaker's stream.
struct Stream {
    uint32_t avatar = 0, incarnation = 0;
    uint64_t lastHeard = 0;
    Jitter jitter;
    OpusDecoder *decoder = nullptr;
    bool concealing = false;
};
std::array<Stream, 18> streams;

Stream *stream(uint32_t avatar, uint32_t incarnation, uint64_t now) {
    Stream *found = nullptr, *idle = nullptr;
    for (auto &s : streams) {
        if (s.avatar == avatar) { found = &s; break; }
        if (!idle && (!s.avatar || now - s.lastHeard > 30000)) idle = &s;
    }
    if (!found && !idle) return nullptr;
    Stream &s = found ? *found : *idle;
    if (!found || s.incarnation != incarnation) {
        OpusDecoder *decoder = s.decoder;
        s.avatar = avatar;
        s.incarnation = incarnation;
        s.jitter = Jitter{};
        if (decoder) opus_decoder_ctl(decoder, OPUS_RESET_STATE);
        s.decoder = decoder;
    }
    if (!s.decoder) {
        int error = 0;
        s.decoder = opus_decoder_create(Rate, 1, &error);
        if (error != OPUS_OK) s.decoder = nullptr;
    }
    s.lastHeard = now;
    return &s;
}

// --- Capture ---
struct Capture {
    HWAVEIN device = nullptr;
    std::array<WAVEHDR, CaptureBuffers> headers{};
    std::vector<int16_t> memory;
    uint64_t retryAt = 0;
    std::wstring opened;
    bool open(const wchar_t *wanted, HANDLE event, uint64_t now) {
        close();
        UINT id = WAVE_MAPPER;
        const UINT count = waveInGetNumDevs();
        static bool listed = false;
        for (UINT i = 0; i < count; ++i) {
            WAVEINCAPSW caps{};
            if (waveInGetDevCapsW(i, &caps, sizeof caps) != MMSYSERR_NOERROR) continue;
            if (!listed) log("Voice capture device %u: %ls", i, caps.szPname);
            if (wanted && *wanted && !std::wcsncmp(caps.szPname, wanted, 31)) id = i;
        }
        listed = true;
        if (wanted && *wanted && id == WAVE_MAPPER) {
            log("Voice capture device \"%ls\" not found; the microphone stays off", wanted);
            retryAt = now + 10000;
            return false;
        }
        WAVEFORMATEX format{WAVE_FORMAT_PCM, 1, Rate, Rate * 2, 2, 16, 0};
        const MMRESULT result = waveInOpen(&device, id, &format, DWORD_PTR(event), 0, CALLBACK_EVENT);
        if (result != MMSYSERR_NOERROR) {
            device = nullptr;
            log("Voice capture could not open (error %u); retrying in 10 s", unsigned(result));
            retryAt = now + 10000;
            return false;
        }
        memory.assign(size_t(Frame) * CaptureBuffers, 0);
        for (unsigned i = 0; i < CaptureBuffers; ++i) {
            auto &h = headers[i];
            h = {};
            h.lpData = reinterpret_cast<LPSTR>(memory.data() + size_t(i) * Frame);
            h.dwBufferLength = Frame * 2;
            waveInPrepareHeader(device, &h, sizeof h);
            waveInAddBuffer(device, &h, sizeof h);
        }
        waveInStart(device);
        opened = wanted && *wanted ? wanted : L"system default";
        log("Voice capture opened (%ls), 48 kHz mono", opened.c_str());
        return true;
    }
    void close() {
        if (!device) return;
        waveInReset(device);
        for (auto &h : headers)
            if (h.dwFlags & WHDR_PREPARED) waveInUnprepareHeader(device, &h, sizeof h);
        waveInClose(device);
        device = nullptr;
        log("Voice capture closed");
    }
};

// --- Playback ---
struct Playback {
    HWAVEOUT device = nullptr;
    std::array<WAVEHDR, PlaybackBuffers> headers{};
    std::vector<int16_t> memory;
    uint64_t retryAt = 0;
    bool open(HANDLE event, uint64_t now) {
        WAVEFORMATEX format{WAVE_FORMAT_PCM, 2, Rate, Rate * 4, 4, 16, 0};
        const MMRESULT result = waveOutOpen(&device, WAVE_MAPPER, &format, DWORD_PTR(event), 0, CALLBACK_EVENT);
        if (result != MMSYSERR_NOERROR) {
            device = nullptr;
            log("Voice playback could not open (error %u); retrying in 10 s", unsigned(result));
            retryAt = now + 10000;
            return false;
        }
        memory.assign(size_t(Frame) * 2 * PlaybackBuffers, 0);
        for (unsigned i = 0; i < PlaybackBuffers; ++i) {
            auto &h = headers[i];
            h = {};
            h.lpData = reinterpret_cast<LPSTR>(memory.data() + size_t(i) * Frame * 2);
            h.dwBufferLength = Frame * 4;
            waveOutPrepareHeader(device, &h, sizeof h);
            h.dwFlags |= WHDR_DONE; // Free to fill.
        }
        log("Voice playback opened, 48 kHz stereo");
        return true;
    }
    void close() {
        if (!device) return;
        waveOutReset(device);
        for (auto &h : headers)
            if (h.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(device, &h, sizeof h);
        waveOutClose(device);
        device = nullptr;
    }
    unsigned queued() const {
        unsigned n = 0;
        for (const auto &h : headers) n += !(h.dwFlags & WHDR_DONE);
        return n;
    }
};

struct Worker {
    Capture capture;
    Playback playback;
    Activity gate;
    Packer packer;
    OpusEncoder *encoder = nullptr;
    std::deque<std::array<int16_t, Frame>> preroll;
    unsigned spurtFrames = 0;
    VrSettings::Voice settings;
    Scene view;
    bool session = false;

    void queue(const network::VoicePacket &packet) {
        std::lock_guard guard(lock);
        ++labCount.packets;
        if (labLoopback) {
            auto looped = packet;
            looped.clientNonce = looped.serverNonce = 1;
            inbound.push_back({LabSpeaker, 1, looped, GetTickCount64()});
            return;
        }
        if (outbound.size() >= OutboundQueue) outbound.pop_front(); // The freshest are kept.
        outbound.push_back(packet);
    }
    uint64_t labNextFrame = 0, labFrameIndex = 0;
    FILE *labWav = nullptr;
    unsigned labWavFrames = 0;
    // Lab: synthetic 20 ms frames on the wall clock while no microphone is open.
    void labSynthesize(uint64_t now) {
        if (!labNextFrame) labNextFrame = now;
        while (labNextFrame <= now) {
            labNextFrame += ss2vr::voice::FrameMs;
            std::array<int16_t, Frame> pcm{};
            const bool voiced = (labFrameIndex / 50) % 2 == 1;
            for (unsigned i = 0; i < Frame; ++i) {
                const double t = double(labFrameIndex * Frame + i) / Rate;
                double v = std::sin(i * 7.31 + labFrameIndex) * 30;
                if (voiced) v += 5000 * (std::sin(2 * 3.14159265 * 220 * t) + .5 * std::sin(2 * 3.14159265 * 440 * t));
                pcm[i] = int16_t(v);
            }
            ++labFrameIndex;
            ++labCount.synthetic;
            captured(pcm.data());
        }
    }
    void labWrite(const int16_t *stereo) {
        if (labWavFrames >= 600) return;
        if (!labWav) {
            wchar_t path[MAX_PATH]{};
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&labLoopback), &self);
            const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
            std::wstring file(path, n);
            file = file.substr(0, file.find_last_of(L"\\/") + 1) + L"voice-lab.wav";
            labWav = _wfopen(file.c_str(), L"wb");
            if (!labWav) { labWavFrames = 600; return; }
            const uint32_t data = 600u * Frame * 4;
            const uint32_t header[] = {0x46464952u, 36 + data, 0x45564157u, 0x20746d66u, 16, 0x00020001u, Rate, Rate * 4,
                                       0x00100004u, 0x61746164u, data};
            std::fwrite(header, sizeof header, 1, labWav);
        }
        std::fwrite(stereo, 4, Frame, labWav);
        if (++labWavFrames == 600) { std::fclose(labWav); labWav = nullptr; log("Lab voice wrote voice-lab.wav (12 s)"); }
    }
    void endSpurt() {
        network::VoicePacket packet{};
        if (packer.end(packet)) queue(packet);
        spurtFrames = 0;
        sendingNow = false;
    }
    void encode(const int16_t *pcm) {
        uint8_t bytes[ss2vr::voice::EncodedFrameLimit];
        const int length = encoder ? opus_encode(encoder, pcm, int(Frame), bytes, int(sizeof bytes)) : -1;
        network::VoicePacket packet{};
        if (length > 0 && packer.add(bytes, unsigned(length), packet)) queue(packet);
    }
    // One captured 20 ms frame.
    void captured(int16_t *pcm) {
        const float gain = settings.inputGain;
        if (gain != 1)
            for (unsigned i = 0; i < Frame; ++i) pcm[i] = int16_t(std::clamp(std::lround(pcm[i] * gain), -32768l, 32767l));
        const bool voiced = gate.step(Activity::energy(pcm, Frame), settings.sensitivity);
        const bool want = session && (settings.microphone == 1 ? voiced : settings.microphone == 2 && view.pushToTalk);
        if (want && !packer.talking) {
            if (encoder) opus_encoder_ctl(encoder, OPUS_RESET_STATE);
            packer.start();
            sendingNow = true;
            for (auto &frame : preroll) encode(frame.data()); // The onset's first syllable.
            preroll.clear();
        }
        if (packer.talking) {
            if (want) { encode(pcm); ++spurtFrames; }
            else endSpurt();
        } else {
            std::array<int16_t, Frame> copy;
            std::copy(pcm, pcm + Frame, copy.begin());
            preroll.push_back(copy);
            while (preroll.size() > ss2vr::voice::PrerollFrames) preroll.pop_front();
        }
    }
    void takeCapture(uint64_t now) {
        if (!capture.device) return;
        for (auto &h : capture.headers) {
            if (!(h.dwFlags & WHDR_DONE)) continue;
            if (h.dwBytesRecorded == Frame * 2) { ++labCount.captured; captured(reinterpret_cast<int16_t *>(h.lpData)); }
            else if (packer.talking) endSpurt(); // A short read is a discontinuity.
            h.dwFlags &= ~WHDR_DONE;
            h.dwBytesRecorded = 0;
            if (waveInAddBuffer(capture.device, &h, sizeof h) != MMSYSERR_NOERROR) {
                log("Voice capture failed; retrying in 10 s");
                if (packer.talking) endSpurt();
                capture.close();
                capture.retryAt = now + 10000;
                return;
            }
        }
    }
    void takeInbound(uint64_t now) {
        std::deque<Inbound> packets;
        {
            std::lock_guard guard(lock);
            packets.swap(inbound);
        }
        if (!settings.receive) return;
        for (const auto &in : packets) {
            Stream *s = stream(in.avatar, in.incarnation, now);
            if (!s) continue;
            const auto &v = in.packet;
            labCount.received += v.frameCount;
            for (unsigned i = 0; i < v.frameCount; ++i)
                s->jitter.insert(v.talkspurt, v.firstFrame + i, v.frames[i], v.frameBytes[i], in.tick);
            if (v.flags & network::VoiceEnd) s->jitter.end(v.talkspurt, v.firstFrame + v.frameCount, in.tick);
        }
    }
    // Mixes the next 20 ms into a free playback buffer.
    void mix(WAVEHDR &h, uint64_t now) {
        std::array<float, Frame * 2> sum{};
        std::array<int16_t, Frame> pcm;
        uint8_t bytes[network::MaxVoiceFrameBytes];
        for (auto &s : streams) {
            if (!s.avatar || !s.decoder) continue;
            unsigned length = 0;
            const auto pulled = s.jitter.pull(now, bytes, length);
            int samples = 0;
            if (pulled == Jitter::Pull::Frame) samples = opus_decode(s.decoder, bytes, int(length), pcm.data(), int(Frame), 0);
            else if (pulled == Jitter::Pull::Conceal) samples = opus_decode(s.decoder, nullptr, 0, pcm.data(), int(Frame), 0);
            if (samples != int(Frame)) continue;
            ++(pulled == Jitter::Pull::Frame ? labCount.played : labCount.concealed);
            Speaker where{};
            for (const auto &sp : view.speakers)
                if (sp.avatar == s.avatar) { where = sp; break; }
            if (labLoopback && s.avatar == LabSpeaker) where = {LabSpeaker, view.head + view.right * 2.f, true};
            const auto g = ss2vr::voice::spatialGains(view.head, view.right, where.position,
                view.listener && where.positioned, settings.spatialDistance, settings.radioVolume, settings.volume);
            const float scale = 1.5f / 32768.f;
            for (unsigned i = 0; i < Frame; ++i) {
                sum[i * 2] += pcm[i] * scale * g.left;
                sum[i * 2 + 1] += pcm[i] * scale * g.right;
            }
            if (pulled == Jitter::Pull::Frame) {
                std::lock_guard guard(lock);
                Talker *slot = nullptr;
                for (auto &t : talkers)
                    if (t.avatar == s.avatar) { slot = &t; break; }
                if (!slot)
                    for (auto &t : talkers)
                        if (!t.avatar || now > t.until) { slot = &t; break; }
                if (slot) *slot = {s.avatar, now + 150};
            }
        }
        auto *out = reinterpret_cast<int16_t *>(h.lpData);
        for (unsigned i = 0; i < Frame * 2; ++i) out[i] = int16_t(std::clamp(sum[i], -1.f, 1.f) * 32767.f);
        ++labCount.mixed;
        if (labLoopback) labWrite(out);
        if (!playback.device) return; // Lab: the virtual playback clock.
        h.dwFlags &= ~WHDR_DONE;
        if (waveOutWrite(playback.device, &h, sizeof h) != MMSYSERR_NOERROR) {
            log("Voice playback failed; retrying in 10 s");
            playback.close();
            playback.retryAt = now + 10000;
        }
    }
    void run() {
        int error = 0;
        encoder = opus_encoder_create(Rate, 1, OPUS_APPLICATION_VOIP, &error);
        if (error == OPUS_OK) {
            opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24000));
            opus_encoder_ctl(encoder, OPUS_SET_VBR(1));
            opus_encoder_ctl(encoder, OPUS_SET_DTX(1));
        } else {
            encoder = nullptr;
            log("Voice encoder unavailable (%d)", error);
        }
        std::wstring device;
        while (running) {
            WaitForSingleObject(wake, 10);
            const uint64_t now = GetTickCount64();
            {
                std::lock_guard guard(lock);
                settings = config;
                view = scene;
                session = configured && (labLoopback || (view.session && now - sceneTick < 1000));
            }
            if (labLoopback) {
                static uint64_t nextReport = 0;
                if (now >= nextReport) {
                    nextReport = now + 2000;
                    std::lock_guard guard(lock);
                    log("Lab voice capture=%u synthetic=%u packets=%u received=%u played=%u concealed=%u mixed=%u gate=%d noise=%.0f "
                        "mic=%d playback=%d listener=%d",
                        labCount.captured, labCount.synthetic, labCount.packets, labCount.received, labCount.played,
                        labCount.concealed, labCount.mixed, int(gate.open), double(gate.noise), int(capture.device != nullptr),
                        int(playback.device != nullptr), int(view.listener));
                }
            }
            // Capture only while there is someone to talk to and the microphone is on.
            const bool wantCapture = session && settings.microphone != 0 && !labLoopback; // Lab: synthetic only.
            if (wantCapture && (!capture.device || device != settings.device) && now >= capture.retryAt) {
                device = settings.device;
                capture.open(settings.device, wake, now);
            } else if (!wantCapture && capture.device) {
                if (packer.talking) endSpurt();
                capture.close();
                preroll.clear();
                gate = {};
            }
            takeCapture(now);
            if (labLoopback && !capture.device && settings.microphone != 0) labSynthesize(now);
            if (packer.talking && (!session || settings.microphone == 0)) endSpurt();
            // Playback while a session (or a stream) is live.
            const bool wantPlayback = settings.receive && session;
            if (wantPlayback && !playback.device && now >= playback.retryAt) playback.open(wake, now);
            else if (!wantPlayback && playback.device) {
                playback.close();
                for (auto &s : streams) s.jitter.reset();
            }
            takeInbound(now);
            if (playback.device)
                for (auto &h : playback.headers)
                    if ((h.dwFlags & WHDR_DONE) && playback.queued() < PlaybackAhead && playback.device) mix(h, now);
            if (labLoopback && !playback.device) {
                // Lab: no output device (the lab's ALSA device is the game's alone):
                // mix on a virtual 20 ms clock into the WAV.
                static uint64_t nextMix = 0;
                static std::vector<int16_t> scratch(Frame * 2);
                static WAVEHDR virtualHeader{};
                virtualHeader.lpData = reinterpret_cast<LPSTR>(scratch.data());
                if (!nextMix) nextMix = now;
                while (nextMix <= now) { nextMix += ss2vr::voice::FrameMs; mix(virtualHeader, nextMix); }
            }
        }
        if (packer.talking) endSpurt();
        if (labWav) std::fclose(labWav);
        capture.close();
        playback.close();
        for (auto &s : streams)
            if (s.decoder) { opus_decoder_destroy(s.decoder); s.decoder = nullptr; }
        if (encoder) opus_encoder_destroy(encoder);
    }
};

DWORD WINAPI threadMain(void *) {
    Worker *worker = new Worker;
    worker->run();
    delete worker;
    return 0;
}
} // namespace

bool start() {
    bool expected = false;
    if (!running.compare_exchange_strong(expected, true)) return true;
    wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread = wake ? CreateThread(nullptr, 0, threadMain, nullptr, 0, nullptr) : nullptr;
    if (!thread) {
        running = false;
        log("Voice chat thread could not start");
        return false;
    }
    SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
    log("Voice chat ready (Opus 24 kbit/s, 20 ms frames)");
    return true;
}
void stop() {
    if (!running.exchange(false)) return;
    if (wake) SetEvent(wake);
    if (thread) { WaitForSingleObject(thread, 2000); CloseHandle(thread); thread = nullptr; }
}
void configure(const VrSettings::Voice &settings) {
    std::lock_guard guard(lock);
    config = settings;
    configured = true;
}
void publish(const Scene &value) {
    std::lock_guard guard(lock);
    scene = value;
    sceneTick = GetTickCount64();
}
bool takeOutbound(network::VoicePacket &packet) {
    std::lock_guard guard(lock);
    if (outbound.empty()) return false;
    packet = outbound.front();
    outbound.pop_front();
    return true;
}
void receive(uint32_t avatar, uint32_t incarnation, const network::VoicePacket &packet) {
    if (!avatar || !running) return;
    std::lock_guard guard(lock);
    if (inbound.size() >= 64) inbound.pop_front();
    inbound.push_back({avatar, incarnation, packet, GetTickCount64()});
}
bool sending() { return sendingNow.load(std::memory_order_relaxed); }
Scene lastScene() {
    std::lock_guard guard(lock);
    return scene;
}
bool talking(uint32_t avatar) {
    if (!avatar) return false;
    const uint64_t now = GetTickCount64();
    std::lock_guard guard(lock);
    for (const auto &t : talkers)
        if (t.avatar == avatar && now <= t.until) return true;
    return false;
}
} // namespace ss2vr::game::voice
