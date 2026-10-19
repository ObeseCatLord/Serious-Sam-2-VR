#include "common/voice.hpp"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
using namespace ss2vr;
using namespace ss2vr::voice;

static network::VoicePacket packet(uint8_t spurt, uint32_t first, uint8_t flags, unsigned frames, unsigned bytes) {
    network::VoicePacket v{};
    v.clientNonce = 7; v.serverNonce = 9;
    v.talkspurt = spurt; v.firstFrame = first; v.flags = flags; v.frameCount = uint8_t(frames);
    for (unsigned i = 0; i < frames; ++i) {
        v.frameBytes[i] = uint8_t(bytes);
        for (unsigned b = 0; b < bytes; ++b) v.frames[i][b] = uint8_t(i * 31 + b);
    }
    return v;
}

static void wire() {
    // Client -> server and server -> client round trips, within one carrier.
    network::Message m{};
    m.kind = network::Kind::Voice;
    m.voice = packet(3, 1000, network::VoiceStart, 3, 80); // 24 kbit/s frames are ~60 bytes
    std::string carrier;
    assert(network::encode(m, carrier) && carrier.size() <= network::MaxCarrierAscii);
    network::Message back{};
    assert(network::parse(carrier, back) == network::ParseResult::Valid && back.kind == network::Kind::Voice);
    assert(back.voice.firstFrame == 1000 && back.voice.talkspurt == 3 && back.voice.frameCount == 3);
    assert(back.voice.frameBytes[2] == 80 && back.voice.frames[2][79] == uint8_t(2 * 31 + 79));
    m.kind = network::Kind::VoiceRelay;
    m.voiceRelay = {m.voice, 0x1234, 5};
    assert(network::encode(m, carrier) && carrier.size() <= network::MaxCarrierAscii);
    assert(network::parse(carrier, back) == network::ParseResult::Valid && back.kind == network::Kind::VoiceRelay);
    assert(back.voiceRelay.subjectAvatar == 0x1234 && back.voiceRelay.subjectIncarnation == 5);
    // The largest frames the encoder may produce: three still fit a relay.
    m.voiceRelay.voice = packet(3, 1000, 0, 3, EncodedFrameLimit);
    assert(network::encode(m, carrier) && carrier.size() <= network::MaxCarrierAscii);
    // Rejected: no nonce, spurt 0, an empty non-END packet, unknown flags.
    auto bad = packet(1, 0, 0, 1, 10); bad.clientNonce = 0;
    m.kind = network::Kind::Voice; m.voice = bad;
    assert(!network::encode(m, carrier));
    m.voice = packet(0, 0, 0, 1, 10);
    assert(!network::encode(m, carrier));
    m.voice = packet(1, 0, 0, 0, 0);
    assert(!network::encode(m, carrier));
    m.voice = packet(1, 0, network::VoiceEnd, 0, 0); // An END with no frames is valid.
    assert(network::encode(m, carrier));
    m.voice = packet(1, 0, 8, 1, 10);
    assert(!network::encode(m, carrier));
}

static void packer() {
    Packer p;
    network::VoicePacket out{};
    const uint8_t frame[60]{};
    // Outside a spurt frames only advance the counter.
    assert(!p.add(frame, 60, out) && p.frame == 1);
    p.start();
    assert(p.talkspurt == 1);
    assert(!p.add(frame, 60, out) && !p.add(frame, 60, out));
    assert(p.add(frame, 60, out));
    assert(out.flags == network::VoiceStart && out.firstFrame == 1 && out.frameCount == 3 && out.talkspurt == 1);
    assert(!p.add(frame, 50, out));
    assert(p.end(out));
    assert(out.flags == network::VoiceEnd && out.firstFrame == 4 && out.frameCount == 1);
    assert(!p.end(out));
    p.start();
    assert(p.end(out) && out.frameCount == 0 && out.firstFrame == 5 && (out.flags & network::VoiceEnd));
    // Spurt ids wrap without 0.
    p.talkspurt = 255; p.start();
    assert(p.talkspurt == 1);
}

static void admission() {
    Admission a;
    // Duplicates are refused; the budget counts them too.
    auto v = packet(1, 0, 0, 3, 60);
    assert(a.admit(1000, 200, v));
    assert(!a.admit(1001, 200, v));
    unsigned accepted = 1;
    for (unsigned i = 1; i < 200; ++i) {
        v.firstFrame = i * 3;
        if (a.admit(1002, 100, v)) ++accepted;
    }
    assert(accepted < RelayPacketsPerSecond); // The 16000 byte budget binds first here.
    v.firstFrame = 999;
    assert(!a.admit(1500, 10, v));
    assert(a.admit(2001, 10, v)); // The next window.
}

