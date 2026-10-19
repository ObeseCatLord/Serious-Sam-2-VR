#pragma once

// Portable SS2 VR multiplayer payloads.  The native adapter owns CString framing,
// Engine RPC delivery, player lifecycle, inventory, cadence, and server authority.
#include "math.hpp"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace ss2vr::network {

constexpr std::string_view CarrierTag = "~SS2VR1~";
constexpr size_t MaxCarrierAscii = 512;
constexpr uint8_t WireVersion = 7;
constexpr uint8_t HandMask = 0x03;
constexpr uint8_t PoseValidMask = 0x07; // bit 0: head, bits 1/2: left/right grip.
constexpr uint64_t MaxPoseAgeMs = 200;

enum class Kind : uint8_t { Hello = 1, Ack = 2, Pose = 3, Relay = 4, Voice = 5, VoiceRelay = 6 };
enum class ParseResult : uint8_t { NotMod, Malformed, Valid };

struct Hello {
    uint64_t nonce = 0;
};
struct Ack {
    uint64_t clientNonce = 0;
    uint64_t serverNonce = 0;
    uint32_t acceptedSequence = 0;
    // Metadata only; nonce/sequence remain the exact consumption-credit token.
    // Zero epochs on a retired capability return credit without granting intent.
    uint32_t intentEpoch[2]{};
};
// All positions are metres relative to the player's body.  Grip orientations are
// runtime aim orientations; their positions are the calibrated physical grips.
struct PosePacket {
    uint64_t clientNonce = 0;
    uint64_t serverNonce = 0;
    uint32_t sequence = 0;
    uint32_t trackingGeneration = 0;
    Pose head{};
    Pose grip[2]{};
    uint8_t validMask = 0;
    uint8_t physicalDownMask = 0;
    uint8_t fireMask = 0;
    // A completed press/release can be represented for one native simulation
    // interval without turning the released level into a held fire state.
    uint8_t pulseMask = 0;
    uint8_t wheelOrEquipBlockedMask = 0;
    // Current held zoom intent, separate from historical primary-fire pulses.
    uint8_t zoomMask = 0;
    uint8_t pulseZoomMask = 0;
    // Raw action state is distinct from filtered intent.  In particular, an
    // inactive OpenXR action cannot be mistaken for an up/neutral sample.
    uint8_t zoomPhysicalDownMask = 0, zoomSampleEligibleMask = 0, primarySampleEligibleMask = 0;
    // This sample was actually observed below the raw release threshold. A
    // clear hysteresis latch or cumulative release serial alone is insufficient.
    uint8_t primaryNeutralSampleMask = 0;
    uint32_t intentEpoch[2]{};
    uint32_t releasedSerial[2]{}, primaryInputGeneration[2]{};
    uint32_t zoomReleasedSerial[2]{}, zoomInputGeneration[2]{};
    // Canonical 0..16 weapon IDs only.  These are never native weapon handles.
    int16_t requestedWeapon[2]{-1, -1};
    int16_t nativeWeaponId[2]{-1, -1};
    // Physical gesture observations are never manual trigger/history evidence.
    // Quiet identifies a persistent witness in this stream, not a repeat bit.
    uint8_t gestureEligibleMask = 0, gestureDownMask = 0, gestureQuietMask = 0, gesturePulseMask = 0;
    uint32_t gestureGeneration[2]{};
    uint64_t gestureSequence[2]{}, gestureTickMs[2]{};
    uint64_t gestureQuietSequence[2]{}, gestureQuietTickMs[2]{};
};
inline uint8_t intervalZoomMask(const PosePacket &pose) {
    // An UNZOOMED historical shot must override a newer zoomed held level too.
    return uint8_t((pose.zoomMask & ~pose.pulseMask) | pose.pulseZoomMask);
}
inline bool matchesIntentEpoch(const PosePacket &pose, unsigned hand, uint32_t liveEpoch) {
    return hand < 2 && liveEpoch && pose.intentEpoch[hand] == liveEpoch;
}
inline bool currentIntentSample(const PosePacket &captured, const PosePacket &live,
                                unsigned hand, uint32_t liveEpoch) {
    return matchesIntentEpoch(captured, hand, liveEpoch) && matchesIntentEpoch(live, hand, liveEpoch) &&
           captured.clientNonce == live.clientNonce && captured.serverNonce == live.serverNonce &&
           captured.sequence == live.sequence && captured.trackingGeneration == live.trackingGeneration;
}
inline void invalidateGestureIntents(PosePacket &pose, uint8_t hands) {
    const auto keep = uint8_t(~(hands & HandMask));
    pose.gestureEligibleMask &= keep; pose.gestureDownMask &= keep;
    pose.gestureQuietMask &= keep; pose.gesturePulseMask &= keep;
    for (unsigned hand = 0; hand < 2; ++hand)
        if (hands & (1u << hand)) {
            pose.gestureGeneration[hand] = 0;
            pose.gestureSequence[hand] = pose.gestureTickMs[hand] = 0;
            pose.gestureQuietSequence[hand] = pose.gestureQuietTickMs[hand] = 0;
        }
}
inline void invalidateWeaponIntents(PosePacket &pose, uint8_t hands) {
    const auto keep = uint8_t(~(hands & HandMask));
    pose.fireMask &= keep;
    pose.pulseMask &= keep;
    pose.zoomMask &= keep;
    pose.pulseZoomMask &= keep;
    invalidateGestureIntents(pose, hands);
    // Physical held/release witnesses stay native input, not synthetic releases.
}
inline void cancelWeaponRequests(PosePacket &pose, uint8_t hands) {
    for (unsigned hand = 0; hand < 2; ++hand)
        if (hands & (1u << hand))
            pose.requestedWeapon[hand] = -1;
}
struct Relay {
    PosePacket pose{};
    uint32_t subjectAvatar = 0;
    uint32_t subjectIncarnation = 0;
};
// Voice chat: Opus frames of one talk spurt. A client sends its own to the
// server (Voice); the server forwards each to every other negotiated mod peer
// (VoiceRelay, naming the speaker's avatar). Nonces bind both to the sender's
// or recipient's negotiated capability, as for poses. Unreliable transport:
// a lost packet is concealed by the receiver's decoder.
constexpr unsigned MaxVoiceFrames = 4, MaxVoiceFrameBytes = 120;
// START: the first packet of a talk spurt; END: its last (may carry no frames).
constexpr uint8_t VoiceStart = 1, VoiceEnd = 2;
struct VoicePacket {
    uint64_t clientNonce = 0, serverNonce = 0;
    uint32_t firstFrame = 0; // Sender's 20 ms frame counter of frames[0]; frame i is firstFrame + i.
    uint8_t talkspurt = 0;   // Per talk spurt, never 0.
    uint8_t flags = 0, frameCount = 0;
    uint8_t frameBytes[MaxVoiceFrames]{};
    uint8_t frames[MaxVoiceFrames][MaxVoiceFrameBytes]{};
};
struct VoiceRelay {
    VoicePacket voice{};
    uint32_t subjectAvatar = 0;
    uint32_t subjectIncarnation = 0;
};
inline bool validateVoice(const VoicePacket &v) {
    if (!v.clientNonce || !v.serverNonce || !v.talkspurt || v.frameCount > MaxVoiceFrames ||
        (v.flags & ~(VoiceStart | VoiceEnd)))
        return false;
    if (!v.frameCount && !(v.flags & VoiceEnd))
        return false;
    for (unsigned i = 0; i < v.frameCount; ++i)
        if (!v.frameBytes[i] || v.frameBytes[i] > MaxVoiceFrameBytes)
            return false;
    return true;
}
struct Message {
    Kind kind = Kind::Hello;
    Hello hello{};
    Ack ack{};
    PosePacket pose{};
    Relay relay{};
    VoicePacket voice{};
    VoiceRelay voiceRelay{};
};

