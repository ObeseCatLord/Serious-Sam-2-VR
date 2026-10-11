#include "common/frame_policy.hpp"
#include <cstdlib>
#include <iostream>
#include <memory>
using namespace ss2vr;
static void check(bool value, const char *message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
int main() {
    for(uint64_t request : {uint64_t{0},uint64_t{1},uint64_t{2},UINT64_MAX}) {
        for(bool enabled : {false,true}) {
            const int a=labWorldEyeForPass(0,request,enabled),b=labWorldEyeForPass(1,request,enabled);
            check(a>=0 && a<2 && b==1-a,"World passes must consume both distinct physical eye slots");
            check(a==int(enabled && (request&1)),"Only sealed alternate mode can change temporal order");
            const int cameras[2]{11,22},targets[2]{101,202},projections[2]{1001,2002};
            int copied[2]{};
            for(unsigned pass=0;pass<2;++pass) {
                const int i=labWorldEyeForPass(pass,request,enabled);
                copied[i]=cameras[i]+targets[i]+projections[i];
            }
            check(copied[0]==1113 && copied[1]==2226,"Temporal reordering cannot swap camera/target/projection identity");
        }
    }
    check(labWorldEyeForPass(2,1,true)==-1,"Unknown world pass must not alias an eye");
    TrackingEpochs epochs;
    auto first = epochs.advance(), respawn = epochs.advance();
    check(first && respawn > first, "Snapshot reset/respawn cannot reuse the process epoch");
    TrackingEpochs exhausted(ExhaustedEpoch - 1);
    check(!exhausted.advance() && !exhausted.advance(), "Epoch exhaustion cannot wrap or resume");
    check(mayPublishSnapshot(first, first, 0) && !mayPublishSnapshot(first, respawn, first) &&
              !mayPublishSnapshot(first, first, respawn),
          "A copied pre-deletion snapshot cannot overwrite either published or native invalidation");
    auto slots = std::make_unique<Slot[]>(2);
    Request request{};
    request.sequence = 9;
    request.predictedTime = 1000000;
    request.session = request.input.session = 1;
    request.reference = request.input.reference = 2;
    request.trackingGeneration = respawn;
    request.width = request.height = 2;
    request.input.focused = request.input.headValid = 1;
    request.eye[0].p.x = -.032f;
    request.eye[1].p.x = .032f;
    FrameContext context{50, 0, 1, 2, respawn, respawn, 2, 2, true, true, true, true};
    PendingRequest pending{true, false, false, Expiry::None, 150, request};
    auto &slot = slots[0];
    slot.request = request;
    slot.state = SlotState::Ready;
    slot.pixelGeneration = 7;
    check(pollSlot(slot, pending, context) == PollResult::Ready,
          "Complete pair observed after 50ms must survive the former 35ms deadline");
    Request reply = slot.request;
    for (unsigned field=0;field<6;++field) {
        auto changed=request;
        if(field==0) changed.uiRequested=1;
        if(field==1) changed.uiWidth=1.8f;
        if(field==2) changed.uiHeight=1.f;
        if(field==3) changed.uiPanel.p.z=-2.f;
        if(field==4) changed.uiPanel.q.x=.01f;
        if(field==5) changed.uiWidth=-0.f;
        check(!sameRequest(changed,request),"Frozen native UI geometry belongs to immutable pair identity");
    }
    for (unsigned hand=0;hand<2;++hand) {
        auto changed=request;
        changed.input.primaryInputGeneration[hand]=1;
        check(!sameRequest(changed,request),"Logical primary stream generation cannot relabel captured UI input");
        changed=request;
        changed.input.zoomInputGeneration[hand]=1;
        check(!sameRequest(changed,request),"Logical zoom stream generation belongs to immutable request");
    }
    check(sameRequest(reply, request) && slot.pixelGeneration == 7 &&
              reply.eye[0].p.x != reply.eye[1].p.x,
          "Accepted eyes and metadata belong to the same immutable stereo request");
    slot.state = SlotState::Empty;
    pending = {};
    check(pollSlot(slot, pending, context) == PollResult::None, "Consumed Ready cannot be delivered twice");
    pending = {true, false, false, Expiry::None, 150, request};
    slot.state = SlotState::Ready;
    context.now = 150;
    check(pollSlot(slot, pending, context) == PollResult::AgeExpired && slot.state == SlotState::Empty,
          "Age boundary rejects Ready and retires only host-owned output");
    context.now = 50;
    pending = {true, false, false, Expiry::None, 150, request};
    slot.state = SlotState::Ready;
    context.epoch++;
    check(pollSlot(slot, pending, context) == PollResult::Invalidated && slot.state == SlotState::Empty,
          "Early atomic invalidation defeats matching but stale mutex-protected UI");
    context.uiEpoch = context.epoch;
    check(!eligibleFrame(reply, 150, context), "Prior cached pair cannot survive a calibration epoch change");
    context.epoch = context.uiEpoch = respawn;
    pending = {true, false, false, Expiry::None, 150, request};
    slot.state = SlotState::Rendering;
    context.now = 150;
    check(pollSlot(slot, pending, context) == PollResult::AgeExpired && slot.cancelled && pending.active &&
              slot.state == SlotState::Rendering && nativeTransactionPending(slots.get()),
          "Cancelled Rendering remains owned and blocks admission into the other empty slot");
    check(pollSlot(slot, pending, context) == PollResult::None,
          "Repeated polling counts the same expiration only once");
    slot.state = SlotState::Ready;
    check(pollSlot(slot, pending, context) == PollResult::Retired && slot.state == SlotState::Empty &&
              !pending.active,
          "Cancellation before native completion safely retires subsequent Ready");
    pending = {true, true, false, Expiry::Invalidation, 150, request};
    slot.state = SlotState::Ready;
    check(pollSlot(slot, pending, context) == PollResult::Invalidated && slot.state == SlotState::Empty,
          "Native completion before lifecycle cancellation has the same safe outcome");
    context.now = 60;
    check(eligibleFrame(reply, 150, context),
          "Complete valid stereo can be reprojected between native captures");
    context.focused = false;
    check(!eligibleFrame(reply, 150, context), "Focus loss hides cached projection immediately");
    check(nativeFrameIdentity(request, request.input, respawn, true, true, true) &&
              !nativeFrameIdentity(request, request.input, respawn + 1, true, true, true),
          "Native begin/completion identity rejects invalidation after a frame begins");
    slot.request = request;
    slot.state = SlotState::Rendering;
    slot.cancelled = 0;
    check(!commitNativeFrame(slot, request, false) && slot.state == SlotState::Rendering,
          "Invalidation serialized before completion prevents a Ready commit");
    check(commitNativeFrame(slot, request, true) && slot.state == SlotState::Ready,
          "Completion serialized first produces an owned Ready pair");
    pending = {true, false, false, Expiry::None, 150, request};
    context.focused = true;
    context.epoch++;
    check(pollSlot(slot, pending, context) == PollResult::Invalidated && slot.state == SlotState::Empty,
          "Invalidation serialized after Ready prevents host delivery");
    context.epoch = context.uiEpoch = respawn;
    ImageOwnership left, right;
    left.index = 1;
    left.acquired = left.waited = true;
    left.didRelease();
    right.didRelease();
    check(left.previousImageAvailable() && right.previousImageAvailable(),
          "A fully released pair can be reused without acquiring its cached image");
    left.index = left.releasedIndex;
    left.acquired = left.waited = true;
    right.index = 1;
    right.acquired = true; // The asymmetric right wait timed out.
    check(!bothImagesReady(left, right) && !left.previousImageAvailable() && right.previousImageAvailable(),
          "An asymmetric wait cannot overwrite either eye; cached-index acquisition hides projection");
    slot.request = request;
    slot.state = SlotState::Ready;
    pending = {true, false, false, Expiry::None, 150, request};
    check(pollSlot(slot, pending, context) == PollResult::Ready,
          "Image wait timeout preserves the complete IPC pair for retry");
    right.waited = true;
    check(bothImagesReady(left, right), "A retry waits the same acquisition before permitting writes");
    context.now = 150;
    check(pollSlot(slot, pending, context) == PollResult::AgeExpired,
          "A Ready pair expiring during the waits is abandoned without upload");
    left.didRelease();
    right.didRelease();
    check(!left.acquired && !right.acquired, "No-write abandonment retires both waited acquisitions");
    left.acquired = left.waited = right.acquired = right.waited = true;
    check(!left.previousImageAvailable() && !right.previousImageAvailable(),
          "Unreleased cached-index acquisitions remain unavailable for projection");
    left.didRelease();
    check(left.previousImageAvailable() && !right.previousImageAvailable() && right.acquired,
          "One released eye cannot make an unreleased cached-index pair available");
    right.didRelease();
    check(!left.acquired && !right.acquired, "Only two successful releases finish the transaction");
    std::cout << "Frame lifetime, cancellation, epoch and immutable stereo checks passed\n";
}
