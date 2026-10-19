#pragma once
// Multiplayer voice chat: portable parts (no audio device, codec or native
// calls). The design follows QuakeSpasm VR's voice chat: 48 kHz mono 20 ms Opus
// frames, a voice-activity gate with onset, hangover and pre-roll, an adaptive
// per-speaker jitter buffer with loss concealment, unreliable client -> server
// -> clients relay with a per-source budget, and positional playback that
// fades to a centred "radio" voice with distance. Written for this project.
#include "math.hpp"
#include "network.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace ss2vr::voice {

constexpr unsigned SampleRate = 48000, FrameSamples = 960, FrameMs = 20;
constexpr unsigned FramesPerPacket = 3;   // 60 ms per carrier message
// The encoder's per-frame byte limit: three frames and the relay header fit one
// 512-character base64 carrier (24 kbit/s frames are ~60 bytes).
constexpr unsigned EncodedFrameLimit = 100;
constexpr unsigned PrerollFrames = 3;     // Sent ahead of the onset
constexpr unsigned HangoverFrames = 12;   // 240 ms after the last loud frame
constexpr unsigned OnsetFrames = 2;       // Consecutive loud frames that open the gate
constexpr unsigned MinDelayMs = 60, MaxDelayMs = 140, StaleMs = 400;
constexpr unsigned RelayPacketsPerSecond = 75, RelayBytesPerSecond = 16000;

// Wrap-aware frame order.
inline bool before(uint32_t a, uint32_t b) { return int32_t(a - b) < 0; }

// Energy gate. Energy is the frame's mean absolute sample (0..32768); the
// noise floor follows quiet input; sensitivity 0..100 lowers the thresholds.
struct Activity {
    float noise = -1, meter = 0;
    unsigned loud = 0, quiet = 0;
    bool open = false;
    static float energy(const int16_t *pcm, unsigned count) {
        if (!count) return 0;
        double sum = 0;
        for (unsigned i = 0; i < count; ++i) sum += std::abs(int(pcm[i]));
        return float(sum / count);
    }
    // One frame; true while speech is gated on.
    bool step(float e, float sensitivity) {
        const float s = std::clamp(sensitivity, 0.f, 100.f);
        if (noise < 0) noise = std::max(e, 1.f);
        const float ratio = (2304.f - 11.f * s) / 1024.f, margin = (960.f - 7.f * s) / 256.f;
        const float openAt = std::max(noise * ratio, noise + margin);
        const float closeAt = std::max(noise * (ratio - .25f), noise + margin * .5f + .1875f);
        if (!open) {
            loud = e >= openAt ? loud + 1 : 0;
            if (loud >= OnsetFrames) { open = true; quiet = 0; }
        } else {
            quiet = e <= closeAt ? quiet + 1 : 0;
            if (quiet > HangoverFrames) { open = false; loud = 0; }
        }
        // The floor follows the room, slowly while speech may be present.
        if (!open && e < openAt) noise += (e - noise) * (loud ? 1.f / 32 : 1.f / 16);
        else if (open && e <= closeAt) noise += (e - noise) / 64;
        noise = std::max(noise, 1.f);
        meter = meter * .75f + std::min(1.f, e / 8192.f) * .25f;
        return open;
    }
};

// Groups encoded frames into packets of FramesPerPacket. A talk spurt starts
// with START and finishes with END (an END packet may carry no frames).
struct Packer {
    uint32_t frame = 0;    // Next frame number
    uint8_t talkspurt = 0; // Current spurt id (0 outside one)
    bool talking = false;
    network::VoicePacket building{};
    // Begins a spurt; frames added next belong to it.
    void start() {
        talking = true;
        talkspurt = uint8_t(talkspurt % 255 + 1);
        building = {};
        building.talkspurt = talkspurt;
        building.firstFrame = frame;
        building.flags = network::VoiceStart;
    }
    // Adds one encoded frame; true when `out` holds a full packet to send.
    bool add(const uint8_t *bytes, unsigned length, network::VoicePacket &out) {
        if (!talking || !length || length > network::MaxVoiceFrameBytes) { ++frame; return false; }
        if (!building.frameCount) building.firstFrame = frame;
        const unsigned i = building.frameCount++;
        building.frameBytes[i] = uint8_t(length);
        std::memcpy(building.frames[i], bytes, length);
        ++frame;
        if (building.frameCount < FramesPerPacket) return false;
        out = building;
        building = {};
        building.talkspurt = talkspurt;
        return true;
    }
    // Ends the spurt: the remaining frames (if any) with END.
    bool end(network::VoicePacket &out) {
        if (!talking) return false;
        talking = false;
        building.talkspurt = talkspurt;
        building.flags = uint8_t(building.flags | network::VoiceEnd);
        if (!building.frameCount) building.firstFrame = frame;
        out = building;
        building = {};
        return true;
    }
    // A capture gap: frame numbers keep counting so receivers conceal it.
    void skip(unsigned frames) { frame += frames; }
};

// Server admission per source: a one-second budget charged before the
// duplicate check, then a 32-entry duplicate ring.
struct Admission {
    uint64_t windowStart = 0;
    unsigned packets = 0, bytes = 0;
    struct Seen { uint32_t first; uint8_t talkspurt, flags, count; bool used; };
    std::array<Seen, 32> seen{};
    unsigned head = 0;
    bool admit(uint64_t now, unsigned packetBytes, const network::VoicePacket &v) {
        if (!windowStart || now - windowStart >= 1000) { windowStart = now; packets = bytes = 0; }
        ++packets;
        bytes += packetBytes;
        if (packets > RelayPacketsPerSecond || bytes > RelayBytesPerSecond) return false;
        for (const auto &s : seen)
            if (s.used && s.first == v.firstFrame && s.talkspurt == v.talkspurt && s.flags == v.flags &&
                s.count == v.frameCount)
                return false;
        seen[head] = {v.firstFrame, v.talkspurt, v.flags, v.frameCount, true};
        head = (head + 1) % seen.size();
        return true;
    }
};

