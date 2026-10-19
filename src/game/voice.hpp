#pragma once
// Multiplayer voice chat (client DLL; the dedicated server links voice_stub.cpp
// and only relays). An audio thread owns capture, Opus, playout and mixing;
// the game thread publishes the scene and sends packets; the network receive
// path hands packets in. See common/voice.hpp for the design.
#include "common/math.hpp"
#include "common/network.hpp"
#include "common/settings.hpp"
#include <cstdint>

namespace ss2vr::game::voice {

struct Speaker {
    uint32_t avatar = 0; // Local native handle of the player
    Vec3 position{};     // World position of the voice (the player's eyes)
    bool positioned = false;
};
struct Scene {
    bool session = false;   // A negotiated multiplayer session to talk in
    bool listener = false;  // Listener pose valid
    Vec3 head{}, right{};   // World head position and its right direction
    bool pushToTalk = false; // A controller held at the mouth
    uint32_t self = 0;       // Local player's handle (never mixed)
    Speaker speakers[18];
};

// Client only: starts the audio thread (idempotent; false on the server stub).
bool start();
void stop();
// Game thread, every tick.
void configure(const VrSettings::Voice &settings);
void publish(const Scene &scene);
bool takeOutbound(network::VoicePacket &packet);
// Network receive path: one packet of a speaker (local avatar handle and the
// sender's capability incarnation, which renews the stream when it changes).
void receive(uint32_t avatar, uint32_t incarnation, const network::VoicePacket &packet);
// Indicators (any thread).
bool sending();
bool talking(uint32_t avatar);
Scene lastScene();

} // namespace ss2vr::game::voice
