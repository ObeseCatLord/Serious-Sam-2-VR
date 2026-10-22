#pragma once
#include "common/network.hpp"
#include "common/observer_gesture_intents.hpp"
#include <array>
#include <windows.h>
namespace ss2vr::game::multiplayer {
using HookInstaller = bool (*)(HMODULE, const char *, void *, void **);
bool initialize(HMODULE engine, HMODULE core, HMODULE sam, HookInstaller install);
bool remoteClient();
bool server();
bool negotiatedLocal();
// A 5 s log summary of VR session traffic (client or server; otherwise nothing).
void reportDiagnostics() noexcept;
// Voice chat (game/voice.hpp). The sink receives every voice packet for the
// local player, naming the speaker's local avatar handle and the speaker's
// capability incarnation; null (dedicated server) only relays.
using VoiceSink = void (*)(uint32_t avatar, uint32_t incarnation, const network::VoicePacket &packet);
void setVoiceSink(VoiceSink sink);
// Someone to talk to: a client negotiated with its server, or a listen host
// with at least one negotiated peer.
bool voiceSession();
// Game thread: the local player's packet (client: to the server, which relays
// it; listen host: to every negotiated peer).
bool sendVoice(void *localPlayer, network::VoicePacket packet);
// Authoritative negotiated-peer recognition only, including invalid intent.
// Local XR ownership comes from Snapshot.initialized, never a local nonce.
// No native getter, transport operation, freeze or admission grant.
bool knownVrAvatar(uint32_t avatar) noexcept;
// True when the latest pose or a bounded physical pulse was retained locally,
// or a reliable Pose entered native transport. The in/out pose also returns
// current admission independently of transport credit or send success. Capture
// is mandatory, local-only, and owns the exact raw calibrated poses plus CURRENT
// origin/turn. frame.avatar is the native local player handle; trackingEpoch is
// the rig epoch and trackingGeneration matches pose.trackingGeneration. Producer,
// session/reference and sequence/tickMs come from that same admitted input.
// Missing/inactive coordinates require neutral and carry only an inactive Pose;
// they do not block the Hello/capability lifecycle that enables initial tracking.
bool submit(void *player, network::PosePacket &pose, bool reliableEdge,
            const network::LocalPoseCapture &capture);
// Recheck a captured native-client firing intent against current ACK/input
// admission. This borrows existing Local/Pending state, not another scheduler.
bool localPrimaryAllowed(void *player, unsigned hand, uint32_t intentEpoch,
                         uint32_t primaryGeneration, uint64_t inputSequence, bool requireFire = true);
bool localGestureAllowed(void *player, unsigned hand, uint32_t intentEpoch,
                         uint32_t gestureGeneration, uint64_t gestureSequence,
                         uint64_t inputSequence, bool requireDown = true);
// Read the existing ACK/pose ownership at native zoom use; no second gate.
bool localZoomAllowed(void *player,unsigned hand,uint32_t intentEpoch,
                      uint32_t zoomGeneration,uint64_t inputSequence);
struct Sample {
    bool negotiated = false, valid = false;
    uint32_t avatar = 0, incarnation = 0;
    uint64_t receivedMs = 0;
    network::PosePacket pose;
    uint32_t liveIntentEpoch[2]{}; // Validation metadata, never a restamped pose.
    uint64_t presentationRevision = 0; // Local invalidation history; never serialized.
};
Sample freeze(void *player);
// Returns native player objects from the current game-info player slots. This
// works for single-player, clients and the listen host before negotiation.
std::array<void *, 18> activePlayers();
// Opens one authoritative simulation interval before freezing any peers or
// stepping weapon entities. Pair a true result with completeTick after the
// complete native entity/script/physics interval.
bool beginTick();
// Pair with a successful beginTick after the scheduler completes the full
// entity/script/physics interval; this retires only packets staged in it.
void completeTick();
// No native callbacks or sends on abort. Existing tokens are discarded by a
// later normal interval or the existing peer invalidation path.
void abortTick() noexcept;
Sample authority(void *player);
Sample presentation(void *player);
struct ObserverGestureSample {
    Sample latest;
    network::PosePacket pulse[2];
    network::ObserverGestureIntents::Receipt receipt[2];
};
// Simulation-only copy of the recipient-admitted presentation and retained
// physical edges. No lock is held while the caller runs native weapon callbacks.
ObserverGestureSample observerGestures(uint32_t avatar);
bool observerGesturesCurrent(const ObserverGestureSample &, unsigned hand);
bool finishObserverGesture(const ObserverGestureSample &, unsigned hand);
// Narrow read transaction for remote frame publication. Acquired after the
// renderer binding lock; sample() omits age, which is checked at pair admission.
// All presentation lifecycle writers use the exclusive side of this lock.
class PresentationReadGuard {
    bool server_;
    bool held_ = false;
  public:
    explicit PresentationReadGuard(bool acquireNow = true);
    ~PresentationReadGuard();
    void release() noexcept;
    void acquire() noexcept;
    PresentationReadGuard(const PresentationReadGuard &) = delete;
    PresentationReadGuard &operator=(const PresentationReadGuard &) = delete;
    Sample sample(uint32_t avatar) const;
};
void invalidatePlayer(void *player);
void invalidateWeapons(void *player, uint32_t generation, uint8_t handMask = network::HandMask);
} // namespace ss2vr::game::multiplayer
