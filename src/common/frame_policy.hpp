#pragma once
#include "protocol.hpp"
#include <atomic>
#include <bit>
#include <cstring>
#include <limits>
namespace ss2vr {
constexpr uint64_t FrameAgeMs = 150, UiAgeMs = 250;
constexpr uint32_t ExhaustedEpoch = std::numeric_limits<uint32_t>::max();
// Private observation only: physical eye identity always indexes its own
// camera/target/projection/readback. Never swap the request or output arrays.
constexpr int labWorldEyeForPass(unsigned pass,uint64_t request,bool alternate) {
    if(pass>=2)return -1;
    return alternate && (request&1) ? 1-int(pass) : int(pass);
}
inline bool validTrackingEpoch(uint32_t epoch) {
    return epoch && epoch != ExhaustedEpoch;
}
inline bool mayPublishSnapshot(uint32_t captured, uint32_t published, uint32_t nativeCurrent) {
    return validTrackingEpoch(captured) && captured == published && captured >= nativeCurrent;
}
struct ImageOwnership {
    uint32_t index = 0, releasedIndex = 0;
    bool acquired = false, waited = false, released = false;
    bool previousImageAvailable() const {
        return released && !(acquired && index == releasedIndex);
    }
    void didRelease() {
        releasedIndex = index;
        released = true;
        acquired = waited = false;
    }
};
inline bool bothImagesReady(const ImageOwnership &left, const ImageOwnership &right) {
    return left.acquired && left.waited && right.acquired && right.waited;
}
class TrackingEpochs {
    std::atomic<uint32_t> last;

