#pragma once
#include "protocol.hpp"
#include <algorithm>
namespace ss2vr {
// The game's supervision of the headset host process.
//
// Until VR has run (a host connected and began an OpenXR session) a host that
// fails, exits or never appears means desktop play: no runtime or no headset.
// Once VR has run in this game session it is kept: a host that stops for any
// reason (runtime error, the runtime ending the session, a crash) is started
// again, with a growing delay, for as long as the game runs.
struct HostObservation {
    uint64_t now = 0;
    bool spawned = false, alive = false;          // A host process was created; it has not exited.
    // Shared::hostPid, Shared::error and Shared::latest.session (non-zero once any
    // host of this channel began an OpenXR session).
    uint32_t hostPid = 0, error = 0, session = 0;
};
enum class HostStep {
    None,
    Desktop,   // VR never started: the game's own canvas for the rest of the session.
    Stopped,   // The host went down after VR ran (report it); a restart follows.
    Terminate, // The stopped host did not exit (or a new one never connected): end its process.
    Restart    // Release the channel and start a new host, then call restarted().
};
struct HostSupervisor {
    static constexpr uint64_t StartMs = 30000, ExitMs = 10000, StableMs = 30000;
    // Hosts started again, before VR ran, because the runtime was still starting.
    static constexpr unsigned MaxRuntimeRetries = 6;
    unsigned runtimeRetries = 0;
    bool retrying = false;
    uint64_t since = 0, downSince = 0, restartAt = 0, startedAt = 0, healthySince = 0;
    unsigned failures = 0;
    bool seen = false, vrStarted = false, desktop = false, awaitingConnect = false, terminated = false;
    const char *reason = "";
    static constexpr uint64_t restartDelay(unsigned failures) {
        return failures >= 4 ? 15000 : uint64_t(1000) << failures; // 1, 2, 4, 8, then every 15 s.
    }
    HostStep step(const HostObservation &o) {
        if (desktop) return HostStep::None;
        if (!since) since = std::max<uint64_t>(o.now, 1);
        if (o.hostPid) seen = true;
        // The session generation stays on the channel after its host: VR ran even if
        // no observation fell between its start and its end (the game presents nothing
        // while it loads).
        if (o.session) vrStarted = true;
        const bool connected = o.spawned && o.alive && o.hostPid && !o.error;
        if (connected) {
            retrying = false;
            downSince = 0;
            awaitingConnect = terminated = false;
            if (!healthySince) healthySince = std::max<uint64_t>(o.now, 1);
            if (o.now - healthySince >= StableMs) failures = 0;
            return HostStep::None;
        }
        healthySince = 0;
        if (!vrStarted && o.error == HostErrorRuntimeStarting && !retrying) {
            // The runtime was still starting (SteamVR launched by the host restarts its
            // server once) or had no headset yet: a new host, a bounded number of times.
            if (runtimeRetries >= MaxRuntimeRetries) return fallback("the VR runtime did not become ready");
            ++runtimeRetries;
            retrying = true;
        }
        // Whatever goes wrong while retrying (a host that never connects, exits or
        // cannot be created) ends in desktop play after as many restarts.
        if (!vrStarted && retrying && failures > MaxRuntimeRetries) return fallback("the VR runtime did not become ready");
        if (!vrStarted && !retrying) {
            if (o.error) return fallback("headset host reported an error");
            if (seen && !o.hostPid) return fallback("headset host exited");
            if (o.spawned && !o.alive) return fallback("headset host exited before it connected");
            if (!seen && o.now - since > StartMs) return fallback("headset host did not start in 30 s");
            return HostStep::None;
        }
        const bool process = o.spawned && o.alive;
        if (awaitingConnect && process && !o.error) {
            if (o.now - startedAt < StartMs) return HostStep::None; // Starting: it has not opened the channel yet.
            down(o.now, "did not connect");
            terminated = true;
            return HostStep::Terminate;
        }
        if (!downSince) {
            down(o.now, o.error == HostErrorRuntimeStarting ? "found the VR runtime still starting" :
                         o.error ? "reported an error" : process ? "stopped" : "exited");
            return HostStep::Stopped;
        }
        if (process) {
            // Its OpenXR session must be gone before another host starts.
            if (terminated || o.now - downSince < ExitMs) return HostStep::None;
            terminated = true;
            return HostStep::Terminate;
        }
        return o.now >= restartAt ? HostStep::Restart : HostStep::None;
    }
    // After a Restart step: whether the new host process was created.
    void restarted(uint64_t now, bool created) {
        terminated = false;
        if (created) {
            awaitingConnect = true;
            startedAt = now;
            downSince = 0;
        } else
            down(now, "could not be started");
    }

  private:
    HostStep fallback(const char *why) {
        desktop = true;
        reason = why;
        return HostStep::Desktop;
    }
    void down(uint64_t now, const char *why) {
        downSince = std::max<uint64_t>(now, 1);
        restartAt = now + restartDelay(failures);
        if (failures < 16) ++failures;
        awaitingConnect = false;
        reason = why;
    }
};
// The channel as a new host must find it (see Host::connect): no owner, no error
// or shutdown request, and nothing of the last host's input, pointer or shared
// image acknowledgement. The input's sequence, session and reference identities
// stay, so the next host continues them instead of repeating earlier values.
// Returns false while the game still renders into a slot (cancelled here; the
// renderer retires it), unless force is set.
inline bool releaseHostChannel(Shared &shared, bool force) {
    bool rendering = false;
    for (auto &slot : shared.slot) {
        if (slot.state == SlotState::Rendering && !force) {
            slot.cancelled = 1;
            rendering = true;
        } else {
            slot.state = SlotState::Empty;
            slot.cancelled = 0;
        }
    }
    if (rendering) return false;
    Input cleared;
    cleared.sequence = shared.latest.sequence + 1;
    cleared.session = shared.latest.session;
    cleared.reference = shared.latest.reference;
    shared.latest = cleared;
    shared.latestTime = 0;
    shared.hostPid = shared.error = shared.shutdown = 0;
    shared.gripPoseGeneration[0] = shared.gripPoseGeneration[1] = 0;
    shared.gripVelocity[0] = shared.gripVelocity[1] = {};
    shared.worldSubmission = {};
    shared.pointer = {};
    shared.images.opened = 0;
    return true;
}
// The identities a host starts from on a channel an earlier host used: its input
// and request sequences and its session and reference generations continue past
// the last published ones. A new channel gives 0, 0, 1 and 0, as before.
struct HostSeed {
    uint64_t inputSequence = 0, requestSequence = 0;
    uint32_t session = 0, reference = 1;
};
inline HostSeed hostSeed(const Shared &shared) {
    HostSeed seed;
    seed.inputSequence = shared.latest.sequence;
    seed.session = shared.latest.session;
    seed.reference = std::max<uint32_t>(shared.latest.reference, 1);
    seed.requestSequence = std::max(shared.slot[0].request.sequence, shared.slot[1].request.sequence);
    // Requests the last host numbered but never placed in a slot.
    if (seed.requestSequence) seed.requestSequence += 4096;
    return seed;
}
} // namespace ss2vr