inline bool validWeaponId(int16_t id) {
    return id >= -1 && id <= 16 && id != 14;
}
inline bool finite(float f) {
    return std::isfinite(f);
}
inline bool finitePose(const Pose &p) {
    return finite(p.p.x) && finite(p.p.y) && finite(p.p.z) && finite(p.q.x) && finite(p.q.y) &&
           finite(p.q.z) && finite(p.q.w);
}
inline bool normalizeWirePose(Pose &p, float maximumTranslation) {
    if (!finitePose(p))
        return false;
    const float translation2 = p.p.x * p.p.x + p.p.y * p.p.y + p.p.z * p.p.z;
    if (!finite(translation2) || translation2 > maximumTranslation * maximumTranslation)
        return false;
    const float length2 = p.q.x * p.q.x + p.q.y * p.q.y + p.q.z * p.q.z + p.q.w * p.q.w;
    if (!finite(length2))
        return false;
    const float length = std::sqrt(length2);
    // Permit normal floating point drift, but do not turn arbitrary data into a pose.
    if (length < .95f || length > 1.05f)
        return false;
    p.q.x /= length;
    p.q.y /= length;
    p.q.z /= length;
    p.q.w /= length;
    return true;
}
// Shared by the wire validator and local raw-capture composition. This does not
// validate transport identity or grant input; it only checks the existing volume.
inline bool validatePoseCoordinates(PosePacket &p) {
    if (!normalizeWirePose(p.head, MaximumHeadTranslation + TrackingBoundsSlack) ||
        !normalizeWirePose(p.grip[0], MaximumHandTranslation + TrackingBoundsSlack) ||
        !normalizeWirePose(p.grip[1], MaximumHandTranslation + TrackingBoundsSlack) ||
        !headTranslationValid(p.head.p))
        return false;
    for (unsigned hand = 0; hand != 2; ++hand)
        if ((p.validMask & (2u << hand)) ? !handTranslationInVolume(p.grip[hand].p)
                                       : !handTranslationValid(p.head.p, p.grip[hand].p))
            return false;
    return true;
}
inline bool validGestureHand(const PosePacket &p, unsigned hand) {
    if (hand >= 2) return false;
    const uint8_t bit = uint8_t(1u << hand);
    const bool eligible = (p.gestureEligibleMask & bit) != 0, quiet = (p.gestureQuietMask & bit) != 0;
    if (!eligible)
        return !((p.gestureDownMask | p.gestureQuietMask | p.gesturePulseMask) & bit) &&
            !p.gestureGeneration[hand] && !p.gestureSequence[hand] && !p.gestureTickMs[hand] &&
            !p.gestureQuietSequence[hand] && !p.gestureQuietTickMs[hand];
    if (!(p.validMask & 1) || !(p.validMask & (bit << 1)) || (p.wheelOrEquipBlockedMask & bit) ||
        p.nativeWeaponId[hand] != 0 || p.requestedWeapon[hand] >= 0 ||
        !p.gestureGeneration[hand] || !p.gestureSequence[hand] || !p.gestureTickMs[hand]) return false;
    if (!quiet) return !p.gestureQuietSequence[hand] && !p.gestureQuietTickMs[hand];
    return p.gestureQuietSequence[hand] && p.gestureQuietTickMs[hand] &&
        p.gestureQuietSequence[hand] <= p.gestureSequence[hand] &&
        p.gestureQuietTickMs[hand] <= p.gestureTickMs[hand] &&
        ((p.gestureQuietSequence[hand] == p.gestureSequence[hand]) ==
         (p.gestureQuietTickMs[hand] == p.gestureTickMs[hand])) &&
        (!(p.gestureDownMask & bit) || p.gestureQuietSequence[hand] < p.gestureSequence[hand]);
}
inline bool validatePose(PosePacket &p) {
    if (!p.clientNonce || !p.serverNonce || !p.sequence || !p.trackingGeneration ||
        (p.validMask & ~PoseValidMask) || (p.physicalDownMask & ~HandMask) || (p.fireMask & ~HandMask) ||
        (p.pulseMask & ~HandMask) || (p.wheelOrEquipBlockedMask & ~HandMask) ||
        (p.zoomMask & ~HandMask) || (p.pulseZoomMask & ~p.pulseMask) ||
        (p.zoomPhysicalDownMask & ~HandMask) || (p.zoomSampleEligibleMask & ~HandMask) ||
        (p.primarySampleEligibleMask & ~HandMask) || (p.primaryNeutralSampleMask & ~HandMask) ||
        (p.primaryNeutralSampleMask & ~p.primarySampleEligibleMask) ||
        (p.primaryNeutralSampleMask & p.physicalDownMask) ||
        ((p.gestureEligibleMask | p.gestureDownMask | p.gestureQuietMask | p.gesturePulseMask) & ~HandMask))
        return false;
    if ((p.fireMask && !(p.validMask & 1)) || (p.fireMask & ~(p.validMask >> 1)) ||
        (p.fireMask & ~p.physicalDownMask) || (p.fireMask & p.wheelOrEquipBlockedMask))
        return false;
    if ((p.pulseMask && !(p.validMask & 1)) || (p.pulseMask & ~(p.validMask >> 1)) ||
        (p.pulseMask & p.wheelOrEquipBlockedMask))
        return false;
    if ((p.zoomMask && !(p.validMask & 1)) || (p.zoomMask & ~(p.validMask >> 1)) ||
        (p.zoomMask & p.wheelOrEquipBlockedMask) ||
        (p.zoomMask & ~(p.zoomPhysicalDownMask & p.zoomSampleEligibleMask)))
        return false;
    if (!validatePoseCoordinates(p))
        return false;
    for (unsigned hand = 0; hand != 2; ++hand) {
        if (!validGestureHand(p, hand)) return false;
        if (!validWeaponId(p.requestedWeapon[hand]) || !validWeaponId(p.nativeWeaponId[hand]))
            return false;
        if (((p.zoomMask | p.pulseZoomMask) & (1u << hand)) &&
            (p.nativeWeaponId[hand] != 13 || p.requestedWeapon[hand] >= 0))
            return false;
    }
    return true;
}
// ReverseMapEntityHandle converts the local client handle to the server wire
// handle used by the VM.  The wire handle is intentionally not locally
// resolvable.
inline uint32_t outboundTargetHandle(bool client, uint32_t localHandle, uint32_t serverWireHandle) {
    return client ? serverWireHandle : localHandle;
}
inline bool outboundHandlesValid(bool localSourceResolved, uint32_t wireHandle) {
    return localSourceResolved && wireHandle != 0;
}

// Local-only provenance. Never serialized or treated as a movement receipt.
// origin/turn are the CURRENT source rig when submitted; head/grips below are
// raw calibrated tracking-space poses, before any tracking-volume clamp.
struct LocalCaptureFrame {
    Pose origin{};
    float turn = 0;
    uint32_t avatar = 0, producer = 0, session = 0, reference = 0;
    uint32_t trackingEpoch = 0, trackingGeneration = 0;
    uint64_t sequence = 0, tickMs = 0;
};
struct LocalPoseCapture {
    LocalCaptureFrame frame;
    Pose head{}, grip[2]{};
};
inline bool sameCaptureOwner(const LocalCaptureFrame &a, const LocalCaptureFrame &b) {
    return a.avatar == b.avatar && a.producer == b.producer && a.session == b.session &&
           a.reference == b.reference && a.trackingEpoch == b.trackingEpoch &&
           a.trackingGeneration == b.trackingGeneration;
}
inline bool captureNotOlder(const LocalCaptureFrame &current, const LocalCaptureFrame &previous) {
    return current.sequence >= previous.sequence && current.tickMs >= previous.tickMs &&
           (current.sequence != previous.sequence || current.tickMs == previous.tickMs);
}
inline bool validRawCapturePose(const Pose &pose) {
    if (!finitePose(pose)) return false;
    const auto q = pose.q;
    const double norm = double(q.x)*q.x + double(q.y)*q.y + double(q.z)*q.z + double(q.w)*q.w;
    return std::abs(norm - 1.0) <= 32 * std::numeric_limits<float>::epsilon();
}
inline bool validLocalCapture(const LocalPoseCapture &capture, uint32_t trackingGeneration, uint64_t now,
                              uint8_t activeHands) {
    const auto &f = capture.frame;
    if (!(f.avatar && f.producer && f.session && f.reference && f.trackingEpoch &&
           f.trackingGeneration && f.trackingGeneration == trackingGeneration && f.sequence &&
           f.tickMs && now >= f.tickMs && now - f.tickMs < MaxPoseAgeMs &&
           std::isfinite(f.turn) && validRawCapturePose(f.origin) &&
           validRawCapturePose(capture.head)) || (activeHands & ~HandMask)) return false;
    for (unsigned hand = 0; hand < 2; ++hand)
        if ((activeHands & (1u << hand)) && !validRawCapturePose(capture.grip[hand])) return false;
    return true;
}
inline bool relativeCapturedHead(const LocalCaptureFrame &current, const Pose &rawHead, Pose &head) {
    head = relativeTracking(current.origin, rawHead);
    return finitePose(head) &&
        std::isfinite(dot(Vec3{head.p.x, 0, head.p.z}, Vec3{head.p.x, 0, head.p.z}));
}
inline bool composeCapturedHand(const LocalCaptureFrame &current, const Pose &rawHead,
                                const Pose &rawGrip, Pose &result) {
    // Reject overflowing intermediates before the clamps could hide them.
    Pose head;
    if (!relativeCapturedHead(current, rawHead, head)) return false;
    const auto grip = relativeTracking(current.origin, rawGrip);
    const auto reach = grip.p - head.p;
    if (!finitePose(grip) ||
        !std::isfinite(dot(reach, reach)))
        return false;
    result = bodyHandTracking(current.origin, current.turn, rawHead, rawGrip);
    return finitePose(result);
}
inline bool composeLocalPose(PosePacket &pose, const LocalPoseCapture &capture, uint64_t now) {
    if (!validLocalCapture(capture, pose.trackingGeneration, now, uint8_t(pose.validMask >> 1))) return false;
    Pose head;
    if (!relativeCapturedHead(capture.frame, capture.head, head)) return false;
    auto composed = pose;
    composed.head = bodyHeadTracking(capture.frame.origin, capture.frame.turn, capture.head);
    for (unsigned hand = 0; hand != 2; ++hand) {
        // Inactive hands retain the existing canonical current-head position.
        if (!(pose.validMask & (2u << hand))) composed.grip[hand] = Pose{{}, composed.head.p};
        else if (!composeCapturedHand(capture.frame, capture.head, capture.grip[hand], composed.grip[hand]))
            return false;
    }
    if (!validatePoseCoordinates(composed)) return false;
    pose = composed;
    return true;
}
// Coordinate rejection does not reject the connection. An inactive snapshot
// still carries native capability/retirement metadata, never an invented release.
inline bool prepareLocalPose(PosePacket &pose, const LocalPoseCapture &capture,
                             const LocalCaptureFrame &previous, uint32_t avatar, uint64_t now) {
    if ((pose.validMask & 1) && capture.frame.avatar == avatar &&
        (!previous.avatar || !sameCaptureOwner(capture.frame, previous) ||
         captureNotOlder(capture.frame, previous)) && composeLocalPose(pose, capture, now))
        return true;
    pose.validMask = 0;
    pose.head = pose.grip[0] = pose.grip[1] = {};
    invalidateWeaponIntents(pose, HandMask);
    cancelWeaponRequests(pose, HandMask);
    pose.wheelOrEquipBlockedMask |= HandMask;
    pose.primarySampleEligibleMask = pose.primaryNeutralSampleMask = pose.zoomSampleEligibleMask = 0;
    return false;
}

