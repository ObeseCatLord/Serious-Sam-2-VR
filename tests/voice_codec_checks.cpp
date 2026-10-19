// End to end without the game: a synthetic voice through the voice gate,
// packer, Opus encoder, carrier encode/parse, server admission, jitter buffer
// (with loss and reordering) and Opus decoder; the received audio must carry
// the voice at the right pitch.
#include "common/voice.hpp"
#include <opus.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace ss2vr;
using namespace ss2vr::voice;

int main() {
    int error = 0;
    OpusEncoder *encoder = opus_encoder_create(SampleRate, 1, OPUS_APPLICATION_VOIP, &error);
    assert(error == OPUS_OK);
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24000));
    opus_encoder_ctl(encoder, OPUS_SET_VBR(1));
    opus_encoder_ctl(encoder, OPUS_SET_DTX(1));
    OpusDecoder *decoder = opus_decoder_create(SampleRate, 1, &error);
    assert(error == OPUS_OK);

    // 0.6 s quiet, 1.2 s of a 220 Hz voiced tone with harmonics, 0.6 s quiet.
    const unsigned frames = 120;
    std::vector<int16_t> source(size_t(frames) * FrameSamples);
    for (size_t i = 0; i < source.size(); ++i) {
        const double t = double(i) / SampleRate;
        const bool voiced = t >= .6 && t < 1.8;
        const double noise = std::sin(i * 12.9898) * 43758.5453;
        double v = (noise - std::floor(noise) - .5) * 40;
        if (voiced) v += 6000 * (std::sin(2 * M_PI * 220 * t) + .5 * std::sin(2 * M_PI * 440 * t) + .25 * std::sin(2 * M_PI * 660 * t));
        source[i] = int16_t(std::clamp(v, -32768.0, 32767.0));
    }

    Activity gate;
    Packer packer;
    Admission admission;
    std::vector<network::VoicePacket> wire;
    std::vector<std::vector<int16_t>> preroll;
    unsigned sentFrames = 0;
    for (unsigned f = 0; f < frames; ++f) {
        const int16_t *pcm = &source[size_t(f) * FrameSamples];
        const bool want = gate.step(Activity::energy(pcm, FrameSamples), 55);
        const auto encode = [&](const int16_t *p) {
            uint8_t bytes[EncodedFrameLimit];
            const int n = opus_encode(encoder, p, int(FrameSamples), bytes, int(sizeof bytes));
            assert(n > 0 && n <= int(EncodedFrameLimit));
            network::VoicePacket packet{};
            ++sentFrames;
            if (packer.add(bytes, unsigned(n), packet)) wire.push_back(packet);
        };
        if (want && !packer.talking) {
            packer.start();
            for (auto &p : preroll) encode(p.data());
            preroll.clear();
        }
        if (packer.talking) {
            if (want) encode(pcm);
            else {
                network::VoicePacket packet{};
                if (packer.end(packet)) wire.push_back(packet);
            }
        } else {
            preroll.emplace_back(pcm, pcm + FrameSamples);
            if (preroll.size() > PrerollFrames) preroll.erase(preroll.begin());
        }
    }
    // One spurt: about the voiced 1.2 s plus pre-roll and hangover, ended.
    assert(!packer.talking && !wire.empty());
    assert((wire.front().flags & network::VoiceStart) && (wire.back().flags & network::VoiceEnd));
    assert(sentFrames >= 60 && sentFrames <= 60 + PrerollFrames + HangoverFrames + 2);
    std::printf("voice codec: %zu packets, %u frames\n", wire.size(), sentFrames);

    // Through the carrier (server relay form) and the server's admission.
    std::vector<network::VoicePacket> received;
    uint64_t t = 10000;
    for (auto packet : wire) {
        packet.clientNonce = 11; packet.serverNonce = 12;
        network::Message m{};
        m.kind = network::Kind::VoiceRelay;
        m.voiceRelay = {packet, 77, 1};
        std::string carrier;
        assert(network::encode(m, carrier));
        network::Message back{};
        assert(network::parse(carrier, back) == network::ParseResult::Valid);
        unsigned bytes = 30;
        for (unsigned i = 0; i < packet.frameCount; ++i) bytes += packet.frameBytes[i] + 1u;
        assert(admission.admit(t, bytes, back.voiceRelay.voice));
        t += 60;
        received.push_back(back.voiceRelay.voice);
    }
    // Swap two packets and lose one in the middle.
    assert(received.size() > 8);
    std::swap(received[3], received[4]);
    received.erase(received.begin() + 6);

    // Arrivals every 60 ms (in packet order); playout pulls every 20 ms.
    Jitter jitter;
    std::vector<int16_t> out;
    size_t next = 0;
    unsigned played = 0, concealed = 0;
    for (uint64_t now = 0; now < 4000; now += FrameMs) {
        while (next < received.size() && next * 60 <= now) {
            const auto &v = received[next++];
            for (unsigned i = 0; i < v.frameCount; ++i)
                jitter.insert(v.talkspurt, v.firstFrame + i, v.frames[i], v.frameBytes[i], now);
            if (v.flags & network::VoiceEnd) jitter.end(v.talkspurt, v.firstFrame + v.frameCount, now);
        }
        uint8_t bytes[network::MaxVoiceFrameBytes];
        unsigned length = 0;
        int16_t pcm[FrameSamples];
        const auto pulled = jitter.pull(now, bytes, length);
        int n = 0;
        if (pulled == Jitter::Pull::Frame) { n = opus_decode(decoder, bytes, int(length), pcm, int(FrameSamples), 0); ++played; }
        else if (pulled == Jitter::Pull::Conceal) { n = opus_decode(decoder, nullptr, 0, pcm, int(FrameSamples), 0); ++concealed; }
        if (n > 0) { assert(n == int(FrameSamples)); out.insert(out.end(), pcm, pcm + n); }
    }
    std::printf("voice codec: played %u, concealed %u\n", played, concealed);
    assert(concealed >= 3 && played + concealed >= sentFrames - 1 && !jitter.active());
    // The decoded voice: loud, and its dominant pitch near 220 Hz (zero crossings
    // of the fundamental over a steady second).
    double energy = 0;
    for (int16_t v : out) energy += std::abs(int(v));
    energy /= double(out.size());
    assert(energy > 1000);
    size_t best = 0;
    double bestEnergy = 0;
    for (size_t i = 0; i + SampleRate / 2 < out.size(); i += FrameSamples) {
        double e = 0;
        for (size_t k = 0; k < SampleRate / 2; ++k) e += std::abs(int(out[i + k]));
        if (e > bestEnergy) { bestEnergy = e; best = i; }
    }
    // Goertzel at 220 Hz against 300 Hz over the loudest half second.
    const auto power = [&](double hz) {
        const double w = 2 * M_PI * hz / SampleRate, c = 2 * std::cos(w);
        double s1 = 0, s2 = 0;
        for (size_t k = 0; k < SampleRate / 2; ++k) {
            const double s0 = out[best + k] + c * s1 - s2;
            s2 = s1; s1 = s0;
        }
        return s1 * s1 + s2 * s2 - c * s1 * s2;
    };
    const double p220 = power(220), p300 = power(300);
    std::printf("voice codec: mean level %.0f, 220 Hz / 300 Hz power %.1f\n", energy, p220 / p300);
    assert(p220 > 20 * p300);
    opus_encoder_destroy(encoder);
    opus_decoder_destroy(decoder);
    std::puts("voice codec checks passed");
}
