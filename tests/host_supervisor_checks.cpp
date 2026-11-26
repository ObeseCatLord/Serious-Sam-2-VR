#include "common/controls.hpp"
#include "common/frame_policy.hpp"
#include "common/host_supervisor.hpp"
#include "common/session_focus.hpp"
#include <cassert>
#include <cstdio>
#include <memory>
using namespace ss2vr;
namespace {
HostObservation at(uint64_t now, bool spawned, bool alive, uint32_t pid, uint32_t error, uint32_t session) {
    HostObservation o;
    o.now = now; o.spawned = spawned; o.alive = alive; o.hostPid = pid; o.error = error; o.session = session;
    return o;
}
// A supervisor whose host connected and began a session at t = 1000..3000.
HostSupervisor running() {
    HostSupervisor s;
    assert(s.step(at(1000, true, true, 0, 0, 0)) == HostStep::None);
    assert(s.step(at(2000, true, true, 77, 0, 0)) == HostStep::None);
    assert(!s.vrStarted);
    assert(s.step(at(3000, true, true, 77, 0, 1)) == HostStep::None);
    assert(s.vrStarted && !s.desktop);
    return s;
}
void beforeVr() {
    { // The host reports an error before any session (no runtime, no headset): desktop play.
        HostSupervisor s;
        assert(s.step(at(1000, true, true, 0, 0, 0)) == HostStep::None);
        assert(s.step(at(1500, true, true, 77, 0, 0)) == HostStep::None);
        assert(s.step(at(2000, true, true, 0, 1, 0)) == HostStep::Desktop && s.desktop);
        assert(s.step(at(2100, true, false, 0, 1, 0)) == HostStep::None); // One-way, reported once.
        assert(s.step(at(90000, true, true, 78, 0, 1)) == HostStep::None && s.desktop);
    }
    { // It connected, never ran a session and left.
        HostSupervisor s;
        assert(s.step(at(1000, true, true, 77, 0, 0)) == HostStep::None);
        assert(s.step(at(5000, true, true, 0, 0, 0)) == HostStep::Desktop);
    }
    { // It exited without connecting (crash at start).
        HostSupervisor s;
        assert(s.step(at(1000, true, true, 0, 0, 0)) == HostStep::None);
        assert(s.step(at(1200, true, false, 0, 0, 0)) == HostStep::Desktop);
    }
    { // No host process (not created; the lab's no-host switch): 30 s, as before.
        HostSupervisor s;
        assert(s.step(at(1000, false, false, 0, 0, 0)) == HostStep::None);
        assert(s.step(at(31000, false, false, 0, 0, 0)) == HostStep::None);
        assert(s.step(at(31001, false, false, 0, 0, 0)) == HostStep::Desktop);
    }
    { // A process that never opens the channel: 30 s too.
        HostSupervisor s;
        assert(s.step(at(1000, true, true, 0, 0, 0)) == HostStep::None);
        assert(s.step(at(30000, true, true, 0, 0, 0)) == HostStep::None);
        assert(s.step(at(31500, true, true, 0, 0, 0)) == HostStep::Desktop);
    }
    { // Connected and waiting for the runtime to start the session: VR stays expected.
        HostSupervisor s;
        assert(s.step(at(1000, true, true, 77, 0, 0)) == HostStep::None);
        assert(s.step(at(600000, true, true, 77, 0, 0)) == HostStep::None && !s.desktop && !s.vrStarted);
        assert(s.step(at(600100, true, true, 77, 0, 1)) == HostStep::None && s.vrStarted);
    }
}
void runtimeStarting() {
    { // SteamVR started by the host restarts once: the host reports "still starting", a
      // new host follows and VR runs.
        HostSupervisor s;
        assert(s.step(at(1000, true, true, 77, 0, 0)) == HostStep::None);
        assert(s.step(at(4000, true, true, 0, HostErrorRuntimeStarting, 0)) == HostStep::Stopped && !s.desktop);
        assert(s.retrying && s.runtimeRetries == 1);
        assert(s.step(at(4500, true, false, 0, HostErrorRuntimeStarting, 0)) == HostStep::None); // Delay 1 s.
        assert(s.step(at(5000, true, false, 0, HostErrorRuntimeStarting, 0)) == HostStep::Restart);
        s.restarted(5000, true);
        assert(s.step(at(5100, true, true, 0, 0, 0)) == HostStep::None);   // Starting, not "exited".
        assert(s.step(at(6000, true, true, 78, 0, 0)) == HostStep::None && !s.retrying);
        assert(s.step(at(9000, true, true, 78, 0, 1)) == HostStep::None && s.vrStarted && !s.desktop);
    }
    { // A runtime that never becomes ready: desktop after the bounded retries.
        HostSupervisor s;
        uint64_t t = 1000;
        uint32_t pid = 77;
        HostStep step = HostStep::None;
        for (int i = 0; i != 40 && !s.desktop; ++i) {
            assert(s.step(at(t, true, true, pid, 0, 0)) == HostStep::None || s.desktop);
            t += 500;
            step = s.step(at(t, true, false, 0, HostErrorRuntimeStarting, 0));
            while (step == HostStep::Stopped || step == HostStep::None) {
                if (s.desktop) break;
                t += 500;
                step = s.step(at(t, true, false, 0, HostErrorRuntimeStarting, 0));
            }
            if (step == HostStep::Restart) s.restarted(t, true);
            ++pid;
        }
        assert(s.desktop && s.runtimeRetries == HostSupervisor::MaxRuntimeRetries);
    }
    { // A plain host error before VR is still desktop play at once.
        HostSupervisor s;
        assert(s.step(at(1000, true, true, 77, 0, 0)) == HostStep::None);
        assert(s.step(at(2000, true, true, 0, HostErrorFailed, 0)) == HostStep::Desktop);
    }
    { // A retried host that crashes before connecting does not loop forever.
        HostSupervisor s;
        assert(s.step(at(1000, true, true, 77, 0, 0)) == HostStep::None);
        uint64_t t = 2000;
        assert(s.step(at(t, true, false, 0, HostErrorRuntimeStarting, 0)) == HostStep::Stopped);
        for (int i = 0; i != 200 && !s.desktop; ++i) {
            t += 1000;
            const HostStep step = s.step(at(t, true, false, 0, 0, 0));
            if (step == HostStep::Restart) s.restarted(t, true);
        }
        assert(s.desktop);
    }
}
void afterVr() {
    { // VR ran and ended between two observations (a load presents no frame): still VR.
        HostSupervisor s;
        assert(s.step(at(1000, true, true, 0, 0, 0)) == HostStep::None);
        assert(s.step(at(9000, true, false, 0, 1, 1)) == HostStep::Stopped && s.vrStarted && !s.desktop);
        assert(s.step(at(10000, true, false, 0, 1, 1)) == HostStep::Restart);
    }
    { // The host stops cleanly (the runtime ended the session): VR is kept and it restarts.
        HostSupervisor s = running();
        assert(s.step(at(10000, true, true, 0, 0, 1)) == HostStep::Stopped && !s.desktop);
        assert(s.restartAt == 11000 && s.failures == 1);
        assert(s.step(at(10500, true, true, 0, 0, 1)) == HostStep::None);  // Still exiting.
        assert(s.step(at(11500, true, true, 0, 0, 1)) == HostStep::None);  // Its session must be gone first.
        assert(s.step(at(11600, true, false, 0, 0, 1)) == HostStep::Restart);
        assert(s.step(at(11616, true, false, 0, 0, 1)) == HostStep::Restart); // Until the caller could do it.
        s.restarted(11700, true);
        assert(s.step(at(11800, true, true, 0, 0, 1)) == HostStep::None);  // Starting.
        assert(s.step(at(12500, true, true, 91, 0, 1)) == HostStep::None); // Connected.
        assert(!s.awaitingConnect && !s.downSince && !s.desktop);
        // Stable for 30 s: the next stop starts from the short delay again.
        assert(s.step(at(42500, true, true, 91, 0, 2)) == HostStep::None && s.failures == 0);
    }
    { // It reports an error (an OpenXR call failed): the same.
        HostSupervisor s = running();
        assert(s.step(at(10000, true, true, 0, 1, 1)) == HostStep::Stopped);
        assert(s.step(at(10100, true, false, 0, 1, 1)) == HostStep::None);
        assert(s.step(at(11000, true, false, 0, 1, 1)) == HostStep::Restart && !s.desktop);
    }
    { // It crashes: the channel still names it.
        HostSupervisor s = running();
        assert(s.step(at(10000, true, false, 77, 0, 1)) == HostStep::Stopped);
        assert(s.step(at(10999, true, false, 77, 0, 1)) == HostStep::None);
        assert(s.step(at(11000, true, false, 77, 0, 1)) == HostStep::Restart);
    }
    { // A stopped host that does not exit is ended once, after 10 s.
        HostSupervisor s = running();
        assert(s.step(at(10000, true, true, 0, 0, 1)) == HostStep::Stopped);
        assert(s.step(at(19999, true, true, 0, 0, 1)) == HostStep::None);
        assert(s.step(at(20000, true, true, 0, 0, 1)) == HostStep::Terminate);
        assert(s.step(at(20016, true, true, 0, 0, 1)) == HostStep::None);
        assert(s.step(at(20100, true, false, 0, 0, 1)) == HostStep::Restart);
    }
    { // Each failed restart waits longer: 1, 2, 4, 8, then every 15 s, without end.
        HostSupervisor s = running();
        uint64_t now = 10000;
        assert(s.step(at(now, true, false, 77, 0, 1)) == HostStep::Stopped);
        const uint64_t delays[]{1000, 2000, 4000, 8000, 15000, 15000, 15000};
        for (const uint64_t delay : delays) {
            assert(s.restartAt == now + delay);
            assert(s.step(at(now + delay - 1, true, false, 0, 0, 1)) == HostStep::None);
            now += delay;
            assert(s.step(at(now, true, false, 0, 0, 1)) == HostStep::Restart);
            s.restarted(now, true);
            assert(s.step(at(now + 10, true, true, 0, 0, 1)) == HostStep::None);
            now += 500; // The new host fails as it starts (the runtime is still away).
            assert(s.step(at(now, true, false, 0, 1, 1)) == HostStep::Stopped && !s.desktop);
        }
        for (int i = 0; i < 100; ++i) { // Hours later it still tries.
            now += 15000;
            assert(s.step(at(now, true, false, 0, 1, 1)) == HostStep::Restart);
            s.restarted(now, true);
            now += 500;
            assert(s.step(at(now, true, false, 0, 1, 1)) == HostStep::Stopped && !s.desktop);
        }
    }
    { // The process cannot be created: tried again later.
        HostSupervisor s = running();
        assert(s.step(at(10000, true, false, 77, 0, 1)) == HostStep::Stopped);
        assert(s.step(at(11000, true, false, 77, 0, 1)) == HostStep::Restart);
        s.restarted(11000, false);
        assert(s.restartAt == 13000);
        assert(s.step(at(12999, false, false, 0, 0, 1)) == HostStep::None);
        assert(s.step(at(13000, false, false, 0, 0, 1)) == HostStep::Restart);
    }
    { // A new host that never opens the channel is ended after 30 s and replaced.
        HostSupervisor s = running();
        assert(s.step(at(10000, true, false, 77, 0, 1)) == HostStep::Stopped);
        assert(s.step(at(11000, true, false, 77, 0, 1)) == HostStep::Restart);
        s.restarted(11000, true);
        assert(s.step(at(40999, true, true, 0, 0, 1)) == HostStep::None);
        assert(s.step(at(41000, true, true, 0, 0, 1)) == HostStep::Terminate);
        assert(s.step(at(41016, true, true, 0, 0, 1)) == HostStep::None);
        assert(s.step(at(41100, true, false, 0, 0, 1)) == HostStep::None);
        assert(s.step(at(43000, true, false, 0, 0, 1)) == HostStep::Restart && !s.desktop);
    }
}
void channel() {
    auto shared = std::make_unique<Shared>();
    Shared &s = *shared;
    { // A new channel: the first host starts as it always did.
        const HostSeed seed = hostSeed(s);
        assert(seed.inputSequence == 0 && seed.requestSequence == 0 && seed.session == 0 && seed.reference == 1);
    }
    s.hostPid = 77; s.error = 1; s.shutdown = 1;
    s.latest.sequence = 5000; s.latest.tickMs = 123; s.latest.session = 3; s.latest.reference = 4;
    s.latest.focused = InputFocused; s.latest.headValid = 1; s.latest.axis[1][0] = 1; s.latest.buttons[0] = Use;
    s.latestTime = 99; s.gripPoseGeneration[0] = 7; s.gripVelocity[1].valid = 3;
    s.worldSubmission.active = 1; s.worldSubmission.producer = 77;
    s.pointer.active = 1; s.images.generation = 2; s.images.opened = 2;
    s.recommendedWidth = 2000; s.recommendedHeight = 2200;
    s.slot[0].state = SlotState::Requested; s.slot[0].request.sequence = 900;
    s.slot[1].state = SlotState::Rendering; s.slot[1].request.sequence = 901;
    // The game still renders into slot 1: cancelled and awaited, the owner kept.
    assert(!releaseHostChannel(s, false));
    assert(s.slot[0].state == SlotState::Empty && s.slot[1].state == SlotState::Rendering && s.slot[1].cancelled == 1);
    assert(s.hostPid == 77 && s.error == 1);
    s.slot[1].state = SlotState::Empty; // Retired by the renderer.
    s.slot[0].state = SlotState::Ready;
    assert(releaseHostChannel(s, false));
    assert(s.slot[0].state == SlotState::Empty && s.slot[1].state == SlotState::Empty && !s.slot[0].cancelled &&
           !s.slot[1].cancelled);
    assert(!s.hostPid && !s.error && !s.shutdown);
    assert(s.latest.sequence == 5001 && s.latest.session == 3 && s.latest.reference == 4);
    assert(!s.latest.focused && !s.latest.headValid && !s.latest.tickMs && s.latest.axis[1][0] == 0 && !s.latest.buttons[0]);
    assert(!s.latestTime && !s.gripPoseGeneration[0] && !s.gripVelocity[1].valid && !s.worldSubmission.active &&
           !s.pointer.active && !s.images.opened);
    // What the game owns stays: its images and the canvas size it follows.
    assert(s.images.generation == 2 && s.recommendedWidth == 2000 && s.recommendedHeight == 2200);
    // The next host continues past everything the game has seen.
    const HostSeed seed = hostSeed(s);
    assert(seed.inputSequence == 5001 && seed.session == 3 && seed.reference == 4 && seed.requestSequence > 901);
    // A slot the renderer never retires is released after the caller's wait.
    s.slot[0].state = SlotState::Rendering;
    assert(!releaseHostChannel(s, false) && releaseHostChannel(s, true));
    assert(s.slot[0].state == SlotState::Empty && !s.slot[0].cancelled && s.latest.sequence == 5002);
}
void focus() {
    assert(sessionFocus(false, false) == SessionHidden && sessionFocus(true, false) == SessionVisible &&
           sessionFocus(true, true) == InputFocused && InputFocused == 1); // 1: as every earlier host published.
    Input input;
    input.sequence = 10; input.tickMs = 20; input.session = 1; input.reference = 1; input.headValid = 1;
    input.head.p = {1, 2, 3};
    input.focused = InputFocused;
    input.handValid[0] = input.handValid[1] = input.gripValid[0] = 1;
    input.hand[1].p = {4, 5, 6};
    input.trigger[1] = 1; input.axis[1][0] = .9f; input.axis[0][1] = -1; input.buttons[0] = Menu | Use;
    input.primaryActiveMask = 3; input.zoomActiveMask = 1; input.zoomDownMask = 1; input.wheelAdmissionMask = 2;
    input.primaryInputGeneration[1] = 8; input.wheelInputEpoch[0] = 9;
    assert(inputFocused(input) && sameInput(admittedInput(input), input));
    // Shown without input focus (the runtime's menu is open): the head stays, no control does.
    input.focused = SessionVisible;
    const Input shown = admittedInput(input);
    assert(!inputFocused(shown) && shown.focused && shown.headValid && shown.head.p.y == 2 && shown.sequence == 10);
    for (unsigned h = 0; h != 2; ++h)
        assert(!shown.handValid[h] && !shown.gripValid[h] && shown.trigger[h] == 0 && shown.axis[h][0] == 0 &&
               shown.axis[h][1] == 0 && !shown.buttons[h] && shown.hand[h].p.x == 0);
    assert(!shown.primaryActiveMask && !shown.zoomActiveMask && !shown.zoomDownMask && !shown.wheelAdmissionMask);
    assert(shown.primaryInputGeneration[1] == 8 && shown.wheelInputEpoch[0] == 9); // Stream identity is kept.
    assert(!primaryActionEligible(shown, 1) && !recenterHeld(shown));
    // What the host publishes in that state already has no controls: unchanged.
    Input published;
    published.sequence = 11; published.tickMs = 21; published.session = published.reference = 1;
    published.focused = SessionVisible; published.headValid = 1;
    assert(sameInput(admittedInput(published), published));
    // The world is still rendered for it: frame eligibility asks only that the session is shown.
    Request request;
    request.sequence = 1; request.session = request.reference = 1; request.trackingGeneration = 1;
    request.width = 64; request.height = 64; request.input = published;
    FrameContext context;
    context.now = 100; context.uiTick = 90; context.session = context.reference = 1; context.epoch = context.uiEpoch = 1;
    context.width = context.height = 64; context.renderer = context.gameplay = context.focused = context.headValid = true;
    assert(eligibleFrame(request, 200, context));
    request.input.focused = SessionHidden;
    assert(!eligibleFrame(request, 200, context));
}
} // namespace
int main() {
    beforeVr();
    runtimeStarting();
    afterVr();
    channel();
    focus();
    std::puts("host supervisor ok");
}