// Shared provenance admission inside the existing local/peer owners. This is
// transport quiet/rearm only: it cannot establish native consumed-low.
struct GestureAdmission {
    // Monotonic stream generation within a capability, including producer and
    // binding replacement. A new generation may restart its source sequence.
    uint32_t generation = 0;
    uint64_t sequence = 0, tickMs = 0, quietSequence = 0, quietTickMs = 0, receivedMs = 0;
    uint64_t afterSequence = 0, afterTickMs = 0, lastPulseSequence = 0;
    bool armed = false, down = false;
    void requireQuiet() {
        armed = false; afterSequence = sequence; afterTickMs = tickMs;
    }
    bool observe(const PosePacket &p, unsigned hand, uint64_t now) {
        if (hand >= 2) return false;
        const auto bit = uint8_t(1u << hand);
        if (!(p.gestureEligibleMask & bit) || !validGestureHand(p, hand)) {
            requireQuiet(); return false;
        }
        if (p.gestureGeneration[hand] < generation) return false;
        if (p.gestureGeneration[hand] != generation) {
            *this = {}; generation = p.gestureGeneration[hand];
        }
        if (p.gestureSequence[hand] < sequence || p.gestureTickMs[hand] < tickMs ||
            ((p.gestureSequence[hand] == sequence) != (p.gestureTickMs[hand] == tickMs))) return false;
        if (sequence && (now < receivedMs || now - receivedMs > MaxPoseAgeMs)) requireQuiet();
        if (p.gestureSequence[hand] == sequence)
            return quietSequence == p.gestureQuietSequence[hand] && quietTickMs == p.gestureQuietTickMs[hand] &&
                down == bool(p.gestureDownMask & bit);
        if (p.gestureQuietSequence[hand] < quietSequence || p.gestureQuietTickMs[hand] < quietTickMs ||
            ((p.gestureQuietSequence[hand] == quietSequence) != (p.gestureQuietTickMs[hand] == quietTickMs)))
            return false;
        sequence = p.gestureSequence[hand]; tickMs = p.gestureTickMs[hand]; receivedMs = now;
        quietSequence = p.gestureQuietSequence[hand]; quietTickMs = p.gestureQuietTickMs[hand];
        down = (p.gestureDownMask & bit) != 0;
        if ((p.gestureQuietMask & bit) && quietSequence > afterSequence && quietTickMs > afterTickMs)
            armed = true;
        return true;
    }
    bool current(const PosePacket &p, unsigned hand, uint64_t now = 0) const {
        return hand < 2 && armed && (p.gestureEligibleMask & (1u << hand)) && validGestureHand(p, hand) &&
            p.gestureGeneration[hand] == generation && p.gestureSequence[hand] == sequence &&
            p.gestureTickMs[hand] == tickMs && p.gestureQuietSequence[hand] == quietSequence &&
            p.gestureQuietTickMs[hand] == quietTickMs && down == bool(p.gestureDownMask & (1u << hand)) &&
            (!now || (now >= receivedMs && now - receivedMs <= MaxPoseAgeMs));
    }
};

// A physical press survives a full reliable-send window only as a per-hand
// pulse. The next granted Pose carries it once, then normal level state takes
// over; a released tap cannot become held while waiting for transport.
class PendingIntents {
  public:
    static constexpr uint64_t ExpiryMs = MaxPoseAgeMs;
    // Local value receipt for an ordinary native transport handoff, not an ACK
    // or native weapon completion. The caller also rechecks its Local/send token.
    struct HandoffReceipt {
        struct Hand {
            LocalCaptureFrame frame;
            Pose head{}, grip{};
            uint64_t expiresAt = 0;
            uint32_t trackingGeneration = 0, intentEpoch = 0, primaryGeneration = 0, zoomGeneration = 0;
            uint32_t gestureGeneration = 0;
            uint64_t gestureSequence = 0, gestureTickMs = 0, quietSequence = 0, quietTickMs = 0;
            int16_t nativeWeaponId = -1;
            uint8_t kinds = 0;
            bool zoomed = false;
        } hand[2];
        const PendingIntents *owner = nullptr;
    };

