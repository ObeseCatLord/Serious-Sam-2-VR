# OpenXR host and controls

## Current source — 2026-10-07

Current game/host IPC is10 and the offline suite has61 groups. A response-owned
head-clearance receipt now limits current head/eye placement and cached-image
age when the default-off head guard is enabled. Game, server and host also carry
read-only exported build-input/version/layout contracts for package verification.
See HEAD_VOLUME_INTEGRATION.md and PACKAGE_IDENTITY.md. All products cross-build;
Controlled Linux/Proton simulated-Monado gameplay has produced distinct native
eye images and seven-pose camera response. Native Windows and both actual headset
targets remain unverified. See RUNTIME_ACCEPTANCE_MATRIX.md and USER_VR_TEST.md.
The original integration
record below retains historical ABI6 wording and delegation provenance.

## Original host integration record

The final x64 Windows host is implemented in src/host/host.cpp and linked with the official pinned OpenXR loader. No host/runtime/headset execution occurred. Final integration supersedes the worker's initial frozen-header status; source construction and a strict x64 compile were delegated to GPT-6.1 Sol at xhigh, then integrated by the main agent.

The host opens the per-launch IPC channel, checks ABI/size/PID ownership and monitors the game process. It loads the adjacent official loader by absolute filename, resolves APIs through xrGetInstanceProcAddr, enables XR_KHR_D3D11_enable, selects PRIMARY_STEREO/opaque blending, queries adapter LUID/minimum feature level, and creates D3D11 on the runtime-selected adapter. LOCAL and VIEW spaces, independent left/right aim and grip action spaces and a single action set supply actual HMD/controller poses.

READY begins the session, STOPPING ends it, and loss/exit invalidates input and pending work. Reference changes advance a generation at their predicted change time. Lost runtime events fail closed. Every frame uses WaitFrame/BeginFrame/LocateViews/SyncActions and independently publishes the latest finite valid and actively tracked poses/input. Frame guards end failed frames with zero layers. Idle/unfocused controls remain neutral.

ABI6's two owned stereo slots carry immutable sequence/time/session/reference/native-epoch/eye/FOV/input snapshots. Input publishes every XR frame; the host polls completion without waiting for game capture and keeps at most one request waiting while the game holds the other slot (Rendering, or Ready awaiting its GPU completion or this loop); a waiting request, cancelled Rendering or expiring slot holds further requests, and the newest Ready pair wins (an older Ready pair is retired). ABI14 pairs normally arrive as shared GPU images: the host opens the game's D3D9 shared render targets once per generation, copies a pair only after the game marks it gpuReady, and waits for its own copy before the slot can be requested again. Cancellation never reclaims native Rendering. A complete eligible pair replaces the cached pair before the provisional 150 ms enqueue-age cutoff; its original LOCAL poses/FOV may be reused while lifecycle/focus/tracking/dimension/epoch checks remain valid. A final atomic epoch/age check occurs before EndFrame; invalidation immediately after that check remains a distributed race, not an atomic cross-process submission guarantee.

Both swapchain images must finish acquisition/wait before either upload/release. Timed-out waits retain the existing acquisition and Ready for retry. Reacquiring the last-released image suppresses reuse. Cache is invalidated before replacement and committed only after both releases and a device-health check. A post-wait abandoned candidate releases waited images without writes; this also invalidates cache because the runtime's most-recent released image changes. Outstanding timed-out acquisitions are retried and retired without acquiring additional images. Submitted eye poses/FOV are those used to render, not a later sample. Native BGRA8 CPU pixels upload to BGRA8 UNORM swapchains, or are explicitly converted to RGBA8 UNORM if necessary. sRGB encodings are not guessed.

The game draws each open hand wheel in the stereo world (native weapon display meshes, or icons as a fallback, on a ring anchored 15 cm ahead of where the hand opened it, with the action bubbles on a separate arc beyond its outer side). The host adds one small per-hand LOCAL quad at that ring centre, captured from its own hand sample when the wheel opens: GDI draws the hovered weapon's stock name and ammo, or the bubble name, into a translucent BGRA card. Selection/deadzones remain game-owned. The game's own HUD is composited into each stereo pair on a comfortable panel; there is no substitute host HUD. The game's 2D view (menus, HUD, movies) is laid out at `[Render] UiAspect` (16:9 default) and the flat screen shows exactly that band. Every frame the stereo path cannot own (menus, loading, cinematic cameras, movies, Netricsa, death/third-person views, missing stereo resources) uses a separate level LOCAL-space threshold-follow screen quad; it never becomes a stereo projection. Only native menus carry a pointer interaction generation. Menu controls do not need window focus: native menus receive system commands (syscmdOk/Back/arrows/F1/F2) through their own dispatcher and other screens get keys posted to the game window. Controller rays intersect the actual submitted menu quad; a visible ring shows the hit. The native cursor/button dispatcher accepts coordinates and trigger from that presented sample while its menu generation matches and its visual frame trails the newest menu copy by at most eight frames; a click survives a sub-150 ms pointer gap on the same menu. Stick/Use/Menu remain available, with duplicate trigger-to-Enter confirmation suppressed during pointer mode. Required composition-layer capacity is four.

