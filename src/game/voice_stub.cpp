// Dedicated server: voice is relayed by multiplayer.cpp, never captured or played.
#include "voice.hpp"
namespace ss2vr::game::voice {
bool start() { return false; }
void stop() {}
void configure(const VrSettings::Voice &) {}
void publish(const Scene &) {}
bool takeOutbound(network::VoicePacket &) { return false; }
void receive(uint32_t, uint32_t, const network::VoicePacket &) {}
bool sending() { return false; }
bool talking(uint32_t) { return false; }
Scene lastScene() { return {}; }
} // namespace ss2vr::game::voice
