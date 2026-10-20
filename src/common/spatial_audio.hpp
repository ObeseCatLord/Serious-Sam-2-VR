#pragma once
// Spatial audio for the game's OpenAL output. The design follows QuakeSpasm
// VR's spatial audio (written for this project, no code taken): the tracked
// head is the listener and HRTF renders each sound's direction (OpenAL Soft);
// a sound behind walls is muffled. One ray per sound from the head, refreshed
// every 0.2 s, at most eight a frame, the result smoothed over 75 ms; an
// occluded sound passes a lowpass and sends less to the room's reverb. The
// player's own sounds (at the head) stay dry and are never traced.
#include "math.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace ss2vr::spatial_audio {

constexpr unsigned MaxSources = 96, TracesPerFrame = 8;
constexpr uint64_t TraceIntervalMs = 200;
constexpr float SmoothingSeconds = .075f;
constexpr float OwnRadius = .75f;      // Sounds this close to the head are the player's own.
constexpr float MaxTraceDistance = 150; // Farther sounds keep their last result.
constexpr float NearMiss = 1.f;         // A hit this close to the sound is the sound's own body.

// Direct-path lowpass (OpenAL EFX: overall gain, gain at high frequencies) and
// reverb send scale for an occlusion amount 0..1.
struct Filter {
    float gain = 1, gainHF = 1, send = 1;
};
inline Filter occlusionFilter(float occlusion) {
    const float o = std::clamp(std::isfinite(occlusion) ? occlusion : 0.f, 0.f, 1.f);
    return {1 - .4f * o, 1 - .85f * o, 1 - .3f * o};
}

struct Source {
    uint32_t id = 0;
    Vec3 relative{}; // Listener-relative position as the game gave it to OpenAL.
    float occlusion = 0, target = 0;
    uint64_t traced = 0;
    bool used = false, positioned = false;
};

class Tracker {
  public:
    std::array<Source, MaxSources> sources{};

    Source *find(uint32_t id) {
        for (auto &s : sources)
            if (s.used && s.id == id) return &s;
        return nullptr;
    }
    // The game moved a sound (listener-relative metres).
    void position(uint32_t id, Vec3 relative) {
        if (!std::isfinite(relative.x + relative.y + relative.z)) return;
        Source *s = find(id);
        if (!s) {
            for (auto &candidate : sources)
                if (!candidate.used) { s = &candidate; break; }
            if (!s) return;
            *s = {};
            s->used = true;
            s->id = id;
        }
        s->relative = relative;
        s->positioned = true;
        if (own(*s)) s->target = 0;
    }
    void remove(uint32_t id) {
        if (Source *s = find(id)) *s = {};
    }
    static bool own(const Source &s) { return dot(s.relative, s.relative) < OwnRadius * OwnRadius; }
    // Sounds due a trace, the longest waiting first: their ids and world
    // positions from the listener pose the game placed them with.
    unsigned due(const Pose &listener, uint64_t now, uint32_t *ids, Vec3 *ends, unsigned max) const {
        std::array<const Source *, MaxSources> order{};
        unsigned count = 0;
        for (const auto &s : sources) {
            if (!s.used || !s.positioned || own(s) || (s.traced && now - s.traced < TraceIntervalMs)) continue;
            if (dot(s.relative, s.relative) > MaxTraceDistance * MaxTraceDistance) continue;
            order[count++] = &s;
        }
        std::sort(order.begin(), order.begin() + count, [](const Source *a, const Source *b) { return a->traced < b->traced; });
        count = std::min(count, max);
        for (unsigned i = 0; i < count; ++i) {
            ids[i] = order[i]->id;
            ends[i] = compose(listener, Pose{{0, 0, 0, 1}, order[i]->relative}).p;
        }
        return count;
    }
    // A ray from the head toward the sound: blocked when it stops short of it.
    void traced(uint32_t id, bool hit, float hitDistance, float distance, uint64_t now) {
        Source *s = find(id);
        if (!s) return;
        s->traced = now ? now : 1;
        s->target = hit && std::isfinite(hitDistance) && hitDistance < distance - NearMiss ? 1.f : 0.f;
    }
    void step(float seconds) {
        const float k = seconds > 0 && std::isfinite(seconds) ? 1 - std::exp(-seconds / SmoothingSeconds) : 0.f;
        for (auto &s : sources)
            if (s.used) s.occlusion += (s.target - s.occlusion) * k;
    }
};

} // namespace ss2vr::spatial_audio