    void requireNeutral(uint8_t handMask = HandMask) {
        requirePrimaryNeutral(handMask);
        for (unsigned hand = 0; hand < 2; ++hand)
            if (handMask & (1u << hand)) gesture_[hand].requireQuiet();
        cancel(handMask);
    }
    bool primaryAllowed(unsigned hand) const {
        return hand < 2 && !(neutralRequired_ & (1u << hand));
    }
    bool gestureAllowed(unsigned hand) const {
        return hand < 2 && gesture_[hand].armed;
    }
    bool currentPrimaryIntent(const PosePacket &latest, unsigned hand, uint32_t capturedEpoch,
                              uint32_t capturedPrimaryGeneration, bool requireFire = true) const {
        return primaryAllowed(hand) && matchesIntentEpoch(latest, hand, capturedEpoch) &&
               capturedPrimaryGeneration && latest.primaryInputGeneration[hand] == capturedPrimaryGeneration &&
               (latest.primarySampleEligibleMask & (1u << hand)) &&
               (!requireFire || (latest.fireMask & (1u << hand)));
    }
    void filterPrimaryIntents(PosePacket &pose) const {
        filterManualIntents(pose);
        for (unsigned hand = 0; hand < 2; ++hand)
            if (!gesture_[hand].current(pose, hand)) invalidateGestureIntents(pose, uint8_t(1u << hand));
    }
    bool currentGestureIntent(const PosePacket &pose, unsigned hand, uint32_t epoch,
                              uint32_t generation, uint64_t sequence, bool requireDown = true) const {
        return hand < 2 && matchesIntentEpoch(pose, hand, epoch) &&
            gesture_[hand].current(pose, hand) && pose.gestureGeneration[hand] == generation &&
            pose.gestureSequence[hand] == sequence && (!requireDown || (pose.gestureDownMask & (1u << hand)));
    }
  private:
    void filterManualIntents(PosePacket &pose) const {
        for (unsigned hand = 0; hand < 2; ++hand)
            if (!primaryAllowed(hand)) {
                const auto keep = uint8_t(~(1u << hand));
                pose.fireMask &= keep;
                pose.pulseMask &= keep;
                pose.pulseZoomMask &= keep;
            }
    }
    void removeKind(unsigned hand, uint8_t kind) {
        kinds_[hand] &= uint8_t(~kind);
        if (!kinds_[hand]) expiresAt_[hand] = 0;
    }
    void requirePrimaryNeutral(uint8_t hands) {
        neutralRequired_ |= hands & HandMask;
        for (unsigned hand = 0; hand < 2; ++hand)
            if (hands & (1u << hand)) removeKind(hand, Manual);
    }
  public:
    void observeSnapshot(const PosePacket &pose, uint64_t now = 0) {
        if (trackingGeneration_ && pose.trackingGeneration != trackingGeneration_)
            requireNeutral();
        trackingGeneration_ = pose.trackingGeneration;
        for (unsigned hand = 0; hand != 2; ++hand) {
            const uint8_t bit = uint8_t(1u << hand);
            if (lastIntentEpoch_[hand] && lastIntentEpoch_[hand] != pose.intentEpoch[hand])
                requireNeutral(bit);
            if (lastPrimaryInputGeneration_[hand] &&
                lastPrimaryInputGeneration_[hand] != pose.primaryInputGeneration[hand])
                requirePrimaryNeutral(bit);
            lastIntentEpoch_[hand] = pose.intentEpoch[hand];
            lastPrimaryInputGeneration_[hand] = pose.primaryInputGeneration[hand];
            if (!pose.intentEpoch[hand]) gesture_[hand].requireQuiet();
            else gesture_[hand].observe(pose, hand, now);
            if ((kinds_[hand] & Gesture) && expiresAt_[hand] &&
                (!gesture_[hand].current(pose, hand) || nativeWeaponId_[hand] != pose.nativeWeaponId[hand] ||
                 intentEpoch_[hand] != pose.intentEpoch[hand] ||
                 gestureGeneration_[hand] != pose.gestureGeneration[hand])) removeKind(hand, Gesture);
            if (expiresAt_[hand] &&
                (kinds_[hand] & Manual) &&
                (nativeWeaponId_[hand] != pose.nativeWeaponId[hand] || pose.requestedWeapon[hand] >= 0 ||
                 intentEpoch_[hand] != pose.intentEpoch[hand] || !pose.intentEpoch[hand] ||
                 primaryInputGeneration_[hand] != pose.primaryInputGeneration[hand] ||
                 !(pose.primarySampleEligibleMask & bit)))
                requirePrimaryNeutral(bit);
            if (expiresAt_[hand] && zoomed_[hand] &&
                (zoomInputGeneration_[hand] != pose.zoomInputGeneration[hand] ||
                 !(pose.zoomSampleEligibleMask & bit)))
                removeKind(hand, Manual);
            if (!(pose.validMask & 1) || !(pose.validMask & (bit << 1)) ||
                (pose.wheelOrEquipBlockedMask & bit) || !(pose.primarySampleEligibleMask & bit) ||
                !pose.primaryInputGeneration[hand] || !pose.intentEpoch[hand]) {
                requirePrimaryNeutral(bit);
                continue;
            }
            if ((neutralRequired_ & bit) && (pose.validMask & 1) && (pose.validMask & (bit << 1)) &&
                (pose.primaryNeutralSampleMask & bit) && !(pose.physicalDownMask & bit) && !(pose.fireMask & bit) &&
                !(pose.wheelOrEquipBlockedMask & bit))
                neutralRequired_ &= uint8_t(~bit);
        }
    }
    bool observeCapture(const PosePacket &pose, const LocalPoseCapture &capture, uint64_t now) {
        if (!validLocalCapture(capture, pose.trackingGeneration, now, uint8_t(pose.validMask >> 1))) {
            requireNeutral();
            return false;
        }
        for (unsigned hand = 0; hand != 2; ++hand)
            if (expiresAt_[hand]) {
                if (!sameCaptureOwner(capture.frame, capture_[hand].frame))
                    requireNeutral(uint8_t(1u << hand));
                else if (!captureNotOlder(capture.frame, capture_[hand].frame)) {
                    requireNeutral();
                    return false;
                }
            }
        auto admitted = pose;
        for (unsigned hand = 0; hand < 2; ++hand)
            if ((pose.gestureEligibleMask & (1u << hand)) &&
                (pose.gestureSequence[hand] > capture.frame.sequence || pose.gestureTickMs[hand] > capture.frame.tickMs ||
                 now < pose.gestureTickMs[hand] || now - pose.gestureTickMs[hand] > MaxPoseAgeMs))
                invalidateGestureIntents(admitted, uint8_t(1u << hand));
        observeSnapshot(admitted, now);
        return true;
    }
    bool retain(const PosePacket &pose, uint8_t pressedHands, uint64_t now,
                const LocalPoseCapture &capture, uint8_t gesturePressed = 0) {
        expire(now);
        if (!observeCapture(pose, capture, now)) return false;
        bool retained = false;
        for (unsigned hand = 0; hand != 2; ++hand) {
            const uint8_t bit = uint8_t(1u << hand);
            const bool manual = (pressedHands & bit) && !(neutralRequired_ & bit) &&
                (pose.validMask & 1) && (pose.validMask & (bit << 1)) && (pose.physicalDownMask & bit) &&
                (pose.fireMask & bit) && !(pose.wheelOrEquipBlockedMask & bit) &&
                pose.requestedWeapon[hand] < 0 && (pose.primarySampleEligibleMask & bit) &&
                pose.primaryInputGeneration[hand] && pose.intentEpoch[hand] &&
                (!(pose.zoomMask & bit) || ((pose.zoomSampleEligibleMask & bit) && pose.zoomInputGeneration[hand]));
            const bool gesture = (gesturePressed & bit) && pose.intentEpoch[hand] &&
                gesture_[hand].current(pose, hand) && (pose.gestureDownMask & bit) &&
                pose.gestureSequence[hand] == capture.frame.sequence && pose.gestureTickMs[hand] == capture.frame.tickMs &&
                (lastGestureGeneration_[hand] != pose.gestureGeneration[hand] ||
                 pose.gestureSequence[hand] > lastGestureSequence_[hand]);
            if (gesture) { // Capacity loss discards this edge; it cannot turn into later debt.
                lastGestureGeneration_[hand] = pose.gestureGeneration[hand];
                lastGestureSequence_[hand] = pose.gestureSequence[hand];
            }
            if (!manual && !gesture) continue;
            const bool same = expiresAt_[hand] && sameCaptureOwner(capture.frame, capture_[hand].frame) &&
                capture.frame.sequence == capture_[hand].frame.sequence &&
                capture.frame.tickMs == capture_[hand].frame.tickMs &&
                !std::memcmp(&capture.head, &capture_[hand].head, sizeof(Pose)) &&
                !std::memcmp(&capture.grip[hand], &capture_[hand].grip, sizeof(Pose)) &&
                nativeWeaponId_[hand] == pose.nativeWeaponId[hand] && intentEpoch_[hand] == pose.intentEpoch[hand];
            if (expiresAt_[hand] && !same && ((kinds_[hand] & Manual) || !manual)) continue;
            if (same && (!manual || (kinds_[hand] & Manual)) && (!gesture || (kinds_[hand] & Gesture))) continue;
            if (!same) { expiresAt_[hand] = now + ExpiryMs; kinds_[hand] = 0; }
            kinds_[hand] |= uint8_t((manual ? Manual : 0) | (gesture ? Gesture : 0));
            if (gesture) {
                gestureGeneration_[hand] = pose.gestureGeneration[hand];
                gestureSequence_[hand] = pose.gestureSequence[hand]; gestureTickMs_[hand] = pose.gestureTickMs[hand];
                gestureQuietSequence_[hand] = pose.gestureQuietSequence[hand];
                gestureQuietTickMs_[hand] = pose.gestureQuietTickMs[hand];
            }
            capture_[hand] = {capture.frame, capture.head, capture.grip[hand]};
            nativeWeaponId_[hand] = pose.nativeWeaponId[hand];
            zoomed_[hand] = (pose.zoomMask & bit) != 0;
            generation_[hand] = pose.trackingGeneration;
            intentEpoch_[hand] = pose.intentEpoch[hand];
            primaryInputGeneration_[hand] = pose.primaryInputGeneration[hand];
            zoomInputGeneration_[hand] = zoomed_[hand] ? pose.zoomInputGeneration[hand] : 0;
            retained = true;
        }
        return retained;
    }
    // Compose without consuming: a failed native send must retain the same tap.
    // Return manual pulses for existing callers; gesturePulseMask is separate.
    // Before unlocking for a send, receipt the union of BOTH transmitted masks;
    // on successful handoff consume that union with the captured receipt.
    uint8_t apply(PosePacket &pose, uint64_t now, const LocalPoseCapture &current) {
        if (!observeCapture(pose, current, now)) {
            invalidateWeaponIntents(pose, HandMask);
            return 0;
        }
        filterPrimaryIntents(pose);
        const uint8_t pulses = uint8_t(mask(now) | gestureMask(now));
        pose.pulseMask = mask(now);
        pose.gesturePulseMask = gestureMask(now);
        pose.pulseZoomMask = 0;
        for (unsigned hand = 0; hand != 2; ++hand)
            if (pulses & (1u << hand)) {
                Pose grip;
                const auto &captured = capture_[hand];
                if (!composeCapturedHand(current.frame, captured.head, captured.grip, grip) ||
                    !normalizeWirePose(grip, MaximumHandTranslation + TrackingBoundsSlack) ||
                    !handTranslationInVolume(grip.p)) {
                    requireNeutral(uint8_t(1u << hand));
                    invalidateWeaponIntents(pose, uint8_t(1u << hand));
                    continue;
                }
                pose.grip[hand] = grip;
                pose.nativeWeaponId[hand] = nativeWeaponId_[hand];
                if ((kinds_[hand] & Manual) && zoomed_[hand])
                    pose.pulseZoomMask |= uint8_t(1u << hand);
                if (kinds_[hand] & Gesture) {
                    pose.gestureEligibleMask |= uint8_t(1u << hand);
                    pose.gestureGeneration[hand] = gestureGeneration_[hand];
                    pose.gestureSequence[hand] = gestureSequence_[hand]; pose.gestureTickMs[hand] = gestureTickMs_[hand];
                    pose.gestureQuietSequence[hand] = gestureQuietSequence_[hand];
                    pose.gestureQuietTickMs[hand] = gestureQuietTickMs_[hand];
                    if (gestureQuietSequence_[hand]) pose.gestureQuietMask |= uint8_t(1u << hand);
                    else pose.gestureQuietMask &= uint8_t(~(1u << hand));
                }
                // observeSnapshot canceled any generation mismatch above.
                pose.trackingGeneration = generation_[hand];
            }
        filterManualIntents(pose);
        return pose.pulseMask;
    }
    uint8_t mask(uint64_t now) {
        return kindMask(now, Manual);
    }
    uint8_t gestureMask(uint64_t now) {
        return kindMask(now, Gesture);
    }
  private:
    uint8_t kindMask(uint64_t now, uint8_t kind) {
        expire(now);
        uint8_t result = 0;
        for (unsigned hand = 0; hand != 2; ++hand)
            if (expiresAt_[hand] && (kinds_[hand] & kind))
                result |= uint8_t(1u << hand);
        return result;
    }
  public:
    HandoffReceipt handoffReceipt(uint8_t handMask) const {
        HandoffReceipt receipt;
        receipt.owner = this;
        for (unsigned hand = 0; hand < 2; ++hand)
            if ((handMask & (1u << hand)) && expiresAt_[hand]) {
                auto &out = receipt.hand[hand];
                out.frame = capture_[hand].frame; out.head = capture_[hand].head; out.grip = capture_[hand].grip;
                out.expiresAt = expiresAt_[hand]; out.kinds = kinds_[hand];
                out.nativeWeaponId = nativeWeaponId_[hand]; out.trackingGeneration = generation_[hand];
                out.intentEpoch = intentEpoch_[hand]; out.primaryGeneration = primaryInputGeneration_[hand];
                out.zoomGeneration = zoomInputGeneration_[hand]; out.zoomed = zoomed_[hand];
                out.gestureGeneration = gestureGeneration_[hand]; out.gestureSequence = gestureSequence_[hand];
                out.gestureTickMs = gestureTickMs_[hand]; out.quietSequence = gestureQuietSequence_[hand];
                out.quietTickMs = gestureQuietTickMs_[hand];
            }
        return receipt;
    }
    void consume(uint8_t handMask, const HandoffReceipt &receipt) {
        if (receipt.owner != this) return;
        static_assert(sizeof(Pose) == 28);
        for (unsigned hand = 0; hand < 2; ++hand) {
            const auto &sent = receipt.hand[hand];
            const auto &capture = capture_[hand];
            if (!(handMask & (1u << hand)) || !sent.kinds || !expiresAt_[hand] ||
                expiresAt_[hand] != sent.expiresAt || !sameCaptureOwner(capture.frame, sent.frame) ||
                capture.frame.sequence != sent.frame.sequence || capture.frame.tickMs != sent.frame.tickMs ||
                std::memcmp(&capture.head, &sent.head, sizeof(Pose)) ||
                std::memcmp(&capture.grip, &sent.grip, sizeof(Pose)) ||
                nativeWeaponId_[hand] != sent.nativeWeaponId || generation_[hand] != sent.trackingGeneration ||
                intentEpoch_[hand] != sent.intentEpoch) continue;
            // Origin/turn are deliberately not identity: the same raw input may
            // be recomposed after rig settlement. Match each submitted kind only.
            uint8_t consumed = 0;
            const uint8_t matchingKinds = uint8_t(sent.kinds & kinds_[hand]);
            if ((matchingKinds & Manual) && primaryInputGeneration_[hand] == sent.primaryGeneration &&
                zoomed_[hand] == sent.zoomed && zoomInputGeneration_[hand] == sent.zoomGeneration)
                consumed |= Manual;
            if ((matchingKinds & Gesture) && gestureGeneration_[hand] == sent.gestureGeneration &&
                gestureSequence_[hand] == sent.gestureSequence && gestureTickMs_[hand] == sent.gestureTickMs &&
                gestureQuietSequence_[hand] == sent.quietSequence && gestureQuietTickMs_[hand] == sent.quietTickMs)
                consumed |= Gesture;
            removeKind(hand, consumed);
        }
    }
    // Unconditional retirement for existing explicit-discard/test callers only.
    void consume(uint8_t handMask) {
        for (unsigned hand = 0; hand != 2; ++hand)
            if (handMask & (1u << hand))
                expiresAt_[hand] = kinds_[hand] = 0;
    }
    void cancel(uint8_t handMask) {
        for (unsigned hand = 0; hand != 2; ++hand)
            if (handMask & (1u << hand))
                expiresAt_[hand] = kinds_[hand] = 0;
    }
    bool active(unsigned hand, uint64_t now) {
        expire(now);
        return hand < 2 && expiresAt_[hand] != 0;
    }