COM, Win32, GDI and OpenXR objects have owned teardown. Normal STOPPING invalidates presentation and retires existing acquisitions in every stereo/UI chain before EndSession, with a 500 ms retry bound and host exit on failure. Resize/destruction attempts no-write retirement after GPU drain; an unwaited timed-out image is disposed of by destroying the drained chain, never released illegally. GPU resize/shutdown uses a 500 ms drain; a hung GPU retains graphics/runtime objects until OS teardown instead of destroying in-use resources. Concise errors and five-second accepted/submitted/reused/retired/expired/invalidated/image-wait counters go to a bounded host log. Pixel-section pairs (fallback) still use CPU readback/upload; neither path nor the native renderer is latency-bounded or verified on hardware. A five-second `Host perf` line reports loop, frame wait, submit and per-pair copy times.

## Controller bindings

Profile/component paths were checked against pinned Khronos xr.xml. Triggers are independent float actions; sticks/trackpads are vector actions. The grips (Touch/Frame squeeze values above 0.65, Index squeeze force above 0.4 so a resting hand does not grab, Vive/Microsoft squeeze clicks) jump while held on foot on profiles without a jump button (not Index, whose right B jumps), unless that squeeze picks up a takeable object (carry wins); in vehicles they grasp the steering wheel or handles, and both grips plus the left stick/trackpad click recenter. Each hand's weapon wheel opens while its own wheel button (below) is held (Index: a light trackpad press, force above 0.25); a wheel button lost or disallowed while held blocks that wheel until it is released, so the loss never selects. Netricsa, the game menu and quick save/load are weapon-wheel bubbles. The runtime may remap or reject profile suggestions; rejected suggestions are logged.

| Profile | Axes | Weapon wheel (left / right) | Use | Jump | Menu | Sprint | Reload |
|---|---|---|---|---|---|---|---|
| Oculus Touch | Thumbsticks | Y / B | A | Grip | Left menu | Either stick click | X |
| Valve Index | Thumbsticks | Trackpad press, each hand | Right A | Right B | Left B | Left stick click | Left A |
| HTC Vive | Trackpads | Trackpad click, each hand | Right menu | Grip | Left menu | Left trackpad click | — |
| Microsoft Motion | Thumbsticks | Trackpad click, each hand | Left menu | Grip | Right menu | Either stick click | — |
| Valve Frame (extension advertised) | Thumbsticks | Left D-pad up / right B | Right A | Grip | Left view / right menu | Either stick click | Left D-pad down |

On every profile the left stick click sprints and the right stick click zooms a held sniper rifle, otherwise it throws a hand grenade. Use (right A on Index/Touch) enters and leaves turrets and vehicles: look at the seat. Native prompts name the VR button ("Press A to enter").

The Vive right hand has no free button for its contextual sniper zoom (formerly
the right trackpad jump), so sniper zoom is unavailable on that hand.

Menus: point and pull a trigger to select; Use/A confirms, Menu/B goes back, the
left stick moves the selection, Reload and the wheel button act as the X/Y menu
functions (F1/F2); the wheel button also closes the Netricsa screen.
While VR is live the menus show the game's own controller prompts (A/B/X/Y) and
the game does not need window focus: menu keys go through the native menu
dispatcher, and the engine is kept in its foreground state.

The host enables XR_VALVE_frame_controller_interaction only when enumerated, then
suggests /interaction_profiles/valve/frame_controller_valve. Left X/Y paths are
not used for that native profile. Its squeeze/value uses the existing release and
availability gates. Older runtimes retain all existing suggestions; Valve's
documented Frame → Generic Controller → Touch fallback is separate from Index.
See [Valve's input documentation](https://partner.steamgames.com/doc/steamhardware/steamframe/input).
Native Frame bindings are source implementation pending actual controller/runtime
acceptance; simulated Monado does not certify Frame remapping or streaming.

Recenter: both grips plus left stick/trackpad click. That chord suppresses gameplay actions and resets the common origin while retaining real grip and trigger values for physical-release gating. It cannot fabricate a release or incidentally reopen a wheel. Reserved system buttons are not bound. In a menu, left axis navigates, Use or a freshly pressed trigger confirms, and Menu goes back. Holding a trigger across menu entry does not confirm. The selected pointing hand's trigger clicks native items; invalid hand/reference/menu/focus releases the owned mouse button and requires neutral rearming.

Khronos primary references: [image waits](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrWaitSwapchainImage.html), [frame end](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrEndFrame.html), [D3D11 requirements](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrGetD3D11GraphicsRequirementsKHR.html). Runtime/profile acceptance, native capture, headset readability, physical weapon alignment and Proton routing remain unverified. See IMPLEMENTATION_STATUS.md.

Comfort defaults: yaw following starts after35° relative turn and stops with8° residual error, at up to90°/s. Position following has0.35m entry/0.10m stop and0.35s easing. Panels remain level and at least1m in front; lifecycle/recenter/visibility discontinuities reseed the anchor. Menu defaults are2.2m distance/2.2m width; HUD1.5m distance/1.05m width with4:1 geometry. User settings clamp safe finite ranges. Physical size/readability/comfort were not evaluated in a headset.