// Per-speaker playout. Frames are inserted as they arrive; the mixer pulls one
// per 20 ms. Playout starts the target delay after a spurt's first arrival;
// a frame missing at its deadline is concealed. The delay adapts per spurt to
// the impairments seen (reordering, overflow, concealment): 60..140 ms.
class Jitter {
  public:
    enum class Pull { None, Frame, Conceal };
    unsigned target = 80;
    unsigned impairment = 0;
    bool active() const { return running; }
    void reset() { *this = Jitter{target}; }
    // Returns false for a frame that came too late or did not fit.
    bool insert(uint8_t talkspurt, uint32_t seq, const uint8_t *bytes, unsigned length, uint64_t now) {
        if (running && talkspurt != spurt && before(seq, next)) return false; // A straggler of the last spurt.
        if (!running || talkspurt != spurt) begin(talkspurt, seq, now);
        lastArrival = now;
        if (!started && before(seq, next)) next = seq; // Out of order before playout began.
        if (before(seq, next)) { bump(); return false; }
        if (uint32_t(seq - next) >= slots.size()) { bump(); return false; } // Too far ahead.
        auto &slot = slots[seq % slots.size()];
        if (slot.used && slot.seq == seq) return false; // Duplicate.
        if (slot.used) bump();
        if (highest && before(seq, highestSeq)) bump(); // Reordered.
        if (!highest || before(highestSeq, seq)) { highestSeq = seq; highest = true; }
        slot.used = true;
        slot.seq = seq;
        slot.length = uint8_t(std::min<unsigned>(length, network::MaxVoiceFrameBytes));
        std::memcpy(slot.bytes, bytes, slot.length);
        return true;
    }
    // The spurt's end: no frame at or after `endFrame` will follow.
    void end(uint8_t talkspurt, uint32_t endFrame, uint64_t now) {
        if (!running || talkspurt != spurt) return;
        ended = true;
        endAt = endFrame;
        lastArrival = now;
    }
    // One 20 ms playout step at `now`.
    Pull pull(uint64_t now, uint8_t *bytes, unsigned &length) {
        length = 0;
        if (!running) return Pull::None;
        if (now < deadline) return Pull::None;
        started = true;
        if (ended && !before(next, endAt)) { finish(); return Pull::None; }
        if (!ended && now - lastArrival > StaleMs) { finish(); return Pull::None; }
        auto &slot = slots[next % slots.size()];
        deadline += FrameMs;
        if (now > deadline + 200) deadline = now; // A stall is not caught up frame by frame.
        if (slot.used && slot.seq == next) {
            slot.used = false;
            std::memcpy(bytes, slot.bytes, slot.length);
            length = slot.length;
            ++next;
            return Pull::Frame;
        }
        ++next;
        bump();
        return Pull::Conceal;
    }

  private:
    struct Slot { uint32_t seq = 0; uint8_t length = 0; bool used = false; uint8_t bytes[network::MaxVoiceFrameBytes]{}; };
    std::array<Slot, 24> slots{};
    bool running = false, started = false, ended = false, highest = false;
    uint8_t spurt = 0;
    uint32_t next = 0, endAt = 0, highestSeq = 0;
    uint64_t deadline = 0, lastArrival = 0;
    explicit Jitter(unsigned keepTarget) : target(keepTarget) {}
  public:
    Jitter() = default;
  private:
    void bump() { impairment = std::min(impairment + 1, 4u); }
    void begin(uint8_t talkspurt, uint32_t seq, uint64_t now) {
        if (running) finish();
        const unsigned keep = target;
        *this = Jitter{keep};
        running = true;
        spurt = talkspurt;
        next = seq;
        deadline = now + target;
    }
    void finish() {
        target = impairment ? std::clamp(MinDelayMs + FrameMs * impairment, MinDelayMs, MaxDelayMs)
                            : std::max(MinDelayMs, target > FrameMs ? target - FrameMs : MinDelayMs);
        const unsigned keep = target;
        *this = Jitter{keep};
    }
};

// Stereo gains for a speaker. Near, the voice is positional (panned by its
// direction from the head, softer with distance); far, it fades to a centred
// radio voice. Without a position it is radio only.
struct Gains { float left = 0, right = 0; };
inline Gains spatialGains(Vec3 listener, Vec3 right, Vec3 speaker, bool positioned, float spatialDistance,
                          float radioVolume, float volume) {
    Gains g;
    const float radio = std::clamp(radioVolume, 0.f, 1.f);
    if (!positioned || !(spatialDistance > 0)) { g.left = g.right = radio * volume; return g; }
    const Vec3 d = speaker - listener;
    const float distance = std::sqrt(dot(d, d));
    if (!std::isfinite(distance)) { g.left = g.right = radio * volume; return g; }
    const float blend = std::clamp(distance / spatialDistance, 0.f, 1.f);
    const float closeness = 1.f / (1.f + distance / 16.f);
    const float pan = distance > 1e-3f ? std::clamp(dot(d, right) / distance, -1.f, 1.f) : 0.f;
    const float positionalLeft = closeness * (1.f - .5f * pan), positionalRight = closeness * (1.f + .5f * pan);
    g.left = ((1 - blend) * positionalLeft + blend * radio) * volume;
    g.right = ((1 - blend) * positionalRight + blend * radio) * volume;
    return g;
}

} // namespace ss2vr::voice