  private:
    static constexpr uint8_t Manual = 1, Gesture = 2;
    void expire(uint64_t now) {
        for (auto &expires : expiresAt_)
            if (expires && now >= expires)
                expires = 0;
    }
    uint64_t expiresAt_[2]{};
    uint8_t kinds_[2]{};
    GestureAdmission gesture_[2];
    uint32_t gestureGeneration_[2]{}, lastGestureGeneration_[2]{};
    uint64_t gestureSequence_[2]{}, gestureTickMs_[2]{}, gestureQuietSequence_[2]{}, gestureQuietTickMs_[2]{};
    uint64_t lastGestureSequence_[2]{};
    struct HandCapture { LocalCaptureFrame frame; Pose head{}, grip{}; };
    HandCapture capture_[2]{};
    int16_t nativeWeaponId_[2]{-1, -1};
    uint32_t generation_[2]{};
    uint32_t intentEpoch_[2]{};
    uint32_t primaryInputGeneration_[2]{};
    uint32_t zoomInputGeneration_[2]{};
    bool zoomed_[2]{};
    uint8_t neutralRequired_ = 0;
    uint32_t trackingGeneration_ = 0;
    uint32_t lastIntentEpoch_[2]{}, lastPrimaryInputGeneration_[2]{};
};

struct ConsumptionToken {
    uint64_t clientNonce = 0, serverNonce = 0;
    uint32_t sequence = 0; // Hello grants use sequence zero.
};

// Every native reliable submission takes one bounded credit. Only a matching
// completed/discarded server ACK returns it; a pose sequence can never retire
// another capability's token.
class ConsumptionCredits {
  public:
    static constexpr unsigned Capacity = 4;
    bool hasOutstandingCapability(uint64_t clientNonce, uint64_t serverNonce) const {
        for (const auto &entry : entries_)
            if (entry.clientNonce == clientNonce && entry.serverNonce == serverNonce)
                return true;
        return false;
    }
    bool grant(ConsumptionToken token) {
        if (!token.clientNonce || (token.sequence && !token.serverNonce) ||
            hasOutstandingCapability(token.clientNonce, token.serverNonce))
            return false;
        for (auto &entry : entries_)
            if (!entry.clientNonce) {
                entry = token;
                return true;
            }
        return false;
    }
    bool retire(const Ack &ack) {
        for (auto &entry : entries_)
            if (entry.clientNonce == ack.clientNonce && entry.sequence == ack.acceptedSequence &&
                (!entry.sequence || entry.serverNonce == ack.serverNonce)) {
                entry = {};
                return true;
            }
        return false;
    }
    // Old-capability ACKs still return their aggregate credit, but cannot
    // establish or refresh the current capability lease.
    bool acknowledge(const Ack &ack, uint64_t clientNonce, uint64_t serverNonce, bool leaseLive = true) {
        return retire(ack) && ack.clientNonce == clientNonce &&
               (!serverNonce || ack.serverNonce == serverNonce) && leaseLive;
    }
    bool full() const {
        for (const auto &entry : entries_)
            if (!entry.clientNonce)
                return false;
        return true;
    }
    unsigned count() const {
        unsigned result = 0;
        for (const auto &entry : entries_)
            result += entry.clientNonce != 0;
        return result;
    }
    void revoke(ConsumptionToken token) {
        for (auto &entry : entries_)
            if (entry.clientNonce == token.clientNonce && entry.serverNonce == token.serverNonce &&
                entry.sequence == token.sequence) {
                entry = {};
                return;
            }
    }

  private:
    ConsumptionToken entries_[Capacity]{};
};

enum class TransportPhase { BeforeCall, EnteredCall, ReturnedCall };
// An interruption before entry proves no submission. Entry without return is
// ambiguous; only the original matching server ACK may return that credit.
inline void revokeUnsubmitted(ConsumptionCredits &credits, ConsumptionToken token,
                              TransportPhase phase) noexcept {
    if (phase == TransportPhase::BeforeCall)
        credits.revoke(token);
}

namespace detail {
inline void put8(std::vector<uint8_t> &out, uint8_t value) {
    out.push_back(value);
}
inline void put16(std::vector<uint8_t> &out, uint16_t value) {
    out.push_back(uint8_t(value));
    out.push_back(uint8_t(value >> 8));
}
inline void put32(std::vector<uint8_t> &out, uint32_t value) {
    for (unsigned i = 0; i != 4; ++i)
        out.push_back(uint8_t(value >> (i * 8)));
}
inline void put64(std::vector<uint8_t> &out, uint64_t value) {
    for (unsigned i = 0; i != 8; ++i)
        out.push_back(uint8_t(value >> (i * 8)));
}
inline void putFloat(std::vector<uint8_t> &out, float value) {
    uint32_t bits = 0;
    static_assert(sizeof bits == sizeof value);
    std::memcpy(&bits, &value, sizeof bits);
    put32(out, bits);
}
inline void putPose(std::vector<uint8_t> &out, const Pose &p) {
    putFloat(out, p.p.x);
    putFloat(out, p.p.y);
    putFloat(out, p.p.z);
    putFloat(out, p.q.x);
    putFloat(out, p.q.y);
    putFloat(out, p.q.z);
    putFloat(out, p.q.w);
}
class Reader {
  public:
    explicit Reader(const std::vector<uint8_t> &bytes) : bytes_(bytes) {}
    bool u8(uint8_t &out) {
        if (position_ == bytes_.size())
            return false;
        out = bytes_[position_++];
        return true;
    }
    bool u16(uint16_t &out) {
        uint8_t a, b;
        if (!u8(a) || !u8(b))
            return false;
        out = uint16_t(a) | (uint16_t(b) << 8);
        return true;
    }
    bool u32(uint32_t &out) {
        uint8_t v[4];
        for (auto &x : v)
            if (!u8(x))
                return false;
        out = uint32_t(v[0]) | (uint32_t(v[1]) << 8) | (uint32_t(v[2]) << 16) | (uint32_t(v[3]) << 24);
        return true;
    }
    bool u64(uint64_t &out) {
        uint8_t v[8];
        for (auto &x : v)
            if (!u8(x))
                return false;
        out = 0;
        for (unsigned i = 0; i != 8; ++i)
            out |= uint64_t(v[i]) << (i * 8);
        return true;
    }
    bool i16(int16_t &out) {
        uint16_t v;
        if (!u16(v))
            return false;
        out = static_cast<int16_t>(v);
        return true;
    }
    bool number(float &out) {
        uint32_t bits;
        if (!u32(bits))
            return false;
        std::memcpy(&out, &bits, sizeof out);
        return true;
    }
    bool pose(Pose &out) {
        return number(out.p.x) && number(out.p.y) && number(out.p.z) && number(out.q.x) && number(out.q.y) &&
               number(out.q.z) && number(out.q.w);
    }
    bool done() const {
        return position_ == bytes_.size();
    }

