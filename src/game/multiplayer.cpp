#include "multiplayer.hpp"
#include "common/winproc.hpp"
#include "common/presentation_identity.hpp"
#include "common/intent_boundary.hpp"
#include "common/voice.hpp"
#include "game.hpp"
#include "native_finally.hpp"
#include <array>
#include <atomic>
#include <bcrypt.h>
#include <cstring>
#include <vector>
namespace ss2vr::game::multiplayer {
namespace {
using ThisVoid = void(__thiscall *)(void *);
using Rpc = void(__thiscall *)(void *, void *);
using Execute = void(__thiscall *)(void *, int, uint32_t, int, int, uint8_t *);
using SetAvatar = void(__thiscall *)(void *, uint32_t, int);
using Disconnect = void(__thiscall *)(void *, int, const char *);
using SetData = void(__thiscall *)(void *, int, uint32_t, int, void *);
using ReverseMap = uint32_t *(__thiscall *)(void *, uint32_t *, uint32_t);
using MemoryStream = void(__thiscall *)(void *, const void *, int);
using BrainGet = uint32_t *(__thiscall *)(void *, uint32_t *, int);
using GameGet = uint32_t *(__cdecl *)(uint32_t *);
static void *(__cdecl *resolve)(uint32_t) = nullptr;
static uint32_t(__cdecl *handleOf)(void *) = nullptr;
static int(__cdecl *isClient)() = nullptr, (__cdecl * isServer)() = nullptr;
static void *(__cdecl *currentNet)() = nullptr;
static void(__cdecl *sendNative)(void *) = nullptr, (__cdecl * sendToNative)(void *, int) = nullptr;
static ThisVoid streamCtor = nullptr, streamDtor = nullptr, reliableCtor = nullptr, reliableDtor = nullptr,
                unreliableCtor = nullptr, unreliableDtor = nullptr;
static MemoryStream createMemory = nullptr;
static SetData reliableData = nullptr, unreliableData = nullptr;
static ReverseMap reverseMap = nullptr;
static BrainGet getBrain = nullptr;
static GameGet getGame = nullptr;
static Rpc originalReliable = nullptr, originalUnreliable = nullptr;
static Execute originalClient = nullptr;
static Rpc originalClientReliable = nullptr, originalClientUnreliable = nullptr;
struct ClientContext {
    void *client;
    uint8_t *data;
    int length;
    ClientContext *previous;
};
static thread_local ClientContext *clientContext = nullptr;
static SetAvatar originalSetAvatar = nullptr;
static Disconnect originalDisconnect = nullptr;
static ThisVoid originalClose = nullptr;
static ThisVoid originalClientClose = nullptr;
static uintptr_t samBase = 0;
static SRWLOCK lock = SRWLOCK_INIT;
// Explicit ownership sits above the foreign finally frame. Preparation may
// call native getters while this metadata lock is held; GNU RAII alone does
// not release it when native MS SEH unwinds a MinGW frame.
template<bool exclusive = true, class Body> static bool withPeerLock(Body &&body) noexcept {
    bool held = false;
    return withNativeFinally([&] {
        if constexpr (exclusive)
            AcquireSRWLockExclusive(&lock);
        else
            AcquireSRWLockShared(&lock);
        held = true;
        body();
    }, [&](bool aborted) noexcept {
        if (held) {
            held = false;
            if constexpr (exclusive)
                ReleaseSRWLockExclusive(&lock);
            else
                ReleaseSRWLockShared(&lock);
        }
        if (aborted)
            nativeInputFailed();
    });
}
static void *activeServer = nullptr;
static uint32_t nextIncarnation = 0;
static uint64_t nextPresentationRevision = 0;
static uint64_t advancePresentationRevision() {
    if (nextPresentationRevision == UINT64_MAX)
        return 0; // Exhaustion cannot reuse an admitted identity.
    return ++nextPresentationRevision;
}
static bool compatiblePose(const network::PosePacket &a, const network::PosePacket &b) {
    return samePresentationIdentity(1, 1, a, 1, 1, b, true, true);
}
struct Peer : network::OrderedPosePolicy {
    uint32_t brain = 0, avatar = 0, incarnation = 0;
    uint64_t frozenTick = 0, presentationRevision = 0;
    Sample frozen;
    uint64_t rateStart = 0, lastRelay = 0;
    unsigned rateCount = 0;
    ss2vr::voice::Admission voice; // Per-source voice budget and duplicates.
    void resetForCapability(uint32_t nextBrain, uint32_t nextAvatar) noexcept {
        const bool owned = retainGameplayOwnership(brain, avatar, nextBrain, nextAvatar);
        *this = {};
        xrGameplayOwned = owned;
        brain = nextBrain;
        avatar = nextAvatar;
    }
};
static bool receivePeer(Peer &peer, const network::PosePacket &pose, uint64_t now,
                        network::Ack &discard, bool capacity = true) {
    const bool previouslyValid = peer.validation.fresh(now);
    const auto previous = peer.hasPending ? peer.pending : peer.active;
    const bool accepted = peer.receive(pose, now, discard, capacity);
    if (accepted && (!previouslyValid || !peer.presentationRevision || !compatiblePose(previous, pose)))
        peer.presentationRevision = advancePresentationRevision();
    return accepted;
}
static std::array<Peer, 18> peers;
// Diagnostics: message counts and the last relayed remote poses, summarised in
// the log every 5 s (reportDiagnostics) so a session's traffic is visible.
static std::atomic<unsigned> seenHello{0}, seenPose{0}, seenVoice{0}, seenAck{0}, seenRelay{0}, seenVoiceRelay{0},
    sentHello{0}, sentPose{0}, sentVoice{0};
static network::PosePacket lastRelayPose{};
static uint32_t lastRelayAvatar = 0;
static uint64_t tickSerial = 0;
static bool tickPrepared = false;
struct Local {
    void *net = nullptr;
    uint32_t brain = 0, avatar = 0;
    uint64_t clientNonce = 0, serverNonce = 0;
    uint64_t lastHello = 0, lastSent = 0, lastAck = 0;
    uint32_t sequence = 0;
    uint8_t lastFireMask = 0;
    network::PendingIntents pending;
    network::ConsumptionCredits credits;
    network::PosePacket latest;
    network::LocalPoseCapture capture;
    uint8_t lastGestureDownMask = 0;
    bool hasLatest = false;
    uint64_t inputSequence = 0;
    IntentInputBoundary intentBoundary[2];
};
static Local local;
// Existing Local/Pending owners only. Preserve all native consumption tokens;
// unusable coordinates revoke intent, never manufacture a release or an ACK.
static void rejectLocalCapture() {
    local.pending.requireNeutral();
    local.hasLatest = false;
    local.lastFireMask = 0;
    local.lastGestureDownMask = 0;
    network::invalidateWeaponIntents(local.latest, network::HandMask);
    for (auto &peer : peers)
        if (peer.avatar == local.avatar && peer.brain == local.brain && local.net == activeServer) {
            peer.validation.invalidateTracking();
            peer.frozen.valid = false;
            // Retire eligibility, not the already handed-off packet or token.
        }
}
// Caller holds the existing MP lock. No IPC lock is acquired on the ACK path.
static void installIntentEpochs(const uint32_t (&epochs)[2], uint64_t now) {
    for (unsigned hand = 0; hand != 2; ++hand)
        if (local.intentBoundary[hand].install(epochs[hand], local.inputSequence, now)) {
            const uint8_t bit = uint8_t(1u << hand);
            local.pending.requireNeutral(bit);
            network::invalidateWeaponIntents(local.latest, bit);
            local.latest.intentEpoch[hand] = 0; // Cached input is never restamped.
            local.lastFireMask &= uint8_t(~bit);
        }
}
static void stampInputEpochs(network::PosePacket &packet, uint64_t sequence, uint64_t tickMs, uint64_t now) {
    for (unsigned hand = 0; hand != 2; ++hand) {
        packet.intentEpoch[hand] = local.intentBoundary[hand].echo(sequence, tickMs, now);
        if (!packet.intentEpoch[hand])
            network::invalidateWeaponIntents(packet, uint8_t(1u << hand));
    }
}
struct Remote {
    uint32_t avatar = 0, incarnation = 0;
    Sample sample;
    network::ObserverGestureIntents gestures;
};
static std::array<Remote, 18> remotes;
static uint64_t nonce() {
    uint64_t result = 0;
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&result), sizeof(result),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        return 0;
    return result;
}
static uint32_t read32(void *object, size_t offset) {
    uint32_t result;
    std::memcpy(&result, static_cast<uint8_t *>(object) + offset, sizeof(result));
    return result;
}
static uint32_t brainFor(int slot) {
    // Native GameInfo owns 17 handles at +24..+64; +68 is GameStats.
    // Network bookkeeping has spare capacity, not another native player slot.
    if (slot < 0 || slot >= 17)
        return 0;
    uint32_t game = 0, brain = 0;
    getGame(&game);
    if (auto p = resolve(game))
        getBrain(p, &brain, slot);
    return brain;
}
static uint32_t avatarFor(uint32_t brain) {
    auto p = resolve(brain);
    return p ? read32(p, 0x28) : 0;
}
static int slotFor(void *player) {
    uint32_t avatar = handleOf(player);
    for (int slot = 0; slot < 18; ++slot)
        if (avatar && avatarFor(brainFor(slot)) == avatar)
            return slot;
    return -1;
}
static bool bound(void *server, int slot, uint32_t brain) {
    if (!server || slot < 0 || slot >= 18 || !brain || brainFor(slot) != brain)
        return false;
    auto entry = static_cast<uint8_t *>(server) + 0xa70 + 0xa18 * slot;
    const uint32_t avatar = avatarFor(brain), recorded = read32(entry, 0x998);
    if (int32_t(read32(entry, 0)) < 0 || !avatar || !recorded)
        return false;
    if (avatar == recorded)
        return true;
    // A level change gives the slot's brain a new avatar with no SetAvatar: the entry
    // keeps the first level's handle (V968). It is stale while no slot's brain still
    // owns it; one that does belongs to another player.
    for (int other = 0; other < 17; ++other)
        if (avatarFor(brainFor(other)) == recorded)
            return false;
    return true;
}
static int chatSelector() {
    auto descriptor = *reinterpret_cast<uint8_t **>(samBase + 0x40694c);
    return descriptor ? int(read32(descriptor, 0x54)) : -1;
}
static bool tagged(const uint8_t *data, int length) {
    return data && length >= int(2 + network::CarrierTag.size()) &&
           std::memcmp(data + 2, network::CarrierTag.data(), network::CarrierTag.size()) == 0;
}
static network::ParseResult decode(const uint8_t *data, int length, network::Message &message) {
    if (!tagged(data, length))
        return network::ParseResult::NotMod;
    uint16_t bytes;
    std::memcpy(&bytes, data, 2);
    if (length != int(bytes) + 2 || bytes > network::MaxCarrierAscii)
        return network::ParseResult::Malformed;
    return network::parse(std::string_view(reinterpret_cast<const char *>(data + 2), bytes), message);
}
static bool send(uint32_t brain, const network::Message &message, bool reliable, int slot = -1,
                 network::TransportPhase *phase = nullptr) {
    if (!brain || !currentNet())
        return false;
    const bool localSourceResolved = resolve(brain) != nullptr;
    if (!localSourceResolved)
        return false;
    if (slot >= 0 && (!isServer() || !bound(currentNet(), slot, brain)))
        return false;
    const int selector = chatSelector();
    std::string carrier;
    // Direct codec allocation failures are definitely before native transport.
    // Contain them here so callers retain/revoke their original exact token.
    try {
        if (selector < 0 || !network::encode(message, carrier))
            return false;
    } catch (...) {
        return false;
    }
    std::array<uint8_t, network::MaxCarrierAscii + 2> bytes{};
    uint16_t count = uint16_t(carrier.size());
    std::memcpy(bytes.data(), &count, 2);
    std::memcpy(bytes.data() + 2, carrier.data(), count);
    std::string{}.swap(carrier); // No C++ heap owner crosses a native call.
    uint32_t target = brain;
    if (slot < 0 && isClient()) {
        uint32_t mapped = 0;
        // Engine!ReverseMapEntityHandle (F10B0) is an sret thiscall: ECX is
        // CClientInterface, then (&serverWireHandle, localHandle), and ret 8.
        reverseMap(currentNet(), &mapped, brain);
        target = network::outboundTargetHandle(true, brain, mapped);
        // The mapped result is a server wire handle; VM ExecuteRPC_t consumes
        // it directly and it is not valid in this client's local handle table.
        if (!network::outboundHandlesValid(localSourceResolved, target))
            return false;
    }
    // Native VM SendRPC uses these exact stack objects. MemoryStream, SetData
    // and native Send each copy/clone before the corresponding destructor.
    alignas(4) std::array<uint8_t, 8> stream{};
    alignas(4) std::array<uint8_t, 0x28> rpc{};
    streamCtor(stream.data());
    createMemory(stream.data(), bytes.data(), int(count) + 2);
    (reliable ? reliableCtor : unreliableCtor)(rpc.data());
    (reliable ? reliableData : unreliableData)(rpc.data(), 0, target, selector, stream.data());
    if (phase)
        *phase = network::TransportPhase::EnteredCall;
    if (slot < 0)
        sendNative(rpc.data());
    else
        sendToNative(rpc.data(), slot);
    if (phase)
        *phase = network::TransportPhase::ReturnedCall;
    (reliable ? reliableDtor : unreliableDtor)(rpc.data());
    streamDtor(stream.data());
    return true; // Native transport cloned the complete immutable RPC.
}
static void resetLocal() {
    local = {};
    remotes = {};
}
static void resetPeer(int slot) {
    if (slot >= 0 && slot < int(peers.size()))
        peers[slot] = {};
}
static bool sendPeerAck(void *server, int slot, uint32_t brain, uint32_t incarnation,
                        const network::Ack &token) {
    network::Ack ack = token;
    ack.intentEpoch[0] = ack.intentEpoch[1] = 0;
    bool current = false;
    if (!withPeerLock<false>([&] {
        current = hooksReady.load(std::memory_order_acquire) && activeServer == server &&
                             currentNet() == server && slot >= 0 && slot < int(peers.size()) &&
                             (peers[slot].brain == brain || (!peers[slot].brain && !incarnation)) &&
                             peers[slot].incarnation == incarnation;
        if (current && peers[slot].validation.clientNonce == token.clientNonce &&
            peers[slot].validation.serverNonce == token.serverNonce &&
            peers[slot].validation.intentEpoch[0] && peers[slot].validation.intentEpoch[1]) {
            ack.intentEpoch[0] = peers[slot].validation.intentEpoch[0];
            ack.intentEpoch[1] = peers[slot].validation.intentEpoch[1];
        }
    }) || !current)
        return false;
    network::Message message{};
    message.kind = network::Kind::Ack;
    message.ack = ack;
    return send(brain, message, true, slot); // Also verifies the native slot binding.
}
static VoiceSink voiceSink = nullptr;
// The listen host's own voice names this capability incarnation (peers count up from 1).
constexpr uint32_t HostVoiceIncarnation = 0x80000001u;
struct VoiceRecipient { int slot; uint32_t brain; uint64_t clientNonce, serverNonce; };
// Caller holds the peer lock: every negotiated peer but the speaker.
static size_t voiceRecipients(uint32_t speaker, std::array<VoiceRecipient, 18> &out) {
    size_t count = 0;
    for (int slot = 0; slot < int(peers.size()); ++slot) {
        const auto &peer = peers[slot];
        if (peer.avatar && peer.avatar != speaker && peer.validation.serverNonce && peer.validation.clientNonce &&
            bound(activeServer, slot, peer.brain))
            out[count++] = {slot, peer.brain, peer.validation.clientNonce, peer.validation.serverNonce};
    }
    return count;
}
static void relayVoice(const network::VoicePacket &voice, uint32_t speaker, uint32_t incarnation,
                       const std::array<VoiceRecipient, 18> &to, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        network::Message forwarded{};
        forwarded.kind = network::Kind::VoiceRelay;
        forwarded.voiceRelay = {voice, speaker, incarnation};
        forwarded.voiceRelay.voice.clientNonce = to[i].clientNonce;
        forwarded.voiceRelay.voice.serverNonce = to[i].serverNonce;
        send(to[i].brain, forwarded, false, to[i].slot); // Unreliable: a loss is concealed.
    }
}
static bool receiveServer(void *server, void *rpc, bool reliableTransport) {
    if (!hooksReady.load(std::memory_order_acquire))
        return false;
    auto data = reinterpret_cast<uint8_t *>(uintptr_t(read32(rpc, 0x20)));
    int length = int(read32(rpc, 0x1c));
    if (!tagged(data, length))
        return false;
    network::Message message;
    auto parsed = decode(data, length, message);
    const int slot = int(read32(server, 0xc054));
    const uint32_t brain = read32(rpc, 0x14);
    if (parsed != network::ParseResult::Valid || read32(rpc, 0x24) != 0 ||
        int(read32(rpc, 0x18)) != chatSelector() || !bound(server, slot, brain)) {
        static unsigned traced = 0;
        if (traced++ < 32)
            log("Multiplayer server refused kind=%d parsed=%d slot=%d brain=%08x slotBrain=%08x bound=%d entry=%d,%08x "
                "avatar=%08x", parsed == network::ParseResult::Valid ? int(message.kind) : -1, int(parsed), slot,
                unsigned(brain), unsigned(brainFor(slot)), int(bound(server, slot, brain)),
                slot >= 0 && slot < 18 ? int(read32(static_cast<uint8_t *>(server) + 0xa70 + 0xa18 * slot, 0)) : -2,
                slot >= 0 && slot < 18 ? unsigned(read32(static_cast<uint8_t *>(server) + 0xa70 + 0xa18 * slot, 0x998)) : 0u,
                unsigned(avatarFor(brain)));
        return true;
    }
    const uint64_t now = GetTickCount64();
    network::Message ack{};
    ack.kind = network::Kind::Ack;
    // A capability replacement can retire at most the current interval and
    // one pending token. Fixed storage owns no heap across native getters.
    std::array<network::Ack, 2> discarded{};
    size_t discardCount = 0;
    uint32_t incarnation = 0;
    bool reply = false;
    std::array<VoiceRecipient, 18> voiceTo{};
    size_t voiceCount = 0;
    bool voiceAdmitted = false;
    uint32_t voiceSpeaker = 0;
    if (!withPeerLock([&] {
        if (activeServer != server) {
            peers = {};
            activeServer = server;
        }
        auto &peer = peers[slot];
        uint32_t avatar = avatarFor(brain);
        if (peer.avatar != avatar || peer.brain != brain)
            peer = {};
        if (message.kind == network::Kind::Hello) {
            ++seenHello;
            if (peer.validation.clientNonce != message.hello.nonce) {
                uint64_t serverNonce = nonce();
                if (serverNonce && nextIncarnation != UINT32_MAX) {
                    log("Multiplayer VR client negotiated slot=%d brain=%08x avatar=%08x incarnation=%u", slot, brain, avatar,
                        nextIncarnation + 1);
                    network::Ack old{};
                    if (peer.finishInterval(old))
                        discarded[discardCount++] = old;
                    if (peer.discardPending(old))
                        discarded[discardCount++] = old;
                    peer.resetForCapability(brain, avatar);
                    peer.incarnation = ++nextIncarnation;
                    peer.validation.bindCapability(message.hello.nonce, serverNonce);
                }
            }
            reply = peer.validation.clientNonce == message.hello.nonce && peer.validation.serverNonce != 0;
        } else if (message.kind == network::Kind::Pose && reliableTransport) {
            ++seenPose;
            if (now - peer.rateStart >= 1000) {
                peer.rateStart = now;
                peer.rateCount = 0;
            }
            // Discard ACKs name the submitted capability, including retired ones;
            // they return credit without reviving that capability on either peer.
            reply = !receivePeer(peer, message.pose, now, ack.ack, ++peer.rateCount <= 40);
        } else if (message.kind == network::Kind::Voice && peer.validation.serverNonce &&
                   message.voice.clientNonce == peer.validation.clientNonce &&
                   message.voice.serverNonce == peer.validation.serverNonce) {
            unsigned bytes = 30;
            for (unsigned i = 0; i < message.voice.frameCount; ++i) bytes += message.voice.frameBytes[i] + 1u;
            ++seenVoice;
            voiceAdmitted = peer.voice.admit(now, bytes, message.voice);
            if (voiceAdmitted) {
                voiceSpeaker = peer.avatar;
                voiceCount = voiceRecipients(peer.avatar, voiceTo);
            }
        }
        if (message.kind == network::Kind::Hello)
            ack.ack = {peer.validation.clientNonce, peer.validation.serverNonce, 0};
        incarnation = peer.incarnation;
    }))
        return true; // Tagged mod input failed; never forward it as native chat.
    if (voiceAdmitted) {
        relayVoice(message.voice, voiceSpeaker, incarnation, voiceTo, voiceCount);
        if (voiceSink) voiceSink(voiceSpeaker, incarnation, message.voice); // The listen host hears it.
    }
    for (size_t i = 0; i < discardCount; ++i)
        sendPeerAck(server, slot, brain, incarnation, discarded[i]);
    if (reply && !sendPeerAck(server, slot, brain, incarnation, ack.ack)) {
        static unsigned traced = 0;
        if (traced++ < 16)
            log("Multiplayer server reply failed kind=%d slot=%d brain=%08x incarnation=%u", int(message.kind), slot,
                unsigned(brain), incarnation);
    }
    return true;
}
static void dispatchServer(void *server, void *rpc, bool reliableTransport) noexcept {
    // Decode may allocate and native entity lookup can unwind. Each reentered
    // RPC boundary contains GNU errors locally; native SEH still propagates.
    withNativeFinally([&] {
        if (!receiveServer(server, rpc, reliableTransport))
            (reliableTransport ? originalReliable : originalUnreliable)(server, rpc);
    }, [](bool aborted) noexcept {
        if (aborted)
            nativeInputFailed();
    });
}
static void __fastcall reliable(void *server, void *, void *rpc) {
    dispatchServer(server, rpc, true);
}
static void __fastcall unreliable(void *server, void *, void *rpc) {
    dispatchServer(server, rpc, false);
}
static void __fastcall setAvatar(void *server, void *, uint32_t avatar, int slot) {
    {
        static unsigned traced = 0;
        if (traced++ < 32) log("Multiplayer server SetAvatar slot=%d avatar=%08x", slot, unsigned(avatar));
    }
    if (!hooksReady.load(std::memory_order_acquire)) {
        originalSetAvatar(server, avatar, slot);
        return;
    }
    AcquireSRWLockExclusive(&lock);
    resetPeer(slot);
    ReleaseSRWLockExclusive(&lock);
    originalSetAvatar(server, avatar, slot);
}
static void __fastcall disconnect(void *server, void *, int slot, const char *reason) {
    if (!hooksReady.load(std::memory_order_acquire)) {
        originalDisconnect(server, slot, reason);
        return;
    }
    AcquireSRWLockExclusive(&lock);
    resetPeer(slot);
    ReleaseSRWLockExclusive(&lock);
    originalDisconnect(server, slot, reason);
}
static void __fastcall close(void *server, void *) {
    if (!hooksReady.load(std::memory_order_acquire)) {
        originalClose(server);
        return;
    }
    AcquireSRWLockExclusive(&lock);
    peers = {};
    activeServer = nullptr;
    resetLocal();
    ReleaseSRWLockExclusive(&lock);
    originalClose(server);
}
static void __fastcall closeClient(void *client, void *) {
    if (!hooksReady.load(std::memory_order_acquire)) {
        originalClientClose(client);
        return;
    }
    AcquireSRWLockExclusive(&lock);
    resetLocal();
    ReleaseSRWLockExclusive(&lock);
    originalClientClose(client);
}
// Client wrapper has already mapped the RPC target before this virtual call.
static void executeClientBody(void *unusedThis, int h, uint32_t brain, int j, int length,
                              uint8_t *data) {
    if (!hooksReady.load(std::memory_order_acquire)) {
        originalClient(unusedThis, h, brain, j, length, data);
        return;
    }
    network::Message message;
    auto parsed = decode(data, length, message);
    if (parsed == network::ParseResult::NotMod) {
        originalClient(unusedThis, h, brain, j, length, data);
        return;
    }
    if (parsed != network::ParseResult::Valid || h || j != chatSelector() || !clientContext ||
        clientContext->data != data || clientContext->length != length)
        return;
    void *client = clientContext->client;
    withPeerLock([&] {
        if (message.kind == network::Kind::Ack &&
            !(client == local.net && brain == local.brain && avatarFor(brain) == local.avatar)) {
            static unsigned traced = 0;
            if (traced++ < 16)
                log("Multiplayer client ack refused net=%d brain=%08x local=%08x avatar=%08x local=%08x",
                    int(client == local.net), unsigned(brain), unsigned(local.brain), unsigned(avatarFor(brain)),
                    unsigned(local.avatar));
        }
        if (client == local.net && brain == local.brain && avatarFor(brain) == local.avatar) {
            if (message.kind == network::Kind::Ack) {
                ++seenAck;
                const uint64_t now = GetTickCount64();
                if (local.credits.acknowledge(message.ack, local.clientNonce, local.serverNonce,
                                             !local.serverNonce || now - local.lastAck <= 500)) {
                    if (!message.ack.intentEpoch[0] || !message.ack.intentEpoch[1]) {
                        return; // Retired/exhausted metadata can retire credit, never grant a lease.
                    }
                    if (!message.ack.acceptedSequence && local.serverNonce != message.ack.serverNonce)
                        local.pending.requireNeutral();
                    if (!message.ack.acceptedSequence) {
                        if (local.serverNonce != message.ack.serverNonce)
                            log("Multiplayer VR negotiated with the server (avatar=%08x)", local.avatar);
                        local.serverNonce = message.ack.serverNonce;
                    }
                    if (local.serverNonce == message.ack.serverNonce)
                        local.lastAck = now;
                    installIntentEpochs(message.ack.intentEpoch, now);
                }
            }
            if (message.kind == network::Kind::VoiceRelay) ++seenVoiceRelay;
            if (message.kind == network::Kind::VoiceRelay && local.serverNonce && voiceSink &&
                message.voiceRelay.voice.clientNonce == local.clientNonce &&
                message.voiceRelay.voice.serverNonce == local.serverNonce) {
                // The speaker's server handle, mapped as for pose relays.
                using MapEntity = int(__thiscall *)(void *, uint32_t, uint32_t *);
                auto table = *reinterpret_cast<void ***>(client);
                auto mapper = reinterpret_cast<MapEntity>(table[0xc8 / 4]);
                uint32_t mapped[3]{};
                if (mapper(client, message.voiceRelay.subjectAvatar, mapped) >= 0 && resolve(mapped[1]) &&
                    mapped[1] != local.avatar)
                    voiceSink(mapped[1], message.voiceRelay.subjectIncarnation, message.voiceRelay.voice);
            }
            if (message.kind == network::Kind::Relay) {
                ++seenRelay;
                lastRelayPose = message.relay.pose;
                lastRelayAvatar = message.relay.subjectAvatar;
            }
            if (message.kind == network::Kind::Relay && local.serverNonce &&
                message.relay.pose.clientNonce == local.clientNonce &&
                message.relay.pose.serverNonce == local.serverNonce) {
                // Same native mapper and stack order as ExecuteRRPC's target lookup.
                using MapEntity = int(__thiscall *)(void *, uint32_t, uint32_t *);
                auto table = *reinterpret_cast<void ***>(client);
                auto mapper = reinterpret_cast<MapEntity>(table[0xc8 / 4]);
                uint32_t mapped[3]{};
                if (mapper(client, message.relay.subjectAvatar, mapped) >= 0 && resolve(mapped[1])) {
                    bool player = false;
                    for (int slot = 0; slot < 18; ++slot)
                        player |= avatarFor(brainFor(slot)) == mapped[1];
                    if (player && mapped[1] != local.avatar) {
                        Remote *destination = nullptr;
                        for (auto &remote : remotes)
                            if (remote.avatar == mapped[1]) {
                                destination = &remote;
                                break;
                            }
                        if (!destination)
                            for (auto &remote : remotes)
                                if (!remote.avatar || GetTickCount64() - remote.sample.receivedMs > 1000) {
                                    destination = &remote;
                                    break;
                                }
                        if (destination) {
                            auto &remote = *destination;
                            const auto &relay = message.relay;
                            bool same =
                                remote.avatar == mapped[1] && remote.incarnation == relay.subjectIncarnation;
                            if ((!same && (!remote.avatar || remote.avatar != mapped[1] ||
                                           network::newer(relay.subjectIncarnation, remote.incarnation))) ||
                                (same && network::newer(relay.pose.sequence, remote.sample.pose.sequence))) {
                                if (!same) remote = {};
                                const uint64_t revision = same && remote.sample.valid &&
                                    compatiblePose(remote.sample.pose, relay.pose) ?
                                    remote.sample.presentationRevision : advancePresentationRevision();
                                remote.avatar = mapped[1];
                                remote.incarnation = relay.subjectIncarnation;
                                remote.sample = {
                                    true,      true, mapped[1], relay.subjectIncarnation, GetTickCount64(),
                                    relay.pose, {}, revision};
                            }
                            if (remote.avatar == mapped[1] && remote.incarnation == relay.subjectIncarnation)
                                remote.gestures.receive(remote.sample.pose, relay.pose, GetTickCount64());
                        }
                    }
                }
            }
        }
    });
}
static void __fastcall executeClient(void *unusedThis, void *, int h, uint32_t brain, int j, int length,
                                     uint8_t *data) {
    // Decode may allocate. Contain GNU failures in this reentered mod callback,
    // before they can cross the native ExecuteRPC frame. Native unwind remains
    // native; inner peer-lock ownership is retired by its own finally extent.
    withNativeFinally([&] {
        executeClientBody(unusedThis, h, brain, j, length, data);
    }, [](bool aborted) noexcept {
        if (aborted)
            nativeInputFailed();
    });
}
static void dispatchClient(void *client, void *rpc, Rpc original) {
    auto data = reinterpret_cast<uint8_t *>(uintptr_t(read32(rpc, 0x20)));
    int length = int(read32(rpc, 0x1c));
    ClientContext context{client, data, length, clientContext};
    // This stack record lives above the foreign finally frame. A native
    // exception must not leave TLS pointing into an unwound dispatch stack.
    withNativeFinally([&] {
        clientContext = &context;
        original(client, rpc); // Preserve native raw-to-local target remapping.
    }, [&](bool aborted) noexcept {
        clientContext = context.previous;
        if (aborted)
            nativeInputFailed();
    });
}
static void __fastcall clientReliable(void *client, void *, void *rpc) {
    dispatchClient(client, rpc, originalClientReliable);
}
static void __fastcall clientUnreliable(void *client, void *, void *rpc) {
    dispatchClient(client, rpc, originalClientUnreliable);
}
} // namespace
bool remoteClient() {
    return hooksReady.load(std::memory_order_acquire) && isClient && isClient();
}
bool server() {
    return hooksReady.load(std::memory_order_acquire) && isServer && isServer();
}
bool negotiatedLocal() {
    if (!hooksReady.load(std::memory_order_acquire) || !nativeInputHealthy())
        return false;
    bool result = false;
    withPeerLock<false>([&] {
        result = local.serverNonce && local.net == currentNet() && GetTickCount64() - local.lastAck <= 500;
    });
    return result;
}
void setVoiceSink(VoiceSink sink) { voiceSink = sink; }
bool voiceSession() {
    if (!hooksReady.load(std::memory_order_acquire) || !nativeInputHealthy() || !isServer || !isClient)
        return false;
    // A client's voice session lasts while its capability with this server
    // does (cleared on disconnect or a new capability), not only while pose
    // acknowledgements flow: cutscenes and intermissions send no poses.
    if (isClient()) {
        bool held = false;
        withPeerLock<false>([&] { held = local.serverNonce && local.clientNonce && local.net == currentNet(); });
        return held;
    }
    if (!isServer()) return false;
    bool any = false;
    withPeerLock<false>([&] {
        std::array<VoiceRecipient, 18> to{};
        any = activeServer == currentNet() && voiceRecipients(0, to) > 0;
    });
    return any;
}
bool sendVoice(void *localPlayer, network::VoicePacket voice) {
    if (!hooksReady.load(std::memory_order_acquire) || !nativeInputHealthy() || !localPlayer)
        return false;
    bool sent = false;
    withNativeFinally([&] {
        if (isClient && isClient()) {
            uint32_t brain = 0;
            withPeerLock<false>([&] {
                if (local.net == currentNet() && local.serverNonce && local.clientNonce && local.avatar == handleOf(localPlayer)) {
                    brain = local.brain;
                    voice.clientNonce = local.clientNonce;
                    voice.serverNonce = local.serverNonce;
                }
            });
            if (!brain) return;
            network::Message message{};
            message.kind = network::Kind::Voice;
            ++sentVoice;
            message.voice = voice;
            sent = send(brain, message, false);
        } else if (isServer && isServer()) {
            // The listen host: straight to every negotiated peer.
            const uint32_t speaker = handleOf(localPlayer);
            std::array<VoiceRecipient, 18> to{};
            size_t count = 0;
            withPeerLock<false>([&] {
                if (activeServer == currentNet()) count = voiceRecipients(speaker, to);
            });
            voice.clientNonce = voice.serverNonce = 1; // Replaced per recipient.
            relayVoice(voice, speaker, HostVoiceIncarnation, to, count);
            sent = count > 0;
        }
    }, [](bool aborted) noexcept {
        if (aborted) nativeInputFailed();
    });
    return sent;
}
bool knownVrAvatar(uint32_t avatar) noexcept {
    if (!avatar) return false;
    AcquireSRWLockShared(&lock);
    bool known = false;
    for (const auto &peer : peers)
        known |= peer.avatar == avatar && peer.gameplayOwned();
    ReleaseSRWLockShared(&lock);
    return known; // Local nonce/presentation replicas do not establish XR ownership.
}
struct ClientHandoff {
    network::ConsumptionToken token{};
    network::TransportPhase phase = network::TransportPhase::BeforeCall;
    bool granted = false;
};
static bool submitInput(void *player, network::PosePacket &admitted, bool reliableEdge,
            const network::LocalPoseCapture &capture, ClientHandoff &handoff) {
    auto incoming = admitted;
    const auto inputSequence = capture.frame.sequence, inputTickMs = capture.frame.tickMs;
    network::invalidateWeaponIntents(admitted, network::HandMask);
    admitted.intentEpoch[0] = admitted.intentEpoch[1] = 0;
    if (!hooksReady.load(std::memory_order_acquire) || !player || !currentNet())
        return false;
    int slot = slotFor(player);
    {
        // A client's Hello state as it changes (a level change hands it a new player).
        static uint64_t traced = 0;
        static unsigned count = 0;
        const uint64_t key = uint64_t(uint32_t(slot)) << 32 | handleOf(player);
        if (isClient() && key != traced && count < 32) {
            traced = key; ++count;
            log("Multiplayer client player=%08x slot=%d brain=%08x net=%d", unsigned(handleOf(player)), slot,
                unsigned(slot >= 0 ? brainFor(slot) : 0), int(currentNet() != nullptr));
        }
    }
    if (slot < 0)
        return false;
    uint32_t brain = brainFor(slot), avatar = handleOf(player);
    uint64_t now = GetTickCount64();
    network::Message outgoing;
    bool ready = false, retained = false;
    auto &token = handoff.token;
    uint8_t sentPulseMask = 0;
    network::PendingIntents::HandoffReceipt pulseReceipt;
    void *submittedNet = nullptr;
    bool accepted = false;
    if (!withPeerLock([&] {
        if (local.net != currentNet() || local.brain != brain || local.avatar != avatar) {
            resetLocal();
            local.net = currentNet();
            local.brain = brain;
            local.avatar = avatar;
            local.clientNonce = nonce();
        }
        const bool coordinatesReady = network::prepareLocalPose(incoming, capture, local.capture.frame, avatar, now);
        if (!coordinatesReady || (local.capture.frame.avatar &&
                                 !network::sameCaptureOwner(capture.frame, local.capture.frame)))
            rejectLocalCapture();
        if (coordinatesReady)
            local.capture = capture;
        // Hello and capability expiry must run even without usable tracking.
        // The first enabled remote-client source itself depends on that Hello.
        if (inputSequence > local.inputSequence)
            local.inputSequence = inputSequence;
        if (isServer()) {
            if (bound(currentNet(), slot, brain)) {
                activeServer = currentNet();
                auto &peer = peers[slot];
                if (peer.avatar != avatar || peer.brain != brain || !peer.validation.intentEpoch[0] ||
                    !peer.validation.intentEpoch[1]) {
                    peer.resetForCapability(brain, avatar);
                    if (nextIncarnation != UINT32_MAX)
                        peer.incarnation = ++nextIncarnation;
                    peer.validation.bindCapability(local.clientNonce, nonce());
                    local.intentBoundary[0] = local.intentBoundary[1] = {};
                }
                installIntentEpochs(peer.validation.intentEpoch, now);
                auto packet = incoming;
                packet.clientNonce = peer.validation.clientNonce;
                packet.serverNonce = peer.validation.serverNonce;
                packet.sequence = ++local.sequence;
                stampInputEpochs(packet, inputSequence, inputTickMs, now);
                if (coordinatesReady)
                    local.pending.observeCapture(packet, capture, now);
                local.pending.filterPrimaryIntents(packet);
                local.latest = packet;
                local.hasLatest = true;
                network::Ack discarded{};
                accepted = receivePeer(peer, packet, now, discarded);
                peer.validation.filterWeaponIntents(packet);
                admitted = packet;
            }
        } else if (isClient()) {
            if ((local.serverNonce && now - local.lastAck > 500) ||
                (!local.serverNonce && local.lastHello && now - local.lastHello > 500 &&
                 local.credits.hasOutstandingCapability(local.clientNonce, 0))) {
                local.credits.revokeHellos(); // Unanswered: a new Hello may go out.
                local.serverNonce = 0;
                local.lastHello = 0;
                local.lastAck = 0;
                local.sequence = 0;
                local.lastFireMask = 0;
                local.lastGestureDownMask = 0;
                local.pending = {};
                local.pending.requireNeutral();
                local.intentBoundary[0] = local.intentBoundary[1] = {};
                local.clientNonce = nonce();
            }
            if (!local.clientNonce)
                local.clientNonce = nonce();
            auto packet = incoming;
            packet.clientNonce = local.clientNonce;
            packet.serverNonce = local.serverNonce;
            stampInputEpochs(packet, inputSequence, inputTickMs, now);
            if (coordinatesReady)
                local.pending.observeCapture(packet, capture, now);
            local.pending.filterPrimaryIntents(packet);
            local.latest = packet;
            local.hasLatest = true;
            admitted = packet;
            const uint8_t pressed = packet.fireMask & ~local.lastFireMask;
            const uint8_t gesturePressed = packet.gestureDownMask & ~local.lastGestureDownMask;
            local.lastFireMask = packet.fireMask;
            local.lastGestureDownMask = packet.gestureDownMask;
            retained = coordinatesReady && reliableEdge &&
                local.pending.retain(packet, pressed, now, capture, gesturePressed);
            retained = local.hasLatest || retained;
            if (!local.serverNonce && local.clientNonce && !local.credits.full() &&
                !local.credits.hasOutstandingCapability(local.clientNonce, 0)) {
                if (!local.lastHello || now - local.lastHello >= 500) {
                    outgoing.kind = network::Kind::Hello;
                    ++sentHello;
                    outgoing.hello.nonce = local.clientNonce;
                    token = {local.clientNonce, 0, 0};
                    ready = handoff.granted = local.credits.grant(token);
                    local.lastHello = now;
                    static unsigned traced = 0;
                    if (traced++ < 32)
                        log("Multiplayer client hello brain=%08x avatar=%08x granted=%d", unsigned(brain),
                            unsigned(avatar), int(ready));
                }
            } else if (local.serverNonce && local.hasLatest && !local.credits.full() &&
                       !local.credits.hasOutstandingCapability(local.clientNonce, local.serverNonce) &&
                       local.sequence < UINT32_MAX && now - local.lastSent >= 50) {
                outgoing.kind = network::Kind::Pose;
                ++sentPose;
                outgoing.pose = local.latest;
                outgoing.pose.serverNonce = local.serverNonce;
                outgoing.pose.sequence = ++local.sequence;
                if (coordinatesReady)
                    sentPulseMask = local.pending.apply(outgoing.pose, now, local.capture);
                sentPulseMask |= outgoing.pose.gesturePulseMask;
                pulseReceipt = local.pending.handoffReceipt(sentPulseMask);
                submittedNet = local.net;
                if (!network::validatePose(outgoing.pose)) {
                    rejectLocalCapture();
                    network::invalidateWeaponIntents(admitted, network::HandMask);
                    admitted.intentEpoch[0] = admitted.intentEpoch[1] = 0;
                    retained = false;
                    return;
                }
                token = {local.clientNonce, local.serverNonce, outgoing.pose.sequence};
                ready = handoff.granted = local.credits.grant(token);
                if (ready)
                    local.lastSent = now;
            }
        }
    }))
        return false;
    if (ready) {
        accepted = send(brain, outgoing, true, -1, &handoff.phase);
        if (!accepted) {
            AcquireSRWLockExclusive(&lock);
            network::revokeUnsubmitted(local.credits, token, handoff.phase);
            handoff.granted = false;
            ReleaseSRWLockExclusive(&lock);
        }
    }
    if (accepted && sentPulseMask) {
        AcquireSRWLockExclusive(&lock);
        if (local.net == submittedNet && local.avatar == avatar &&
            local.clientNonce == token.clientNonce && local.serverNonce == token.serverNonce)
            local.pending.consume(sentPulseMask, pulseReceipt);
        ReleaseSRWLockExclusive(&lock);
    }
    return accepted || retained;
}
bool submit(void *player, network::PosePacket &admitted, bool reliableEdge,
            const network::LocalPoseCapture &capture) {
    bool result = false;
    ClientHandoff handoff; // Explicit token/phase ownership ABOVE finally.
    withNativeFinally([&] {
        if (nativeInputHealthy())
            result = submitInput(player, admitted, reliableEdge, capture, handoff);
    }, [&](bool aborted) noexcept {
        if (handoff.granted && handoff.phase == network::TransportPhase::BeforeCall) {
            AcquireSRWLockExclusive(&lock);
            network::revokeUnsubmitted(local.credits, handoff.token, handoff.phase);
            ReleaseSRWLockExclusive(&lock);
            handoff.granted = false;
        }
        if (aborted) {
            result = false;
            nativeInputFailed();
        }
        if (!nativeInputHealthy()) {
            network::invalidateWeaponIntents(admitted, network::HandMask);
            admitted.intentEpoch[0] = admitted.intentEpoch[1] = 0;
        }
    });
    return result;
}
bool localPrimaryAllowed(void *player, unsigned hand, uint32_t intentEpoch,
                         uint32_t primaryGeneration, uint64_t inputSequence, bool requireFire) {
    if (hand >= 2 || !intentEpoch || !primaryGeneration ||
        !hooksReady.load(std::memory_order_acquire) || !player || !isClient || !isClient())
        return false;
    const auto avatar = handleOf(player);
    auto *net = currentNet();
    const auto now = GetTickCount64();
    AcquireSRWLockShared(&lock);
    const bool allowed = net && local.net == net && avatar && local.avatar == avatar &&
        local.clientNonce && local.serverNonce && local.lastAck && now >= local.lastAck &&
        now - local.lastAck <= 500 && local.hasLatest && local.inputSequence == inputSequence &&
        local.intentBoundary[hand].epoch == intentEpoch &&
        local.pending.currentPrimaryIntent(local.latest, hand, intentEpoch, primaryGeneration, requireFire);
    ReleaseSRWLockShared(&lock);
    return allowed;
}
bool localGestureAllowed(void *player, unsigned hand, uint32_t intentEpoch,
                         uint32_t gestureGeneration, uint64_t gestureSequence,
                         uint64_t inputSequence, bool requireDown) {
    if (hand >= 2 || !intentEpoch || !gestureGeneration || !gestureSequence || !player ||
        !hooksReady.load(std::memory_order_acquire) || !isClient || !isClient()) return false;
    const auto avatar = handleOf(player);
    auto *net = currentNet();
    const auto now = GetTickCount64();
    AcquireSRWLockShared(&lock);
    const bool allowed = net && local.net == net && avatar && local.avatar == avatar &&
        local.clientNonce && local.serverNonce && local.lastAck && now >= local.lastAck &&
        now - local.lastAck <= 500 && local.hasLatest && local.inputSequence == inputSequence &&
        local.intentBoundary[hand].epoch == intentEpoch &&
        local.pending.currentGestureIntent(local.latest, hand, intentEpoch, gestureGeneration,
                                           gestureSequence, requireDown);
    ReleaseSRWLockShared(&lock);
    return allowed;
}
bool localZoomAllowed(void *player,unsigned hand,uint32_t intentEpoch,
                      uint32_t zoomGeneration,uint64_t inputSequence) {
    if(hand>=2 || !intentEpoch || !zoomGeneration || !player || !isClient || !isClient() ||
        !hooksReady.load(std::memory_order_acquire)) return false;
    const auto avatar=handleOf(player);
    auto *net=currentNet();const auto now=GetTickCount64();
    AcquireSRWLockShared(&lock);
    const uint8_t bit=uint8_t(1u<<hand);
    const bool allowed=net && local.net==net && avatar && local.avatar==avatar &&
        local.clientNonce && local.serverNonce && local.lastAck && now>=local.lastAck &&
        now-local.lastAck<=500 && local.hasLatest && local.inputSequence==inputSequence &&
        local.intentBoundary[hand].epoch==intentEpoch &&
        network::matchesIntentEpoch(local.latest,hand,intentEpoch) &&
        local.latest.zoomInputGeneration[hand]==zoomGeneration &&
        (local.latest.zoomSampleEligibleMask&bit) && !(local.latest.wheelOrEquipBlockedMask&bit);
    ReleaseSRWLockShared(&lock);
    return allowed;
}
static Sample freezeInput(void *player) {
    Sample result;
    uint32_t avatar = handleOf(player);
    uint64_t now = GetTickCount64();
    struct Recipient { int slot; uint32_t brain; uint64_t clientNonce, serverNonce; };
    std::array<Recipient, 18> recipients{};
    size_t recipientCount = 0;
    network::PosePacket relayPose{};
    uint32_t relayAvatar = 0, relayIncarnation = 0;
    if (!withPeerLock([&] {
        if (!tickPrepared)
            return;
        for (auto &peer : peers) {
            if (peer.avatar != avatar || !peer.validation.serverNonce)
                continue;
            if (tickPrepared && peer.frozenTick == tickSerial) {
                result = peer.frozen;
                break;
            }
            result.negotiated = peer.gameplayOwned();
            result.avatar = avatar;
            result.incarnation = peer.incarnation;
            result.valid = peer.freeze(now, result.pose, &relayPose);
            result.receivedMs = peer.activeReceivedMs;
            result.presentationRevision = peer.presentationRevision;
            peer.frozen = result;
            if (tickPrepared)
                peer.frozenTick = tickSerial;
            if (result.valid && (relayPose.pulseMask || relayPose.gesturePulseMask ||
                                 !peer.lastRelay || now - peer.lastRelay >= 50)) {
                peer.lastRelay = now;
                relayAvatar = peer.avatar;
                relayIncarnation = peer.incarnation;
                for (int destination = 0; destination < int(peers.size()); ++destination) {
                    auto &recipient = peers[destination];
                    if (recipient.avatar != avatar && recipient.relayRecipientReady(now) &&
                        bound(activeServer, destination, recipient.brain))
                        recipients[recipientCount++] = {destination, recipient.brain,
                            recipient.validation.clientNonce, recipient.validation.serverNonce};
                }
            }
            break;
        }
    }))
        return {};
    for (size_t index = 0; index < recipientCount; ++index) {
        const auto &recipient = recipients[index];
        network::Message forwarded{};
        forwarded.kind = network::Kind::Relay;
        forwarded.relay = {relayPose, relayAvatar, relayIncarnation};
        forwarded.relay.pose.clientNonce = recipient.clientNonce;
        forwarded.relay.pose.serverNonce = recipient.serverNonce;
        send(recipient.brain, forwarded, (relayPose.pulseMask | relayPose.gesturePulseMask) != 0, recipient.slot);
    }
    return result;
}
Sample freeze(void *player) {
    Sample result;
    withNativeFinally([&] {
        if (nativeInputHealthy())
            result = freezeInput(player);
    }, [&](bool aborted) noexcept {
        if (aborted) {
            result = {};
            nativeInputFailed();
        }
    });
    return result;
}
std::array<void *, 18> activePlayers() {
    std::array<void *, 18> result{};
    if (!hooksReady.load(std::memory_order_acquire))
        return result;
    for (int slot = 0; slot < int(result.size()); ++slot)
        if (uint32_t brain = brainFor(slot))
            if (uint32_t avatar = avatarFor(brain))
                result[slot] = resolve(avatar);
    return result;
}
void reportDiagnostics() noexcept {
    static uint64_t next = 0;
    const uint64_t now = GetTickCount64();
    if (now < next || !hooksReady.load(std::memory_order_acquire)) return;
    next = now + 5000;
    const bool isSrv = isServer && isServer(), isCli = isClient && isClient();
    if (!isSrv && !isCli) return;
    unsigned negotiated = 0;
    for (const auto &peer : peers) negotiated += peer.validation.serverNonce != 0;
    const auto &r = lastRelayPose;
    log("Multiplayer %s: in hello=%u pose=%u voice=%u ack=%u relay=%u voiceRelay=%u out hello=%u pose=%u voice=%u "
        "peers=%u negotiated=%d relayAvatar=%08x head=%.2f,%.2f,%.2f grip0=%.2f,%.2f,%.2f grip1=%.2f,%.2f,%.2f",
        isSrv ? "server" : "client", seenHello.load(), seenPose.load(), seenVoice.load(), seenAck.load(), seenRelay.load(),
        seenVoiceRelay.load(), sentHello.load(), sentPose.load(), sentVoice.load(), negotiated, int(local.serverNonce != 0),
        lastRelayAvatar, double(r.head.p.x), double(r.head.p.y), double(r.head.p.z), double(r.grip[0].p.x),
        double(r.grip[0].p.y), double(r.grip[0].p.z), double(r.grip[1].p.x), double(r.grip[1].p.y), double(r.grip[1].p.z));
}
bool beginTick() {
    if (!hooksReady.load(std::memory_order_acquire) || !isServer || !isServer())
        return false;
    AcquireSRWLockExclusive(&lock);
    if (tickPrepared) {
        ReleaseSRWLockExclusive(&lock);
        return false;
    }
    if (tickSerial == UINT64_MAX) {
        tickSerial = 1;
        for (auto &peer : peers)
            peer.frozenTick = 0;
    } else {
        ++tickSerial;
    }
    // An interrupted active slot still owns its exact discard token. Stage it
    // in this normal interval even if its avatar is no longer enumerated.
    for (auto &peer : peers)
        if (peer.awaitingConsumption && !peer.hasActive)
            peer.frozenTick = tickSerial;
    tickPrepared = true;
    ReleaseSRWLockExclusive(&lock);
    return true;
}
namespace {
struct PeerAckOwner { int slot; uint32_t brain, incarnation; network::Ack ack; };
static void abortPeerInterval(Peer &peer) noexcept {
    if (peer.hasActive)
        peer.abortInterval();
    else
        peer.validation.invalidateTracking();
    peer.frozen.valid = false;
    network::invalidateWeaponIntents(peer.frozen.pose, network::HandMask);
    network::cancelWeaponRequests(peer.frozen.pose, network::HandMask);
    peer.presentationRevision = advancePresentationRevision();
    if (local.avatar == peer.avatar && local.net == activeServer)
        installIntentEpochs(peer.validation.intentEpoch, GetTickCount64());
}
// Called only with captures from an actual slot. Exact token/incarnation checks
// prevent a native send's reentry/replacement from retiring another interval.
static void settlePeerAck(const PeerAckOwner &entry, bool submitted) noexcept {
    AcquireSRWLockExclusive(&lock);
    auto &peer = peers[entry.slot];
    network::Ack live{};
    if (peer.brain == entry.brain && peer.incarnation == entry.incarnation &&
        peer.peekInterval(live) && live.clientNonce == entry.ack.clientNonce &&
        live.serverNonce == entry.ack.serverNonce && live.acceptedSequence == entry.ack.acceptedSequence) {
        if (submitted)
            peer.confirmInterval(entry.ack);
        else
            abortPeerInterval(peer);
    }
    ReleaseSRWLockExclusive(&lock);
}
} // namespace
void completeTick() {
    std::array<PeerAckOwner, 18> completed{};
    size_t count = 0, next = 0;
    void *server = nullptr;
    withNativeFinally([&] {
        if (!withPeerLock([&] {
            if (!tickPrepared)
                return;
            for (int index = 0; index < int(peers.size()); ++index) {
                auto &peer = peers[index];
                if (!peer.awaitingConsumption || peer.frozenTick != tickSerial)
                    continue;
                network::Ack ack{};
                if (peer.peekInterval(ack))
                    completed[count++] = {index, peer.brain, peer.incarnation, ack};
            }
            server = activeServer;
            tickPrepared = false;
        }))
            return;
        for (; next < count; ++next) {
            const auto &entry = completed[next];
            const bool submitted = sendPeerAck(server, entry.slot, entry.brain, entry.incarnation, entry.ack);
            settlePeerAck(entry, submitted);
        }
    }, [&](bool aborted) noexcept {
        if (aborted)
            nativeInputFailed();
        // Slots retain every token until confirmed handoff. Ambiguous/unsent
        // ACKs are safe to retry through that same inactive slot; duplicates
        // cannot refresh a client lease or return another token's credit.
        for (; next < count; ++next)
            settlePeerAck(completed[next], false);
    });
}
void abortTick() noexcept {
    AcquireSRWLockExclusive(&lock);
    if (tickPrepared) {
        for (auto &peer : peers)
            if (peer.frozenTick == tickSerial)
                abortPeerInterval(peer);
        tickPrepared = false;
    }
    ReleaseSRWLockExclusive(&lock);
}
Sample authority(void *player) {
    Sample result;
    uint32_t avatar = handleOf(player);
    AcquireSRWLockShared(&lock);
    for (auto &peer : peers)
        if (peer.avatar == avatar && peer.validation.serverNonce) {
            result = peer.frozen;
            // A transport nonce alone cannot take over desktop gameplay. An
            // admitted XR owner remains recognized through tracking loss.
            result.avatar = peer.avatar;
            result.incarnation = peer.incarnation;
            result.negotiated = peer.gameplayOwned();
            result.liveIntentEpoch[0] = peer.validation.intentEpoch[0];
            result.liveIntentEpoch[1] = peer.validation.intentEpoch[1];
            peer.validation.filterWeaponIntents(result.pose);
            if (GetTickCount64() - result.receivedMs > network::MaxPoseAgeMs)
                result.valid = false;
            break;
        }
    ReleaseSRWLockShared(&lock);
    return result;
}
Sample presentation(void *player) {
    if (server())
        return authority(player);
    Sample result;
    uint32_t avatar = handleOf(player);
    AcquireSRWLockShared(&lock);
    for (const auto &remote : remotes)
        if (remote.avatar == avatar) {
            result = remote.sample;
            if (GetTickCount64() - result.receivedMs > network::MaxPoseAgeMs)
                result.valid = false;
            break;
        }
    ReleaseSRWLockShared(&lock);
    return result;
}
ObserverGestureSample observerGestures(uint32_t avatar) {
    ObserverGestureSample result;
    if (server()) return result;
    const auto now = GetTickCount64();
    void *net = currentNet();
    AcquireSRWLockExclusive(&lock);
    if (local.net && local.net == net && local.clientNonce && local.serverNonce)
        for (auto &remote : remotes)
            if (remote.avatar == avatar && remote.sample.valid &&
                remote.sample.pose.clientNonce == local.clientNonce &&
                remote.sample.pose.serverNonce == local.serverNonce &&
                now >= remote.sample.receivedMs && now - remote.sample.receivedMs <= network::MaxPoseAgeMs) {
                result.latest = remote.sample;
                for (unsigned h = 0; h < 2; ++h)
                    remote.gestures.sample(remote.sample.pose, h, now, result.pulse[h], result.receipt[h]);
                break;
            }
    ReleaseSRWLockExclusive(&lock);
    return result;
}
bool observerGesturesCurrent(const ObserverGestureSample &captured, unsigned hand) {
    if (hand >= 2 || !captured.latest.valid) return false;
    const auto live = observerGestures(captured.latest.avatar);
    const auto &a = captured.latest; const auto &b = live.latest;
    if (!b.valid || a.incarnation != b.incarnation || a.presentationRevision != b.presentationRevision ||
        a.pose.sequence != b.pose.sequence || a.pose.clientNonce != b.pose.clientNonce ||
        a.pose.serverNonce != b.pose.serverNonce || a.pose.trackingGeneration != b.pose.trackingGeneration ||
        a.pose.intentEpoch[hand] != b.pose.intentEpoch[hand] ||
        a.pose.nativeWeaponId[hand] != b.pose.nativeWeaponId[hand]) return false;
    if (captured.receipt[hand].hand != hand) return true; // Current held latest, no retained edge.
    const auto now = GetTickCount64();
    bool result = false;
    AcquireSRWLockShared(&lock);
    for (const auto &remote : remotes)
        if (remote.avatar == a.avatar && remote.incarnation == a.incarnation &&
            remote.sample.pose.sequence == a.pose.sequence &&
            remote.sample.presentationRevision == a.presentationRevision &&
            remote.sample.pose.clientNonce == local.clientNonce &&
            remote.sample.pose.serverNonce == local.serverNonce) {
            result = remote.gestures.current(remote.sample.pose, captured.receipt[hand], now); break;
        }
    ReleaseSRWLockShared(&lock);
    return result;
}
bool finishObserverGesture(const ObserverGestureSample &captured, unsigned hand) {
    if (hand >= 2 || captured.receipt[hand].hand != hand) return false;
    bool result = false;
    AcquireSRWLockExclusive(&lock);
    if (local.clientNonce == captured.latest.pose.clientNonce &&
        local.serverNonce == captured.latest.pose.serverNonce)
        for (auto &remote : remotes)
            if (remote.avatar == captured.latest.avatar && remote.incarnation == captured.latest.incarnation &&
                remote.sample.pose.clientNonce == local.clientNonce &&
                remote.sample.pose.serverNonce == local.serverNonce) {
                result = remote.gestures.finish(captured.receipt[hand]); break;
            }
    ReleaseSRWLockExclusive(&lock);
    return result;
}
PresentationReadGuard::PresentationReadGuard(bool acquireNow) : server_(server()) {
    if (acquireNow) acquire();
}
void PresentationReadGuard::acquire() noexcept {
    if (!held_) {
        AcquireSRWLockShared(&lock);
        held_ = true;
    }
}
PresentationReadGuard::~PresentationReadGuard() {
    release();
}
void PresentationReadGuard::release() noexcept {
    if (held_) {
        held_ = false;
        ReleaseSRWLockShared(&lock);
    }
}
Sample PresentationReadGuard::sample(uint32_t avatar) const {
    if (server_) {
        for (const auto &peer : peers)
            if (peer.avatar == avatar && peer.validation.serverNonce) {
                Sample result = peer.frozen;
                result.negotiated = peer.gameplayOwned();
                result.valid = result.valid &&
                    validFrozenPresentationRevision(result.presentationRevision, peer.presentationRevision);
                return result;
            }
    } else if (local.net && local.net == currentNet() && local.clientNonce && local.serverNonce) {
        for (const auto &remote : remotes)
            if (remote.avatar == avatar && remote.sample.pose.clientNonce == local.clientNonce &&
                remote.sample.pose.serverNonce == local.serverNonce)
                return remote.sample;
    }
    return {};
}
void invalidatePlayer(void *player) {
    std::array<PeerAckOwner, 18> discarded{};
    size_t count = 0, next = 0;
    void *server = nullptr;
    withNativeFinally([&] {
        const uint32_t avatar = handleOf(player);
        if (!withPeerLock([&] {
            for (int slot = 0; slot < int(peers.size()); ++slot) {
                auto &peer = peers[slot];
                if (peer.avatar != avatar)
                    continue;
                // An outstanding active token and a pending token are mutually
                // exclusive in OrderedPosePolicy. Move pending into that SAME
                // slot for explicit discard, without executing native gameplay.
                if (peer.hasPending && !peer.awaitingConsumption) {
                    network::PosePacket unused;
                    peer.freeze(GetTickCount64(), unused);
                }
                abortPeerInterval(peer);
                network::Ack ack{};
                if (peer.peekInterval(ack) && peer.brain) {
                    discarded[count++] = {slot, peer.brain, peer.incarnation, ack};
                    if (tickPrepared)
                        peer.frozenTick = tickSerial;
                }
            }
            for (auto &remote : remotes)
                if (remote.avatar == avatar)
                    remote = {};
            if (local.avatar == avatar)
                resetLocal();
            server = activeServer;
        }))
            return;
        for (; next < count; ++next) {
            const auto &entry = discarded[next];
            const bool submitted = sendPeerAck(server, entry.slot, entry.brain, entry.incarnation, entry.ack);
            settlePeerAck(entry, submitted);
        }
    }, [&](bool aborted) noexcept {
        if (aborted)
            nativeInputFailed();
        for (; next < count; ++next)
            settlePeerAck(discarded[next], false);
    });
}
void invalidateWeapons(void *player, uint32_t generation, uint8_t handMask) {
    uint32_t avatar = handleOf(player);
    AcquireSRWLockExclusive(&lock);
    for (auto &remote : remotes)
        if (remote.avatar == avatar) {
            remote.gestures.cancel(handMask);
            network::invalidateWeaponIntents(remote.sample.pose, handMask);
            remote.sample.presentationRevision = advancePresentationRevision();
        }
    for (auto &peer : peers)
        if (peer.avatar == avatar) {
            peer.validation.invalidateWeaponGeneration(generation, handMask);
            if (local.avatar == avatar && local.net == activeServer)
                installIntentEpochs(peer.validation.intentEpoch, GetTickCount64());
            peer.presentationRevision = advancePresentationRevision();
            network::invalidateWeaponIntents(peer.frozen.pose, handMask);
            network::invalidateWeaponIntents(peer.pending, handMask);
            network::invalidateWeaponIntents(peer.active, handMask);
            network::cancelWeaponRequests(peer.frozen.pose, handMask);
            network::cancelWeaponRequests(peer.pending, handMask);
            network::cancelWeaponRequests(peer.active, handMask);
        }
    ReleaseSRWLockExclusive(&lock);
}
bool initialize(HMODULE engine, HMODULE core, HMODULE sam, HookInstaller install) {
    samBase = reinterpret_cast<uintptr_t>(sam);
    bool ok = true;
#define S(m, name, out)                                                                                      \
    out = loadProc<decltype(out)>(m, name);                                                                  \
    ok = bool(out) && ok
    S(core, "?hvHandleToPointer@SeriousEngine@@YAPAXK@Z", resolve);
    S(core, "?hvPointerToHandle@SeriousEngine@@YAKPAX@Z", handleOf);
    S(core, "??0CStream@SeriousEngine@@QAE@XZ", streamCtor);
    S(core, "??1CStream@SeriousEngine@@QAE@XZ", streamDtor);
    S(core, "?CreateMemoryStream_t@CStream@SeriousEngine@@QAEXPBXJ@Z", createMemory);
    S(engine, "?netIsClient@SeriousEngine@@YAHXZ", isClient);
    S(engine, "?netIsServer@SeriousEngine@@YAHXZ", isServer);
    S(engine, "?netGetCurrent@SeriousEngine@@YAPAVCNetworkInterface@1@XZ", currentNet);
    S(engine, "?netSendMessage@SeriousEngine@@YAXAAVCNetworkMessage@1@@Z", sendNative);
    S(engine, "?netSendMessageTo@SeriousEngine@@YAXAAVCNetworkMessage@1@J@Z", sendToNative);
    S(engine,
      "?ReverseMapEntityHandle@CClientInterface@SeriousEngine@@QAE?AV?$Handle@VCEntity@SeriousEngine@@@2@V32@@Z",
      reverseMap);
    S(sam, "?samGetGameInfoEntity@SeriousEngine@@YA?AV?$Handle@VCGameInfoEntity@SeriousEngine@@@1@XZ",
      getGame);
    S(sam,
      "?GetPlayerBrain@CGameInfoEntity@SeriousEngine@@UAE?AV?$Handle@VCPlayerBrainEntity@SeriousEngine@@@2@J@"
      "Z",
      getBrain);
    S(engine, "??0CNMReliableRPC@SeriousEngine@@QAE@XZ", reliableCtor);
    S(engine, "??1CNMReliableRPC@SeriousEngine@@UAE@XZ", reliableDtor);
    S(engine, "??0CNMUnreliableRPC@SeriousEngine@@QAE@XZ", unreliableCtor);
    S(engine, "??1CNMUnreliableRPC@SeriousEngine@@UAE@XZ", unreliableDtor);
    S(engine,
      "?SetData@CNMReliableRPC@SeriousEngine@@QAEXHV?$Handle@VCEntity@SeriousEngine@@@2@JAAVCStream@2@@Z",
      reliableData);
    S(engine,
      "?SetData@CNMUnreliableRPC@SeriousEngine@@QAEXHV?$Handle@VCEntity@SeriousEngine@@@2@JAAVCStream@2@@Z",
      unreliableData);
#undef S
    if (!ok)
        return false;
#define H(name, fn, original)                                                                                \
    ok = install(engine, name, reinterpret_cast<void *>(fn), reinterpret_cast<void **>(&original)) && ok
    H("?ExecuteRRPC@CServerInterface@SeriousEngine@@AAEXPAVCNMReliableRPC@2@@Z", reliable, originalReliable);
    H("?ExecuteURPC@CServerInterface@SeriousEngine@@AAEXPAVCNMUnreliableRPC@2@@Z", unreliable,
      originalUnreliable);
    H("?ExecuteRPC@CClientInterface@SeriousEngine@@MAEXHV?$Handle@VCEntity@SeriousEngine@@@2@JJPAE@Z",
      executeClient, originalClient);
    H("?ExecuteRRPC@CClientInterface@SeriousEngine@@MAEXPAVCNMReliableRPC@2@@Z", clientReliable,
      originalClientReliable);
    H("?ExecuteURPC@CClientInterface@SeriousEngine@@MAEXPAVCNMUnreliableRPC@2@@Z", clientUnreliable,
      originalClientUnreliable);
    H("?SetAvatar@CServerInterface@SeriousEngine@@UAEXV?$Handle@VCEntity@SeriousEngine@@@2@J@Z", setAvatar,
      originalSetAvatar);
    H("?DisconnectClient@CServerInterface@SeriousEngine@@QAEXJPBD@Z", disconnect, originalDisconnect);
    H("?Close@CServerInterface@SeriousEngine@@UAEXXZ", close, originalClose);
    H("?Close@CClientInterface@SeriousEngine@@UAEXXZ", closeClient, originalClientClose);
#undef H
    return ok;
}
} // namespace ss2vr::game::multiplayer