static void activity() {
    Activity g;
    std::vector<int16_t> quiet(FrameSamples), loud(FrameSamples);
    for (unsigned i = 0; i < FrameSamples; ++i) {
        quiet[i] = int16_t((i % 7) * 6 - 18);
        loud[i] = int16_t((i & 1 ? 1 : -1) * 3000);
    }
    const float eq = Activity::energy(quiet.data(), FrameSamples), el = Activity::energy(loud.data(), FrameSamples);
    for (int i = 0; i < 50; ++i) assert(!g.step(eq, 55));
    assert(!g.step(el, 55));           // One loud frame is not speech,
    assert(g.step(el, 55));            // two are.
    for (unsigned i = 0; i < HangoverFrames; ++i) assert(g.step(eq, 55)); // Hangover,
    assert(!g.step(eq, 55));           // then closed.
    assert(g.meter > 0);
    // Sensitivity 0 needs a louder voice than 100.
    Activity a, b;
    for (int i = 0; i < 50; ++i) { a.step(eq, 0); b.step(eq, 100); }
    const float mid = eq * 1.6f + 1;
    a.step(mid, 0); b.step(mid, 100);
    assert(!a.step(mid, 0) && b.step(mid, 100));
}

static void jitter() {
    Jitter j;
    uint8_t bytes[network::MaxVoiceFrameBytes], frame[4]{1, 2, 3, 4};
    unsigned length = 0;
    // Playout begins the target delay after the first arrival.
    assert(j.insert(1, 100, frame, 4, 1000));
    assert(j.pull(1000, bytes, length) == Jitter::Pull::None);
    assert(j.insert(1, 102, frame, 4, 1010)); // 101 arrives late,
    assert(j.insert(1, 101, frame, 4, 1015)); // still before playout.
    assert(j.pull(1000 + j.target, bytes, length) == Jitter::Pull::Frame && length == 4);
    assert(j.pull(1100, bytes, length) == Jitter::Pull::Frame);
    assert(j.pull(1120, bytes, length) == Jitter::Pull::Frame);
    // 103 never arrives: concealed; 104 plays.
    assert(j.insert(1, 104, frame, 4, 1130));
    assert(j.pull(1140, bytes, length) == Jitter::Pull::Conceal);
    assert(j.pull(1160, bytes, length) == Jitter::Pull::Frame);
    assert(!j.insert(1, 103, frame, 4, 1165)); // Too late now.
    // END after 104: the spurt finishes and the delay adapts to its impairments.
    j.end(1, 105, 1170);
    assert(j.pull(1180, bytes, length) == Jitter::Pull::None && !j.active());
    assert(j.target >= MinDelayMs && j.target <= MaxDelayMs && j.target > 60);
    // A clean spurt lowers it again.
    const unsigned before = j.target;
    assert(j.insert(2, 200, frame, 4, 2000));
    j.end(2, 201, 2001);
    unsigned pulled = 0;
    for (uint64_t t = 2000; t < 2400; t += 20)
        if (j.pull(t, bytes, length) == Jitter::Pull::Frame) ++pulled;
    assert(pulled == 1 && !j.active() && j.target == std::max(MinDelayMs, before - FrameMs));
    // A speaker who stops sending without END goes stale.
    assert(j.insert(3, 300, frame, 4, 3000));
    for (uint64_t t = 3000; t < 3000 + StaleMs + 400; t += 20) j.pull(t, bytes, length);
    assert(!j.active());
    // A straggler of the previous spurt does not restart playout.
    assert(j.insert(4, 400, frame, 4, 4000));
    assert(!j.insert(3, 301, frame, 4, 4001) && j.active());
}

static void spatial() {
    const Vec3 head{0, 1.6f, 0}, right{1, 0, 0};
    // A speaker to the right: louder right; centred straight ahead.
    auto g = spatialGains(head, right, Vec3{3, 1.6f, 0}, true, 24, .45f, 1);
    assert(g.right > g.left);
    g = spatialGains(head, right, Vec3{0, 1.6f, -3}, true, 24, .45f, 1);
    assert(std::abs(g.left - g.right) < 1e-5f);
    // Far: all radio, centred; unpositioned: radio.
    g = spatialGains(head, right, Vec3{100, 1.6f, 0}, true, 24, .45f, 1);
    assert(std::abs(g.left - .45f) < 1e-4f && std::abs(g.right - .45f) < 1e-4f);
    g = spatialGains(head, right, Vec3{}, false, 24, .45f, 2);
    assert(std::abs(g.left - .9f) < 1e-4f && g.left == g.right);
    // Closer is louder.
    assert(spatialGains(head, right, Vec3{0, 1.6f, -1}, true, 24, .45f, 1).left >
           spatialGains(head, right, Vec3{0, 1.6f, -8}, true, 24, .45f, 1).left);
}

int main() {
    wire();
    packer();
    admission();
    activity();
    jitter();
    spatial();
    std::puts("voice checks passed");
}