  private:
    const std::vector<uint8_t> &bytes_;
    size_t position_ = 0;
};
inline bool readPosePacket(Reader &r, PosePacket &p) {
    if (!(r.u64(p.clientNonce) && r.u64(p.serverNonce) && r.u32(p.sequence) && r.u32(p.trackingGeneration) &&
           r.pose(p.head) && r.pose(p.grip[0]) && r.pose(p.grip[1]) && r.u8(p.validMask) &&
           r.u8(p.physicalDownMask) && r.u8(p.fireMask) && r.u8(p.pulseMask) &&
           r.u8(p.wheelOrEquipBlockedMask) && r.u8(p.zoomMask) && r.u8(p.pulseZoomMask) &&
           r.u8(p.zoomPhysicalDownMask) && r.u8(p.zoomSampleEligibleMask) && r.u8(p.primarySampleEligibleMask) &&
           r.u8(p.primaryNeutralSampleMask) &&
           r.u32(p.intentEpoch[0]) && r.u32(p.intentEpoch[1]) && r.u32(p.releasedSerial[0]) &&
           r.u32(p.releasedSerial[1]) && r.u32(p.primaryInputGeneration[0]) &&
           r.u32(p.primaryInputGeneration[1]) && r.u32(p.zoomReleasedSerial[0]) &&
           r.u32(p.zoomReleasedSerial[1]) && r.u32(p.zoomInputGeneration[0]) && r.u32(p.zoomInputGeneration[1]) &&
           r.i16(p.requestedWeapon[0]) && r.i16(p.requestedWeapon[1]) && r.i16(p.nativeWeaponId[0]) &&
           r.i16(p.nativeWeaponId[1]) && r.u8(p.gestureEligibleMask) && r.u8(p.gestureDownMask) &&
           r.u8(p.gestureQuietMask) && r.u8(p.gesturePulseMask))) return false;
    for (unsigned hand = 0; hand < 2; ++hand)
        if (!(r.u32(p.gestureGeneration[hand]) && r.u64(p.gestureSequence[hand]) && r.u64(p.gestureTickMs[hand]) &&
              r.u64(p.gestureQuietSequence[hand]) && r.u64(p.gestureQuietTickMs[hand]))) return false;
    return true;
}
inline void writePosePacket(std::vector<uint8_t> &out, const PosePacket &p) {
    put64(out, p.clientNonce);
    put64(out, p.serverNonce);
    put32(out, p.sequence);
    put32(out, p.trackingGeneration);
    putPose(out, p.head);
    putPose(out, p.grip[0]);
    putPose(out, p.grip[1]);
    put8(out, p.validMask);
    put8(out, p.physicalDownMask);
    put8(out, p.fireMask);
    put8(out, p.pulseMask);
    put8(out, p.wheelOrEquipBlockedMask);
    put8(out, p.zoomMask);
    put8(out, p.pulseZoomMask);
    put8(out, p.zoomPhysicalDownMask);
    put8(out, p.zoomSampleEligibleMask);
    put8(out, p.primarySampleEligibleMask);
    put8(out, p.primaryNeutralSampleMask);
    put32(out, p.intentEpoch[0]);
    put32(out, p.intentEpoch[1]);
    put32(out, p.releasedSerial[0]);
    put32(out, p.releasedSerial[1]);
    put32(out, p.primaryInputGeneration[0]);
    put32(out, p.primaryInputGeneration[1]);
    put32(out, p.zoomReleasedSerial[0]);
    put32(out, p.zoomReleasedSerial[1]);
    put32(out, p.zoomInputGeneration[0]);
    put32(out, p.zoomInputGeneration[1]);
    put16(out, static_cast<uint16_t>(p.requestedWeapon[0]));
    put16(out, static_cast<uint16_t>(p.requestedWeapon[1]));
    put16(out, static_cast<uint16_t>(p.nativeWeaponId[0]));
    put16(out, static_cast<uint16_t>(p.nativeWeaponId[1]));
    put8(out, p.gestureEligibleMask); put8(out, p.gestureDownMask);
    put8(out, p.gestureQuietMask); put8(out, p.gesturePulseMask);
    for (unsigned hand = 0; hand < 2; ++hand) {
        put32(out, p.gestureGeneration[hand]);
        put64(out, p.gestureSequence[hand]); put64(out, p.gestureTickMs[hand]);
        put64(out, p.gestureQuietSequence[hand]); put64(out, p.gestureQuietTickMs[hand]);
    }
}
inline const char Base64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
inline int base64Value(char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}
inline std::string base64(const std::vector<uint8_t> &in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        const size_t left = in.size() - i;
        const uint32_t v = uint32_t(in[i]) << 16 | (left > 1 ? uint32_t(in[i + 1]) << 8 : 0) |
                           (left > 2 ? uint32_t(in[i + 2]) : 0);
        out += Base64[(v >> 18) & 63];
        out += Base64[(v >> 12) & 63];
        out += left > 1 ? Base64[(v >> 6) & 63] : '=';
        out += left > 2 ? Base64[v & 63] : '=';
    }
    return out;
}
inline bool unbase64(std::string_view in, std::vector<uint8_t> &out) {
    if (in.empty() || in.size() % 4 || in.size() > MaxCarrierAscii - CarrierTag.size())
        return false;
    out.clear();
    out.reserve(in.size() / 4 * 3);
    for (size_t i = 0; i < in.size(); i += 4) {
        const bool last = i + 4 == in.size();
        const int a = base64Value(in[i]), b = base64Value(in[i + 1]);
        const int c = in[i + 2] == '=' ? -2 : base64Value(in[i + 2]);
        const int d = in[i + 3] == '=' ? -2 : base64Value(in[i + 3]);
        if (a < 0 || b < 0 || c == -1 || d == -1 || (!last && (c == -2 || d == -2)))
            return false;
        if (c == -2 && d != -2)
            return false;
        if (c == -2 && (b & 15))
            return false;
        if (d == -2 && c >= 0 && (c & 3))
            return false;
        const uint32_t v =
            uint32_t(a) << 18 | uint32_t(b) << 12 | uint32_t(c < 0 ? 0 : c) << 6 | uint32_t(d < 0 ? 0 : d);
        out.push_back(uint8_t(v >> 16));
        if (c != -2)
            out.push_back(uint8_t(v >> 8));
        if (d != -2)
            out.push_back(uint8_t(v));
    }
    return true;
}
inline void writeVoice(std::vector<uint8_t> &out, const VoicePacket &v) {
    put64(out, v.clientNonce);
    put64(out, v.serverNonce);
    put32(out, v.firstFrame);
    put8(out, v.talkspurt);
    put8(out, v.flags);
    put8(out, v.frameCount);
    for (unsigned i = 0; i < v.frameCount; ++i) {
        put8(out, v.frameBytes[i]);
        out.insert(out.end(), v.frames[i], v.frames[i] + v.frameBytes[i]);
    }
}
inline bool readVoice(Reader &r, VoicePacket &v) {
    if (!r.u64(v.clientNonce) || !r.u64(v.serverNonce) || !r.u32(v.firstFrame) || !r.u8(v.talkspurt) || !r.u8(v.flags) ||
        !r.u8(v.frameCount) || v.frameCount > MaxVoiceFrames)
        return false;
    for (unsigned i = 0; i < v.frameCount; ++i) {
        if (!r.u8(v.frameBytes[i]) || !v.frameBytes[i] || v.frameBytes[i] > MaxVoiceFrameBytes)
            return false;
        for (unsigned b = 0; b < v.frameBytes[i]; ++b)
            if (!r.u8(v.frames[i][b]))
                return false;
    }
    return true;
}
} // namespace detail

