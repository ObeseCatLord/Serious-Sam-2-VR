#pragma once
// Spatial audio (common/spatial_audio.hpp): the game's sound runs on its own
// OpenAL backend (SfxOAL) with OpenAL Soft from Bin/SS2VR, HRTF configured by
// Bin/SS2VR/alsoft.ini, the tracked head as the listener, and sounds behind
// walls muffled through EFX lowpass filters. Main thread only unless noted.
#include "common/math.hpp"
#include <cstdint>

namespace ss2vr::game::spatial_audio {

struct Config {
    bool enabled = true;   // [Audio] Spatial
    bool occlusion = true; // [Audio] Occlusion
};
void configure(const Config &config);
// At Direct3DCreate9, before the game starts its sound: hooks Engine's
// sfxStartup_t so sound starts on OpenAL (the configured API is left alone).
void installStartupHook();
// Loads Bin/SS2VR/OpenAL32.dll (so SfxOAL's import binds to it) with its
// configuration and checks that it opens a device. True when the game may use
// the OpenAL backend.
bool prepare();
// Called with the game's own sound API choice: the one to start instead.
int chooseApi(int api);
// The listener: the head's world pose when it is current and close to the
// game's own listener placement (not a far cutscene camera).
void noteHead(const Pose &head, uint64_t tickMs);
bool listener(const Pose &native, Pose &out);
// Each frame: hooks OpenAL once the game loaded it, smooths occlusion and
// applies the filters.
void frame();
// Each frame (main thread): a looping saw engine still playing when the local
// player has held no saw for a second is stopped (active: single player).
void guardSawEngine(bool active, bool sawHeld);
// Occlusion rays (from the native ray context): sounds due a trace.
unsigned dueTraces(Vec3 &from, uint32_t *ids, Vec3 *ends, unsigned max);
void traced(uint32_t id, bool hit, float hitDistance, float distance);

} // namespace ss2vr::game::spatial_audio