  public:
    explicit TrackingEpochs(uint32_t initial = 0) : last(initial) {}
    uint32_t advance() {
        uint32_t old = last.load(std::memory_order_relaxed);
        while (old != ExhaustedEpoch)
            if (last.compare_exchange_weak(old, old + 1, std::memory_order_relaxed))
                return old + 1 == ExhaustedEpoch ? 0 : old + 1;
        return 0; // Exhaustion is terminal: no wrapped/reused identity.
    }
};
inline bool sameInput(const Input &a, const Input &b) {
    return a.sequence == b.sequence && a.tickMs == b.tickMs && a.session == b.session &&
           a.reference == b.reference && a.focused == b.focused && a.headValid == b.headValid &&
           !std::memcmp(&a.head, &b.head, sizeof(a.head)) && !std::memcmp(a.hand, b.hand, sizeof(a.hand)) &&
           !std::memcmp(a.handValid, b.handValid, sizeof(a.handValid)) &&
           !std::memcmp(a.trigger, b.trigger, sizeof(a.trigger)) &&
           !std::memcmp(a.axis, b.axis, sizeof(a.axis)) &&
           !std::memcmp(a.buttons, b.buttons, sizeof(a.buttons)) &&
           !std::memcmp(a.grip, b.grip, sizeof(a.grip)) &&
           !std::memcmp(a.gripValid, b.gripValid, sizeof(a.gripValid)) && a.blockedWheels == b.blockedWheels &&
           a.wheelAdmissionMask == b.wheelAdmissionMask &&
           !std::memcmp(a.wheelInputEpoch, b.wheelInputEpoch, sizeof(a.wheelInputEpoch)) &&
           a.primaryActiveMask == b.primaryActiveMask && a.zoomActiveMask == b.zoomActiveMask &&
           a.zoomDownMask == b.zoomDownMask &&
           !std::memcmp(a.primaryInputGeneration,b.primaryInputGeneration,sizeof(a.primaryInputGeneration)) &&
           !std::memcmp(a.zoomInputGeneration,b.zoomInputGeneration,sizeof(a.zoomInputGeneration)) &&
           !std::memcmp(a.zoomSourceButton,b.zoomSourceButton,sizeof(a.zoomSourceButton));
}
inline bool sameRequest(const Request &a, const Request &b) {
    return a.sequence == b.sequence && a.predictedTime == b.predictedTime && a.session == b.session &&
           a.reference == b.reference && a.width == b.width && a.height == b.height &&
           a.trackingGeneration == b.trackingGeneration && a.reserved == b.reserved &&
           !std::memcmp(a.eye, b.eye, sizeof(a.eye)) && !std::memcmp(a.fov, b.fov, sizeof(a.fov)) &&
           sameInput(a.input, b.input) && a.uiRequested == b.uiRequested &&
           !std::memcmp(&a.uiPanel,&b.uiPanel,sizeof(a.uiPanel)) &&
           std::bit_cast<uint32_t>(a.uiWidth) == std::bit_cast<uint32_t>(b.uiWidth) &&
           std::bit_cast<uint32_t>(a.uiHeight) == std::bit_cast<uint32_t>(b.uiHeight) &&
           a.foveated == b.foveated && a.lowWidth == b.lowWidth && a.lowHeight == b.lowHeight &&
           a.insetWidth == b.insetWidth && a.insetHeight == b.insetHeight &&
           a.foveationReserved == b.foveationReserved && !std::memcmp(a.insetCentre, b.insetCentre, sizeof(a.insetCentre));
}
struct FrameContext {
    uint64_t now = 0, uiTick = 0;
    uint32_t session = 0, reference = 0, epoch = 0, uiEpoch = 0, width = 0, height = 0;
    bool renderer = false, gameplay = false, focused = false, headValid = false;
};
inline bool eligibleFrame(const Request &r, uint64_t deadline, const FrameContext &c) {
    return r.sequence && r.session && r.reference && validTrackingEpoch(r.trackingGeneration) &&
           r.reserved == 0 && c.renderer && c.gameplay && c.focused && c.headValid && c.now < deadline &&
           c.now >= c.uiTick && c.now - c.uiTick <= UiAgeMs && r.session == c.session &&
           r.reference == c.reference && r.trackingGeneration == c.epoch && c.epoch == c.uiEpoch &&
           r.width == c.width && r.height == c.height && r.input.session == r.session &&
           r.input.reference == r.reference && r.input.focused && r.input.headValid;
}
// Display only: a pair that was shown may stay up a little past its age limit
// and across a tracking-epoch change (weapon switch, snap turn, recentre) until
// the next pair arrives, instead of the frame going black. Session, reference,
// focus, head tracking, gameplay and size still have to hold.
constexpr uint64_t FrameHoldMs = 250;
inline bool holdableFrame(const Request &r, uint64_t deadline, const FrameContext &c) {
    return r.sequence && r.session && r.reference && validTrackingEpoch(r.trackingGeneration) && r.reserved == 0 &&
           c.renderer && c.gameplay && c.focused && c.headValid && c.now < deadline + FrameHoldMs &&
           c.now >= c.uiTick && c.now - c.uiTick <= UiAgeMs && r.session == c.session && r.reference == c.reference &&
           validTrackingEpoch(c.epoch) && r.width == c.width && r.height == c.height && r.input.session == r.session &&
           r.input.reference == r.reference && r.input.focused && r.input.headValid;
}
inline bool nativeFrameIdentity(const Request &r, const Input &nativeInput, uint32_t epoch, bool initialized,
                                bool gameplay, bool samePlayer) {
    return initialized && gameplay && samePlayer && validTrackingEpoch(epoch) &&
           r.trackingGeneration == epoch && r.session == nativeInput.session &&
           r.reference == nativeInput.reference && r.input.session == r.session &&
           r.input.reference == r.reference && r.input.focused && r.input.headValid && !r.reserved;
}
// Caller holds both slot ownership and the native snapshot lock through this commit.
inline bool commitNativeFrame(Slot &slot, const Request &request, bool current) {
    if (!current || slot.cancelled || slot.state != SlotState::Rendering ||
        !sameRequest(slot.request, request))
        return false;
    slot.state = SlotState::Ready;
    return true;
}
enum class Expiry { None, Age, Invalidation };
struct PendingRequest {
    bool active = false, expired = false, expiryReported = false;
    Expiry expiry = Expiry::None;
    uint64_t deadline = 0;
    Request request;
};
enum class PollResult { None, Ready, Retired, AgeExpired, Invalidated, IdentityError };
inline PollResult pollSlot(Slot &slot, PendingRequest &pending, const FrameContext &context) {
    if (!pending.active)
        return slot.state == SlotState::Empty ? PollResult::None : PollResult::IdentityError;
    if (slot.state == SlotState::Empty) {
        pending = {};
        return PollResult::Retired;
    }
    if (!sameRequest(slot.request, pending.request))
        return PollResult::IdentityError;
    if (!pending.expired && !eligibleFrame(pending.request, pending.deadline, context)) {
        pending.expired = true;
        pending.expiry = context.now >= pending.deadline ? Expiry::Age : Expiry::Invalidation;
    }
    PollResult result = PollResult::None;
    if (pending.expired && !pending.expiryReported) {
        pending.expiryReported = true;
        result = pending.expiry == Expiry::Age ? PollResult::AgeExpired : PollResult::Invalidated;
    }
    switch (slot.state) {
    case SlotState::Requested:
    case SlotState::Ready:
        if (pending.expired || slot.cancelled) {
            slot.state = SlotState::Empty;
            slot.cancelled = 0;
            pending = {};
            return result == PollResult::None ? PollResult::Retired : result;
        }
        return slot.state == SlotState::Ready ? PollResult::Ready : result;
    case SlotState::Rendering:
        if (pending.expired)
            slot.cancelled = 1; // Only the native renderer can retire Rendering.
        return result;
    default:
        return PollResult::IdentityError;
    }
}
inline bool nativeTransactionPending(const Slot *slots) {
    for (unsigned i = 0; i < 2; i++)
        if (slots[i].state == SlotState::Requested || slots[i].state == SlotState::Rendering)
            return true; // Includes cancelled Rendering, regardless of age.
    return false;
}
} // namespace ss2vr