inline bool encode(const Message &message, std::string &carrier) {
    Message copy = message;
    std::vector<uint8_t> bytes;
    bytes.reserve(144);
    detail::put8(bytes, WireVersion);
    detail::put8(bytes, static_cast<uint8_t>(copy.kind));
    detail::put16(bytes, 0);
    switch (copy.kind) {
    case Kind::Hello:
        if (!copy.hello.nonce)
            return false;
        detail::put64(bytes, copy.hello.nonce);
        break;
    case Kind::Ack:
        if (!copy.ack.clientNonce || !copy.ack.serverNonce)
            return false;
        detail::put64(bytes, copy.ack.clientNonce);
        detail::put64(bytes, copy.ack.serverNonce);
        detail::put32(bytes, copy.ack.acceptedSequence);
        detail::put32(bytes, copy.ack.intentEpoch[0]);
        detail::put32(bytes, copy.ack.intentEpoch[1]);
        break;
    case Kind::Pose:
        if (!validatePose(copy.pose))
            return false;
        detail::writePosePacket(bytes, copy.pose);
        break;
    case Kind::Relay:
        if (!copy.relay.subjectAvatar || !copy.relay.subjectIncarnation || !validatePose(copy.relay.pose))
            return false;
        detail::writePosePacket(bytes, copy.relay.pose);
        detail::put32(bytes, copy.relay.subjectAvatar);
        detail::put32(bytes, copy.relay.subjectIncarnation);
        break;
    case Kind::Voice:
        if (!validateVoice(copy.voice))
            return false;
        detail::writeVoice(bytes, copy.voice);
        break;
    case Kind::VoiceRelay:
        if (!copy.voiceRelay.subjectAvatar || !copy.voiceRelay.subjectIncarnation || !validateVoice(copy.voiceRelay.voice))
            return false;
        detail::writeVoice(bytes, copy.voiceRelay.voice);
        detail::put32(bytes, copy.voiceRelay.subjectAvatar);
        detail::put32(bytes, copy.voiceRelay.subjectIncarnation);
        break;
    default:
        return false;
    }
    carrier = std::string(CarrierTag) + detail::base64(bytes);
    return carrier.size() <= MaxCarrierAscii;
}

inline ParseResult parse(std::string_view carrier, Message &message) {
    if (!carrier.starts_with(CarrierTag))
        return ParseResult::NotMod;
    if (carrier.size() > MaxCarrierAscii)
        return ParseResult::Malformed;
    for (char c : carrier)
        if (static_cast<unsigned char>(c) > 0x7f)
            return ParseResult::Malformed;
    std::vector<uint8_t> bytes;
    if (!detail::unbase64(carrier.substr(CarrierTag.size()), bytes))
        return ParseResult::Malformed;
    detail::Reader r(bytes);
    uint8_t version = 0, kind = 0;
    uint16_t reserved = 0;
    if (!r.u8(version) || !r.u8(kind) || !r.u16(reserved) || version != WireVersion || reserved != 0)
        return ParseResult::Malformed;
    Message parsed{};
    switch (static_cast<Kind>(kind)) {
    case Kind::Hello:
        parsed.kind = Kind::Hello;
        if (!r.u64(parsed.hello.nonce) || !parsed.hello.nonce)
            return ParseResult::Malformed;
        break;
    case Kind::Ack:
        parsed.kind = Kind::Ack;
        if (!r.u64(parsed.ack.clientNonce) || !r.u64(parsed.ack.serverNonce) ||
            !r.u32(parsed.ack.acceptedSequence) || !r.u32(parsed.ack.intentEpoch[0]) ||
            !r.u32(parsed.ack.intentEpoch[1]) || !parsed.ack.clientNonce || !parsed.ack.serverNonce)
            return ParseResult::Malformed;
        break;
    case Kind::Pose:
        parsed.kind = Kind::Pose;
        if (!detail::readPosePacket(r, parsed.pose) || !validatePose(parsed.pose))
            return ParseResult::Malformed;
        break;
    case Kind::Relay:
        parsed.kind = Kind::Relay;
        if (!detail::readPosePacket(r, parsed.relay.pose) || !r.u32(parsed.relay.subjectAvatar) ||
            !r.u32(parsed.relay.subjectIncarnation) || !parsed.relay.subjectAvatar ||
            !parsed.relay.subjectIncarnation || !validatePose(parsed.relay.pose))
            return ParseResult::Malformed;
        break;
    case Kind::Voice:
        parsed.kind = Kind::Voice;
        if (!detail::readVoice(r, parsed.voice) || !validateVoice(parsed.voice))
            return ParseResult::Malformed;
        break;
    case Kind::VoiceRelay:
        parsed.kind = Kind::VoiceRelay;
        if (!detail::readVoice(r, parsed.voiceRelay.voice) || !r.u32(parsed.voiceRelay.subjectAvatar) ||
            !r.u32(parsed.voiceRelay.subjectIncarnation) || !parsed.voiceRelay.subjectAvatar ||
            !parsed.voiceRelay.subjectIncarnation || !validateVoice(parsed.voiceRelay.voice))
            return ParseResult::Malformed;
        break;
    default:
        return ParseResult::Malformed;
    }
    if (!r.done())
        return ParseResult::Malformed;
    message = parsed;
    return ParseResult::Valid;
}

inline bool newer(uint32_t value, uint32_t previous) {
    return int32_t(value - previous) > 0;
}

// Stores only peer-provided codec state.  Its caller owns player identity,
// incarnation, authority, and when a weapon/inventory generation changes.
struct PeerState {
    uint64_t clientNonce = 0;
    uint64_t serverNonce = 0;
    uint32_t lastSequence = 0;
    uint32_t trackingGeneration = 0;
    uint32_t weaponGeneration = 0;
    uint64_t lastPoseLocalTick = 0;
    uint32_t observedReleasedSerial[2]{};
    uint32_t observedPrimaryInputGeneration[2]{};
    uint32_t observedZoomReleasedSerial[2]{};
    uint32_t observedZoomInputGeneration[2]{};
    uint32_t intentEpoch[2]{};
    bool physicalReleaseObserved[2]{};
    bool zoomReleaseObserved[2]{};
    GestureAdmission gesture[2];
    bool hasPose = false;

    void resetCapability() {
        clientNonce = serverNonce = 0;
        lastSequence = trackingGeneration = weaponGeneration = 0;
        lastPoseLocalTick = 0;
        observedReleasedSerial[0] = observedReleasedSerial[1] = 0;
        observedPrimaryInputGeneration[0] = observedPrimaryInputGeneration[1] = 0;
        observedZoomReleasedSerial[0] = observedZoomReleasedSerial[1] = 0;
        observedZoomInputGeneration[0] = observedZoomInputGeneration[1] = 0;
        intentEpoch[0] = intentEpoch[1] = 0;
        physicalReleaseObserved[0] = physicalReleaseObserved[1] = false;
        zoomReleaseObserved[0] = zoomReleaseObserved[1] = false;
        gesture[0] = gesture[1] = {};
        hasPose = false;
    }
    bool bindCapability(uint64_t client, uint64_t server) {
        if (!client || !server)
            return false;
        resetCapability();
        clientNonce = client;
        serverNonce = server;
        intentEpoch[0] = intentEpoch[1] = 1;
        return true;
    }
    void invalidateTracking() {
        // Sequence is connection-wide, not a tracking-generation counter. Retain
        // it across tracking loss so an old generation cannot revive old input.
        lastPoseLocalTick = 0;
        hasPose = false;
        physicalReleaseObserved[0] = physicalReleaseObserved[1] = false;
        zoomReleaseObserved[0] = zoomReleaseObserved[1] = false;
        for (auto &hand : gesture) hand.requireQuiet();
    }
    void invalidateWeaponGeneration(uint32_t generation, uint8_t handMask = HandMask) {
        weaponGeneration = generation;
        for (unsigned hand = 0; hand < 2; ++hand)
            if (handMask & (1u << hand)) {
                // UINT32_MAX has no successor in this capability: fail closed
                // rather than reusing epoch 1 after wrap.
                if (intentEpoch[hand] == UINT32_MAX)
                    intentEpoch[hand] = 0;
                else if (intentEpoch[hand])
                    ++intentEpoch[hand];
                physicalReleaseObserved[hand] = false;
                zoomReleaseObserved[hand] = false;
                gesture[hand].requireQuiet();
            }
    }
    // Clears filtered weapon intent, preserving raw action state for the next
    // genuinely eligible neutral sample.  It is deliberately safe at receive,
    // freeze, and use boundaries.
    void filterWeaponIntents(PosePacket &pose, uint64_t now = 0) const {
        for (unsigned hand = 0; hand != 2; ++hand) {
            const uint8_t bit = uint8_t(1u << hand);
            if (!matchesIntentEpoch(pose, hand, intentEpoch[hand])) {
                invalidateWeaponIntents(pose, bit);
                cancelWeaponRequests(pose, bit);
                continue;
            }
            if (!gesture[hand].current(pose, hand, now)) invalidateGestureIntents(pose, bit);
            if (!physicalReleaseObserved[hand] ||
                pose.primaryInputGeneration[hand] != observedPrimaryInputGeneration[hand]) {
                pose.fireMask &= uint8_t(~bit);
                pose.pulseMask &= uint8_t(~bit);
                pose.pulseZoomMask &= uint8_t(~bit);
            }
            if (!zoomReleaseObserved[hand] ||
                pose.zoomInputGeneration[hand] != observedZoomInputGeneration[hand]) {
                pose.zoomMask &= uint8_t(~bit);
                if (pose.pulseZoomMask & bit) {
                    pose.fireMask &= uint8_t(~bit);
                    pose.pulseMask &= uint8_t(~bit);
                    pose.pulseZoomMask &= uint8_t(~bit);
                }
            }
        }
    }
    bool acceptPose(PosePacket &pose, uint64_t localTick) {
        if (pose.clientNonce != clientNonce || pose.serverNonce != serverNonce || !validatePose(pose))
            return false;
        if (lastSequence && !newer(pose.sequence, lastSequence))
            return false;
        if (trackingGeneration && pose.trackingGeneration != trackingGeneration &&
            !newer(pose.trackingGeneration, trackingGeneration))
            return false;
        if (trackingGeneration != pose.trackingGeneration || (hasPose && !fresh(localTick)))
            invalidateTracking();
        trackingGeneration = pose.trackingGeneration;
        // Stale/zero epochs cannot advance either raw release baseline.
        for (unsigned hand = 0; hand != 2; ++hand)
            if (!intentEpoch[hand] || pose.intentEpoch[hand] != intentEpoch[hand])
                invalidateWeaponIntents(pose, uint8_t(1u << hand));
        lastSequence = pose.sequence;
        lastPoseLocalTick = localTick;
        hasPose = true;
        for (unsigned hand = 0; hand != 2; ++hand) {
            if (!intentEpoch[hand] || pose.intentEpoch[hand] != intentEpoch[hand])
                continue;
            updatePrimaryAdmission(pose, hand);
            updateZoomAdmission(pose, hand);
            if (!gesture[hand].observe(pose, hand, localTick))
                invalidateGestureIntents(pose, uint8_t(1u << hand));
            if (pose.gesturePulseMask & (1u << hand)) {
                if (pose.gestureSequence[hand] <= gesture[hand].lastPulseSequence)
                    pose.gesturePulseMask &= uint8_t(~(1u << hand));
                else gesture[hand].lastPulseSequence = pose.gestureSequence[hand];
            }
        }
        filterWeaponIntents(pose);
        return true;
    }
    bool fresh(uint64_t localTick) const {
        return hasPose && localTick >= lastPoseLocalTick && localTick - lastPoseLocalTick <= MaxPoseAgeMs;
    }
    bool physicalFireAllowed(unsigned hand) const {
        return hand < 2 && physicalReleaseObserved[hand];
    }
    bool zoomAllowed(unsigned hand) const {
        return hand < 2 && zoomReleaseObserved[hand];
    }

  private:
    void updatePrimaryAdmission(const PosePacket &pose, unsigned hand) {
        const uint8_t bit = uint8_t(1u << hand);
        const uint32_t generation = pose.primaryInputGeneration[hand];
        if (generation && generation < observedPrimaryInputGeneration[hand])
            return; // A retired stream cannot relabel or poison current witnesses.
        if (generation && generation != observedPrimaryInputGeneration[hand]) {
            observedPrimaryInputGeneration[hand] = generation;
            observedReleasedSerial[hand] = 0;
            physicalReleaseObserved[hand] = false;
        }
        if (!(pose.primarySampleEligibleMask & bit) || !generation || !(pose.validMask & 1) ||
            !(pose.validMask & (bit << 1)) || (pose.wheelOrEquipBlockedMask & bit)) {
            physicalReleaseObserved[hand] = false;
            return;
        }
        const bool serialAdvanced = pose.releasedSerial[hand] > observedReleasedSerial[hand];
        if (serialAdvanced)
            observedReleasedSerial[hand] = pose.releasedSerial[hand];
        if ((pose.primaryNeutralSampleMask & bit) && !(pose.physicalDownMask & bit) && serialAdvanced)
            physicalReleaseObserved[hand] = true;
    }
    void updateZoomAdmission(const PosePacket &pose, unsigned hand) {
        const uint8_t bit = uint8_t(1u << hand);
        const uint32_t generation = pose.zoomInputGeneration[hand];
        if (generation && generation < observedZoomInputGeneration[hand])
            return;
        if (generation && generation != observedZoomInputGeneration[hand]) {
            observedZoomInputGeneration[hand] = generation;
            observedZoomReleasedSerial[hand] = 0;
            zoomReleaseObserved[hand] = false;
        }
        if (!(pose.zoomSampleEligibleMask & bit) || !generation || !(pose.validMask & 1) ||
            !(pose.validMask & (bit << 1)) || (pose.wheelOrEquipBlockedMask & bit)) {
            zoomReleaseObserved[hand] = false;
            return;
        }
        const bool serialAdvanced = pose.zoomReleasedSerial[hand] > observedZoomReleasedSerial[hand];
        if (serialAdvanced)
            observedZoomReleasedSerial[hand] = pose.zoomReleasedSerial[hand];
        if (!(pose.zoomPhysicalDownMask & bit) && serialAdvanced)
            zoomReleaseObserved[hand] = true;
    }
};

// The native adapter owns the scheduler boundary. This policy owns only the
// single immutable receive slot and its exact consumption/discard token.
struct OrderedPosePolicy {
    PeerState validation;
    PosePacket pending, active;
    bool hasPending = false, hasActive = false, awaitingConsumption = false;
    // Transport negotiation is also used by headset-free mod clients. Only a
    // validated, accepted tracked head can transfer native gameplay ownership.
    // Tracking loss retires intent, not ownership; the native avatar lifecycle
    // must reset this fact before a different pawn can claim it.
    bool xrGameplayOwned = false;
    uint64_t pendingReceivedMs = 0, activeReceivedMs = 0;

    bool gameplayOwned() const noexcept {
        return xrGameplayOwned && validation.serverNonce != 0;
    }
    bool relayRecipientReady(uint64_t now) const noexcept {
        // Desktop participants still receive tracked peers' presentation.
        return validation.serverNonce && validation.fresh(now);
    }
    bool retainGameplayOwnership(uint32_t oldBrain, uint32_t oldAvatar,
                                 uint32_t newBrain, uint32_t newAvatar) const noexcept {
        return xrGameplayOwned && oldBrain && oldAvatar &&
               oldBrain == newBrain && oldAvatar == newAvatar;
    }

    static Ack token(const PosePacket &pose) {
        return {pose.clientNonce, pose.serverNonce, pose.sequence};
    }
    // False means explicit discard; the caller must send discard reliably.
    bool receive(const PosePacket &incoming, uint64_t now, Ack &discard, bool permitted = true) {
        PosePacket pose = incoming;
        discard = token(pose);
        if (!permitted || hasPending || awaitingConsumption || !validation.acceptPose(pose, now))
            return false;
        if (pose.validMask & 1u)
            xrGameplayOwned = true;
        pending = pose;
        pendingReceivedMs = now;
        hasPending = true;
        return true;
    }
    bool freeze(uint64_t now, PosePacket &sample, PosePacket *relay = nullptr) {
        if (hasPending) {
            active = pending;
            activeReceivedMs = pendingReceivedMs;
            hasActive = awaitingConsumption = true;
            hasPending = false;
        }
        validation.filterWeaponIntents(active, now);
        const uint8_t ordinaryFire = active.fireMask;
        sample = active;
        sample.fireMask |= sample.pulseMask;
        active.pulseMask = 0;
        active.pulseZoomMask = 0;
        active.gesturePulseMask = 0; // One interval; never expands manual fire or gesture held level.
        for (unsigned hand = 0; hand != 2; ++hand)
            if (!validation.physicalFireAllowed(hand)) {
                sample.fireMask &= uint8_t(~(1u << hand));
                // Rejected historical fire has no damage/zoom context to apply.
                // Keep the latest held zoom intent for this hand unchanged.
                sample.pulseMask &= uint8_t(~(1u << hand));
                sample.pulseZoomMask &= uint8_t(~(1u << hand));
            }
        const bool valid = hasActive && validation.fresh(now) && now >= activeReceivedMs &&
                           now - activeReceivedMs <= MaxPoseAgeMs;
        if (relay) {
            // The wire keeps an accepted historical pulse distinct from live
            // physical-down fire. Capture this interval's filtered pulse before
            // active's one-use pulse is lost; never serialize expanded fire.
            *relay = valid ? sample : PosePacket{};
            relay->fireMask &= ordinaryFire;
        }
        return valid;
    }
    bool peekInterval(Ack &consumed) const noexcept {
        if (!awaitingConsumption)
            return false;
        consumed = token(active);
        consumed.intentEpoch[0] = validation.intentEpoch[0];
        consumed.intentEpoch[1] = validation.intentEpoch[1];
        return true;
    }
    bool finishInterval(Ack &consumed) {
        if (!peekInterval(consumed))
            return false;
        awaitingConsumption = false;
        return true;
    }
    bool confirmInterval(const Ack &submitted) noexcept {
        if (!awaitingConsumption || active.clientNonce != submitted.clientNonce ||
            active.serverNonce != submitted.serverNonce || active.sequence != submitted.acceptedSequence)
            return false;
        awaitingConsumption = false;
        return true;
    }
    // Native simulation did not finish this interval. Keep its exact existing
    // consumption/discard token until a normal native boundary can send it;
    // neither this active intent nor a pre-interruption neutral may revive it.
    void abortInterval() noexcept {
        validation.invalidateTracking();
        validation.invalidateWeaponGeneration(validation.weaponGeneration);
        hasActive = false;
        invalidateWeaponIntents(active, HandMask);
        cancelWeaponRequests(active, HandMask);
    }
    bool discardPending(Ack &discard) {
        if (!hasPending)
            return false;
        discard = token(pending);
        hasPending = false;
        return true;
    }
};

} // namespace ss2vr::network
