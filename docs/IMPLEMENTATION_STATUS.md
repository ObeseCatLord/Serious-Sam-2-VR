# Implementation status — 2026-10-10

## 2026-10-13 (late) — exact single pass, foveated rendering, VR options

- Single pass correctness: the engine read the viewport back mid-pass (it got
  the double-wide viewport after an instanced draw); viewport, scissor, scissor
  enable and shader getters now return the engine's values during the pass,
  the engine's state is restored after every stereo draw, and render-target
  switches (5 per pass) restore it and suspend the stereo bindings. This was
  the source of the sky/moon/smoke/waterfall "instancing artifacts".
- Instancing is exact: kill planes split half a pixel off the seam (D3D9
  pixel centres are integers), the eye test goes only into the vertex
  program's lowest unwritten texcoord set (pixel programs read unwritten sets),
  and per-half draws use the same wide-viewport mapping. A same-frame mirror
  target (lab) shows 0 differing pixels for every program. It saves ~0.1 ms of
  render thread, so it stays opt-in.
- The pass target is now a layout of regions (affine maps of eye NDC applied in
  clip space through the per-region record; any engine viewport maps exactly).
- Foveated rendering ([Render] Foveation 1 eye-tracked / 2 fixed): on zero-copy
  pairs the host asks for foveated rendering (Request.foveated 2); the pass draws
  both eyes' full views at FoveationPeriphery and a full-resolution
  FoveationInset region around the gaze (XR_EXT_eye_gaze_interaction, centre
  without it) into one atlas from the same traversal; each eye is the full
  view stretched over the eye with the inset blended in (feathered edge).
  Steam Frame's runtime foveation (XR_FB/META foveation) needs the app to render
  into Vulkan swapchain images and does not apply to this D3D9 path.
  Lab 1344x1512: eye GPU 1.25-1.40 -> 1.23-1.25 ms, render thread +0.3 ms; the
  saving grows with resolution and render scale.
- VR options panel: a weapon-wheel bubble opens a host panel (foveation and
  its sizes, turning, turn speed, lasers, haptics, render scale, instancing).
  Values apply at once and go to SS2VR.user.ini (overrides SS2VR.ini, kept
  by the installer across updates); the game re-reads it when it changes.
- Known: in some launches a translucent sky haze layer shows a straight edge
  near the outer edge of one eye (x=1202 right / 141 left of 1344) in single
  pass only (shifted and wide issue alike; not in two-pass). Not yet traced.

## 2026-10-13 — single-pass stereo, second headset test fixes

- Single pass ([Render] SinglePass=1, default): the world is traversed once
  from a centre camera (midpoint of the eyes, frustum = union of both eyes plus
  a margin) into a 2W x H target. Every draw of the pass is issued once per eye
  half; a patched vs_1_1 program (src/common/shader_patch.hpp, 39/39 engine
  programs) reads the eye's clip-space reprojection R = AP_eye * E * AP_c^-1
  (replay_math rewrite) from v10..v15 on stream 15, so constants are untouched
  (skinned programs def c255 and index bones from c17). Each eye then adopts
  its derived camera for the HUD projection and markers; the halves are copied
  into the per-eye images (no host change). Replaces the record/replay right
  eye, whose occlusion artifacts (objects through terrain) came from replaying
  one eye's draws; both halves now use the same live data.
- Lab: eyes 3.4 -> 1.8-2.1 ms per pair (Jungle), 1.3 ms (Metropolis); render
  thread 5.1 -> 2.9-4.0 ms; images match a two-pass render (geometry; specular
  is from the centre camera).
- Instancing (SinglePassInstancing=1, off): one two-instance draw with a
  texkill per half; artifacts remain (1.x pixel programs: translucent objects
  through occluders, missing smoke; 2.0: bleeding beams, sky streaks). Query
  boxes (colour writes off) stay per half (instanced they culled visible
  objects).
- The root's executed matrices are followed when they differ from the
  prepared ones (camera bob/shake ~1.3 cm on the headset dropped most pairs);
  world markers accept a camera within 30 cm / 10 degrees of the eye.
- Dual wielding is switched on once per player; a wheel choice the game does
  not pair is kept (toggling back restored the previous weapons).
- Turrets: the right stick aims (smooth-turn rate), the view no longer turns.
- Lasers: ~55 % translucent; need the grip pose too; the uzi beam starts at
  the rod under the barrel.

## 2026-10-12 (night) — first headset test fixes

- Flat screen: the host compared the scaled flat image (<=1920x1080) with the
  pixel-section size and rejected every menu, loading screen and cutscene since
  the GPU scaling change; it now checks capacity. The last flat frame stays up
  while loading blocks presentation (up to 60 s).
- Blackouts: the host reused its last stereo pair for only 150 ms, so runs of
  dropped pairs (left-eye root-capture faults, seen with the double shotgun on
  the headset only) went black; the pair is now held for 1 s. Fault sites log
  their source line (`at=`); the trigger is not reproduced in the lab.
- Right-eye replay: block MVPs drawn through the eye frustum with another depth
  row are decomposed from their x/y/w rows (`rewriteFrustumMvp`, tested); a
  draw still unrecognized (>16 primitives) renders that right eye natively.
  Derived projections keep their perspective rows bit-exact (a float mismatch
  had discarded every replay on Metropolis).
- Index: grip = squeeze force > 0.4 (squeeze value is finger curl; holding the
  controller jumped constantly), wheel = trackpad force > 0.25 thresholded in
  the host, jump = right B only (`JumpBound` button bit disables grip jump);
  right stick click throws a grenade (`plcmdTertFire`) unless a sniper zooms.
- Mouse axes (CKbdMouseInputDevice controls 0x78-0x7b) read 0 while VR input is
  live: the menu pointer's cursor moves pitched the view after a pause.
- Turrets: ride boarding picks the seat in a cone around
  CPlayerPuppetEntity::GetViewOrigin, the native look that VR never turns; the
  two boarding callers (0xEA4FE, 0xEA465) now see the tracked head. Seated, the
  right trigger is Fire and the left AltFire (no lone-weapon remap). Native
  prompts name VR buttons via a CInputBindings::SubstituteCommands rewrite.
- Grips: auto shotgun on its pistol grip, double shotgun on the stock wrist
  (V212/V216 side views).

## 2026-10-12 (later) — right eye by record and replay; mod overhead cut

- The right eye is no longer a second engine render. The left eye's world
  render is recorded at the D3D9 call level (`eye_replay.cpp`) and replayed
  into the right eye's targets with the view-dependent vertex constants
  rewritten exactly (`common/replay_math.hpp`, unit tested): `gfxBindMatrix`
  uploads by matrix type, and the shader system's constant blocks by their
  c1..c4 MVP when it decomposes as an MVP of the recorded view state (or the
  root/background view for draws after the root returned). The right eye's
  root capture (HUD projection, markers) is derived identically; a lab verify
  mode matched a native right eye to 3e-5. Captures: disparity of near and far
  objects identical to a native right eye (-1 px far, -55/-60 px weapons).
- Data lifetime: vertex data is append-only within a frame; the engine's
  dynamic index ring wraps several times per frame, so dynamic index buffers
  are written through a CPU shadow and recorded draws keep their indices for
  a private replay index buffer. Any unsafe buffer lock, user-pointer draw or
  replay failure renders the right eye natively for that frame, as do frames
  with the mod's own 3D overlays (weapon wheels, vehicle steering wheel),
  whose replay still showed artifacts.
- Lab V207 (1344x1512): right eye 1.5-1.9 ms -> 0.3 ms; stereo CPU per pair
  ~3.2 -> 2.2 ms. Not yet measured on the real headset. `[Render] EyeReplay=1`
  (default); lab switches `SS2VR_LAB_EYE_REPLAY=0`, `SS2VR_LAB_REPLAY_VERIFY=1`.
- The mod's own per-draw overhead (scope/idle probe resets on every draw, the
  roomscale resource gates' register/FP saves) was ~25 % of each eye's CPU;
  fast paths removed it (render-thread CPU ~9.4 -> 5.3 ms per frame in the lab;
  `SS2VR_LAB_NO_FASTPATH=1` restores the old paths). An in-process sampler
  (`SS2VR_LAB_SAMPLER=1`) profiles the render thread by module.
- Single-pass stereo by instancing was planned (private plan) but set aside
  for record/replay, which needs no shader patching.


## 2026-10-12 — HUD layer, dynamic shadow reuse, trigger chainsaw, housekeeping

- HUD layer: each admitted HUD draw renders once into a canvas-sized
  premultiplied layer (separate alpha blending keeps coverage for over and
  additive blends); the layer is composited 1:1 onto the backbuffer and,
  perspective-correct with trilinear mips, onto each eye's panel (fixed
  function, pre-transformed vertices, state restored by a state block). Fades
  and blend modes a layer cannot express flush the layer and keep the per-eye
  path. Lab V174: all 18 HUD draws per frame layered, 3 composite draws instead
  of 36 eye duplicates, HUD CPU about 0.53 -> 0.38 ms per frame.
- Dynamic shadow reuse (`[Render] ShadowReuse=1`): at shadow quality 3 the
  first eye renders each caster's light-space image into its own pooled engine
  canvas (the shared canvas pointer is swapped around the engine call) and the
  second eye projects it through the engine's own quality<=2 path with the
  pooled handle, skipping its render-to-texture. Lab V176/V177: every caster
  reused (0 misses); the pooled image is the correct silhouette. Saving is small
  on the CPU (~0.02 ms per caster; projection, ~0.11 ms per caster per eye,
  remains per eye) plus two render-target switches, a clear and a model draw
  per caster on the GPU side. Engine RVAs in `engine.cpp` (shadow_reuse).
- The chainsaw is trigger-only in the user package (`[Melee] Enabled=0`);
  physical swing gestures are not used.
- Offline ABI verifiers (`verify_world_marker_abi`, `verify_vehicle_laser_abi`)
  accept the compiler's current scheduling again; USER_VR_TEST.md rewritten for
  the current build.


## 2026-10-11 (latest) — cheaper, cleaner scope lens

- No preview pass: the scope view observed in the previous frame's ordinary
  left-eye render drives this frame's scope-camera render. The lens draw
  re-expresses its shader constants (aperture rows and eye) in that source
  camera's frame, so the unchanged shader projects exactly into the one-frame-
  old image (rotation and translation; only scene motion is a frame old).
  Zoomed CPU cost: about +7 ms -> +1.7 ms per frame (lab V172); the scope-camera
  pass itself is ~1.5 ms (narrow FOV) and nothing runs after its capture point.
- The lens image has an automatic mip chain sampled trilinearly: the full-size
  source minified onto a small lens sparkled; it is now smooth.
- Request sequences advance several times per frame (late latching), so the
  source age guard is 8 requests; freshness is one frame (each observation is
  used once). The cap-position tolerance is 1e-3 of clip magnitude: float error
  reaches 1.6e-4 depending on level position; 5 mm displacement still rejects.
- GPU (lab, 1882x2117): the scope-camera pass adds ~0.3-0.9 ms.


## 2026-10-11 (late) — sniper scope lens, zoom on the physical hand, sizes

- Sniper zoom follows the hand that physically holds the rifle (the stick click
  of that controller), and contextual buttons follow the physical hand too.
- The magnified scope lens now works (lab V162-V166). Three admissions were
  wrong for the VR pipeline: the weapon draws inside the level's fog (fog is now
  allowed; the lens draw keeps it), the cap-position check compared float GPU
  arithmetic against a 1e-5 tolerance (measured error 3.4e-5; now 1e-4 of the
  vertex's clip magnitude, 1 mm displacement still rejects), and the lens source
  texture used the backbuffer format while zero-copy eye targets are A8R8G8B8,
  so the copy only worked on pixel-fallback frames. The lens texture and shader
  are cached across frames (released on device loss/Reset).
- Forgiving eye box: only a quarter of the eye's sideways offset drives lens
  parallax, so the magnified view stays visible without the head exactly behind
  the scope (the rifle's scope rides ~0.3 m above the controller).
- Cost while zoomed: two extra world passes (pose preview + scope source),
  about +7 ms CPU per frame in the lab; nothing when not zoomed (the preview
  pass was removed later, see above).
- Klodovik and the Serious Bomb are 1.5x larger (87% / 130% of the revolver).
- HeadComfort stays disabled by default; when enabled it now darkens only when a
  fresh probe actually finds the head inside geometry.
- Diagnostics: `Scope view` reasons, `Perf scope` counters; `SS2VR_SCOPE_PROBE=1`
  reads back the lens source luminance once per perf window (lab only).


## 2026-10-11 (night) — frame pacing; first real-headset numbers

- Frame pacing: while pairs stream, the next game frame starts once the host has
  a request waiting (after Present, bounded), so every frame becomes a pair and no
  full-resolution flat frames are wasted; the host queues a second request only
  when the game is behind.
- Flat screen (menus/loading/cutscenes): the 16:9 band is scaled on the GPU to at
  most 1920x1080 before readback (was the full 3561^2 canvas): 18 → 2 ms per frame.
- Real Bigscreen Beyond 2e (Monado 25.1 via Envision, RTX 4090, 3561x3561 per eye,
  75 Hz; measured with `[Lab] FixedPose=1 AutoContinue=1` because the headset was
  untracked): Jungle start 375 frames / 375 pairs per 5 s, 13.3 ms frames of which
  6.7 ms idle; City Metropolis start 9.3 ms idle. Present does not block (GPU not
  limiting at 75 Hz). WayVR running alongside cost dropped pairs (266 vs 4 discarded).
- Next candidates (Fable RE, scratch notes): execute the engine's collected view
  twice ("collect once, execute twice") to drop the second traversal; VR settings
  for dynamic shadows (per-eye shadow render targets); HUD duplication batching.


## 2026-10-11 (evening) — lost pairs and latency

- Pairs lost after rendering were all IPC lock timeouts at publication (2 ms):
  the lab observer held the mutex across file I/O per status poll, and Wine's
  mutex wake-up alone can exceed 2 ms. The observer copies the shared block and
  writes afterwards; publication waits up to 25 ms. V128: one lost pair in 75 s
  (was ~1/s); the stalls also cost frame time (52 pairs/s in the lab).
- A HUD panel partly behind an eye (a head turn the panel has not followed yet)
  failed its projection and dropped every pair until the panel caught up; it is
  now an empty projection and the HUD skips that eye.
- Late-latching: a request the game has not taken yet is refreshed with each host
  loop's newer poses (request-to-swapchain pose age 37 → ~16 ms in the lab).
- Tried and reverted: a host intake thread copying pairs during xrWaitFrame (its
  copies and IPC traffic landed mid game frame: slower frames, no better age).
- Remaining game-thread cost is the engine's own eye rendering; the mod's own
  per-frame work is under 0.2 ms (wheels ~1.3 ms while open).
- Lab: compositor sockets live in /run/user/<uid>/ss2vr-lab; completed fixtures and
  runs move to an archive on the data drive (Wine prefixes need a native FS).


## 2026-10-11 (later) — performance, weapon feel, controls

Measured in the simulated lab with five-second `Perf` lines (game) and `Host perf`
lines (host); a Fable performance review drove the plan.
- Request pipelining: the host keeps a second request queued while the game holds
  the other slot. Before, about half of all game frames were flat frames the headset
  never saw (16 → 30 pairs/s at 1344x1512).
- Zero-copy eye images (ABI 14): pairs render into D3D9 shared render targets
  (DXVK KMT handles, one per slot and eye) that the host opens with
  `OpenSharedResource` and copies on the GPU. A pair is Ready at once and gpuReady
  when its D3D9 event query completes (polled, never waited); the host waits for its
  own copy before the slot is reused. Readback (a 4-10 ms drain per eye in DXVK),
  row copies, the alpha pass and the host upload are gone (→ ~40 pairs/s). A
  lab-only probe writes one zero-copy frame as a PPM; captures request pixel pairs.
- The desktop window mirrors the left eye; the third, desktop-only world render is
  skipped (`[Render] DesktopWorld=1` restores it). The wheel's mesh materials left
  fog on for the HUD; fog is now restored through the engine's switches.
- Remaining lab cost is the window Present into the lab's software (pixman)
  compositor, ~10 ms per frame, which a real desktop does on the GPU.
- Weapons follow the controller (flat viewmodel tilt dropped; authored quarter/half
  turns kept by snapping), muzzle and lasers follow the barrel, lasers are red,
  each trigger fires its own gun (combo weapons), the saw is held by its trigger
  handle, and hands are the same size across weapons (V111/V123 side captures).
- Movement follows head yaw; smooth turning by default; the wheel opens on its own
  button and grips jump on foot (carry wins).


## 2026-10-11 — user feedback: 2D aspect, gray HUD, weapon grips

- 2D aspect: SS2 letterboxes its whole 2D view to `gfx_fAspectRatio` (Sam2Game
  RVA 3F9DE0, 4:3 by default) inside the canvas, which is now square (eye-sized).
  VR sets `[Render] UiAspect` (16:9) and the flat screen copies only that band.
- The host's substitute HUD panel (shown when a pair lacked the native HUD) is
  removed. Host frame totals now count `ui_pairs` and `opaque_pairs`.
- Weapon grips: the private `[Lab] WeaponAudit` mode logged every weapon's first-
  person pose and Hand/Palm bones (V101). Every weapon now has a grip anchor
  (`WeaponGrips`): Hand bones for revolvers/uzi/double shotgun, measured zap/
  sniper handles, rear handles for big guns (V102-V105 captures with the
  controller marked). A lone weapon follows the hand whose wheel chose it.
- Wheel: bubbles moved to a separate arc, ring 15 cm ahead of the hand, held
  weapon hidden while its wheel is open (V106/V107).
- User installs keep `[HeadComfort]` off; its opaque fallback blacks out the world.

## 2026-10-10 (late) — grip carry, mesh weapon wheel, foveated readback

- Grip pickup (`2db90d0`): a grip squeeze beside a takeable prop (Engine
  `cldGetHullsInRange`, takeable flag 2) calls the native `TakeObject` (puppet
  slot 0x290); a `HandleObjectCarrying` detour retargets the manipulator joint to
  the hand each tick; releasing calls `DropObject` (0x29C) and sets the body
  velocity from the hand. V97/V98: ball taken, held at the hand, thrown.
- Hand weapon wheel with stock `*_Pickup.mdl` meshes via `mdlRenderModel`, sized by
  render-mesh bounds, plus Netricsa/menu/quick save/quick load bubbles (V94/V98).
- Foveated readback (ABI 13): downsampled full view + full-resolution inset per
  eye, base + feathered inset projection layers, gaze from
  `XR_EXT_eye_gaze_interaction`. V99 (forced mode 2): 672×756 base + 538×604
  inset from a 1344×1512 eye, 3682 world submissions without rejection. Composite
  appearance needs a headset; Monado builds before MR !2993 may ignore a projection
  layer's FOV without timewarp.

## 2026-10-10 (late) — inputs, render size and weapon scale from user reports

User reports after the first headset session: menu buttons and triggers did
nothing, render resolution was low and weapons looked small. Simulated Monado
runs V90–V93 (interactive hold, Index input driver `tools/monado_input_driver.cpp`)
reproduced and verified each fix:

- Fire: the trigger reached native controls (`route=ordinary fire=1`) but the
  per-hand primary projection refused coupled dual weapons (two guns without
  combo weapons) and forced the lane neutral, so nothing fired. Unrepresentable
  topologies are now left unclaimed; native Fire carries the VR trigger and both
  triggers send Fire for coupled guns. V91/V93: `DoTheFiring result=1` for both
  hands from either trigger.
- Menus: the native dispatcher takes system commands (syscmdOk 0x0B, Back 0x0C,
  arrows 0x0D–0x10, F1/F2 0x15/0x16), not keyboard ids; keys for other screens
  are posted to the game window (Core reads window messages), so no focus is
  needed. The pointer accepted only an exact menu frame and dropped every other
  poll; it now accepts a bounded lag on the same menu. V92/V93: Menu opens the
  pause menu, a pointer trigger click opens the selected submenu, Back closes it.
- Render size: eyes render at the runtime's recommendedImageRect (V92/V93:
  1344×1512 from the remote driver instead of the 1280×705 window). Core's client
  rectangle reports that size for the main window, GfxD3D's windowed depth covers
  it, and pixels travel in per-canvas shared sections (ABI 12). VR needs windowed
  mode; a fullscreen display mode has no window canvas to resize.
- Weapons scale about the hand with shots/lasers following the barrel; automatic
  default from the native weapon/world FOV ratio (V93: world 58.4°, weapon
  20–32°), tempered to the geometric mean of true and flat apparent size.

## 2026-10-10 — full-feature black world and startup stall resolved

The user resumed development after the pause recorded in
[the takeover handoff](TAKEOVER_HANDOFF_2026_10_10.md). Bounded lab receipts
(V63–V68) traced the enabled-profile black world to one cause: every
head-clearance query reached the owned native ray, then the collision
vertex-buffer resource gate cancelled it. Jungle collision meshes use
provider-owned static buffers (format 0x1f) locked through Engine's exported
`_gfxLockStaticVertexBuffer` slot, which the admission rejected as an unknown
provider. The guard then correctly masked the world before UI.

`911f803` admits format 0x1f only when both Engine slots hold the fingerprinted
GfxD3D pair (RVA 92D0: `IDirect3DVertexBuffer9::Lock` with `D3DLOCK_READONLY`
for read type 0x93; RVA 9350: `Unlock`), the buffer is not already locked and the
request fits. Other providers, write access and held buffers still cancel. The
resource ABI verifier pins the provider installation, lock flags and the absence
of cancellation gates between the native lock and unlock. Roomscale body sweeps
share these gates, so provider-buffer levels previously cancelled body movement
queries too. V70 (Roomscale/HeadComfort/Melee=1): head mode Clear, 89 triangle
kernel calls without contact, both actual 1280x720 eyes show the full Jungle
world, weapons and lasers, 611,095 of 921,600 pixels differ between eyes.

Stalled startups (V60, V61, V67, V69) left every thread in ntdll waits, with the
main thread and the former background hook worker blocked on Serious Engine
`CSyncMutex` locks (allocator / string table) during world loading. The worker
attached at an arbitrary point: it called engine string-ID functions from a
foreign thread and suspended the loading thread for MinHook patching.
`e3c4b99` runs attach once on the device-owning thread at the start of an outer
native Present, after the modules load. Ten consecutive fresh private captures
(V71–V80) then completed cleanly with Clear receipts; before it, four of roughly
eleven attempts stalled. This is strong but statistical evidence.

Astra source review: attach GO; provider-lock concerns (lock skipped by a traversal
cancel, failed lock on device loss) were checked against native code: every
cancellation stub exits only its own function frame, and device loss releases
the eye targets the query phase requires. Hardening followed (`attach` inside the
native-finally scope, no VR presentation on the attach frame, all 26 gate sites
checked against the lock span). Builds: all four products rebuilt; Debug and
Release 82/82, artifact, head-volume
(game and server) and resource-ABI verifiers pass. Neutral captures exercise no
head/hand motion, firing, vehicles, hardware or multiplayer; those remain
user-operated acceptance.

## 2026-10-10 — consume native PPM and reject opaque-world readiness

The neutral runtime summary reads the observer's actual P6 RGB PPM files, removing
the legacy collector's PNG conversion dependency. The observer now copies the
existing40-byte head-clearance record from the same locked slot as its request
and both pixel images, then serializes its mode, structural validity and values
with the capture metadata. No native game callback, IPC layout or renderer was
changed. Metadata and decoded image extents must agree; unknown, missing,
invalid or Opaque clearance cannot pass the neutral readiness summary.

Disabled/Clear mode is explicitly reported; Disabled is not collision proof.
The clearance check covers the captured baseline pair, not every counted pair.
Different pixels remain insufficient evidence of visible geometry, head motion
or proper6DoF. V62's absent mode is not retrospectively inferred or filled in.
Astra scoped source GO; current local Astra/xhigh routing verified, independent
backend attestation unavailable. The strict x86 GUI/static observer builds and
all68harness tests pass, including native-format and blocked-clearance controls.
Native products remain61ce671/IPC11/wire7. Fixing this evidence gate does not
repair the black-world cause; the bounded native head diagnostic remains next.


## 2026-10-10 — scene and native pairs completed, but both world views are black

Fresh quiet run20261010T164628 on sourcee59c6f19e9ec3b65bccb450fe82b66cd9ffe501c,
compiled61ce671/IPC11/wire7, loaded the exact Jungle scene and completed private
ID2grant/select/save preparation with Roomscale/HeadComfort/Melee enabled. It
completed99native world/UI projection responses and normal native shutdown,
with no native cleanup errors or termination requests. The private display
stopped with no survivors or cleanup errors. No startup minidump was needed.

The observer produced its actual1280x720RGB PPM eye captures. The new neutral
consumer incorrectly expected PNGs that only the legacy collector generated.
Lossless private conversion allowed actual inspection: both views are almost
entirely black with HUD glyphs; world, weapons and lasers are not visible. Only
2,336of921,600decoded RGB pixels differ. This establishes startup and transport
for this attempt, not visible gameplayVR or a head-motion/collision pass.
Earlier startup stalls remain unexplained; this run does not prove their cause.

Astra's bounded investigation confirms that rejected head clearance can still
publish a completed native pair while the bridge dims its world before UI.
V62did not record the actual clearance mode, so this is a causal inference.
The next source diagnostic is at most8matched query/eye receipts of existing
values, including the first failing predicate and computed radius before rejection.
Native near-plane overflow and an empty-read-lease defect are unproved. Keep
requested features enabled; no changed bounds, fabricated clearance or fake
world acceptance. Correct producer/consumer format and capture actual slot
clearance metadata separately. All images/raw evidence remain private.


## 2026-10-10 — optional normal snapshot for failed initialization

The existing private observer can write `MiniDumpNormal` for a failed, unloaded
neutral startup, using a retained executable/creation-checked process handle.
The system DbgHelp provider loads only for this explicit command. This adds no
live debugger attachment, full-memory capture, native gameplay call or security/
global-runtime change. Dumps and raw stacks stay private. The receipt records
API output/error/closure/liveness; it proves neither valid stacks nor resumption.
Actual dump streams, contexts, ranges, module attribution and unwind require
independent offline inspection before identifying a native waiting function.

The selected diagnostic runs after existing signal deferral, with a12-second
attempt before the normal20-second owned cleanup budget. Final retirement now
finds both IPC-token and pre-host game-path observers. Preserved process-identity
exclusions apply before every discovery branch, including overlapping arguments;
identities are deduplicated. Unresolved final retirement is a cleanup error.
World/simulation exclusion uses fresh native log and existing gameplay evidence
independently of focus/head/completeVR readiness. Outer game collection receives
12additional seconds for the selected diagnostic; tagged sweeps and survivor
checks remain required if its grace preempts inner cleanup.

Astra found and main fixed the helper-discovery gap, delayed world exclusion,
argument-overlap exclusion bypass and ineffective API-refusal regression. Final
scoped Astra source GO; current local Astra/xhigh routing verified, independent
backend attestation unavailable. All68harness regressions pass normally/optimized,
including actual matcher overlap and timeout/failed immediate retirement/final
rediscovery. The x86 GUI/static observer builds with strict warnings; native
products remain61ce671/IPC11/wire7. No new runtime or stack acceptance yet.


## 2026-10-10 — pre-host observations narrow the remaining startup stall

Fresh quiet run20261010T161632 used source760e35ab2dbe9a8c014b3549387efc10cef4f969,
compiled61ce671/IPC11/wire7 and the private full-feature profile. All77owned loading
samples stayed at stage2;76observed the game module mapped. Native startup and
activation repetition flags remained false. Partial-read zeros do not prove null
objects or identify a native instruction. No loaded-scene receipt, host log,
gameplay eye image or native crash report was produced. Per-process fsync was
selected, with no Wine ntsync assertion. Startup C++ exceptions and an audio-driver
warning alone do not establish the cause.

The bounded attempt timed out and retained its exact private logs/settings.
Passive procfs recorded21owned waiting threads, but no guest call stack. Normal
native shutdown was not observed; native cleanup reported no errors or termination
requests. The private display stopped with no surviving tagged processes or
cleanup errors. Original game/assets/settings/saves and old checkout remained
protected. The failed fixture is never rerun/resealed. No world rendering or
join-repair recovery acceptance follows from this attempt.


## 2026-10-10 — expose existing startup observations before host creation

The private observer now shares its existing native loading/input/activation JSON
fields between channel status and pre-host stock status. Module and menu-table
numbers come from values already read by the same owned-process observer. The
change adds no native reads, writes, method calls, lifecycle or readiness policy.
This fills the actual V60 evidence gap before a host/channel exists, using the
existing polling and exact process ownership rather than adding a launcher.

Short-circuit or failed reads can leave zero values. Repetition/admission flags
are observations, not atomic coherence, lifetime pins or permission for new
input. The diagnostic fields do not change activation or loading-continuation
checks. Astra scoped source GO; current local Astra/xhigh routing verified,
independent backend attestation unavailable. Strict x86 GUI/static observer
compile and all63existing harness checks pass normally and under optimized
Python. Native products remain61ce671/IPC11/wire7; this is tooling-only source.
A fresh neutral run is required before interpreting the new fields. No new
startup recovery, world-rendering, motion, firing or hardware pass is claimed.


## 2026-10-10 — repaired full-feature run timed out before scene readiness

Fresh private neutral run20261010T160619 used source
4caca4b159df3351ea5cd707ce56851924e2685e and compiled fingerprint
61ce671273a668d28bffdfd4288a2d4a29e65d7e7b60c1a92f53d0c6b0d3926b.
The independently installed profile enabled Roomscale, HeadComfort and Melee;
all inputs stayed neutral. The private D3D9 timing check and graphical simulated
Monado window initialized. Native routing and VR hooks attached, but no first
admitted presentation, host log, loaded-scene receipt or gameplay eye image was
recorded. All76owned loading observations remained at observer stage2, meaning
verified process identity without an admitted ready loading-menu state. This
marker does not identify the native instruction or worker causing the stall.

The bounded attempt ended with scene-readiness timeout. No native crash report
was produced. Normal native shutdown was not observed; native cleanup reported
no errors or termination requests. Outer cleanup stopped the private display
with no surviving owned processes, errors or deferred signals. Original game,
settings, saves and old checkout remained protected. The attempted fixture and
its exact source/settings/logs are private and immutable.

The common-join source repair has passed its compiled/native checks, but this
attempt never reached the previous failing world-load extent. It therefore
proves neither V59 crash causality nor runtime recovery. The next bounded
investigation is the existing startup/loading-owner and first-presentation route,
using these actual records before proposing a native or launch change. More
optional animation/grasp research is not an ordinary gun-attachment prerequisite.
No head motion, firing, vehicle, multiplayer or hardware acceptance was added.


## 2026-10-10 — repair model-preparation hooks at their common joins

The full-feature startup investigation found two unsafe hook placements. Native
no-child branches entered three bytes after the old hook entries, inside the
five-byte MinHook jump displacements. The gates now enter at common joinsE1F9E/
E203E. Native call paths retain their preceding argument cleanup; no-child paths
reach the same entries directly. The two displaced MOVs resume atE1FA4/E2044.
The unchanged cancellation suffix restores its stack from native EBP, so cleanup
is not duplicated. No query/physics rewrite or feature disablement was introduced.

The owned-native verifier now checks allnine gate patch spans within the complete
E1720–E2056 function:779instructions,118direct edges and no indirect jumps. It
rejects both former bad placements and two actual encoded branch mutations.
This is finite function-local coverage, not a proof about all external code.
Astra investigation and design/candidate review agreed; source GO with current
localAstra/xhigh routing verified, independent backend attestation unavailable.
Allfourproducts rebuilt at61ce671273a668d28bffdfd4288a2d4a29e65d7e7b60c1a92f53d0c6b0d3926b,
IPC11/wire7;82Debug/82assertion-enabledRelease groups, normal/optimized native+
compiled resource checks and product contracts/layouts pass. The confirmed hook
defect is fixed in source; V59 crash causality and runtime recovery remain unproved
until a fresh full-feature neutral scene run. Historical V59 is preserved.

## 2026-10-10 — actual full-feature neutral startup exposed a native load crash

The fresh isolated neutral check used compiledC0 products and a private settings
profile enabling Roomscale, HeadComfort and Melee. D3D9 device/routing, native hooks,
IPC and the D3D11OpenXR host initialized. Monado25.1.0 selected RTX4090,
XR_KHR_D3D11_enable, format87 swapchains and session states1through5 with simulated
Index profiles. It submitted no world frame. Native Jungle loading began, then
Engine raised BREAKPOINT80000003 at RVAE2059, padding after the E1720 return.
There was no loaded-scene receipt, completed world readiness or eye capture.
The IPC observer's later status3 was secondary to the native crash.

The game had already crashed, so normal native shutdown was not observed. Exact
owned cleanup completed with no errors or termination requests; the private display
stopped with no surviving tagged processes. The original installation, old checkout,
settings and saves were unchanged. All generated logs/reports stay private, and the
attempt is never rerun/resealed. Earlier neutral world captures used disabled
feature flags and a different native build; they are not an exact baseline here.

The crash does not yet identify the responsible feature/hook. A bounded read-only
Astra investigation is checking the known native model-preparation return and
existing pass-through gate/placement ABI, with no competing source writer or runtime.
Native stereo/6DoF source and earlier observations remain, but full-feature startup
must be fixed before final user acceptance. Disabling the requested features is
not the completion target. No shooting, movement, vehicle, hardware or network
input was supplied, and this failed startup is not a gameplayVR pass.

## 2026-10-10 — neutral native runtime check independent of optional grip research

The existing private display launcher has an explicitly sealed
`neutral_runtime_probe=true` mode. It reuses the same run/configuration/native
startup/neutral input/cleanup extent, with SIGTERM handling installed before run.
The optional geometry collector is not imported in this mode. Legacy geometry
collection and its stronger reference-ready requirement remain unchanged.

Neutral readiness requires the exact scene/build identity, native scene-stream
receipts, at least30distinct complete native/UI/worldprojection requests, two
bounded valid PNGs of equal size with different decoded pixels, native shutdown
and no cleanup errors or termination requests. Output is
`neutral-runtime-summary.json`, with scope `native-neutral-runtime`; head motion,
hardware, grasp and native release remain explicitly unverified. The private
launcher outcome sets neutral runtime completion separately from geometry
`data_ready_for_review`, which remains false. Different eye pixels alone do not
certify depth parallax or complete6DoF acceptance.

Astra found and main fixed both the outer legacy readiness gate and the inner
optional assessment dependency. Bounded Astra follow-up GO; current local
Astra/xhigh routing verified, independent backend attestation unavailable.
All63combined runtime/private-display regressions pass normally and with optimized
Python and through the existing CTest runtime target. The existing test target now
includes private-display cases; no new launcher/cleanup state machine or native
build input was added. Pillow12.3.0 is already installed locally and is pinned in
tooling requirements for actual decoded-pixel comparison. Compiled products still
matchc0bae37152f4b3d38129bf0392c2feed508e72df70420f0304553dff192eb990,
IPC11/wire7. No new runtime capture has been launched with this mode yet.

## 2026-10-10 — ordinary weapon calibration checks its current model binding

All16used installed native weapon slots now retain model/configuration/resource
and bit-exact stretch identity alongside their local calibration. Placement copies
these values before and after the later flat calibration getter; the first native
placement and camera calls precede this capture. Local muzzle conversion rechecks
the complete binding before and after adaptation. Authority obtains/rechecks its
own current binding. Cached instance addresses remain comparison tokens; current
native model handles are resolved anew. A same-weapon model/configuration change
cannot reuse the old calibration merely because it is younger than100ms.

The existing typechecked read-only model reader, native-thread/render-depth,
owner/unique-hand/selector and normal cleanup checks remain. Native grip correction
still covers IDs1/13 only; other numerical offsets and native animation stay
unchanged. Missing/expired ordinary calibration keeps the existing tracked-origin
fallback and uncalibrated laser rejection. No native resource getter, renderer,
protocol, combat timer or calibration manager was added.

Astra design/candidate source GO adopted; local current Astra/xhigh routing verified,
independent backend attestation unavailable. Production-predicate regressions cover
all used slots, unused/unknown IDs, correction flags and in-place model/instance/
configuration/resource/stretch changes. All82Debug/82assertion-enabledRelease groups
pass. Allfourproducts rebuilt at
c0bae37152f4b3d38129bf0392c2feed508e72df70420f0304553dff192eb990,
IPC11/wire7; both x86 muzzle cleanup gates pass normally/optimized and compiled
contracts/layouts agree. No game, firing or network probe ran. Ordinary headless
model availability at authoritative nativeWeaponReference remains unverified;
failed capture declines tracked fire admission. This is logical binding coherence,
not winning-animation, anatomical grasp, general lifetime or gameplay acceptance.

## 2026-10-10 — preserve admitted native muzzle reach for ordinary weapons

The calibrated model-to-muzzle conversion no longer truncates ordinary weapons'
admitted native offset to0.5units from the controller. The same stateless conversion
now preserves native reach for local and authoritative calls, so a long native
barrel reference can keep its laser/shot origin beyond that former radius. Charge,
static reference correction and native attachment orientation remain applied once.
Astra design ADAPT and bounded patch GO were adopted; current local Astra/xhigh
routing verified, independent backend attestation unavailable. Authoritative
output is staged and must be finite before replacing the original native result. Missing-calibration fallback, laser rejection, current owner/hand,
native dispatch and desktop/nested behavior remain unchanged.

This preserves the existing admitted reference, not universal current-model
freshness: configuration/stretch bookends remain conditional on IDs1/13; other
weapons retain their existing lifecycle and local100ms calibration admission.
No additional native reads, weapon reference geometry, IPC or combat state were
introduced. Portable cases exercise the production conversion through native/laser
parity and rejection fixtures, long offsets, both hands, native-camera movement,
charge and alignment. All82Debug/82assertion-enabledRelease groups pass. All four
products rebuilt at6da93fba945c4ced74d4a47981d7d769be9574a6c83400ba2442127ddcb0d70a,
IPC11/wire7; both x86 muzzle native-unwind gates pass normally and under optimized
Python, and compiled artifact contracts/layouts agree. No game, firing, headset or
network test was launched for this change. User gameplay acceptance remains open.

## 2026-10-10 — practical controller attachment is the requirement

The user clarified that ordinary guns need to attach properly to controllers and
use laser pointers for aiming. Detailed winning-animation/cache attribution and
certified anatomical grasp references are optional research, not prerequisites
for ordinary controller attachment. Earlier stricter alignment statements remain
historical evidence; they must not restart that diagnostic chain as a completion
gate. Preserve actual native lifetime, melee press/release and multiplayer ownership
checks. Do not label practical calibration as a certified anatomical measurement.

The existing implementation uses the physical controller grip position and
runtime aim orientation. Per-weapon position/rotation calibration is applied by
`calibratedGrip`; native model placement, muzzle retargeting and outgoing handheld
poses use that calibrated basis. Lasers originate from the retargeted native
muzzle and use native collision queries. Stock animation, native weapon eligibility,
ammo and firing remain native. Visual fit and laser/shot agreement are part of the
user-operated VR checks; no new firing or hardware acceptance is claimed.

The optional uncommitted Event-copy diagnostic was preserved privately and removed
from active source. Its cache investigation is closed. No new capture is planned
to satisfy that optional proof. Continue concrete gun attachment/laser defects and
the remaining melee, vehicle, collision and multiplayer work.

## 2026-10-10 — right-eye diagnostic copy completed

The fresh follow-up reached native Jungle and completed 126 neutral stereo world
projection pairs, then shut down native processes and the private display cleanly
with no survivors or cleanup errors. Both 1280×720 images were inspected; 602,366
of 921,600 pixels differ, with both guns and laser lines visible. All 64 admitted
native palette/API/content copies completed across both eyes. The diagnostic
reference reader still rejects its optional animation/grasp qualification. This
is not a startup failure and does not block ordinary attachment under the user's
clarification above. No firing, head-motion, headset or multiplayer acceptance is
added. Save reload remains unverified; captures and saves remain private.

## 2026-10-10 — observe each native draw without requiring a repeated event

The ID2 diagnostic adapter now accepts configuration from its own current native
draw bookends when event identity is entirely absent. It rejects malformed
absence and still requires any observed event identity to match. It leaves the
event trace untouched and retains the same native reads, model/draw/cache ownership,
full before/after equality, successful original return and normal cleanup gates.
The offline parser applies the same rule only to explicit ID2 diagnostics and
retains palette-to-own-submission root/render identity. No production weapon
reference, winning animation, grasp or SOURCE2 qualification is enabled.

Astra design ADOPT and bounded patch GO; local Astra/xhigh routing verified,
independent backend identity unavailable. All four products rebuilt with native
source fingerprint011f29229dcba94b41ef1b814a6d86084e007d1d44a1f67d5d7022779641359b,
IPC11/wire7. All82Debug/82assertion-enabledRelease groups and41reader/source
regressions normal/optimized pass, together with native/compiled submission,
projection, cleanup and artifact gates. Reassessing the previous actual capture
is exactly unchanged. The next fresh neutral capture will test right-eye native
copying; its success is not yet established.


## 2026-10-10 — native draw-copy boundary observed in stereo gameplay

A fresh isolated capture of the reviewed diagnostic reached exact Jungle and
native Auto Shotgun grant/equip/save, recorded 130 complete neutral stereo pairs,
and shut down its native processes/private display cleanly. Both 1280×720 world
images were inspected; 602,558 of 921,600 pixels differ. Both guns and per-hand
laser lines are visible. This adds no headset, head-motion, firing or multiplayer
acceptance; save reload remains unverified.

The new boundary receipts distinguish two paths. In the left eye, both hands'
32 draw rows completed the native palette/API/content bookends. In the right eye,
32 rows stopped before the native borrow because the event-derived trace
configuration was absent. Earlier palette-callback cache mismatches therefore
do not explain all draw-copy failures. The diagnostic collector still rejects
both-eye reference readiness; no grasp/alignment/reference qualification is
accepted. A bounded Astra senior review is selecting an adapter at the existing
owned draw boundary, preserving cache/animation/resource requirements.


## 2026-10-10 — diagnostic boundary source implementation and storage cleanup

The passive ID2 palette-copy diagnostic now reports the first returned failing
guard at the existing two-copy boundary. It copies only values available under
original read/lifetime guards; unavailable cache fields remain unknown. Abort,
reentry, retirement, mismatched owner and incomplete outer cleanup cannot produce
a definitive failure receipt. It does not promote SOURCE2, alignment or grasp.
All four native products rebuilt with source fingerprint
9c1df922bdfb02a4e836d2a6027830dcb8e457a75ef3f8e35d6d0c8491b1790f,
IPC11/wire7. All 82 Debug and 82 assertion-enabled Release groups pass. Game and
server native/compiled projection, submission and cleanup gates pass normally
and with optimized Python. The Astra patch review found two reader defects:
impossible API failure shapes and incomplete boundary inventories. Both were
corrected, with 39 reader/source groups passing normally and under optimization;
the bounded Astra follow-up returned GO for both fixes. Effective local
Astra/xhigh routing was verified; independent backend identity was unavailable.
No runtime capture has been made with this diagnostic yet, and no new weapon
alignment is accepted.

The user's cleanup request reclaimed 111.8 GiB by consolidating 546 exact duplicate
archive files from 39 retired agent labs into 14 independent read-only copies.
Every content hash, historical metadata and retirement mapping remains private.
The latest lab stays independent; logs, captures, profiles/settings, saves,
unique evidence, the original installation and old checkout/WIP are preserved.
Retired fixtures must not be launched or resealed. New runnable labs need
independent archive copies. Private storage reduced from roughly 201 GiB to 89 GiB.

Implementation remains open for all-weapon native alignment/grasp references,
wider positively established vehicle controls and final integration/package/test
guide. Connected roomscale/scopes/overlays/melee/remote-head/mixed desktop-VR MP
source paths still require comprehensive user/device/online acceptance. There is
no reliable completion ETA.

## Actual SOURCE2 matrix capture; draw-copy boundary still unresolved

The fresh capture with preserved game logging reached exact Jungle simulation
and native ID2 grant/equip/save preparation.138distinct complete neutral stereo
pairs and both1280x720eye images were recorded using projection presentation.
The images show the game world, both Auto Shotguns and per-hand laser lines;
605766of921600pixels differ. This neutral simulated capture adds no head-motion,
hardware, firing, save-reload or multiplayer acceptance. Native shutdown and
private-display retirement completed with no owned survivors or cleanup errors.

All32raw SOURCE2 pairs completed without invalidation/overflow: both eyes and
both hands are represented; control word127 and cache flags8→14 are consistent.
The existing Poly Bump calculation selected by the known native SOURCE2 route
reproduces every captured VP/MVP word from independent copied M/V/P inputs.
This corroborates arithmetic for these captures; it does not establish own-DIP
membership, winning cached animation, grasp or alignment.

This run emitted no usable palette/API copies. Original indexed calls succeeded,
but their metadata remained unknown. Existing Palette-stage diagnostics report
evaluated/root-linked cache mismatch in18invocations; that earlier callback is
not evidence of the failing DIP-time guard. A bounded Astra design review is
selecting the smallest passive failure receipt at the existing copy boundary.
Do not switch cache ownership to a guessed instance, relax animation/admission,
or enable SOURCE2 reference qualification solely from numerical agreement.

Native products remain b268d3e4... IPC11/wire7, with the reviewed observer-only
logging correction. All attempted fixtures and original user files are preserved.

## Raw capture startup timeout; real game log preservation corrected

One fresh neutral capture of the reviewed b268d3e4... build passed actual private
D3D9 readiness but timed out before the native scene, preparation, host or matrix
data. The native log stopped after CodecOGG module loading; the previous successful
run continued through Shaders/ProcRender/Sam2Game. No loaded-scene or raw-reference
acceptance is claimed. Native shutdown was not observed; exact-owned cleanup left
no survivors/errors and retired the private display. The failed fixture is preserved.

Both this failed run and the earlier successful run exposed a logging defect:
status observers launched Proton with logging enabled, replacing the game's
app-id log. Observer launches now copy the environment and disable only their
Proton logging; original game logging, arguments and cleanup stay unchanged.
Installed Proton's actual logging implementation and the captured final observer
headers establish the cause.34runtime-evidence checks pass normal/-O, including
unchanged game environment and observer arguments. This tool-only change leaves
all4native product identities and b268d3e4.../IPC11/wire7 unchanged.

Bounded Astra/xhigh source GO covers this logging correction; current default-role
local tags verified, backend unattested. Debug/Release runtime-evidence consumer
groups pass. It does not establish startup causation or a successful fresh run.

Next is a fresh sealed capture retaining the actual game startup trace. The
timeout does not by itself implicate the raw matrix adapter: its target scene
and weapon invocations were not observed. Original installation/WIP/user files
remain preserved; all-weapon alignment and full user acceptance are still open.

## ID2 SOURCE2 raw diagnostic connected; capture next

The observed native SOURCE2 callbacks now use the existing installed bookends,
integer sampler and fixed probe storage under a separate admitted-ID2 current
binding check. Native-finally lookup/reentry/pass checks precede the pre-snapshot
and follow the first post-snapshot, outside the matrix production interval.
Original Slots/Fog calls, cleanup and returned Fog pointer remain native.

Astra's design ADAPT found an additional raster-association consumer; its guard
was adopted. ID2 SOURCE2 cannot supply raster or palette projection sequences or
independent reference/grasp/alignment admission. The latest corresponding row
does not fall back to an older SOURCE1 match. Raw pairs remain historical copied
diagnostics when later unclassified callbacks occur. No pose/configuration or
animation-query admission was widened. Bounded Astra patch source GO, current
local default/Astra/xhigh tags verified; backend attestation unavailable.

All4products rebuilt at native fingerprint
b268d3e447c512ef3e09ffb7abd67f8e51b0928addd48d2a11c4c0b55c944196;
IPC11/wire7 unchanged.82Debug/82assert-enabledRelease groups, nine projection
verifier cases normal/-O, game/server compiled/native projection gates normal/-O
and artifact/product contracts pass. Reviewer lacked Capstone in its interpreter;
main's dependency-equipped full tests passed. Negative controls cover owner,
late failure/binding, completion and forbidden raster-reference bypasses; portable
checks cover raw SOURCE2 retirement without source1 reference promotion.

Next: one fresh matched private neutral capture of SOURCE2 control/flags/operands
and cache bookends in both eyes. Production alignment remains IDs1/13-only; own-DIP
membership, winning animation/grasp and full all-weapon equivalence remain open.

## Bounded projection opportunities measured in actual ID2 gameplay

Added invocation-local bounded scalar counters under the existing32ID2 trace cap,
without changing animation/configuration or projection/reference admission.
Bookkeeping stays outside native matrix-production snapshots; output uses copied
values after normal outer cleanup. The reader requires complete joined chronology
and retains false reference/grasp/alignment claims. Astra/xhigh design ADAPT and
bounded patch source GO, current local routing verified/backend unattested.

All4products rebuilt at native fingerprint
3f49bd6c0dc8fdb2aa26902ef41e68e2078e2ea6c3aa9d6f65ad9bb59cf8f4ab;
IPC11/wire7/layout unchanged.82Debug/82assert-enabledRelease groups and actual
game/server native/compiled projection checks normal/-O pass. Optimized C++
verification prompted a robust capacity check and invalid-count regression.

One fresh private neutral capture confirmed exact Jungle simulation, native ID2
preparation and clean native/display shutdown. All32joined opportunity owners
have zero overflow. Both eyes show source2slots/fog callbacks at pass4with current
weapon bindings; source1was absent within these admitted invocations. Source0
callbacks follow before the second submission. This does not identify every
possible producer or establish reference/animation/grasp validity.

The next source decision is support for the observed source2path in the existing
adapter, including unclassified later-callback handling. No sampler was widened,
no trace configuration was seeded and no alignment correction was activated.
Captured data/assets remain private; hardware, firing and multiplayer acceptance
remain user-operated. Earlier3f49-predecessor identities below are historical.

## Actual neutral Auto Shotgun capture; reference gate still open

The rebuilt observer successfully opened IPC11 in a fresh private capture.
The exact Jungle scene hash was confirmed at native simulation; native ID2
grant/equip/save completed,135distinct neutral complete stereo pairs were
observed, and both1280x720eye images were captured with projection presentation.
Native shutdown and private-display retirement completed with no cleanup errors
or owned survivors. No firing/movement or actual headset/network test occurred.
The private cheated save exists; its reload is not yet verified.

All32copied palette-content draws match the characterized stock mesh variant;
none qualifies as an independent projection reference. Each reports
`projection-association-unavailable`; the raw producer probe has no pairs.
Second-eye traces lack the animation-query/config receipt despite successful
native indexed draws. These are actionable diagnostic results, not evidence
that alignment, winning animation, GPU geometry or physical grasp is complete.
Production alignment remains IDs1/13-only. A bounded Astra design review is
examining the smallest current-draw resource/projection diagnostic boundary,
without replacing native owners, rendering or animation.

## Capture authority resolved; stale observer rejected before launch

The user explicitly permits all necessary captures, including private neutral
weapon-reference preparation. Earlier pending ID2 authority entries below are
historical. Preserve private-display/no-desktop-interference and no-firing/movement
limits; broader gameplay and device/multiplayer acceptance remains user-operated.

An authorized fresh private run exposed a stale observer: its compiled Channel
checks were IPC10/shared83887840 while all installed products were IPC11/
shared83887888. Actual D3D9 readiness passed, the expected native scene stream was
opened and the OpenXR host initialized, but observer status3 stopped collection.
This was not native geometry or gameplay acceptance. Owned cleanup had no
survivors/errors; native shutdown was not observed. The failed fixture is preserved.

The helper now exports its compiler-derived read-only layout. Preflight requires
a unique bounded export in GUI PE32 read-only/non-executable data and compares
ABI plus all five structure sizes with the matching installed products. Helper
source and reader are added to the existing preparation tool seals. No production
IPC/native source, rendering, physics or transport changed. Native fingerprint
0047bd77db502e9a24addda7dbfdf3f9bbaed7df42ee8b7de78891582d1eaa9e remains current.

Astra/xhigh scoped source GO, local current-turn tags verified/backend unattested;
its PE32 optional-header check was adopted with a regression.33runtime-evidence
checks pass normal/-O;24existing private-display checks and12actual compiled-PE
mutation cases pass. The relevant Debug/Release CTest group passes. A fresh
sealed fixture uses the rebuilt GUI observer; actual IPC opening and collected
weapon-reference evidence remain required before alignment changes.

## Matched ID2 collector refreshed after continuity fixes

A fresh V48 private package/fixture now matches native fingerprint
0047bd77db502e9a24addda7dbfdf3f9bbaed7df42ee8b7de78891582d1eaa9e,
IPC11/wire7. The reviewed collector wrapper and source tools are unchanged;
staged products/install receipt,17file seals,3exact link targets and3independent
profile-file copies were checked. Exact scene/archive bytes and neutral disabled
swimming/melee settings were independently verified; check-only preflight passed.
No game, Wine, Monado, display, native preparation or input was launched.

V46/V47 and all prior fixtures remain preserved. Private identities/guide record
the exact V48 paths and hashes. The existing ID2 grant/equip/save capture question
still has no answer; preflight is not authorization or loaded-reference evidence.
The next alignment operation is this observation after authorization. No guessed
grasp correction or full-goal/hardware/multiplayer completion claim.

## Physical gesture: skipped grip-loss correction

Fixed the same demonstrated continuity class in physical melee: an omitted grip
loss/recovery could bridge3cm/20ms into an eligible high while the trigger stream
was unchanged. The existing grip generation now reaches sampling, local current
validation and pose publication. Replaced/zero generations revoke provenance;
recovery uses the existing causal reset, fresh baseline and new quiet requirement.
Native consumption/release/manual160/348 and wire7 remain unchanged. No new registry
or protocol; accounted native High still retains its reconciliation obligation.

Astra design ADOPT and bounded patch source GO, current local routing tags verified
per turn/backend unattested. Main's actual i686 compiler layout probes confirm the
new word consumes former padding: size176, alignment8, last164, new field172.
All4 products rebuilt at0047bd77db502e9a24addda7dbfdf3f9bbaed7df42ee8b7de78891582d1eaa9e,
IPC11/wire7 unchanged.82Debug/82assert-enabledRelease groups, game/server compiled
unwind normal/-O and artifact/product contracts pass. Native contact/release,
gameplay, headset and multiplayer runtime acceptance remain untested. The protected
installation, original WIP and user profiles/settings/saves remain unchanged.

The reviewed V47 collector was refreshed and fully seal/preflight checked before
this additional src correction, with17files/3exactlinks/3independent profile files.
It is now preserved and stale, along with V46. A fresh matched package/fixture is
required before capture. ID2 grant/equip/save authority remains pending. Full
alignment/wider controls and user hardware/mixed-MP acceptance remain unfinished.

## Optional swimming: skipped tracking-loss correction

Fixed a demonstrated cross-feature continuity defect: recovered grip positions
could generate arm-pull input when the game missed the intervening tracking-loss
sample. Swimming now uses the existing per-hand grip generations, reseeds on
either change, rejects zero and checks captured/current agreement before and
after native direction preparation. No new IPC, transport or physics path.
Joystick priority/head-directed fallback and default Immersive=0 are preserved.

Astra identified the source chain; main reproduced the pre-fix portable output1,
then added skipped-loss producer/consumer and late-generation regressions.
Bounded patch source GO, local current Astra/xhigh tags verified; backend
unattested. A subsequent exhaustion-to-zero regression covers the existing
producer boundary. This is not a native swimming or headset pass.

All4 products rebuilt; native contract9f73c429305371d11581e68f79d23fc228036c042f3690680c104ffb1c45d790,
IPC11/wire7 unchanged.82Debug and82assert-enabledRelease groups pass. Three
Release Python groups initially lacked the existing dependency environment;
all three passed when rerun with the correct PYTHONPATH. Native/compiled water
ABI checks pass normal/-O; artifact/product contracts pass. Exact product hashes
and counterexample remain private. No game/runtime, firing/movement or vehicle
probe, installation or capture occurred. The old prepared V46 collection products
are now stale against current source; preserve them and stage a fresh matched
package/fixture before any authorized capture. ID2 authority remains pending.

## ID2 collection evidence connected and replay budget bounded

The existing offline collector now consumes the separate palette replay output.
Copied channel matches, qualified numerical references and two-eye cohorts are
reported independently. Cohorts require identical request/input/native binding,
asset/channel and copied instance/surface/config/world/canonical identity, with
at least one qualified draw per eye. Each draw retains its own camera sequence
and submission/ordinal. Multiple material draws can form one cohort; no one-to-one
correspondence, winning animation, GPU/grasp/alignment or complete coverage claim.

The collector now shares one120-second absolute replay deadline across all
existing evaluators, reducing the previous per-child10-second allowance to the
remaining budget. Expiry preserves copied inputs/receipt and failure.json through
existing cleanup. This is a checked replay/subprocess allowance, not a hard bound
for filesystem/scheduling/output writes. Native runtime cleanup occurs first.

Astra/xhigh reporting ADAPT was adopted (cohort naming plus four-draw regression),
and final deadline/wrapper delta received bounded source GO. Local tags verified;
independent backend attestation unavailable.11collector and13replay cases pass
normal/-O; seven relevant Debug and assert-enabled Release consumer groups pass.
The real synthetic two-eye assessment→match→replay→summary path is covered.
Pure Python/tool-test changes leave native fingerprint98ebe103... IPC11/wire7 and
all4 previously built product identities unchanged; no native rebuild required.

A new private collection path is being staged from the reviewed V46 wrapper.
Older V45 preparation remains preserved and intentionally stale after source-tool
changes. Final fresh package/config/receipt/profile/inode/scene seals and offline
preflight are recorded in the private handoff. No game, Monado, Wine, display,
weapon grant/equip, firing/movement or hardware test has run. Current native ID2
runtime preparation remains authorization-pending. The runnable collection obtains
loaded reference evidence needed before a justified Auto Shotgun grasp correction;
it does not finish all-weapon alignment or the full mod goal.

| Review recommendation | Disposition |
| --- | --- |
| Do not call four independent submissions one matched pair | Adopted: cohort terminology and explicit four-draw regression. |
| Preserve separate content/reference/grasp acceptance | Adopted: qualified reference counters never promote GPU/whole-trace/grasp flags. |
| Bound aggregate CPU replay, not only each child | Adopted: one deadline reaches legacy, retained and palette evaluations. |
| Preserve copied evidence on budget failure | Adopted: existing input receipt/failure/temporary cleanup path tested. |
| Promise a hard whole-command wall clock | Rejected: only checked replay/child allowances are claimed. |

## ID2 own-submission camera receipt and multi-palette CPU replay

The selected Auto Shotgun diagnostic now records its exact latest SOURCE1 camera
producer across the existing native/API/original submission bracket. Raw camera,
cache/control and model/draw/world words must stay current; an older address match
cannot supply the reference. The diagnostic probe continues after ID2 Palette-stage
single-affine rejection. An optional receipt mismatch suppresses only that receipt,
preserving native palette diagnostics and all existing original/cleanup gates.

The existing offline evaluator now supports actual rigid local indices0/1/2 and
one to three canonical matrices. Its separate palette consumer requires the own
cold PC24/nearest producer receipt, reproduces MVP independently from native M/V/P,
and stages local transforms through the existing position interpreter. It does
not promote agreement to whole API coverage, world visibility, grasp or alignment.
Actual ID2 acquisition/program/reference/grasp remain unobserved. No game launch,
fixture grant/equip, hardware test, install or staging occurred.

Astra/xhigh design ADAPT and receipt P1/P2 findings were adopted; final bounded
source GO. Local current routing tags verified; backend unattested. A nonblocking
review finding corrected the collapsed-index test to use valid index-zero routing,
then require projection mismatch at vertex1 instead of merely address overflow.
See ID2_PALETTE_COMPANION.md for policy and historical reports.

| Review recommendation | Disposition |
| --- | --- |
| Replace address search with submission-time live-camera receipt | Adopted; exact latest raw producer, every existing native/API bookend. |
| Keep native diagnostics when optional projection/upload disagree | Adopted; sequence0, independent native-copy equality and strict reader. |
| Record positive test before pending row finalization | Adopted; actual reserve/before/after/finalize lifecycle. |
| Use existing VM/canonical equality, no additional inverse bind | Adopted; bounded schema4 and separate palette results. |
| Test valid collapsed index route | Adopted; mismatch at second distinct canonical vertex. |
| Infer GPU/grasp/complete-mod success from CPU agreement | Rejected; all broader acceptance flags remain false. |

All4 products match fingerprint
98ebe10308471db3ca952e8c58030df16490b774c910364d0590e96cb913eba7,
IPC11/wire7, Input272/Request440/Shared83887888.82Debug and82assert-enabled
Release groups pass.33reader and12replay cases pass normal and Python-O,
including synthetic full collector→match→CPU replay. Both game/server compiled
submission, integer camera sampling and native collector gates pass; relevant
optimized gates and artifact/product contracts pass. Synthetic tests are not
native observations. Exact artifact SHA identities remain in the private manifest.

The requested independent tool research found only secondary new UI/network/
server-status aids; adopted appropriate optional USER_VR_TEST.md checks. Existing
Editor2/Edit Data, evaluated importer and Macro authoring remain the useful asset
paths. Helicopter model/skeleton/animation/puppet/debris investigation established
no positive driver control within its finite inspected chain; exact uninspected
coverage is retained in IMMERSIVE_STEERING.md and private evidence.

Next implementation/evidence: actual all-weapon grasp/reference coverage and wider
positively established vehicle controls. First-use saw, controlling-local MP origin
capture and mono remote heads already have source callers; native/hardware contact,
lifetime, mixed-role correction convergence and appearance acceptance remain open.
The full goal is active; user primarily tests shooting, vehicles, headset and MP.

## Physical hover-grip source integration

Connected successfully submitted world-pair handle geometry to the borrowed native
mode2 look callback. IPC11 appends world-receipt/grip-continuity metadata while
Input272/Request440 and wire7 remain unchanged. Actual triangle contact, one-/
two-hand yaw, continuous transfer and causal Low/Down rearm use bounded local
value copies. Native throttle/aim/fire/physics and ClientAction remain the owners.
Vehicles.ImmersiveGrips defaults on for admitted Fighter/Saucer handle profiles;
literal wheels and wider control coverage remain required.

Astra ADAPT source findings are closed; final bounded caller-wiring/relocation
fixes received GO. Current local Astra/xhigh tags verified per turn; backend
unattested. Native heading radians/sign/angular wrap are statically established;
equivalent targets can differ in native wake bookkeeping. See
VEHICLE_GRIP_INTEGRATION_PLAN.md and NATIVE_HOVER_HEADING.md. This source checkpoint
is not physical-vehicle or complete-mod runtime acceptance.

All4 products match fingerprint
46e1a609659bc0c5002ce9cbe2ff14dfa83335f55192670f2ef4352e7c254520,
IPC11/wire7.82Debug/82assert-enabledRelease groups and relevant normal/-O
compiled/corruption/artifact gates pass. New checks reject all four actual caller/
COFF falsepasses discovered by Astra, plus earlier owner/cancellation/reentry
cases. No runtime, installation or staging occurred.

Next: broaden actual authored vehicle-control coverage without guessed wheel
geometry, and verify remaining all-weapon grasp references and multiplayer convergence.
First-use saw and mono remote-head source paths are already connected; their
runtime contact/lifetime/appearance acceptance remains open. The full goal and user-operated hardware/
shooting/vehicle/MP acceptance remain active. Existing-tool research is recorded
in EXISTING_SS2_TOOLS.md; use the validated importer and Editor2/Macro authoring
for the specific remaining geometry/fixture needs.

## Completed stereo ride-bank handoff fixed

Found and corrected an actual source ordering defect: endEye clears the native
draw-use flag before stereo Ready, but copied ride publication required it.
The retained bank owner survives that transition. The existing gate now has
Drawing/Retained phases; only the successful copied-value publisher uses Retained.
Seven native borrowers keep Drawing, and both phases preserve all bank/thread/
reset/suppression checks. No new lifetime state or native draw-retarget behavior.
See RIDE_RENDER_OBSERVATION.md for exact ordering and limits.

Astra/xhigh source GO; current local routing verified/backend unattested. All4
products rebuilt at fingerprint
7aacdd344915191190993727b80f5f0088e70cf3a9852a180623c0856a83294d,
IPC10/wire7;82Debug/82assert-enabledRelease groups and18 relevant normal/-O
compiled/artifact/mutation reports pass. New regression covers the actual
draw-end/bank-retained phase order, with four source bypass controls and all eight
compiled false-gate consumers. No runtime/staging/installation.

NEXT: production geometry value handoff must occur within successful commit,
before its finally invalidates the bank token. Join coherent current local rider,
model/config/parameter and rig/input/rearm ownership; do not borrow the bank after
commit returns or promote diagnostic budgets into continuous grabbing. The next
design review evaluates a completed native-render geometry reference, not an
unsupported current-input Main/Seat query. Physical controls and full goal remain open.

## Current vehicle handle geometry and transform adapter

The existing default-off ride GPU collector now extracts both actual handle
meshes only after all five known channel fingerprints match. It preserves their
native vertices/winding and checks actual local Main weight/index ownership.
The bounded existing position interpreter has a separate vehicle input policy;
weapon limits remain unchanged. An accessor reuses the current executed stereo
root V/adjusted P, with exact player/eye/graphics/tracking/rig/root ownership and
camera bookends through the final Release. Per-vertex clip agreement and a
conditioned independent-camera backprojection within1mm govern copied world
geometry. The separate diagnostic reports CPU agreement with zero grasp/steering
claims; mono/nested/unsupported contexts decline. See RIDE_RENDER_OBSERVATION.md.

Astra/xhigh design ADAPT, source GO and fresh bounded follow-up GO; current local
routing verified per turn/backend unattested. Adopted payload clearing only on
charged attempts and noncommuting/collapsed-clip numerical regressions. Four
products rebuilt at fingerprint
b69204fa9eb7dbffe3c6b1591a750e5481f4d63ff3796b26eff0249cf4e5c7d9,
IPC10/wire7;82Debug/82assert-enabledRelease groups and14 relevant normal/-O
compiled/artifact reports pass. Initial full build had one stale checker literal,
corrected with three new negative controls. Both owned-resource parser checks
pass: Fighter25/25 and Saucer35/35 vertices. No runtime/installation/staging.

Existing-tool research additionally identifies SE2 Macro guides for private
fixture authoring; the validated importer parser remains reusable, and
SeriousSaveEditor explicitly does not support SS2. See EXISTING_SS2_TOOLS.md.

NEXT: join copied current world geometry to a current native rider/input interval
with grasp/rearm/hand transfer and continuous production ownership. The existing
diagnostic64-attempt cap is not production gripping. Live shader admission,
alignment/physical controls, melee lifetime/release, authoritative multiplayer
settlement and real-device Windows/Linux acceptance remain open. Full goal active.

## Vehicle GPU evidence adapter implemented

The existing default-off ride collector now optionally copies actual bound
declaration, vertex program, raw float constants, stream/descriptor keys and five
owned-slice digests for the exact two characterized vehicle surfaces. It reuses
the existing GPU probe owner, locks, scratch and cleanup. Only index scratch
capacity grows; weapon limits remain unchanged. Eight attempts per eye/bank and
64 per process bound failed/unpublished work. Schema5 retains schemas1–4 and
independently valid mapping-only output. See RIDE_RENDER_OBSERVATION.md.

Astra/xhigh design ADAPT, native/consumer GO and final bounded checker-fix GO;
current local routing verified per review turn/backend unattested. All4 products rebuilt at
dfce2d83925c815089118a80cfe5f7d746ba1aa031e5a9457fb9d31cce623fd4,
IPC10/wire7;82Debug/82assert-enabledRelease groups and28 relevant normal/-O
compiled/artifact reports pass. No runtime/input/vehicle launch, installation
or staging. Separate slice digests and matching sampled metadata do not establish
an immutable content epoch, shader consumption, usable grasp or completed steering.
Next consume exact content and position-program evidence to derive current handle
geometry, then join native input freshness/rearm/hand transfer. Full goal remains open.

## Vehicle actual Main draw association implemented

The existing default-off observer now binds the occupied ride's Main/Seat frame
to an actual native indexed draw with exact root/model/configuration/resource/LOD
and Main-definition ownership. It records the actual local Main slot on a bounded
multi-bone surface; no whole-draw affine or authored slot is invented. Original
successful forwarding and complete pre/post bookends precede certification after
all COM Releases. Fresh reentry/generation/routing and owner checks gate normal
native-finally publication of an immutable receipt into the existing bank.

Schema4 adds up to eight draw copies per eye and explicit overflow, raw actual
palette/world matrices, API arguments and channel descriptors. Zero/asymmetric
eye inventories preserve Main/Seat. Schemas1–3 remain readable. Declared inventory
completeness is not native draw coverage, GPU/program evidence, grasp or controls.
See RIDE_RENDER_OBSERVATION.md and IMMERSIVE_STEERING.md.

The Saucer handles belong to a15-bone surface, so SingleAffine is insufficient.
Fighter's one-bone surface has2806 triangles versus existing2673-triangle idle
storage. Next adapt existing bounded buffer/program plumbing with an explicit
vehicle policy and indexed Main-weighted subsets; ID2's three-entry policy stays
unchanged. Physical controls and input-time freshness remain unimplemented.

Astra/xhigh design ADAPT and final source GO; local routing verified, backend
unattested. All four products rebuilt at fingerprint
fe0b725923e0e08543d6ccf134ba7bdb54a86e1bf58a83dfdbc83b76fc9d92ae,
IPC10/wire7.82Debug and82assert-enabledRelease groups,12reader cases normal/-O,
relevant compiled owner/forwarding/unwind/dispatch checks and16 Core-query mutation
controls normal/-O pass. The finite Core proof assumes pinned unmodified code/imports
and normal loader lifetime; its sentinel is not initialized-main-thread proof.
No game, editor, vehicle, input, headset or network runtime, installation or staging.

## Vehicle matrix semantics corrected

The copied canonical Main/Seat matrices are native draw-palette sources, not
ordinary bone placements. The pinned native bone-placement helper removes the
stored inverse bind using its rigid inversion import; the draw-palette producer
copies canonical P directly. Existing observer/reader schemas and raw words are
unchanged. The reader now explicitly denies bone-placement-copy and actual live
draw-map verification. The actual admitted draw palette remains the reusable
transform for proven Main-weighted authored vertices. Extra inverse-bind application
or final-draw equality inferred from the producer is unsupported.

The existing native verifier adds 41 Engine instruction sites/import identity;
44 altered instruction/import/ABI controls reject normal/-O. This is bounded
static evidence, not a new native query, GPU validation or physical grasp. Source
rendering is unchanged and vehicle physical controls remain unimplemented pending
the actual surface/resource/LOD/program association and input lifecycle.

Astra/xhigh design ADAPT and source GO, local routing verified/backend unattested.
Ten reader cases pass normal/-O and the registered Debug/Release groups pass.
The four existing native products retain exact hashes and fingerprint
f8eb2b3474a1bcf1c982d9e4fcae1b767d2b3420921f0d383a82ce6af3d8fc87,
IPC10/wire7. No native source/CMake edit or rebuild, runtime, staging or installation.

## Observer gesture receipt context repair

Integrated combat-only source ec6271f29c188757987af6d7b85da829e1d806d3 prevents a
captured receipt from validating or consuming a replacement physical-melee edge
with coincident sequence/generation/millisecond stamps but different hand context.
Native wrappers, wire layout, damage/release history and per-hand dispatch are
unchanged. Final Astra/xhigh bounded source GO; local routing verified, backend
unattested. All four products rebuilt at fingerprint
f8eb2b3474a1bcf1c982d9e4fcae1b767d2b3420921f0d383a82ce6af3d8fc87,
IPC10/wire7; 82 Debug and 82 assertion-enabled Release groups plus 12 relevant
compiled/artifact reports normal/-O pass. No runtime or staging. Actual native
interleaving, first-use/copy/release/contact and multiplayer gameplay remain open.
See OBSERVER_GESTURE_RECEIPTS.md. Other held reference contributions were not imported.

## Optional live flat attachment metadata

The existing ride observer now optionally maps the current native occupied-seat
name to its attachment descriptor. It declines pending parameter resources, proves
actual flat child membership, copies declared pose/scale through complete bookends
and preserves the existing Main/Seat copy on mapping failure. Schema3 reports normal
child-record presence separately and never synthesizes child world. Crossed stereo
metadata clears both optional copies. Astra/xhigh design/source GO; current local
routing verified, backend unattested. See RIDE_RENDER_OBSERVATION.md.

All4 products rebuilt with source fingerprint
03892dd28f8ceaaecfe40973aef873ede6652b68ab7839207f0b0446ab8758ed,
IPC10/wire7.82Debug/82assert-enabledRelease groups,10reader cases normal/-O and
54compiled/artifact/mutation reports pass. Final scalar checks also cover both raw
samplers and all four owner-gate consumers. Initial CTest failed one stale schema2
source-mutation literal while the reader worker was editing; the corrected checks
pass. New consumer JNE rejection fallthrough is verified against actual bytes.
No game/editor/vehicle/input/runtime launch, package staging or controls activation.

The external parser matched20 authored child descriptors across Fighter/Saucer;
normal rendering omits their null-model Seat children. Native original query/TLS
scaffolding and mandatory childWorld were therefore removed from the design.
Actual grip geometry needs current mesh/palette/LOD/resource association, then
input-interval/lifecycle/rearm integration. The copied MainCanonical is a native
draw-palette source, with current draw mapping/content still unproved. Physical controls,
full alignment, melee lifetime/release and authoritative MP settlement remain open;
the full goal and user/device acceptance are unchanged.

## Existing-tool adoption and occupied-seat mapping

Follow-up research identifies official editor Entity IDs/debug variables and an
author-documented AWF export route; current native SDK/symbol support remains
unverified. The unchanged external standalone parser now matches authored seat,
attachment and ride-part names for four characterized hovercraft parameter
resources against the independent decoder. No editor/game launch occurred.

The existing native steering verifier now binds the occupied-name → attachment
route with 33 instruction sites, three exports, six hover dispatch slots and three
import identities. Normal/-O positive gates pass; 45 in-memory native-byte/metadata
controls are rejected in both modes. Astra/xhigh bounded source GO, current local
routing verified and backend unattested. This supplies a future integration boundary,
not safe-query lifetime, loaded resource correspondence, grasp or controls.
See [native seat evidence](NATIVE_VEHICLE_SEAT_AUDIT.md) and
[tool adoption](EXISTING_SS2_TOOLS.md). The current sweep already supports rotated
swimming capsules, and head-volume capture has no single-player-only gate; older
inactive/upright-only notes are historical.

This tools/docs-only change preserves all four native products and fingerprint
1a453f2210cbefa6199c866aaac2f14317fbb106bc2cfc6d79e978fb0b17aec2,
IPC10/wire7; current product-contract verification passes. Previous 82-group builds
remain the native build evidence, not new runtime acceptance. Physical controls,
full alignment, melee lifetime/release, authoritative MP settlement and user/device
acceptance remain open. No new runtime hook, staging or native execution.

## Paired live Main/Seat source capture; external parser evaluated

The existing optional vehicle render observer now copies distinct Main and Seat
from the same validated model/selected LOD/canonical cache, using global bone
indices and the existing raw bookends. Schema2 includes the source fingerprint;
the existing ride assessor reads complete mono/stereo groups without promoting
Seat-name membership to operated-seat attachment, grasp or input-time freshness.
Historical schema1 remains source-unverified and Seat-absent. Astra/xhigh design
and corrected-prefix source GO; current local tags verified, backend unattested.
See [paired frame evidence](RIDE_RENDER_OBSERVATION.md).

All4products rebuilt after source/test-registration changes with fingerprint
1a453f2210cbefa6199c866aaac2f14317fbb106bc2cfc6d79e978fb0b17aec2,
IPC10/wire7.82Debug/82assert-enabledRelease groups,9normal/-O reader cases and
54normal/-O compiled/artifact/mutation reports pass. An earlier targeted CTest
regex matched no tests; the ride reader is now explicitly registered and passed
as group81. Earlier81-group/pre-registration builds are historical evidence.
No game/editor/headset/vehicle/network/firing/movement run or package staging.

The author-linked SE2 importer standalone parser was evaluated privately against
both characterized AutoShotgun variants and the Fighter embedded model. Channel
ranges/hashes, buffer identities, counts/bounds and Main/Seat local transforms
agree with existing independent decoders. No Blender UI/import conversion was
executed. This can reduce further authored resource decoding; it does not prove
runtime loaded resources/animation, grasp or controls. See EXISTING_SS2_TOOLS.md.
Physical cockpit controls, broader weapon alignment, multiplayer authoritative
body/origin settlement and full user/device acceptance remain unfinished.


## Multi-palette content snapshot and existing-tool research

Optional ID2 schema2 diagnostics now copy five bounded content hashes alongside
native palette/API evidence through existing owners and cleanup. Shared offline
bounds and candidate matching keep palette results separate from affine geometry
or replay admission. Astra/xhigh design and native/consumer source GO follow two
lock barriers and exclusion of prior failed collection attempts. Local routing
verified; backend unattested. See [content evidence](ID2_PALETTE_COMPANION.md).

All four products rebuilt at
526ed4ff593dd670f562d92892f3207c1f8752abd6f3e49a76250bcd6c3109c9,
IPC10/wire7.81Debug/81assert-enabledRelease groups,30normal/-O reader regressions
and58normal/-O compiled/artifact/mutation reports pass. No runtime/editor launch,
package installation or new alignment acceptance. ID2 fixture preparation/runtime
scope remains separate from this source instrumentation.

[Existing SS2 tools](EXISTING_SS2_TOOLS.md) records the verified official2025Edit
Data, bundled editor plugins, author-documented SE2 Blender importer and resource
packer. They may reduce authored asset work; fidelity/build/license checks and
live instance/cache/native ownership evidence remain necessary. The protected
installation was read only. Full weapon alignment, physical cockpit controls,
multiplayer body/origin settlement and the full goal remain unfinished.


## Joined palette reader connected to the existing collector

The existing submission reader now consumes bounded ID2 native/API companions
and preserves multi-bone diagnostic metadata. It checks exact owners, reservation
ordinals, complete per-row arrays and duplicated draw words; consistent canonical
disagreement remains data. Missing emitted rows do not establish coverage and all
content/index/GPU/grasp/alignment claims remain false. No replay admission change.

Astra/xhigh design/source GO follows corrected zero-request and contradictory
single-bone-name mutations.27normal/-O cases and7Debug/Release consumer groups
pass. The preserved V42ID13 log re-read yields7complete copied observations and
25diagnostics, without launching a target. Native/products remain at
d836a37c958f4e16c6efa8ca3e010adf8c1b3951afbe075c4e064353fbbdf8c1,
IPC10/wire7; pure Python changes require no rebuild. See ID2_PALETTE_COMPANION.md.
Actual ID2 program/index/reference evidence and full remaining implementation
remain open; reader readiness is not alignment or runtime acceptance.


## Joined ID2 palette evidence connected; REA static provider usable

The optional draw-local companion now copies1–3 mappings and independent canonical
matrices beside the actual shader/constants/declaration/bindings under the existing
submission owner. Emission requires matching bookends, successful original draw,
post-release validity and normal complete outer cleanup. Existing count-one affine
admission remains unchanged; certified pre-state is reused without extra queries.
No register/base/index association, authored grasp or native replay is inferred.
See [diagnostic and Astra disposition](ID2_PALETTE_COMPANION.md).

All four products rebuilt at source fingerprint
d836a37c958f4e16c6efa8ca3e010adf8c1b3951afbe075c4e064353fbbdf8c1, IPC10/wire7.
81Debug/81assert-enabledRelease groups and58normal/-O compiled/artifact reports
passed. Astra/xhigh source GO follows two corrected callback/state-ordering issues;
local routing verified, backend unattested. No new runtime/geometry acceptance.

User-installed REA6.3.0 works through its configured Ghidra12.1.2/Java21 CLI.
A static pinned Engine upload-function check succeeded; raw evidence remains
private. It corroborates existing static facts rather than runtime association.
See [REA workflow](REA_WORKFLOW.md). All-weapon alignment, physical cockpit
controls, multiplayer body/origin settlement and full goal remain unfinished.


## Controlled ride/render association implemented; copied rigid inputs added

The optional ride diagnostic now positively joins the local operated hover ride
through its controlling brain and renderable owner backlink to a unique native
model record and selected Main cache. It uses the existing frozen render bank,
fresh dependent resolutions and two complete raw-word bookends; exports require
normal mono or successful stereo completion. Foreign callbacks decline before
ordinary bank reads. No resource/Seat/grasp/steering or vehicle runtime completion
is claimed. See [render observation](RIDE_RENDER_OBSERVATION.md).

The offline position VM accepts explicit copied rigid palette indices0/1/2 and
strict weights in the5/6 and7/8 input families. Legacy defaults and integral-only
address rejection remain. Native multi-palette admission/ID2 replay remain disabled
pending actual draw/program/upload association and independent references. See
[Auto Shotgun boundary](IDLE_AUTOSG_COLLECTION.md).

Astra/xhigh Source/tool GO after correcting the bank-read race, admission-prefix
and completion-order checker gaps, and a vacuous negative fixture. Current local
routing tags verified; backend unattested. All four products match source
fingerprint173cc0fe88540e77298c559c7291d6076494d7748eabe546bba4b10aa11254d0,
IPC10/wire7.81Debug/81Release groups pass; normal/-O game/server forwarding,
unwind, ownership-decline and actual-byte controls plus matching artifacts pass.
These are offline results. No gameplay/input/vehicle/firing/headset/network launch.

Non-scoped per-hand lasers and mixed desktop/VR participation remain explicit
requirements with source paths connected. Visual all-weapon alignment and actual
native mixed multiplayer remain user acceptance. Full physical cockpit controls,
broader alignment, multiplayer body/origin settlement and full goal remain open.

## Non-scoped native laser coverage established; mono remote heads connected

The native audit established the original shooting getter for15 non-sniper weapon
classes and the separate sniper getter, preserving native attachment and dual
matrix subdispatch. The inspected collision route supplies the nearest eligible
straight-ray hit. No extra getter, physics change or relaxed alignment bound is
indicated. Main spot-checked hashes, exports and all16 vslots. Per-hand wheel/loss,
invalid endpoint and request-replacement regressions pass. Visual calibration and
all-weapon grasp alignment remain user acceptance; see LASER_NATIVE_COVERAGE.md.

Remote heads now use the existing frozen bank during an admitted outer mono world
draw. Exact bank tokens retire abandoned stereo publication and mono ownership;
nested/reset continuation suppresses both adapters until outer cleanup. Original
rendering stays once-only per native draw. Astra/xhigh Source/tool GO after fixes
to parent teardown, composed reset restoration and compiled callback binding;
current local tags verified, backend unattested. See MIXED_DESKTOP_VR.md and
REMOTE_HEAD_RESOURCE_OWNERSHIP.md. No native/gameplay/network probe was run.

All four products rebuilt with fingerprint
63eebe666f9deb0c2c2dbe07111af634c4ad25e897f91c4da37acf9ab251db41,
IPC10/wire7;81Debug/81Release groups pass. Actual callback/receiver/cleanup gates,
mutated-byte controls and matching product/layout checks remain separate from
runtime acceptance. Mixed desktop/VR native input ownership was fixed in the
previous source checkpoint. Vehicle grasp/resource/frame association, broader
weapon alignment and full user/device/multiplayer acceptance remain open.

## Mixed desktop input fixed in source; native laser coverage review active

The existing peer admission now separates transport capability from positively
accepted XR gameplay ownership. Hello-only/neutral desktop peers keep native
primary/history and equipment controls while continuing to consume/ACK packets
and receive VR presentation. Exact same-owner renewal retains XR ownership;
replacement/disconnect clears it; tracking loss retains managed-neutral VR input.
Four native gameplay/presentation queries use the same ownership predicate.
See [mixed desktop/VR](MIXED_DESKTOP_VR.md). Astra/xhigh Source GO; local tags
verified, backend unattested. Portable predicates and projection/credit/codec
checks passed; native RPC/gameplay and mixed multiplayer are not runtime-verified.

The passive ride diagnostic additionally has a bounded borrowed getter-time
receiver/renderable/instance join. Its actual MinHook caller is trampoline+8;
trampoline rejection uses common rollback. It adds no pointer dereference or
cross-frame owner map. The separate render-command ride-owner edge, resources,
evaluated control frame and physical grabs remain unimplemented. See
[ride observation](RIDE_CONTROL_OBSERVATION.md). Source and finite compiled
consumer Astra GO; no vehicle run or completed physical steering is claimed.

All four products rebuilt with fingerprint
605c07abcb4aa82104842c886198438a192a29382f6a2cceffdf73c975913583,
IPC10/wire7;81Debug/81Release groups passed. Both x86 objects pass normal/-O
control/getter forwarding/cleanup and actual-byte counterexamples, including
nonzero finally-delegate relocation addends. Multiplayer dispatch/remote cleanup
gates and artifact/layout verification pass. No game, firing, movement, network
or headset was launched. Non-scoped per-hand lasers are connected and default
enabled; exact native getter/query coverage is being investigated. Mono remote
heads remain the next mixed-presentation source step; user gameplay acceptance
and the full goal remain open.

## Passive ride-control observer implemented — physical grabs still open

The existing mounted-look route now has an opt-in borrowed-receiver scalar
observer for the two pinned hovercraft classes. It retains the existing rider
resolution as an equality token, forwards native clamps once, and restores TLS
through native-finally cleanup. It copies mode/execution/movement abilities and
nullable parameter/renderable tokens; it performs no model/resource lookup and
applies no steering. Default controls, native physics and ClientAction remain
unchanged. See [RIDE_CONTROL_OBSERVATION](RIDE_CONTROL_OBSERVATION.md).

All four products rebuilt with fingerprint
8f6062cbd41f1aebe8c5493c2ecb10babeb2f4e5a6ca1f15a997c2ea80206ff3,
IPC10/wire7.80Debug/80Release groups passed. Current game/server compiled
forwarding/cleanup checks and actual-instruction mutation controls pass in normal
and Python -O modes. Scalar parser controls reject false resource/frame/steering
claims. Astra/xhigh bounded native source, parser and corrected checker reviews accepted
the observer. Local current routing tags verified; backend unattested. No vehicle/game/input/headset/network runtime
occurred for this change. A fresh native-owned ride→renderable→instance join,
installed resource identity and evaluated control frame remain prerequisites for
physical cockpit grabs; the scalar observer alone does not satisfy them.

## Auto Shotgun collection source path implemented

Native ID2 now uses the existing isolated neutral grant/equip/save preparation,
with exact AutoShotgun/base-idle readiness and separate save/receipt identity.
Sniper-only zoom/attachment diagnostics stay ID13-only. Collector storage grows
while ID1/13 admission limits remain unchanged. Both exact stock ID2 mesh
variants pass selected-field/content extraction with rigid palette indices0/1/2.
ID2 pose replay explicitly remains unsupported before any single-body fallback;
production alignment is still ID1/13-only. See IDLE_AUTOSG_COLLECTION.md.

Astra/xhigh scoped native and consumer source GO; local tags verified, backend
unattested.79Debug/79Release and20compiled normal/-O gates pass; actual-object
argument/receiver/stack/save/target mutations reject. Matching products use
source fingerprintdfb44d3c52cb357dd081efa8bea2dab264fba396539b62b7c080aa3e66f198d8,
IPC10/wire7. No ID2 runtime or firing/vehicle/network/headset probe occurred.
Loaded/render association and actual multi-index pose arithmetic remain the next
ID2 gate; physical driver controls and full user/device/MP acceptance remain open.


## Native diagnostic capture succeeds — both-eye reference retained

The fresh private run completed collection with seven qualifying Black draws
spanning both eyes,140matched native/OpenXR world projection pairs, exact Jungle
scene identity, private native sniper save, and clean native/display shutdown.
12protected original files remain unchanged. No firing, movement, zoom, vehicle,
network, head-motion or desktop-input probe occurred.

Rejected observations were15name failures and10palette failures. Nine palette
records lack the required own-event/configuration association (checks118), so no
ownership-only receipt is emitted for them. One ownership-only failure (checks111)
records both comparisons false: the selected instance has linked=0 and the copied
cache owner is0. This proves rejection of that association, not what the native
fallback/cache object means. No predicate or attempt quota was relaxed. Full
collection readiness is true; visual grasp/contact and exact shooting-parent
reconstruction remain unverified. Sourcea6a611410c5d9a6baeadd583e1d121fc4dbd24b7,
products78b36974e14483481e6bafd53c747662b727f321bce9f67572eb5e80baa0a090.


## Palette ownership diagnosis — corrected capture accounting

Astra inspected the actual retained V39/V40/V41 records. V40 and V41 each have
14animation-name rejections and18palette rejections, rather than32name failures.
V39 also contains that same failing name header, alongside ten qualifying draws.
The existing native Idle lookup/ABI is supported by those successful observations;
no name-offset, alternate IDENT or attachment-hook cause is established.

All18palette rejections passed the name gate and failed the combined equality
`evaluated == linked && cacheOwner == selectedInstance`. Existing logs do not say
which equality failed. The32attempt quota is charged before qualification and
stays unchanged. A value-only first-failure receipt now records operands already
read at the existing palette boundary. It preserves every rejection/admission
predicate, adds no native getter/read/evaluation and certifies no usable palette.
Astra/xhigh native/parser source GO and79Debug/79Release checks plus16compiled
normal/-O gates pass. A fresh neutral receipt remains pending. The last native
sniper reach correction remains verified; full implementation remains open.


## Sniper reach correction confirmed in neutral native runtime

A fresh isolated run reports an accepted ID13 `reachPreserved=1` witness with
radius0.702938914m. Offline current-math reproduction matches the copied vector
and target exactly; the previous bounded target differs by approximately0.203m.
The run produced146matched native/OpenXR projection pairs and per-eye images,
created its private sniper save, and shut down native/display processes without
survivors or cleanup errors.12protected original files remain unchanged. No
firing/movement/zoom/desktop input or physical-headset probe occurred.

The broader model collector still rejected32early own-animation-name samples,
so this run provides no new qualified rendered-handle/visual-muzzle association.
Exact same-invocation parent reconstruction and visual muzzle coincidence remain
unverified. Keep that evidence separate from the verified existing-route reach
correction. Source034fd8cfc34bd3afa75cafdf3b18ba3309d26b60, products
9c71b84624456b670815dbb6e77758a369444d6788d48528bdfd5ba187d47196.
Full remaining implementation and user-operated gameplay/device acceptance stay open.


## Admitted sniper native reach preserved

The ID13 local and authoritative muzzle adapters now preserve the existing-route
vector after the ordinary calibration/ownership/final binding checks. Other IDs
and unadmitted bindings retain the0.5m bound. Native displacement, alignment,
orientation and getter selection are unchanged. No visual child point or captured
IDENT replaces the selected native origin. The private witness records whether
reach preservation applied.

Astra/xhigh policy and final source GO; local routing verified, backend unattested.
All4products rebuilt;79Debug/79Release and8compiled attachment/muzzle normal/-O
gates pass. Regression checks retain a long native vector, charge exactly once,
short-vector equality and unchanged orientation, and reject other/unadmitted IDs.
Runtime verification of this new reach policy remains pending. Exact same-call
parent reconstruction, visual muzzle coincidence and user-operated firing/device/
multiplayer acceptance remain unverified. Full implementation remains open.


## Existing-call sniper attachment captured in native runtime

The fresh isolated neutral run loaded the exact Jungle scene, completed native
sniper grant/equip/save, produced149matched native/OpenXR world projection pairs
and per-eye images, and shut down native/display processes with no survivors or
cleanup errors.12protected original files remain unchanged. No firing, movement,
zoom, desktop input or hardware-headset probe occurred.

One accepted right-ID13 receipt copied the actual opaque IDENT and twelve matrix
words from the existing native attachment call, plus that invocation's original
shooting pose and current admitted model binding. This establishes native-call
provenance. The earlier calibration reference is not an animation fence.

The broader model collector rejected all32early observations at its own-animation
name check (reason19), yielding zero qualified draws. Its exit1 is an evidence
rejection after a clean native run, not a startup/rendering crash. That fixture
is immutable; own-event checks remain strict. Visual muzzle coincidence remains
unverified and the0.5m bound remains unchanged pending the bounded native-origin
review. All4products use1645e894228f61ccbec652bc713a08caf670b0afc379548419ce6249e52626d8;
79Debug/79Release and8compiled attachment/muzzle gates normal/-O pass. Seven
altered-object controls reject missing forwarding/copy/cleanup. Strict parser
also rejects unsupported/truncated records mixed with an otherwise valid receipt.


## Passive native sniper attachment collector integrated

The isolated lab now observes the existing native attachment call and copies its
opaque IDENT and successful 48-byte matrix without adding a lookup or changing
aim. Protected receipt lifetime rejects duplicate/nested/aborted calls; publication
requires the existing final binding checks and exact model instance. Copied
calibration serial/request/input/eye and native reference support correlation,
not unchanged-animation proof. See NATIVE_ATTACHMENT_OBSERVATION.md.

Astra/xhigh scoped source GO accepted forwarding, TLS cleanup and publication;
local model tags verified, backend unattested. The neutral right-ID13 runtime
receipt remains to be collected. The existing 0.5 m bound is unchanged. Full
implementation and user-operated firing/device/multiplayer acceptance remain open.


## Desktop sniper calibration recovered in native runtime

The fresh private capture completed with141matched native/OpenXR stereo pairs,
ten qualified Black observations covering both eyes, complete native save/preload
preparation and clean native/display shutdown. Both laser lines are visible in
the private eye images after the narrow desktop-delegation fix.12protected files
remain unchanged. No firing, movement, zoom or physical-headset test occurred.

One accepted idle right-ID13 muzzle witness emitted with cache age44ms and
stretch1. Its actual pre-clamp radius is0.702913761m, limited to0.5m. Independent
offline C++ reproduction from the copied inputs matches vector and target exactly.
This proves actual truncation of that native return; it does not establish visual
muzzle coincidence or an all-animation maximum. The remaining muzzle policy is
under bounded Astra review. No new bound or unrestricted exception is active.

Published source checkpointb1539dfbba5e906ca9f0e164d2421597e6240560, all99
reachable author/committer emails empty, Actions disabled, workflows absent.
Native products are3d4d8ab6387eb0ec4cb387903c27a668b746edb838d9e1b3c53b11ce4fddebe3,
IPC10/wire7. Exact private package/capture receipts retain hashes. Full source
implementation and user-operated device/gameplay acceptance remain unfinished.

## Native stereo capture succeeds — desktop sniper delegation fix pending

The private aligned-sniper run completed collection and safe shutdown:147matched
native/OpenXR pairs, two1280x720 game-world eye images, ten qualified Black draws
spanning both eyes, native save/preload creation, no cleanup errors/survivors,
12protected files unchanged. Both native sniper models render. Static patch
correlations show near ground22px and17px disparity, scope80px, background tree0px;
these are selected-image candidates, not whole-scene depth/device/head-motion
acceptance. Trigger values were zero. No firing or movement probe occurred.

The passive muzzle witness was absent because a subsequent unmanaged desktop
sniper->base render invalidates valid XR calibration at depth2/reason3. Astra
matched publication/invalidation/query tuples at39/44ms; successful publication
already proves flat placement succeeded. A narrow stack-owned route now preserves
that positively identified native desktop delegation's original return without
VR adaptation or calibration mutation. Physical XR and unknown routes retain
strict borrow/ownership/failure checks. Astra final source GO; all4products
rebuilt on3d4d8ab6387eb0ec4cb387903c27a668b746edb838d9e1b3c53b11ce4fddebe3,
77Debug/77Release and4compiled muzzle gates normal/-O pass. A fresh sealed
fixture passed preflight/artifact verification; no new run yet.

## Live sniper alignment integrated — passive muzzle witness next

The latest private capture completed native grant/select/save and supplied twelve
qualified Black draws spanning both eyes and both hands (2,904vertices each,
maximum clip error2.24969e-7). Each record has its own native Idle event, current
resource/model/palette identity and own-source2 upload/reference association.
No cross-eye cache receipt was borrowed. Native shutdown/private display cleanup
were clean;12protected files unchanged. The new save/preload remain private.

The harness stopped before images:29ready sniper status samples spanned10.042s,
exhausting its ten-second pose-phase budget. Validated neutral idle collection
now allows20s for the same30observations, still clipped to the global deadline;
other modes keep10s. No image/device acceptance is claimed from this capture.

The existing model-root adapter now supports the verified ID13 handle convention.
Native ID participates in fresh binding/equality; placement, local and authority
aim use the same correction once, preserving native charge/animation/stretch.
Scope inherits that root. ID1 remains unchanged; unknown IDs do not receive this
reference. Astra source GO, local Astra/xhigh tags verified, backend unattested.
77Debug/77Release and4compiled muzzle-unwind gates normal/-O pass.

The existing0.5m attachment clamp remains pending one passive, accepted idle
original-return witness. A one-shot private row records its returned attachment,
current receipt, camera/root/hand, charge/correction, pre-clamp vector/radius and
unchanged target. It adds no native getter or firing. A fresh sealed package is
being prepared; rendered-muzzle coincidence and full mod completion remain open.

## Presentation-only preparation deferral — source correction

The fresh alternating-order run identified the exact stop: initial
presentation-idle failed at Selected, after every preceding phase predicate
passed. It reached the pinned Jungle scene and shut down cleanly;12protected files
stayed unchanged. Four early ID13 records were rejected and no new save/image
was requested. The earlier successful save and all attempted fixtures remain.

The preparation adapter now defers only that initial presentation-busy condition,
before player/resource borrowing or native action. Later owner/pre-action checks
and all other phase/health failures still latch failure. Native action stages,
issued-tick and existing deadline are retained; waiting does not reissue selection
or extend the deadline. Explicit Astra/xhigh confirmation now accepted; local tags verified and backend
unattested.77Debug/77Release checks and8compiled observer gates normal/-O pass.
All4products/package use fingerprint5d514b1ffcea51b4b380a9ab86ef4cac48cd09a91509959d9fbd1256bc07bbce.
A fresh sealed fixture passed check-only preflight and installed artifact verification.
The failing predicate includes
thread/device/lifecycle conditions as well as transaction occupancy; the log
does not establish which constituent failed. No new runtime claimed.

## Reviewed neutral eye-order experiment prepared

A sealed lab-only option alternates temporal first-eye order by request parity
while preserving physical-eye camera/depth/target/projection/postlude/readback
indices and both original passes. Production order remains unchanged. Scope
preview retains its existing eye0 convention. Each eye must supply its own native
Idle event and complete geometry/reference association; no cache receipt is
transferred, recalculation forced, tolerance widened or animation gate relaxed.
Skipped requests and the32attempt budget can leave evidence inconclusive.

Astra source GO, local three-turn Astra/xhigh tags verified; backend unattested.
28runtime groups normal/-O and77Debug/77Release checks pass. All4products rebuilt
onad11383 (full fingerprint in private package/artifact receipt), IPC10/wire7.
A fresh independently copied/sealed fixture passed check-only preflight; no native
run yet. The phase-failure diagnostic is included. Alignment remains inactive.

## Fresh neutral capture — guarded stop, second-eye evidence gap identified

The fresh run on sourceaee8f6c/native fingerprint9c8602b reached the pinned Jungle
scene, selected ID13, and submitted eight native/OpenXR projection pairs. The
preparation adapter stopped before save with simulation-phase failure. Native
shutdown was observed, no cleanup errors, and12protected files stayed unchanged.
This was a guarded preparation rejection, not a game startup/stereo failure.
The earlier successful sniper save remains preserved; attempted fixtures are
immutable. No firing/movement/zoom or desktop input/focus occurred.

Offline replay qualifies one completed first-eye/right-hand Black draw. Second-eye
records have actual qualified native API draws/current evaluated palette but no
animation query callback; the collector correctly rejects missing event evidence
(Palette reason6/checks118), without borrowing first-eye animation ownership.
Astra investigates the narrow native cache route for a defensible passive receipt.
A diagnostic-only first-failed-phase label preserves all existing preparation
predicate ordering/short-circuiting; all4products rebuilt on
1df54a57b15b2e5c1384a63d9819b6f0990901e53b29fd6ca8969e30545bf25a.
77Debug/77Release checks pass. No new package or runtime. Astra accepted the diagnostic hunk: original
predicates retain order, short-circuiting and single evaluation; labels add no
native queries. Local Astra/xhigh tags verified, backend unattested.
No guard was relaxed.
ID13 alignment and full feature completion remain open.

## Sniper transform reference unblocked — both-eye capture next

The own source2 conditional adapter now reproduces seven completed Black draws,
2,904 vertices each, at unchanged tolerance (maximum clip error2.23153e-7). It
selects arithmetic from the actual own consumer/source/mode and verifies native
API model/draw identity; missing/foreign/unknown evidence rejects. The reference
kind remains uploaded-transform-corroboration. Producer ownership/mode, whole API
coverage, world/GPU precision, physical grasp and alignment remain unverified.
All seven records are first-eye/right-hand; no both-eye acceptance is claimed.

The PID-domain correction is separately reviewed and tested: Windows IPC PID is
compared to prior Windows PID, while Linux incarnation ownership is independently
checked. Targeted Debug/Release checks and15reference groups normal/-O pass.
Astra/xhigh design recommendation accepted; local tags verified, backend unattested.
Final source GO follows a replay-level cold-source bypass regression. A fresh
sealed neutral fixture passed check-only preflight. Native products remain
9c8602b54d253f08531195dacfeb06e34d6eb477b0e5270ec9b58c87dcb171e0.
ID13 correction remains off; full implementation and user gameplay tests remain.

## Native sniper fixture created — harness identity check corrected

The fresh corrected run created a native sniper save (1,842,870bytes) and preload
companion (97,517bytes) after owned idle ID13 observation. Exact file hashes stay
in the private receipt. The isolated fixture is explicitly cheated; reload is
unverified. The run reached exact Jungle bytes and12native baseline stereo pairs,
then stopped on a harness PID-domain mistake: Windows PID was compared with Linux
/proc PID. Actual Windows PID remained unchanged. The source now compares Windows
identifiers and independently checks Linux ownership;27normal/-Ogroups pass.

Native/process/display shutdown was clean;12protected files unchanged. No firing,
movement, zoom or desktop input/focus occurred. No new per-eye image was requested
before the identity check stopped the harness. Attempted fixture remains immutable.

The native Poly Bump observer produced14actual source2 pairs in7completed Black
observations: PC24/nearest, Scope flags8->14 and own Black reuse14->14. Black's own
pair matches its own factor/constant data and native draw key. Prior Scope/Black
sample endpoints have equal operands/cache words, but this is not a write-continuity
certificate. Exact arithmetic/reference decision remains under bounded Astra
review; ID13 alignment is still off. Save creation succeeded independently of that
remaining position-qualification gate. Full implementation remains unfinished.

## Next private sniper capture — reviewed source, corrected save path

The native save argument now uses its verified game-relative filename, retaining
absolute destination/collision checks and a fresh cwd ownership check. The copied
Black-to-own-API association adapter is source-reviewed and retains exact MVP-word
rejection and false whole-API coverage for surplus geometry.

A diagnostic-only paired Poly Bump source2 observer is implemented, restricted to
ID13. It records raw producer mode/cache/operand evidence through the existing
integer sampler; source1 behavior and source1-only arithmetic qualification stay
unchanged. Unknown/crossed sources, wrong caller/table and capacity reject.
Astra source/compiled GO, local tags verified, backend unattested. All4products
rebuilt on9c8602b54d253f08531195dacfeb06e34d6eb477b0e5270ec9b58c87dcb171e0,
IPC10/wire7;77Debug/77Release and36normal/-O native/compiled gates pass.
Reference12/parser9/verifier8groups pass normal/-O. Fresh sealed capture is prepared;
corrected native save creation and actual source2 evidence have not yet run.
See SNIPER_PROJECTION_DIAGNOSTICS.md. Full feature completion remains open.

## Private sniper runtime — grant/equip observed, save and replay gates open

One fresh neutral simulated run on sourcea32523c/fingerprint916191bb reached the
pinned Jungle stream (10,562,049bytes, original position0, exact configured SHA)
and13complete native baseline stereo pairs. Native Give All and selection issued
once for the same local pawn; the adapter observed an owned right-hand unzoomed
idle ID13 before invoking save. Native save reported Could not open file for the
absolute Windows destination; no save/preload was created, so preparation stopped.
This is a save-path failure after scene/equip, not a startup or stereo failure.

Normal native/owned-process/display shutdown was observed, no survivors or cleanup
errors, and12protected files stayed unchanged. No firing, movement, zoom or desktop
input/focus occurred. Baseline image capture was not reached after the preparation
failure, so no new per-eye image or head-motion acceptance is claimed.

Offline postprocessing of that immutable run gives2complete/20rejected ID13
observations. Two first-eye right-hand Black copies uniquely match the patch
candidate's complete buffer lengths/ranges and five consumed-channel hashes.
Qualified position replay is still false:2native API attempts accompany1copied
geometry, and the existing consumer's whole-coverage/index guard rejects it.
Exact draw-to-own-API-row association needs review; no fence was relaxed and ID13
alignment remains inactive. A separate narrow save-path review will determine the
minimal native filename repair before any fresh capture. The attempted fixture
is preserved and must never be rerun/resealed. Full mod completion remains open.

## Native private sniper preparation — source GO, actual probe next

A one-shot private adapter now uses the verified stock cheat data argument,
native Give All, native ID13 selection and normal save. It preserves native
inventory/ammo/changeability, exact sole-local roster/brain/pawn ownership and
online isolation. Each action advances its stage before entry and runs only once
on a later outer main-thread simulation tick. Contained callback failure and
native abort permanently latch failure; exact caller/phase/revision/presentation
are rechecked immediately before every mutation. No release or zoom priming.

The sealed launcher admits preparation only with neutral ID13; fresh private
save/preload paths, ordered same-owner completion, nonempty bounded file hashes
and current right-hand sniper are required. The private fixture remains cheated;
reload, grasp and alignment are explicitly unverified. ID13 correction stays off.
See PRIVATE_SNIPER_PREPARATION.md and OFFLINE_IDLE_COLLECTION.md.

Astra/xhigh source GO; local tags verified, independent backend unattested.
All4products rebuilt on d458bd6474c1b12cd89996c2b3fa66b30a2c70859a87164a880a209ffb4a4168,
IPC10/wire7;77Debug/77Release and36normal/-O native/compiled gates pass.
Runtime evidence26groups and wrapper9groups pass normal/-O. No new native run
has been claimed. Next: one fresh private neutral grant/equip/save capture,
then actual Black draw association and alignment integration; broader full-mod
features and user-operated device/gameplay acceptance remain unfinished.

## Private sniper fixture preparation — current operation

The user has no existing sniper save and explicitly authorized verified save
editing or cheats to create one privately. Earlier user-originated-save-only
language below is historical. Astra/xhigh verified the native cheat data argument,
Give All, ID13 selection and normal save route from owned binaries. Implement the
smallest one-shot adapter at the existing simulation boundary; preserve native
inventory/changeability, local ownership and private save isolation. Actual
preparation/equip/save and ID13 rendered association have not run yet.

The offline collector reuses the current assessor/matcher/evaluator, stores exact
input receipts privately and never accepts grasp/alignment automatically. A narrow
lab selector now admits exact integer1/13 and serializes the same checked selector,
with sealed configuration tamper rejection. See OFFLINE_IDLE_COLLECTION.md.
The source build after test registration passes76Debug/76Release; all4product
contracts agree on3e01492f5ddd35b9339b71f17ed7a9641b4357d17d7017fa6dcf8fa126a24d46,
IPC10/wire7. This is build/offline evidence, not a new native runtime result.

## Sniper collection capacity — source-ready, live association open

The bounded capacity adapter is implemented: ID1 retains1490vertices/1332triangles;
selected ID13 admits up to2904/2245; unknown IDs reject. Fixed scratch storage and
offline128KiB arithmetic budgets cover both inspected Black variants. The selected
ID is copied before AddRef and reused at all three range-admission sites. Scope
production limits/getter order/currentness/program/ordinal/phase remain unchanged.

Both private real-asset candidate exports now succeed after removing ONLY verified
unreferenced trailing-1buffer-table slots from selected metadata. Asset/buffer
bytes and hashes are unchanged.2899/2904vertices pass synthetic identity arithmetic;
this is not native shader/world/GPU or live association proof. Historical missing-ID
replay defaults only that field to1; other binding fields stay strict. Completed
and retained explicit13/legacy1 paths, overflow/truncation and missing fields have
regressions. Main caught/reproduced/fixed the legacy KeyError; Astra source GO.

All4products rebuilt on ec5541eabcf9365d802fc4b2ea8314015b2f0274286fc9f7570f9d663646753e,
IPC10/wire7;75Debug/75Release and36normal/optimized native/compiled gates pass.
Targeted subsequent consumer25, replay10 and candidate16groups pass normal/-O.
All9current local review tags are Astra/xhigh; backend unattested. No runtime or
equip/switch/firing/movement/vehicle/HMD/network operation this landing.

Next exact evidence: a qualifying unzoomed ID13 Black draw, unique copied-range/
buffer-length/five-channel-hash association to either private candidate and
qualified position replay. Prepare/review a concrete user-originated collection
path; the neutral initial-Zap lab cannot provide that ID13 draw. ID13 correction
remains inactive until association. Sniper muzzle coincidence/current bound,
remaining weapons and physical driver controls are still open.

## Stock sniper reference and passive selector — capacity next

Ten selected owned stock sniper archive objects were decoded/roundtripped without
runtime. Astra/xhigh derived one fixed virtual handle reference for both retained
mesh variants; owner independently reproduced the section centroid/area and
selected vertices. No authored hand exists in those selected FP assemblies.
Unzoomed native delegation/placement and existing scope-root propagation were
verified. ID13 correction remains inactive pending actual gun-draw association.

Source adds exact separately labelled passive ID13 selection and unzoomed-state
rechecks; ID1/native gameplay/optics unchanged. New headers retain nativeID, reject
mixed-ID logs and preserve historical ID1. Reviewed selector/verifier/consumer
GO; stray-semicolon source-guard bypass caught/fixed. All4products rebuilt on
a0a70ce20f36ca996aa480d42cb334ed6e7f59a7c7d116e6207a54ee5722698d,
IPC10/wire7;75Debug/75Release and36effective normal/optimized gates pass.
No new runtime/equip/switch/firing/vehicle/hardware/network operation.

The gun Black surface exceeds existing1490vertex/1332triangle copies: known
variants need2899/2904vertices and2245triangles. This prevents a complete runnable
association path; no user action fixes that source limit. Next: coordinated
bounded producer/assessor/replay/candidate capacity, retaining ID1 and production
scope fences, then prepare/review an actual collection path. The authored muzzle
also exceeds current0.5unit offset bound at unit stretch; no coincidence/limit
acceptance inferred. See [sniper decision](SNIPER_MODEL_ALIGNMENT.md).

## Latest neutral diagnostic — visible lasers, intermittent cause unresolved

Source4ee5b35/fingerprint674ef564 reached pinned Jungle with129native stereo
projection pairs, normal native/owned/display shutdown, no owned survivors or
cleanup errors and12protected files unchanged. Both eye images were inspected:
full world stereo with BOTH aiming beams visible.294muzzle attempts accepted
matching calibration/current bindings;588three-line dispatch records returned,
294per eye. No binding difference was observed.

Freeze admitted294hand entries and rejected18for input-sequence mismatch. Cache
publication logging saturated; other stage counters remained available. This
proves the aligned path can present lasers, not the cause of the earlier absent
beam image or a permanent fix. Diagnostic logging may change timing. Do not
relax sequence/freshness/phase/binding fences. Whole both-eye reference collection
remains incomplete independently; native startup/scene succeeded. No firing,
movement, switch, zoom, vehicle, headset or multiplayer probe.

Astra/xhigh diagnostic source GO; local tags verified, backend unattested. All4
products rebuilt,73Debug/73Release and36normal/optimized gates pass. Next source
step is isolate intermittent request/sample scheduling with these exact records,
then resume remaining weapon references and physical driver controls.

## Connected ID1 runtime observation — laser regression open

Source3ada6e8/fingerprint424b6917 reached pinned Jungle in a fresh private neutral
capture:127native stereo pairs, normal native/process/display shutdown, no owned
survivors/cleanup errors and12protected files unchanged. Both eye images were
inspected: full world stereo, closer native guns/hands. Actual input records show
zero axes/triggers and closed wheels. No movement/firing/switch/zoom/vehicle/HMD/MP
probe. A preceding incorrectly invoked inner collector was rejected before native
launch; that attempted fixture is preserved and was not rerun.

10complete first-eye hand/gun factor joins give20uploaded-transform references,
all agreeing under unchanged tolerance. Fixed handle-reference/controller error
is at most8.99416e-6native units; retained animated handle residual.000804926.
These are copied-factor/native placement arithmetic, not independent world/GPU
precision or physical contact. Second-eye pose remains unqualified and whole
collector readiness incomplete; its wrapper rejection was not startup failure.

Before/after images expose an active regression: old capture shows both aim lines,
new aligned capture shows neither although Laser Enabled=1. Cause is not established.
Astra completed the bounded investigation: sampling rejection versus hidden line
dispatch remains unresolved. The proposed shooting-to-absolute-placement cache
poisoning route is unproved; verified shooting calls use the in-view matrix and
attachment getter. Adopted next step: capped passive records at existing cache,
muzzle, sampling, freeze and draw boundaries, preserving native getter order and
short-circuit evaluation. Fix the demonstrated boundary before extending alignment
or declaring laser acceptance. Source GO does not certify laser runtime behavior.

## Stock ID1 handle alignment — connected source

The virtual handle centre is connected at native model placement, local shot/laser
calibration and authority muzzle conversion. Exact native selector/signed floor,
nonuniform stretch and charge are retained. Binding/render-phase guards reject
stale or ambiguous model/configuration/stretch evidence. Headless renderer
dependency found and removed; existing simulation owner and pinned configuration
type serve both modes. Other weapons, shared gripOffset, remote children and
Scope13 are unchanged. Contact and headless model availability remain runtime
rows; this is not full weapon alignment or full-mod completion.

Astra/xhigh source GO; all7local tags verified, backendunattested. All4products
rebuilt on424b6917ea95b893f0ed3287ee7f72dd2c36d007ccd83abc17c331d886d22bac,
IPC10/wire7;73Debug/73Release and36normal/optimized native/compiled gates pass.
Fresh x86 normal/abort render-depth restoration checked, four in-memory controls
per object rejected. No runtime/input/firing/vehicle/HMD/network probe ran.
See [disposition](ID1_MODEL_ALIGNMENT.md). Next: remaining weapon references and
physical driver controls, with a fresh neutral alignment observation if needed.
Full user-operated acceptance remains open; older no-correction notes are history.

## Current alignment decision — anatomy review

Native world stereo remains working in the simulated lab; the latest neutral
run captured111projection pairs and distinct world-eye images. Earlier controlled
runs verified native camera response to head translation and rotation. Hardware
acceptance is separate. The current source task is weapon/hand grip alignment.

Astra/xhigh accepted the nine-copy hand content/local-transform evidence, but
rejected the old raw-bounds/Palm comparison and surface-marker-as-grip inference.
No numerical alignment is installed. The bounded next review uses the matched
gun and hand together to define a virtual handle-centre convention/reference
pose. See [corrected alignment gate](HAND_MODEL_ALIGNMENT.md). The older
palm-centroid next-step wording below is superseded by that explicit convention.

## V27 actual hand content and uploaded-transform reference

The ten-copy build f6370d8/fingerprint28126fc reached pinned Jungle with111native
projection pairs, clean native/owned/display shutdown and12protected files
unchanged. Both world-eye images were visually inspected. Native collection now
has9Complete/23Rejected (19Palette6/4Name19); all90copied channel sets uniquely
match81gunmesh3 plus9handmesh2. The hand copies cover7right/2left, eye0, with
native reflection/stretch preserved. Eye1 still lacks its own complete pose copy.

The hand's exact320-word program uses local palette c21–c23, then projection
c1–c4. Its independently copied Model/View/Projection factors reproduce every
uploaded projection word; copied Local matches the palette words. The narrow
new offline selector requires these exact matches plus same-copy API identity,
factor bookends, program/declaration and actual influence bytes. Uploads compare
only; they never seed the target. Cold-native precedence remains. Unqualified
applicable references cannot fall back to passing legacy results; tolerance,
collector, renderer, native hooks and IPC/wire stay unchanged.

Fresh offline replay of the actual capture produces18uploaded corroborations
(9gun/9hand). All9hand copies agree for317vertices each, maximum clip error
1.15836e−7. The archived primary legacy replay remains unchanged and failed.
Nine measured annotation candidates now retain separate position-reference and
unverified collapsed-world provenance; they are not accepted physical grasps.
Astra/xhigh source/correction GO followed overflow-label and downstream
provenance fixes. All local turn tags verified; backendunattested.

72Debug/72Release checks and9reference/9replay groups normal/optimized pass.
Compiled artifacts retain fingerprint28126fc, IPC10/wire7, and pass the current
contract check; no native source changed since the four-product rebuild. Native
cache producer, PC/RC, GPU precision and physical/world calibration are not claimed
by uploaded-transform corroboration. Next: establish a defensible model-relative
palm-centroid reference and static asset alignment while preserving native
animation/recoil/stretch, muzzle/laser/scope and remote attachments. Physical
driver controls and user-operated hardware/shooting/melee/MP acceptance remain
open. V27 immutable; no firing/movement/switch/device/vehicle/network probe ran.
See [reference adapter and disposition](IDLE_UPLOADED_TRANSFORM_REFERENCE.md).

## Ten-copy content collector — implemented, native proof next

V26's actual ten-call sequence justified extending the existing bounded geometry
collector from8to10owned copies. Emission uses schema4; offline assessment maps
schema3→8/schema4→10, requires ordinallayout1for4 and rejects mixedschemas.
All geometry/count/capacity branches and direct matcher/replay/runtime manifest
consumers handle the version. V25/V26 serialized schema3 reassessments are exactly
unchanged. No geometry grammar, native forwarding, COM cleanup, projection cap8,
position reference/tolerance, separate collector or hand-size admission was added.
The ninth call already collected before rejecting storage: this adds two retained
slots and only one further collection for the observed tenth call. An eleventh
otherwise-qualified call still collects/forwards then rejects storage normally.

All four products rebuilt on
`28126fc622a87ab5cc47562cb8b714f43869b12a52e3124301a02a9166828dc5`,
IPC10/wire7.72Debug/72Release and36normal/optimized native/compiled gates pass;
22reader/9replay/8projection groups pass normally/optimized. Tests include ten
owned copies/repeated native IDs, eleventh no-overwrite, twenty synthetic two-eye
replay draws, slots8/9 associations, exact mixed-schema and capacity rejection.
Astra/xhigh source GO and final scoped confirmation cover both narrow regression
corrections and projection slots8/9. Local routing verified, backendunattested. Fixed trace storage
105928bytes; both compiled x86 frames106672bytes (+15008), with probing. This is
not runtime stack-safety acceptance. Production invocation reserves the frame even
when diagnostic collection is off; no ownership relocation is justified by data.

No runtime of the ten-copy source yet. Prepare a fresh13-tool-sealed neutral
fixture to obtain actual tenth-surface bytes/hashes/declaration/program or the
precise newly exposed rejection. V26 immutable. Layout317/338 alone is not a hand
resource/content/pose/grasp certificate; the tenth surface currently has no
same-invocation independent projection receipt. Physical alignment, driver controls
and full user-operated hardware/shooting/melee/multiplayer acceptance remain open.
See [bounded extension and disposition](IDLE_TEN_COPY_COLLECTOR.md).

## V26 native indexed observation — next finite content edge

Fresh source848021b/fingerprint732daee reached pinned Jungle with108native
stereo projection pairs. Native shutdown, owned cleanup/survivors and private
screen retirement passed;12protected files unchanged. Main visually inspected
both eye images: world, weapons/lasers and stereo displacement. This stationary
simulation does not certify dynamic head response or real hardware/network.

All32invocations produced10qualified indexed API bookends each:320rows,
nooverflow, normalHRESULT0 and matching pre/post identities surviving cleanup.
Ordinal10/drawRecord5 in every invocation has317vertices/338triangles and all
four channel layouts equal the private authored hand candidate. Both eyes and
both hands are represented. This is an exact layout hint, not actual copied
content, resource join, GPU visibility, winning pose or physical grasp proof.
ModelRecord1/root config remain shared with the weapon pass; do not infer a
separate hand instance from authored asset numbering.

Whole readiness remains0accepted/32rejected:19Palette6,9capacityDraw10,
4Name19. Nine first-eye invocations retain8copies each; all72copies uniquely
match gunmesh3.18qualified cold native-order references agree;54legacy references
agree36/fail18. Do not borrow cold associations for later material/cache draws.
The wrapper's incomplete-reference outcome is not a scene/startup failure.
V26 is archived immutable; no rerun/reseal or firing/movement/device/vehicle/
network probe. The ten-call boundary now supports reviewing a minimal bounded
collector budget change to obtain actual last-surface content. Other pose/name/
reference/grasp gates remain open; no alignment correction has been installed.

## Current V25 result and indexed-submission observer

Native VR world rendering is working in the simulated lab. V25 reached the
pinned Jungle scene with 122 native stereo projection pairs, normal native
shutdown, no owned survivors/cleanup errors and 12 protected files unchanged.
Earlier controlled runs verified camera response to head translation and rotation;
V25 was stationary and does not repeat those checks or certify real hardware.

The exact NoUV78 adapter is now runtime-observed: 21 copies match all five
private channel hashes/ranges (14 surfaces of 132 vertices/108 triangles and
7 of 32 vertices/16 triangles). All 56 retained copies match gun mesh3, not the
separate hand mesh2. Seven traces reach the unchanged eight-copy capacity.
Whole collector readiness remains rejected; physical grip alignment is open.

A new passive indexed-submission observer records up to 64 attempted API calls
independently of the geometry collector's rejection/cap. It samples compact
resource/render identities around the existing unsplit original call, retains
only matching normal-return observations after cleanup, and invalidates reentry,
retirement, failed return, split, unwind and unknown samples. It changes neither
native forwarding nor geometry admission. It proves API bookends only, not GPU
visibility, vertex content, hand identity, grasp or first-use melee release.

Astra/xhigh gave scoped source GO after signed-count/byte-width and contradictory
status fixes. Local turn tags verified; independent backend attestation unavailable.
All four products rebuilt on
`732daee3193668262f31a2c3b8cbfdb5455b0d205b34a8cf5c1e94fa192950dc`,
IPC10/wire7. 72 Debug/72 Release groups and 36 normal/optimized native/compiled
gates pass. Metadata storage is 8,972 bytes; compiled x86 tracked-render frame
reservation is 91,664 bytes in both products with stack probing. This is compiled
measurement, not runtime stack-safety acceptance. No runtime of the observer yet.

Next: fresh sealed private neutral observation with 13 tool seals, to identify
post-cap resource/render coverage and any independently associated hand candidate.
Earlier attempted fixtures remain immutable. No guessed grip, cache borrowing,
release priming or firing/movement/vehicle/headset/network probe. Physical driver
controls and full user-operated device/dual-wield/melee/multiplayer acceptance
remain open. See [observer design and review](IDLE_SUBMISSION_OBSERVER.md).

## V24 result and exact NoUV78 copy adapter — source GO

V24 (sourceca07dd7/fp5dbeaa9, private run20261009T042942) reached the pinned
Jungle scene with109native stereo pairs and clean native/owned/display shutdown;
protected12unchanged.0accepted/32rejected:17Palette6,10CollectInputs32,5Name19.
All50retained copies match weaponmesh3;20cold-native-reference copies agree.
Tencomplete passive receipts flags511/0invalidations contain the same194-word
program. Astra verified oPos depends only onv0/v7.x-y/v8.x, relativec23-c25,
shaderDEFc255 andc1-c4. Otherinputs1-6 stayunknown, including declaredv2.
Conditional60compiled evaluations use actualprogram/constants butsynthetic
vertices/influences and a nonnative comparator; they certify no nativecontent.

The exactfive-row declaration now selects ID1 enum4NoUV78 withactual0/7/2/8
getters. Inactive3stillpoints at the previous14-vertex surfaceUV68848; the pinned
132-vertex surface requires68960. Stream2content is not yet observed; its copied
auxiliary bytes must independently match allfivehashes/ranges. No offset is
hardcoded, v2/v3remainunknown toposition, and otherfamilies keepstream3.
Scope13 remains unchanged. Cap8, owner/COM/nativefinally/bookends and source1
cold-reference qualification/tolerance stay unchanged; historicalV24layout0
assessment is identical and never promoted.

Astra scopedsource GO followed the revisedauxiliary boundary and an actual
producer-routing source gate with negative mutations. LocalturntagsAstra/xhigh
verified; backendunattested. Allfourproducts rebuilt on
`9eb1a42db87a25cc2e951b1ee9bcc6a312a318c883ea9fee45902933fa42a00c`,
IPC10/wire7.72Debug/72Release and32normal/optimized native/compiled gates pass;
20passive/21reader/9replay groups passnormally/optimized. No runtime ofthiscopy
adapter yet. Nextfreshsealedneutral capture must establish actualstream2
bytes/hashes, exactstop andhandasset/ownedreference coverage. V24immutable,
no guessedgrasp, cachedassociation borrowing, releasepriming orfiring/vehicle/
headset/network probes. Fullscope remainsopen.
See [implementation/disposition](IDLE_NO_UV78_COPY.md).


## Exact five-row passive extension — reviewed and built

The V23 stop now selects diagnostic0/7/8 getters only for its exact5row
0/2/7/8/END original rejection. Geometry enum/admission, position interpreter,
reference/tolerance, scope13 and native ownership/forwarding/cleanup are unchanged.
Actual rejected shader dependencies remain unknown. Astra scoped source GO followed
a required new-family passive+five-retained-copies integration test. Local review
turns verify Astra/xhigh; backend attestation unavailable.

All four products rebuilt on
`5dbeaa911a05a6949af957dabb4c470e5b0919cecda8bb4fce712aa076224c09`,
IPC10/wire7.72Debug/72Release and32normal/optimized native/compiled gates pass.
19passive,21general reader and9replay groups pass normally/optimized. The first
Release invocation lacked dependency PYTHONPATH; original failure logs are retained
and the corrected full invocation passes. No runtime or new fixture of this build.
Next: fresh sealed neutral observation of actual shader/7/8metadata, not guessed
position roles, geometry admission, hand alignment or first-use release.
See [scoped implementation/disposition](IDLE_FIVE_ROW_PASSIVE.md).


## V23 runtime result — world stereo works, hand reference still open

Source112edb18 / compiled fingerprint
`b2e6cfe603b1c6de5e62ad9df5e3a7e852f1491e21de6820230fe0d9fd46a016`
loaded the exact pinned Jungle scene in the authorized private neutral capture.
122 native projection pairs, normal native shutdown, no owned survivors or
cleanup errors, private display stopped,12protected files unchanged. Main visually
inspected both full eye captures: native world plus both weapon models/lasers,
visible stereo displacement. This stationary simulated run does not re-prove
dynamic head response or actual headset/controls/network acceptance.

The whole collector remains0accepted/32rejected:19Palette6,7CollectInputs32,
6Name19.35retained diagnostic copies uniquely match weapon mesh3; seven new
MultiUV78 material copies now include actual five-channel hashes.14copies have
qualified cold native references and all agree. Seven221-word copies associate
with cache-reuse sequence2, not the cold producer; their legacy reference passes
five and fails two. Do not borrow sequence1 association or widen tolerance.

The next immutable original failure is an exact5row declaration
0FLOAT3/2FLOAT2/7UBYTE4N/8UBYTE4N/END, step20 valid15, after5stored copies.
Its actual shader position dependencies and7/8bindings remain unknown; no passive
receipt selected this family. Next narrow source slice observes it through the
existing passive0/7/8 path, preserving production rejection and original/COM/
nativefinally ownership. It does not admit geometry or infer hand/grasp/release.
V23 is archived immutable, never rerun/resealed. Physical alignment, physical
driver controls and full user-operated acceptance remain open.


## Current exact MultiUV78 copy adapter — source GO

The V22 rejected material has an exact eight-row declaration and221-word position
program. Astra verified its position depends on v0, v7.x/y and v8.x; extra v1-v5
remain unknown. The collector previously fell back to stream5 (FLOAT2) for local
indices, causing its stride/range failure. ID1 now selects exact enum3
ObservedMultiUV78, actual7/8 indices/weights and separately qualified auxiliary3.
Scope13 never selects it. All native owner, COM, range, hash, rebind, original and
raster bookend checks remain. No extra UV is invented or used to certify position.

An independent diagnostic capacity branch exposes only the eight previously
stored copies after the original reason10 capacity rejection. It requires
count8, checks32639 (onlybit7failed), precedingPalette, callback63, copied-state
qualification and no failure/passive companion. The capacity stays8; no ninth
store, skipped draw, retry or promotion occurs. Rejected history remains outside
readiness, cleanup/outer/grasp uncertified. Cap-before-hand remains a prediction.

Astra source GO followed an enum3 partial-failure reader fix: getter failures
atsteps8-11 acceptactual0/3/7/8 and reject5/6; historicallayout0 remains intact.
All local review turns reportAstra/xhigh; backend attestation unavailable.
21 general reader,16 passive and9 replay groups passnormally/optimized; conditional
compiled checks of the actual captured program cover16constant snapshots with
synthetic vertices/influences only, never native byte/hash/GPU acceptance.
All four products rebuilt on
`b2e6cfe603b1c6de5e62ad9df5e3a7e852f1491e21de6820230fe0d9fd46a016`, IPC10/wire7.
72Debug/72Release and32normal/optimized native/compiled gates pass. The artifact
checker scratch-file collision was corrected; concurrent normal/optimized reports
now agree. Original failed evidence is preserved.

No runtime of this new build. Next: one fresh sealed stationary private capture
under existing authority to obtain actual later-surface bytes/hashes and observe
the precise stop/hand coverage. V22 remains immutable. Twelve tool seals, no
firing/movement/vehicle/HMD/network probes, no guessed grip or release priming.
See [adapter/retention disposition](IDLE_MULTI_UV_COPY.md).

## V22 result and native-order reference correction

V22 (source7945639/fped4fc52, private run20261009T023823) reached the exact Jungle
scene with120 native stereo projection pairs and clean native/owned/display
shutdown. Twelve protected files remain unchanged. Whole trace remains0accepted/
32rejected. The actual observer recorded16 pairs, eight cold0→6 and eight reuse
14→14, all control127 (PC24/nearest), no invalidation. All eight cold first-material
Q/R outputs independently match every raw word. Sixteen geometry copies associate
with coldsequence1; all3488 vertices agree with the independently computed native
order and separately rounded local stage under the unchanged tolerance. The old
collapsed reference fails3924 components. Incoming cache producers remain unknown.

The reviewed offline correction is implemented in idle_native_reference.py and
schema3 of idle_position_replay. It supports only source1/control127/cold0→6 with
existing qualified association and exact independent Q/R output corroboration.
Qualified native results become primary regardless of success; legacy results
remain explicit. Arithmetic unknown affects only its copy and suppresses geometry;
structural corruption still rejects. Retained history stays outside readiness.
Sixteen gun-mesh copies now yield diagnostic geometry; sixteen other copies lack
association. The different authored hand asset is still unmatched. No measured
hand/grasp offset, winning second-eye event, GPU precision or actual release is
certified. Production collector/raster reference/admission/tolerance are unchanged.

Astra source GO followed partial-result, observed-flags, deterministic selection,
local cancellation and integration-test fixes. Both local review turns were
verified Astra/xhigh; backend attestation unavailable. All four products rebuilt on
`46e88dffe9e0e325faae37111f0be567f0d2dd1a0a4e66beed8a230bb07f3375`, IPC10/wire7.
72 Debug/72 Release groups and32 normal/optimized native/compiled gates pass.
Five helper and eight replay groups also pass normally/optimized. Future fresh
fixtures need12 tool seals; V22 and all earlier attempts remain immutable.

Next finite edge: interpret the exact later8row material's captured position
inputs and the collector stop before the hand resource. Preserve extra4/5 unknown
until proved; do not relax declaration, owner, copy-cap or resource gates merely
because the earlier gun geometry now replays. The hand asset/mesh and positive
reference must be associated independently before any alignment correction.
Physical driver controls, wider presentation coverage and user-operated hardware/
melee/dual-wield/MP acceptance remain open. No firing/vehicle/HMD/network probe ran.
See [reference implementation/disposition](IDLE_NATIVE_REFERENCE.md).

## Observer source checkpoint before the V22 capture

Two reviewed whole-function hooks now observe the first native material's
call-free matrix-production interval. The slots helper is sampled after its
original and cleanup return; the fog callback is sampled before any owner lookup
or original execution. The raw sampler uses only integer loads and one FNSTCW;
compiled inspection finds no calls, SIMD, floating arithmetic or state writes.
Existing invocation metadata owns one pending pair and at most eight completed
value receipts. Entry guards reject reentry and unmatched boundaries; native
finally cleanup retires only diagnostics. Originals and the native fog output
pointer remain unchanged.

The exact Shader module is fingerprinted and pinned through the existing hook
transaction. Missing loaded Shader module is reported without late installation.
No native cache recalculation, precision setting, reference replacement or
admission/tolerance change occurs. Cache hits require unchanged cached words;
post-flags must equal pre-flags OR 6. Earlier cache-producer provenance stays
unknown. Geometry references the canonical receipt sequence and requires both
existing raster bookends, matching independent M/V/P inputs and actual c1-c4
words. Neither association nor cold/reuse flags certify grasp, GPU execution or
an earlier producer. Retained rejected copies stay outside readiness.

Astra source GO followed fixes for original-entry reentry, unmatched fog, cache-hit
consistency and removal of redundant serial state. Seven reader groups pass
normally and optimized. The new reader is included in the eleven-tool seal.
The compiled verifier checks pinned native boundaries and instruction-level
sampler/tail/prefix paths; its original-call and unwind checks explicitly certify
static call-site presence, with forwarding behavior supported by source review.
Parser/control-flow negative tests cover wrapped SIMD instructions, gaps,
unreachable control reads, LOOP transfers and callbacks hidden by final jumps.
Astra gave final gate source GO; no runtime of this implementation yet.

All four products rebuilt on
`ed4fc52a20cf22eaad7af8953ee7fbbc923fe87067b829fa8e1b8b3ca4a28d48`, IPC10/wire7.
All 71 Debug and 71 Release groups and 32 normal/optimized native/compiled gates
pass. Final corrected verifier checks pass for both x86 objects normally and
optimized; its six negative-control groups and both CTest configurations were
rechecked. Source/product changes were not needed for the verifier correction. Next: a fresh sealed private neutral capture of actual precision and cache
production. V21 and every prior attempted fixture remain immutable. No shooting,
vehicle, headset or network gameplay probe is part of this observation.

## Latest V21 native evidence and passive source GO

V21 ran committed source `7cd43a56c371aaecc5dbeeebc829f4001c0dafbe`, fingerprint
`fa7caae5fd24deb6819519a7cf8519a57576fc404b8ae228ab513d5796654b1f`.
The private run reached the exact Jungle scene with 123 native stereo projection
pairs. Native shutdown, empty native/wrapper cleanup, display retirement and
preservation of all twelve protected files passed. The fixture is archived and
must not be rerun or resealed.

The ordinal adapter recovered 36 individually qualified copies from nine rejected
traces: ordinals 0/1 share native record 0, and 2/3 share record 1. All five channels
uniquely match weapon mesh3, covering its 218- and 972-vertex surfaces. Nine ordinal-1
copies use the actual NoUV56 declaration. All copies carry independently qualified
M/L/V/P factors. Both unchanged original and fixed double P*V*M*L references fail
unchanged limits: maximum absolute errors 2.54009961e-5 and 2.13800334e-5. These are
rejected historical copies, not accepted grasp, GPU, hand or winning-cache evidence.
Do not select a passing grouping, derive a reference from the constants under test,
or widen tolerance. Astra is tracing the narrow native matrix-upload route.

Whole collection remains 0 accepted / 32 rejected: 18 Palette6, 9 CollectInputs32,
5 AnimationName19. The later rejected material has an exact eight-row declaration:
FLOAT3 stream0; FLOAT2 streams2/3/4/5; UBYTE4N streams7/8; END. The new source slice
selects only the existing passive 0/7/8 observations for this exact original
failure. Extra streams4/5 remain unknown; production declaration grammar, position
replay, admission, forwarding and native owners are unchanged. No new native API
or buffer-copy operation is added. Astra gave source GO; no runtime of this slice.

All four products rebuilt on fingerprint
`a64ddfb9c254c1693d1b5ed8d4472553609cc03a864a994b5cab4908459c4743`, IPC10/wire7.
All 68 Debug groups pass. Release passed 67 groups initially; the remaining
predicate check passed on recheck with the existing Python dependency environment.
All 28 normal/optimized native/compiled gates and 16 passive reader groups in both
Python modes pass. Missing Capstone in the first Release invocation was an
invocation-environment error; no dependency installation was needed.

The original source owner retains integration/runtime/publication. Independent
Astra investigations cover native matrix-upload arithmetic and the previously
unopened FlyingSaucer console geometry only. No firing, driving, HMD or network
probe ran. Full implementation and user-operated acceptance remain open.

## Current native result and qualified-copy adapter

V20 ran source `6c0832601bd47fd6574d97ca060ad27f1ae86c33`, fingerprint
`4d1c8050b71f2cc64484fb9fdf2f968ed505e3e8b366b5e60cda543d16aaf45a`.
It reached the exact Jungle scene with122 native projection pairs, normal native
shutdown, empty native/wrapper owned cleanup, retired display and12 protected
files unchanged. Seven stored first copies were recovered with all52 independent
transform words; all are right-hand/eye0 and uniquely match all five channels of
weapon mesh3, LOD0,218vertices. The unchanged202word native program and both the
old and fixed independent doubleP*V*M*L references agree for every vertex under
the unchanged limits. Maximum errors are1.40695455e-5 and9.21622661e-6 respectively.

This does not explain V18's missing-factor failure or justify replacing the
reference/tolerance. Whole collection remains rejected:0accepted/32rejected
(15Palette6,10Name19,7Duplicate11). Actual second-copy contents, the different
hand asset/mesh2, winning second-eye event/cache, grasp and release remain open.
The fixture is archived and cannot be rerun/resealed.

Astra approved using the existing bounded geometry index as a qualified-copy
ordinal. Native drawRecord is a renderer-array index, not invocation identity.
Source removes only that uniqueness assumption; every invocation still passes
its independent collection, original/current, finite-value and lifetime checks
before append. Max8 is preserved, the ninth rejects, and game rendering/physics
remain unchanged. Header `copyLayout=1` explicitly distinguishes the new producer.
The reader rejects unknown/mixed layouts and new-layout reason11, preserves legacy
uniqueness/archive11 and keeps reason32 histories outside readiness. Measurement
now carries geometry_index. Unreachable producer11 retention is retired without
renumbering rejection values or adding another diagnostic owner/schema.

The actual offline evaluator roundtrip distinguishes two assets, programs,
constants, affines and all52 factor words with repeated native record0, both eyes
sharing a request, per-ordinal matching and reference measurement. A bad second
shader or candidate cannot inherit first-copy agreement; failed outer finish keeps
copies without promoting them. Factors present/absent preserve replay results.
Astra source GO; local Astra/xhigh metadata verified, backend unattested.

All four products rebuilt on
`fa7caae5fd24deb6819519a7cf8519a57576fc404b8ae228ab513d5796654b1f`, IPC10/wire7.
All68Debug/68Release and28normal/optimized native/compiled gates pass;
20reader/14passive/7replay groups pass normally/optimized. No runtime of this build.
Next is a fresh sealed neutral capture of the actual subsequent copy, preserving
every influence/program/reference/currentness/ownership guard. Full-mod and
user-operated hardware/dual-wield/vehicle/multiplayer acceptance remain open.

## Native V19 outcome and reason-11 history correction

The fresh neutral capture of source6f93947884ccfc1104d989887e5525eec5b754d1,
fingerprintf7363002cf44be1896440fe9bd7cfeeb67fee2d0076cb667ab85debd6c745b93,
reached the exact Jungle scene with135native projection pairs. Native isolation
shutdown, empty native/wrapper owned cleanup, display retirement and12protected-
file preservation passed. Whole collector failed:0accepted/32rejected records.
Eighteen reject at Palette6, nine at DrawDuplicate11 and five at AnimationName19.

All nine duplicates retain one previously stored draw, eye0 (8right/1left),
precedingPalette/state63/callback63. The duplicate guard runs after successful
original/current qualification but before the second copy's finite-constant check.
That second copy's content is unobserved. The stored first copy was suppressed by
the reason-32-only history predicate, so no geometry/factors were emitted. This is
a collector evidence failure, separate from native stereo world rendering.

The reviewed source correction permits retained history for reason11 with1..7
stored copies, exact copied state/identity and callback63. Capacity rejects eight
before duplicate detection, so an eight-copy reason11 receipt is impossible.
Reason32's existing selector and eight-copy allowance remain unchanged. Only
previously stored copies are exposed; no rejected second copy, invented input
failure, passive payload or outer-current query is introduced. Duplicate rejection,
uniqueness, source admission, original forwarding, finally and tolerance remain.

Astra gave design/source GO. Distinguishable stored bytes survive changed second
programs/hashes/factors and NaN constants. Tests cover1/7/8 capacity, failed
original/current, later cleanup/finish, missing provenance and fabricated inputs.
Positive matching and actual offline replay remain diagnostic. Complete eight-copy
payloads test reason11 rejection and preserve reason32 compatibility. Local
Astra/xhigh metadata verified; backend attestation unavailable.

Four products rebuilt on
4d1c8050b71f2cc64484fb9fdf2f968ed505e3e8b366b5e60cda543d16aaf45a,
IPC10/wire7; all68Debug/68Release and28normal/optimized native/compiled gates pass,
19reader/14passive/7replay groups pass normally/optimized. No runtime of this build.
Next fresh sealed neutral capture should recover the first copy's independent
operands for the fixed arithmetic comparison. Old fixtures remain immutable.
Repeated-DIP admission, hand grasp, winning second-eye cache and native release
are still unproved; full-mod/user-operated hardware/dual-wield/vehicle/MP scope stays open.

## Historical primitive-factor diagnostics — source GO checkpoint

The selected raster's model M/local palette L and already-owned camera V/projection
P are now copied as optional value-only diagnostics. No new native lookup, COM
call, buffer lock, retained pointer or graphics-state write was added. Existing
affine/clip computation, raster equality, original forwarding, finally cleanup,
success/refusal policy and tolerance are unchanged.

The optional companion requires exact raw-bit/index/identity agreement at the
existing pre-original and post-original raster bookends. Missing or changed
operands suppress only that companion. Shared serialization preserves all52words
and labels. Ordinary/retained payloads explicitly certify post-original history,
not cleanup/outer-current/GPU execution. Strict optional reader validation cannot
promote accepted geometry, readiness or grasp.

Astra initial/follow-up source GO adopted exact serialization, failed-pre/restored-
post isolation, foreign/malformed/boundary inventories and positive matching/replay
non-promotion regressions. The final nonblocking refinement also gives different
factor sentinels to multiple draws and both eyes of the same request. Local
Astra/xhigh metadata verified; backend attestation remains unavailable.

All four products rebuilt on fingerprint
f7363002cf44be1896440fe9bd7cfeeb67fee2d0076cb667ab85debd6c745b93,
IPC10/wire7 unchanged. All68Debug/68Release and28normal/optimized native/compiled
gates pass. Eighteen reader groups and7replay groups pass normally/optimized. A
private arithmetic diagnostic has synthetic cancelling-translation and malformed-
output checks; it is not native geometry evidence. The V19 outcome above supersedes
this historical pre-runtime status.

Next is a fresh sealed neutral operand capture, then independent doubleP*V*M*L
comparison alongside the original projection result. Reference replacement and
tolerance widening remain outside the reviewed change. Full implementation and
user-operated hardware/dual-wield/vehicle/network acceptance remain open.

## Current ID1 noUV adapter — scoped source GO

The latest private neutral capture of source791428a27cf90a0787b4fe6c5a8fcd3df6c36bed,
fingerprint23712f1d7a2c896cf245a3d9e073123099ed1db4f9860ec149b1f133c11f346c,
reached the expected Jungle scene with118native projection pairs. Native shutdown,
owned cleanup/display retirement and12protected-file preservation passed. Ten
passive0/5/6 receipts have flags511/no invalidations and the same261word program
SHA2563e8a3fc3156e23f3d4f596e12b3586ddbd5baff8eb553b0be1b2b245686ea7fa.
Whole-trace acceptance remains zero; successful runtime rendering is separate
from failed weapon evidence collection.

Astra verified the exact position dependencies: v0.xyzw, v5.xy, v6.x, localDEFc255,
relative palettec22-c24 and uploaded projectionc1-c4. No v1-v4 dependency or
unsupported operation reaches position. API receipts alone do not certify
influence bytes; freshly copied exact indices/weights remain mandatory.

The narrow ID1 adapter now accepts only the exact five-row NoUV56 declaration.
It reuses the existing canonical-buffer, bounds, five-copy/hash, before/after,
original-call-once and finally gates. Bound stream3 remains an auxiliary asset
fingerprint, explicitly labelled as such in reader/matcher output. NoUV replay
leaves v1-v4 unknown and preserves original bytecode, influence checks and
comparison tolerance. Scope13 continues to require its existing consumed-UV
interface. Astra gave scoped source GO; no new-build runtime acceptance is claimed.
All four products rebuilt on fingerprint
7e9f72eba4463d1c58b75b0c578ce69e3197fa9552c3ca5f2875ebc4d62b5476,
IPC10/wire7 unchanged. All68Debug/68Release and28normal/optimized native/compiled
gates pass. Sixteen general reader,14passive and7replay groups pass normally and
optimized. The nonblocking review gap now has a regression for layout2 failure
indices0/3/5/6, rejection of7/8/altered declarations and historical layout0 passive
compatibility. Reviewer local Astra/xhigh metadata verified; backend unattested.

Ten retained earlier copies still uniquely match weapon mesh3, but all now fail
the independent projection reference. Offline diagnosis isolates translation
coefficient differences (maximum vertex error5.74404e-5); other coefficients
differ by at most1.03866e-7. The shader itself agrees with its factorized uploaded
matrices within1.47034e-7. This does not prove rounding or reference equivalence.
Native model/bone/view/projection factor evidence is the next bounded design
decision, now design-reviewed for diagnostic-only52float per-draw M/L/V/P copies
inside existing validated borrows. This next slice is not yet implemented. Keep
original affine/clip computation and acceptance unchanged; compare independent
doubleP*V*M*L separately, not a reference constructed from shader constants. The
annotation for another asset/mesh2
cannot establish this weapon's grasp. Hardware/dual-wield/vehicle/network tests
remain user-operated, and full implementation remains open.

## Historical exact noUV passive diagnostic — source GO

Astra approved the minimal extension for the observed five-row0/1/5/6 declaration.
One pure helper selects0/7/8 or0/5/6 from the immutable original first-failure
rows; selection, rejected-raster qualification, sampling and emission reuse it.
Resampled declarations cannot relabel the stream slots. The new5/6 branch also
compares the original captured local/weight scalar bindings before claiming
before-current agreement. No production geometry grammar, replay seed, owner
slot, buffer lock or graphics-state write was added.

Reader keysets and failure indices use the same original-family policy. Partial
output/flag/HRESULT/callback63 rules remain; nonzero draws still require a fully
validated retained companion. Changed after-declarations remain diagnostic and
must clear equality. Fifteen general reader and14passive reader groups pass
normally/optimized, including both exact families, malformed/aliased/cross-family
inputs, original-binding contradictions, changed declarations and coexistence.
All68Debug/68Release and28normal/optimized native/compiled gates pass. All four
products rebuilt on23712f1d7a2c896cf245a3d9e073123099ed1db4f9860ec149b1f133c11f346c,
IPC10/wire7 unchanged. Latest local Astra/xhigh metadata verified; backend
attestation unavailable. The native capture above now supersedes this checkpoint's
pre-runtime status.

Next is a fresh sealed neutral shader/binding observation. This extension cannot
prove consumed UV, hand-child association, winning cache or grasp. The earlier
retained weapon matches remain valid but cannot take the different hand asset's
annotation. Preserve original rejection and old fixtures; full-mod scope remains
open. Actual shooting/vehicle/HMD/network acceptance belongs to the user.

## Latest native retained-copy result

The fresh neutral run of source96d971f1cda7215d82363eaade6d4f6ad0e49372,
fingerprint cdc45f54548282a0afdfec4f7478eada860f816f08bea850b28a857f757b1fe4,
reached expected Jungle with105native projection pairs. Native isolation shutdown,
owned cleanup/display retirement and12protected-file preservation passed. Whole
collector remains rejected:0accepted observations,32rejected records.

Eleven retained draws (7right/4left, eye0) uniquely match all five private candidate
channel hashes and replay the unchanged202word native shader against the captured
position reference. All218vertices per draw agree; maximum absolute homogeneous
clip error across the eleven draws is1.57475e-5. These are historical post-original/
pre-cleanup diagnostic copies, not whole-trace/cleanup/GPU/grasp certification.

The matched geometry is weapon mesh3; the existing indexed hand annotation refers
to a different asset/mesh2. It cannot be applied to this weapon or certify grasp.
The subsequent noUV0/1/5/6pass still rejects whole-trace collection, and the second
eye still lacks winning-event admission. Astra designGO approves an exact passive
noUV-shader observation using the existing owned slots and original draw;
implementation is next. Numbers must derive from the original failure declaration,
with original5/6 scalar-binding comparison and all geometry/filter/cache gates
unchanged. Extra native queries are justified
only for this now-demonstrated missing pass; no broad lifetime/census repeat.
Assets, shader bytes and runtime captures stay private. No firing/movement/vehicle/
head-motion/hardware/network acceptance was performed. Prior fixture is archived
and must not be rerun/resealed. Full-mod scope remains open.

## Historical retained-draw diagnostic — source GO

The latest native capture stored one qualified draw in each of12first-eye traces
before a subsequent unsupported pass rejected the whole trace. Source now emits
those already owned geometry copies under a distinct rejected companion. No
native query, buffer lock, state write, owner or cleanup route was added.
Original first rejection and Complete-only ordinary payloads remain unchanged.

The companion states post-original/pre-cleanup history only. Successful original
draw return and then-current checks preceded storage; subsequent cleanup, outer
finish/current state, winning cached-eye ownership and grasp remain unproved.
The strict reader requires callbacks63 and complete copied-state/identity/count
qualification, then reuses the same per-draw payload validator. Nonzero-draw
passive receipts are permitted only after independently validating the retained
companion. They otherwise retain their original zero-draw rule.

Matching and unchanged-bytecode position replay share per-draw helpers but return
retained diagnostic collections separately. Diagnostic agreement cannot affect
accepted matches, coverage, both-eye readiness or alignment. No controller/grip
matrix was emitted for these diagnostic draws; unavailable references stay null.
The reference measurement tool still uses accepted draws only.

Astra combined source GO follows correction of twoP2 findings: missing callback
provenance and valid retained/passive coexistence. Latest local Astra/xhigh turn
metadata verified; independent backend attestation unavailable. Four products
rebuilt on `cdc45f54548282a0afdfec4f7478eada860f816f08bea850b28a857f757b1fe4`, IPC10/wire7 unchanged.
All68Debug/68Release and28normal/optimized native/compiled gates pass;14reader and
11passive-reader groups pass normally/optimized. Tests cover stored copies after
later rejection/abort/retirement, failed-original/current non-contribution,
malformed/foreign/duplicate/truncated companions and replay non-promotion. Earlier
actual logs retain identical interpretation against the prior source reader.

No runtime has used these products. Next is a fresh sealed neutral capture of
historical channel hashes, program and reference; no old fixture may be rerun or
resealed. Full geometry/grasp, cache, melee release, driver controls and device/
network/user acceptance remain incomplete. This is implementation evidence for
the next measurement, not full-mod completion.

## Latest neutral capture — rendering works, geometry gate remains

The rebuilt observed-layout adapter ran in a fresh private fixture on source
`d98c3019d03603f8a857e4296159daa4cb73c497`, fingerprint
`710534dfb75c9bb5ceac0653e5cc89daf8024a39b732526e1e751548c4dafbc1`.
Expected Jungle scene identity and101native projection pairs were recorded.
Both world-eye images were visually inspected and show static stereo parallax.
Native isolation shutdown, exact-owned cleanup and12protected files passed.
No head-motion, physical hardware, firing, vehicle or network acceptance is claimed.

The collector correctly remains incomplete:32rejected records, no completed
whole-trace geometry/reference output. Twelve first-eye traces each retained one
qualified draw before a later pass failed declaration grammar. That later pass
declares0/1/5/6 with noUV3; its basic range checks pass. The current Complete-only
emitter suppresses the earlier owned geometry copies after rejection. This is a
concrete diagnostic boundary, not evidence that gameplay stereo is broken or
that the unseen earlier shader/channel hashes match an asset.

The bounded Astra design review approved preserving already owned per-draw copies
as distinct rejected diagnostics, reusing geometry serialization and strict
per-draw matching/replay validation. Implementation is next. The draw counter
certifies storage after successful original return and current checks, before
owner cleanup; it does not certify cleanup or outer finish/current state. These
historical copies must stay outside completed observations and coverage/readiness.
Extra passive noUV queries are deferred unless this minimal route cannot answer
the next concrete question. Do not widen declaration admission using an inactive
UV binding, reuse cached-eye events or repeat this attempted fixture.

The four parallel commits are integrated and the combined final tree rebuilt;
see [integration mapping](PARALLEL_INTEGRATION.md). Current68Debug/68Release and
28normal/optimized native/compiled gates pass. Source publication is authorized;
private assets, shaders, capture payloads and runtime records remain local.

## Current ID1 observed-layout adapter — source GO

The private neutral V15 run reached the expected Jungle scene and103complete
projection pairs. Thirteen complete passive receipts across both hands, first eye
only, showed stream7 at the local-index descriptor offset and8 at weights, sharing
canonical position-buffer identity with stride4/frequency1. All13copied the same
202-word VS1.1 program. Native isolation shutdown/owned cleanup/protected12files
passed. Geometry remained rejected; no cached-eye/grasp acceptance is inferred.

Astra verified the actual shader's position dependencies and approved a narrow
ID1 adapter. Source now selects exact observed7/8 declaration for ID1 getters,
range validation and replay seeds. Legacy5/6 and Scope13 defaults remain intact.
Mixed/aliased/missing declarations decline; original shader words, five channel
hashes/exact influences, original draw/COMowners/nativefinally/current guards and
unknown-position refusal remain mandatory. Private replay schema2 adds a bounded
layout enum; schema1 retains legacy behavior. Failure receipts explicitly tag the
selected layout, preserving older implicit-legacy logs.

All four products rebuilt on `710534dfb75c9bb5ceac0653e5cc89daf8024a39b732526e1e751548c4dafbc1`.
All68Debug/68Release and28normal/optimized native/compiled checks pass. Focused
10reader/11passive-reader/6compiled-replay groups pass normally/optimized. An
unchanged actual shader/constants compatibility experiment with5synthetic vertices
produced known positions only for Observed78; it is not native geometry/reference
proof. Astra source GO is scoped to this adapter, with latest local Astra/xhigh
metadata verified and backend attestation unavailable. No new build runtime or
deployment yet. Next finite proof is actual consumed-channel capture and unchanged-
bytecode replay against the captured reference in a fresh sealed private fixture.

Product hashes:
- `d3d9.dll`: `6d7244438a9e2f018356472a5ad8821e64079be2f9042af684a05eed6f91584c`
- `ss2vr_host.exe`: `116a56641ac9896efaeaa425744de8f7f8f3a088c4bcbc80c39884553aa13ceb`
- `libopenxr_loader.dll`: `bb011caa82528c541a73967ce6408f82198ff4fd0358b38b54884719d863bd1d`
- `SS2VRServer.dll`: `b0ce2236227c17c4540421a5eb147517a959bf2e1fb691f495749ce28541223a`

Cached-eye winning-event retention, controller/anatomical grasp correspondence,
physical release, driver controls and actual device/network/user acceptance remain
open. No guessed offsets, role remap for Scope13 or native release priming.
Earlier entries retain historical identities and evidence limits.


## Current passive native stream diagnostic — source GO

The exact ID1 step20/declaration rejection now preserves the original failure,
then observes actual0/7/8 bindings and the bounded vertex program around the single
original draw. There are no buffer locks or state/binding writes. Native partial
outputs stay in the existing typed owner; incomplete calls expose only status/
operation/qualified HRESULT. A shared boolean-only raster check cannot admit
geometry. Reentry/abort/epoch/cleanup invalidation survives retirement.

Astra native and corrected-reader source GO follow the bounded design review.
Eleven normal/optimized reader groups cover original-history/attempt causality,
partial/interrupted output qualification, exact copied-input equality, malformed
and incomplete data, retirement and no geometry promotion. The corrected reader
is rechecked through Debug/Release CTest. All68Debug/68Release and28normal/optimized
native/compiled checks pass; all four products rebuilt on `66274aef6f59a7e6d8559c320077f214e41bed5ad386a5f03ebcfc6a936523b8`.
No runtime or deployment has used these products. Fresh sealed preparation is next,
under existing bounded private neutral authority. Roles/grasp/alignment/cache and
full-mod acceptance remain unverified. Existing Scope13 and position replay stay
unchanged; declared7/8 position dependencies remain unknown until proved.

Product hashes:
- `d3d9.dll`: `3bbf8d59d97b09c0640333a9415954be9abe00285b2bd16e4698902172642f79`
- `ss2vr_host.exe`: `d1e244233dd366b03b21b6a9e3c5478b9582f70dd95164bd1066268c6d29dfdb`
- `libopenxr_loader.dll`: `bb011caa82528c541a73967ce6408f82198ff4fd0358b38b54884719d863bd1d`
- `SS2VRServer.dll`: `5b4556363165971bf632036a19be4e9fa37c45512be589f0ec9e9e05274e2d2d`

See docs/IDLE_EVENT_COLLECTOR.md for review dispositions and the exact optional
receipt schema/flags. The new imported reader is included in source-tool seals.
Earlier entries below are historical identities and evidence limits.


## Current rejected-animation diagnostic source checkpoint

The first rejected contributor now preserves its already copied animation header
and expected Idle identifier. Original query/End/finally/first-rejection behavior
is unchanged; no extra native reads or retained references. The strict reader
rejects impossible producer states and never promotes rejected data. Astra native
and corrected-reader source GO are recorded in docs/IDLE_EVENT_COLLECTOR.md.
All products rebuilt on `4038bdcf2d72645b12dfbcdc3df3808733a8976175f541ed4bc7f8883eddb483`;
67 Debug/67 Release groups and 28 normal/optimized native/compiled gates pass.
Nine reader groups pass normally/optimized and through both CTest configurations.
No capture or deployment has used this build. Product hashes:
- `d3d9.dll`: `9a1850e0b30491959b8c48ecce5c36511d32497db70d27cddea45481feeec564`
- `ss2vr_host.exe`: `a862e4194087f173834428ef595c3a8c4719f6511948638b53180d57d803d773`
- `libopenxr_loader.dll`: `bb011caa82528c541a73967ce6408f82198ff4fd0358b38b54884719d863bd1d`
- `SS2VRServer.dll`: `ccc49dbc766c8d9c46aea599e17c8a4aca8e3528d90915e66a89e019c7608635`

Native investigation establishes caller-selected7/8 roles cannot be inferred from
declaration numbers. A passive observation of actual stream bindings/program,
using the existing typed owner and original draw, has Astra design GO; source
implementation is next. It will preserve rejection and keep all geometry/grasp/
cache/Idle gates. Fresh static selected metadata/candidate production matches all
11 earlier supported candidates/channel bytes; one equivalent unsupported reason
text changed. Private assets, outputs and receipts remain local. Full-mod and
hardware/firing/vehicle/network acceptance remain incomplete.

Earlier entries retain historical identities and limits.


## Current integrated checkpoint and actual collector diagnosis

The four reviewed parallel commits have been cherry-picked in order onto
`bb95eb45439abc8af3c8aebbea06b9bf467e0785`. The integrated tip is
`f1bb7f5e5eac7221801f195f4b5726dc4b0b6dcd` (see exact mapping in
[parallel integration](PARALLEL_INTEGRATION.md)). Strict primary-input admission,
private candidate export, pinned selected-field production and CTest registration
are now present. All four products rebuilt against source fingerprint
`162790f5b7f37e6887ff76d44a5f76319af8cdd0fee960c6043cd95d68f8ad85`. All 67 Debug and 67 Release groups, 31 optimized
producer/exporter groups and 28 normal/optimized compiled/native gates pass.
IPC10/wire7 and compiled layouts remain unchanged. This integrated build has not
run in game; isolated contribution binaries were not imported or deployed.

The previous diagnostic build did run: private neutral capture reached native
Jungle gameplay with 106 complete projection pairs, normal native isolation
shutdown, no owned survivors/errors and 12 protected files unchanged. Geometry
remains rejected: 32 rejected records, zero accepted draws. Twelve qualified
bound-input failures stop at range substep20. The native declaration has channels
on streams7/8; the collector currently selects5/6. Their exact roles and bound
resource offsets must be proved before a narrow mapping correction. Event/cache
ownership and winning Idle admission remain separate gates; neither has been
relaxed. No head-motion, hardware, firing, vehicle or network pass is claimed.
The metadata tools prove selected static asset fields only; native rendered
association, physical grasp, alignment and full-mod completion remain open.

Earlier entries below retain historical identities and evidence limits.


## Current bound-input diagnostic checkpoint

The first-rejection capture reached gameplay but identified13first-eye bound-input
failures, three contributor-name failures and16second-eye palettes without event
admission. Geometry/grasp remains incomplete. Native cache reuse is consistent
with this evidence, but winning-producer retention has not been established.
Astra-approved substep/HRESULT/qualified-output diagnostics now reuse the same
COM calls and range predicates. Reader progression/count/width/qualification
checks reject producer-impossible receipts. No acceptance gates were relaxed.
All products rebuilt:65Debug/65Release and28normal/optimized compiled/native
gates pass; the corrected reader is rechecked. No runtime of this build yet.
Source fingerprint `782e716e38b6cf6081d3fe7940004e5ac815e1bd702ee2f987c82fff25e019b3`, IPC10/wire7 unchanged. Product hashes: game
`2200860112cbf2688b60319f8b579f4c0bb84f35d70c8af2ef9a2b3a8969cdd3`, server
`8e148667025f1b92603d5c1c9cb85027d8ac3427e69730796c5f56704eedf45d`, host
`588c4b8d08a20199c9a5d112085cbaed5cd2dcaa2ab29370d11258f03591b771`, loader
`bb011caa82528c541a73967ce6408f82198ff4fd0358b38b54884719d863bd1d`.
Next: fresh sealed private neutral diagnostic capture under existing repeated
authority; use actual substep/scalars to choose one supported fix. Preserve all
old failed attempts and originals. Full geometry/grasp, driver controls and
physical/device/network/user acceptance remain open.


## Latest gameplay and diagnostic build

The eager private runtime setup reached actual Jungle gameplay in the subsequent
neutral capture. Scene identity, 106 complete projection pairs and two distinct
world-eye images were recorded; native loading continuation and isolation shutdown
were observed. Owned cleanup and protected original files passed. This fixed-pose
simulation does not certify head motion, hardware, firing, vehicles or multiplayer.

Geometry remained rejected in all 32 trace records. First-failure instrumentation
is now implemented and Astra-reviewed, preserving all native borrows/calls/gates.
The associated compiled TLS check follows entry/capture origins and all exits;
negative controls cover the previously unsupported compiler shape. All four
products rebuilt; 65 Debug/65 Release groups and28 normal/optimized native/compiled
gates pass. No capture has yet run this newer diagnostic build.
Current source fingerprint `bf13df0ebf1c35cd0a1d25b10cf35479882d53518f3221fe9a0749be074b0093`, IPC10/wire7 unchanged. Product hashes: game
`7446f348fefac0e9aa65f37e570ccc0f6ce851e3972f6837d11b2942bbb7ef35`, server
`fa2bd5cd4962211f8bf0d865fed4ecca2c3a68ae56f79102e24f0b4e640172e2`, host
`952700de14722ee6510961bbedb06f8e8a34b2a8262c201b0048f0eb77535ecb`, loader
`bb011caa82528c541a73967ce6408f82198ff4fd0358b38b54884719d863bd1d`.
Actual consumed geometry/grasp, evidenced driver steering and remaining physical/
device/network acceptance stay open; no native release or alignment is inferred.
Earlier entries below retain their original identities and evidence limits.


## Latest continuation — repeated neutral capture authority

The user explicitly permits necessary private neutral SS2 captures for the
startup/idle-geometry blocker, up to six minutes each. This supersedes the
consumed one-capture limits in historical entries below. Only the owned private
window may be restored/focused and receive one guarded loading Enter pair;
no desktop input, movement, firing or switching. Each attempt uses a fresh
sealed isolated game/prefix and preserves prior evidence and original data.

Two later neutral attempts reached the expected scene stream but remained at
loading. Activation receipts passed process/incarnation/target guards, then
reported SetForegroundWindow failure. Changing the private display from kiosk
to desktop shell did not resolve it. The nongame console/GUI comparison was
negative; do not change observer subsystem policy based on that hypothesis.

A private-only eager graphical Monado option and read-only XRes window receipt
are now implemented for a controlled window-order check. Exact service ownership,
WM_NAME, mapping, InputOutput class and dimensions are required before SS2.
The optional nongame extent reuses the existing D3D9 probe and cleanup, then
ends before SS2. This is a diagnostic setup change, not native gameplay or
OpenXR frame acceptance. Production products remain unchanged at b1b0086367de.
Astra/xhigh gave scoped source GO. The actual nongame ordering passed: the
later exact probe owned the active XID and native foreground, D3D9 reported
60 Hz/raster success and native retirement, and the same Monado XID remained
viewable afterward. Owned cleanup was clean and protected files unchanged.
These are sequential observations, not sustained agreement, pure map-order
causality, submitted VR frames or repaired SS2 readiness. A fresh neutral
capture is prepared with unchanged native guards; source/handoff review is
pending before its run. Grasp alignment,
evidenced driver steering and physical/network/device acceptance remain open.


## Private display prerequisite — native result

The corrected private display/D3D9 prerequisite passed with the actual x86
probe: desktop, adapter and fullscreen swapchain all reported 60 Hz at
1280 × 720; GetRasterStatus returned HRESULT 0 and the native receipt certified
resource/window retirement. The launcher exited normally and the wrapper reported
no owned survivors or cleanup errors. SS2, Monado and the idle collector were
explicitly not launched in that prerequisite run. This proves graphics readiness,
not VR gameplay, geometry, grasp, steering or hardware acceptance.

Earlier failed Xvfb/setup/native diagnostic attempts remain preserved privately.
The diagnostic now writes a bounded exclusive Win32 receipt after native cleanup,
uses the existing direct-prefix diagnostic route and statically links its thread
runtime. Game launch still uses the existing Proton route. The prerequisite-only
mode ends before SS2/Monado even on success. Astra/xhigh gave scoped source GO;
16 display checks and 22 runtime-evidence checks pass normally and with optimized
Python. No src/CMake edits occurred; current mod products below remain unchanged.

The user permits repeated private display checks and previously authorized one
replacement neutral SS2 idle collection. The corrected replacement has now
completed one attempt in a fresh private fixture with current b1b0086367de products.
The expected Jungle scene hash was observed and the host started, but native
loading remained blocked: no gameplay, ID1 geometry or world-eye evidence was
collected. Four existing owned activation requests did not clear readiness.
Native isolation shutdown was observed; wrapper survivors/cleanup errors were
empty and all 12 protected files remained unchanged. No desktop focus, movement
or firing was sent. The replacement capture is consumed. Full grasp,
driver steering and user-operated HMD/MP/firing/vehicle acceptance remain open.
See [private collection procedure](PRIVATE_DISPLAY_COLLECTION.md).

## Native foreground diagnosis — bounded comparison complete

The startup observer now records each existing activation request, including
native guard rejections, exact window ownership and actual foreground results.
Astra caught and corrected a diagnostic classification bug: owner samples taken
only after the child observation could have been treated as coverage. The final
check requires the child's timestamped entry/observation interval, independent
owner samples within it, endpoint brackets and gaps no greater than 100 ms.

The actual private nongame comparison completed with both console and GUI
observers seeing the exact same tool-owned foreground window. Independent owner
observations before, during and after agreed; both intervals were conclusive.
No observer-subsystem focus defect was demonstrated, so no production observer
build policy was changed. Graphics and cleanup passed; SS2/Monado/collector never
launched, and protected original files remained unchanged. Stop this hypothesis.
The next discriminating step is a fresh neutral SS2 observation using the new
per-guard receipts; it requires separately renewed bounded capture authority.
21 display checks and 22 runtime-evidence checks pass normally and with optimized
Python. The probe and both observer builds compile with warnings as errors.
These tool changes do not alter the current mod-product fingerprint below.

## Full-project closure disposition

Rig initialization and recenter now reuse the existing projected horizontal
heading rather than Euler yaw extraction. A 40-degree heading combined with
50-degree pitch/20-degree roll formerly became approximately24.9456degrees.
The corrected reference preserves full relative head rotation, hand geometry and
existing recenter/control/generation policy, with zero/prior-heading fallback for
vertical gaze. Astra/xhigh gave scoped source GO; the16-case mixed-tilt regression
and all65 Debug/Release groups pass. All products rebuilt and28 normal/optimized
compiled/native gates pass. No game/headset/network observation ran.

Current source fingerprint `b1b0086367defd9d4c5f51e5bfb639a1f2e17a8a6d012003409f094022dd653c`;
current products: game `14dea3bdeb5afd51ff338f8075c6eb0ac15b8620df839a04b48581ae10a2b783`,
server `8e61e1f4f04885249576b3b4ca63aa41bddc262bfc2232874ce9c958691b5906`,
host `535436a8f240593997ae9e3862af98ba3cdeff703537211fdc131c0e54d7c5c8`,
loader `bb011caa82528c541a73967ce6408f82198ff4fd0358b38b54884719d863bd1d`.
The earlier neutral collector used separately reviewed immutable425fe8a5
products; its failed private attempt remains preserved. The fresh replacement
preparation uses current b1b0086367de products; its bounded neutral collection
status is recorded above.
No installed game, old checkout/WIP, old lab/prefix or original user data changed.


The [ID1 collector](IDLE_EVENT_COLLECTOR.md) now includes bounded offline shader
replay, exact channel/hash/index-bound surface reference measurement, uncalibrated
world grip/aim capture, and a separate neutral user-operated lab mode. At that source checkpoint, GO
covered these boundaries, with backend routing unattested. No actual native capture
was replayed and no physical-grasp/controller correction is accepted. A separate exact private lab/configuration now passes preflight; Astra gave
scoped source/handoff GO for one user-operated neutral collection. Preparation
seals settings/profile bytes, exact user aliases and harness/reader identities,
and cannot be switched into firing/movement by disabling the neutral selector.
22 runtime-evidence groups pass normally and with optimized Python. At that
checkpoint no native observation or installed-game deployment occurred. The
subsequent failed private attempt is recorded above.
Collector product source fingerprint `425fe8a5ada7aac7fa8f02bc0d301f2fab43a0b2d14c79dbadb47e475b59177a`;
65 Debug + 65 Release groups, 28 normal/optimized compiled/native gates and five
private compiled-byte controls pass. No native exception/COM reentry was executed.
Collector products: game `156bfb4885ba748c648df1831d83a1c617ecc95b8d0b125f4cceab1ed839e65f`,
server `953789fe903a3fdf94dd462361e5583a9d3fb57b67e478c3a6e3a26d3810ee93`,
host `73c174b4737c2a18a60402dcbd59c069fd61be91c98d8c49b34121792a8216cd`,
loader `bb011caa82528c541a73967ce6408f82198ff4fd0358b38b54884719d863bd1d`.
Earlier source/package identities below are historical and immutable.

The [final Astra source pass](FULL_PROJECT_SOURCE_CLOSURE.md) found connected
major feature paths, but full implementation is still open. Resolve one ID1
rendered-instance/resource/idle/stretch/grasp association before any measured
alignment correction. First-use melee source now admits an actual first gesture
press after receiver-fenced quiet, without a trigger-prime cycle or guessed Low. Driver-grab steering still lacks positive wheel/seat geometry.
User-operated testing is separate; no more routine game/firing probes are planned.

## First-use melee source update — 2026-10-08

Astra/xhigh reviewed the incremental first-gesture-high change, caught a post-getter
stop/rearm race, and accepted the corrected engine SHA
`6e87aded8fc9c8c2d4cfab1c2f4f56239214d3c40753985222737d65bc70c0fe`.
Backend execution remains unattested. Unknown/low remains inert; an admitted first
G high invokes the real canonical press. Actual stop completion still resets the
quiet fence. Exact in-flight dispatch identity prevents synthetic redispatch.
All products use source fingerprint
`160c20557788e8059d91a11bcf048daedfd72630a4d24d463d5f8edf261afb03`.
61 Debug + 61 Release groups, 22 normal/optimized gates and 19 compiled-byte
negative controls pass. No native runtime, input, contact or MP role test ran.
Current products: game `98e6ee76bb29ee9bd3d422ed1589941c58f680233772841a5abedf86265f34d9`,
server `a1cd40f7cc885fb7dada3de86ceff7e260a6cfe599bedc65730675966a219d83`,
host `76de8e8b4b2e64c68ff8987a09f3fd2226a3c886b69408b622c7e38ee17c678d`,
loader `bb011caa82528c541a73967ce6408f82198ff4fd0358b38b54884719d863bd1d`.
The previous package below remains immutable, not deployed, and has older behavior.

## Earlier reviewed MP physical melee source — 2026-10-08

The default-off unique-saw adapter now connects the local physical sampler,
server-authoritative native weapons and remote-observer native consumers.
Wire7 adds separate G fields and reuses existing local retained captures, server
pose intervals and Remote owners. It does not write G into native160/348, expand
manual fire, invent observer live intent epochs or replace native combat.
Matching wire7 client/server products are required; IPC remains10.

Integrated Astra/xhigh Source GO covers engine SHA
`271aba3f0c3173a2350111a13a0cec53dd7ef2135d2453774620422ca80f9591`.
Main verified and adopted stop/quiet fencing, cancellation-only observer source
loss, held-only latest fallback after pulse consumption, own quiet witnesses for
delayed pulses, and scoped isolation of original observer manual reads.
See [source disposition](PHYSICAL_MELEE_CONSUMPTION.md).
Current-turn model/effort tags were verified; effective backend introspection was
unavailable. Source GO does not establish native exception or MP gameplay behavior.

All products rebuilt on source fingerprint
`ab9b8ce84e25309b47cce63c40711214b5e8ac9f96d050e22dd0d1f776e42e46`.
All61 Debug and61 Release groups pass with active assertions. Both x86 products'
linked10-callback, native-finally, MP dispatch, muzzle, roomscale and12 melee
cleanup/13 transitive scalar-body checks pass normally and with Python
optimization (22 gate runs).19 private compiled-byte controls reject unsafe
cleanup and removed TLS stores, including both new manual-query restore paths.
The Release dependency-environment failure was corrected using the existing
Capstone installation; no dependency reinstall was needed. Product contracts
agree on IPC10/wire7, Input272/Request440/Ui352/Slot33554928/Shared83887840.

| Product | SHA-256 |
| --- | --- |
| `d3d9.dll` | `22ff0d717ecb657a0ec63155d74500f596475c347abd868ad245bacae48ead9b` |
| `ss2vr_host.exe` | `fa5503d4ddc0b69439687dc0bad6f38342a43741526e73baad90008a0eaa5124` |
| `libopenxr_loader.dll` | `bb011caa82528c541a73967ce6408f82198ff4fd0358b38b54884719d863bd1d` |
| `SS2VRServer.dll` | `a538b7f62412227f11f13bc27be79b0b58a1a974992607f13e2399da17813523` |

No game, host, Wine/OpenXR, native server or input/firing probe ran for these
changes. Nothing was deployed into the original installation or private game
lab. Vehicle, shooting and primary VR/MP acceptance belong to the user. The
comprehensive [test procedure](USER_VR_TEST.md) includes first-use without trigger
priming, short-pulse/observer tests and matching-product requirements. The older
build in this section required a real release first; the current first-gesture
update above supersedes that restriction without guessing initial Low. Positive driver-wheel geometry and
broader vehicle control coverage remain open. Full goal is not claimed complete.
Earlier SP/source entries below remain historical evidence for their own builds.

## Reviewed MP origin and SP saw source — 2026-10-08

The controlling-local-XR roomscale route now supports negotiated multiplayer
roles while preserving original native server movement/correction. Retained
per-hand input captures keep raw calibrated poses and recompose against the
current origin; inactive hand independence, head-only overflow rejection and
capability bootstrap are covered. Astra/xhigh bounded Source GO is recorded in
[MP_LOCAL_ORIGIN_CAPTURE.md](MP_LOCAL_ORIGIN_CAPTURE.md). No MP runtime pass.

The default-off single-player uniquely mapped circular-saw adapter now consumes
physical gesture through exact native canonical/held boundaries, without writing
native160/348. Astra caught and main corrected cross-hand retirement, lost-source
cancellation, nested same-level receipt ordering and non-player receiver access.
Final bounded Source GO is on engine SHA
`81d95cad32bbdde4fbfc6cf4c63e18ab031c2f848605dee19505975aba1e8204`.
See [PHYSICAL_MELEE_CONSUMPTION.md](PHYSICAL_MELEE_CONSUMPTION.md). Initial/copy
history remains unknown until a real native release; MP gesture/observer delivery
remains unimplemented. This slice is not full physical-melee completion.

All products rebuilt on source fingerprint `3acc2744e4cba2ea291ae4c62e22bc3e26ce696ddd88c3765af03d2935b794e6`.
All61 groups pass in Debug and Release with active assertions. Both x86 products
pass linked10-callback ABI, native-finally, multiplayer dispatch, muzzle cleanup,
roomscale integration and11 melee cleanup extents/12 transitive scalar bodies,
normally and with Python optimization.13 private melee byte controls reject
unsafe cleanup or missing TLS restoration. Positive native local-player admission
is inspected at roomscale entry and callback revalidation; four branch inversion
controls were rejected on the preceding worked source. Artifact contracts agree
on IPC10/wire6, Input272/Request440/Ui352/Slot33554928/Shared83887840.
Local Astra/xhigh tags were verified; backend routing introspection unavailable.

| Product | SHA-256 |
| --- | --- |
| `d3d9.dll` | `ef239347b54a8d7c54f62d795784d0e8c04f3e1a8f86a253885a857b7624d002` |
| `ss2vr_host.exe` | `6e52b652b59ca564323f99752a3c24df709c94cec27c1801a3e538ae663a0e92` |
| `libopenxr_loader.dll` | `bb011caa82528c541a73967ce6408f82198ff4fd0358b38b54884719d863bd1d` |
| `SS2VRServer.dll` | `be8aba2855abbe40ed5ec9f1fb5b234c443de146bce83ce7d5c0ab5865db3569` |

No game, host, Wine/OpenXR process or individual firing probe ran for these
changes. User-operated hardware/gameplay acceptance is separate. Driver grab
geometry/controls, MP physical melee/observer delivery and first/copy usability
remain active implementation work. Earlier entries below are historical evidence
for their own source snapshots and do not override this current disposition.

## Active PC implementation

Remote head palettes now require an admitted frozen VR stereo pair; ordinary
desktop draws retain native heads and no longer recapture a post-producer anchor.
Typed stock loading/reference evidence closes the selected resource-cleanup edge,
without new retention, worker locks or scheduling. The restricted option is enabled
by default. Actual multiplayer appearance/lifecycle remains user acceptance.
The transitive compiled cleanup audit found and removed lazy TLS initialization
from UI-fault retirement; original and cloned helpers remain scalar with local
branches. Astra source GO, all-product build/61 Debug groups, both x86 cleanup
gates, eight compiled negative controls and artifact/layout checks pass on source
6993876190e0beb3fc10e966a1edab6280eb5bcc8617e912bda686d4e589ac56.
No runtime was launched for this change. See REMOTE_HEAD_RESOURCE_OWNERSHIP.md.

Tracked native weapon charge/recoil translation now survives model placement and
the shared muzzle/laser conversion. The adapter captures the existing getter
invocation, preserves native scale/reflection and performs no guessed gripping
calibration. Explicit native-finally cleanup also fixes stranded muzzle recursion
state. Astra source GO follows four cache/forwarding fixes; all products rebuilt,
61 Debug groups, compiled callback/cleanup negative controls and artifact/layout
gates pass on sourceff8a2d050e3f81ba18c6f1c4d6df4b7e2955edbf0c903e2206ef8bb71c70d628. No game or firing probe
ran for this change. See NATIVE_WEAPON_DISPLACEMENT.md. Full implementation remains
open in native melee, MP roomscale, driver controls and remaining presentation coverage.


The latest user instruction reserves primary in-VR gameplay testing, including
shooting and vehicles, for the user. Finish remaining implementation and Astra
source reviews, with builds/offline checks; do not resume routine individual
weapon-firing probes. Historical probe plans below are not current instructions.

Run213458 completed a bounded non-firing native resource-identity observation on
compiled sourceb8e77f540f92633b65a9df8f00b83f35b834369030acd5bd867e46ecee8b9882.
All five exact gun/hand resource opens were hashed with their native stream
positions restored. Jungle scene identity,303 neutral complete native pairs and
normal shutdown with no cleanup errors were recorded. See HAND_MODEL_ALIGNMENT.md
for exact resource hashes and the unresolved rendered-instance/grip association.
All products rebuilt;61 Debug groups and compiled fingerprint/IPC10/wire6
artifact checks passed. This does not complete physical hand alignment or the
remaining melee, multiplayer origin, vehicle or remote-head integrations.


Run190429 completes the seven-pose native gameplay follow-up on compiled source
725425de9c8dd4315399e65bf7c28c353bc1390e176747935b4477a4f2d0eb48.
All seven images match actual native cameras, complete world/UI and successful
projection requests;500 distinct neutral complete pairs were observed. Native
translation deltas are approximately0.1m; rotations approximately0.15rad. Static
image patches shift22pixels at the foreground rock,7 at the middle road and1 at
the distant stone arch between eyes, consistent with depth-dependent parallax.
The bounded root-depth regression passes and fixed right-eye occlusion was
visually inspected again. Normal native shutdown and no cleanup errors were recorded.

The run used process-local PROTON_NO_NTSYNC=1, selecting fsync in the actual log.
A previous run exited before scene/host readiness with a Wine assertion; no
mod-hook cause is established. Seven desktop captures failed, recorded separately;
they do not certify desktop image occlusion. Independent native activation probes
identified a minimized game window and verified ordinary restore→foreground/exclusive
reacquisition. See DIRECT_GAMEPLAY_LAB.md. Native dual observations find two ID1
Zap guns; the earlier Colt-only fixture was incorrect. Astra verified native Zap
charge/release/projectile and separate reload-counter paths. The adapted exploratory
probe remains unverified. No model alignment, impact, hardware or network pass is
inferred. All61 Debug groups and compiled artifact/IPC10/wire6/native boundary
checks passed this product. The adapted receipt source88717c8e229682cd15f3f814df612221dbff5cf6b402556690b98791c32eded3
has now rebuilt all products and passed the same61 groups, with16 evidence
negative controls and refreshed native/compiled checks. This source has conditional
Astra GO for the bounded Zap probe; it has not yet run to firing acceptance.

The right-eye Jungle terrain/vegetation/waterfall leak is corrected in the local
source. Actual draw ranges exposed a native-cache/device mismatch introduced by
raw target viewport resets. Astra/max verified the native early-return and geometry
reference; the adapter preserves current depth endpoints across eye/scope/desktop
target changes. Fixed actual eye captures and draw traces agree, with independent
user confirmation. An old-build counterexample reproduced the defect; the fixed
product is restored. See [STOCK_RENDERER_COMPARISON.md](STOCK_RENDERER_COMPARISON.md).
The earlier fixed product fingerprintf945770801a867e9ec9713066ab9b4e200e1b578cbf307851b4055520b8b391f.
All61 Debug groups, compiled product contracts/native-artifact/layout checks pass.

The automated bounded root-partition regression rejects both old-build probes and
accepts the fixed trace. Image occlusion remains separately inspected, not inferred
from counters. Full seven-pose follow-up after this repair encountered native
loading-continuation/owned-window capture races; these launcher gates are being
corrected without changing renderer/simulation policy. Run173527 reached actual
gameplay and201 neutral complete pairs but its desktop capture failed. No new
seven-pose, hardware, network or full-mod pass is claimed. Native dual-wield audit
is complete; ordinary simulated per-hand trigger input and UI weapon/ammo/fire
observations are being prepared for actual independent press/release probes.

The current direct-scene run154502 verified the expected native Jungle stream hash,
guarded loading continuation, complete native world/UI stereo and all seven head
pose image pairs under private simulated Monado/Proton. The harness required300
distinct neutral complete pairs before capture. Exact request-correlated native
cameras and successful projection receipts accompany every capture. Native camera
centers respond by0.1m along each local axis; yaw/pitch/roll respond by0.15rad.
Normal isolated shutdown and no cleanup errors were observed.

Current rendering evidence: [PC_GAMEPLAY_READINESS.md](PC_GAMEPLAY_READINESS.md).
The exact native depth partition and TEXCOORD0 builtin input contract defects
were corrected with bounded Astra reviews. [Senior disposition](RENDERING_SENIOR_REVIEW.md)
records the changed correlation/acceptance contract. All61 current Debug groups
pass; IPC10/wire6 compiled fingerprints agree across architectures. Native menu
transport remains accepted separately. Full-mod/hardware/network acceptance is
open; terrain/vegetation image quality still needs the stock-renderer comparison.
The earlier startup summaries below precede this milestone.

Development is active. Start with [CLOUD_CONTINUATION.md](CLOUD_CONTINUATION.md)
and [PLATFORM_SUPPORT.md](PLATFORM_SUPPORT.md); HANDOFF.md and the records below
are historical evidence, not the current pause state or feature count.

Windows and Linux via Proton are required. Native magnified scope source views
and ordered cap imagery are connected in source, alongside native zoom, mounted
controls/turret lasers, native flat/world overlays and multiplayer integration.
The current offline suite has61 groups. Native products cross-build. The user now
permits PC runtime testing and additional sessions before feature completion.
Initial Xvfb launches hit a stock-reproduced zero-refresh DXVK failure. A focused
real-display IPC10 proxy run now renders the native desktop menu. See
[PC_VALIDATION.md](PC_VALIDATION.md) for identities and observations. The actual x64 host reaches FOCUSED under Proton and native menu transport works.
The bounded simulated gameplay path is recorded above; broader lifecycle and device acceptance remains open.
No headset or network runtime acceptance exists. Older coding-first/no-runtime
instructions are historical; actual observations remain separate from offline checks.
GitHub Actions is disabled at the owner's request; checks remain local.

User targets are now confirmed: SteamVR + Steam Frame and Envision/Monado +
Bigscreen Beyond, with Windows/Linux-Proton coverage. Simulation and brief SS2
focus are authorized. The private simulated Monado baseline and the actual x64
host under Proton both reach FOCUSED. A synthetic IPC fixture exercises the actual
host's D3D11 swapchains, WMR-profile hand/head input and successful menu-quad
xrEndFrame calls; it does not prove native game transport or actual hardware.
The game tracing checkpoint confirms recurring native additional-swapchain
presentation and actual1280x720 RT0 ownership. See
[PRESENTATION_OWNER.md](PRESENTATION_OWNER.md) for review disposition/native limits
and [PC_VALIDATION.md](PC_VALIDATION.md) for exact test identities. The draft capture adapter now routes all buffer consumers through the admitted native
chain; ordinary-path and reset retirement changes are implemented with limited reviews.
Full scope/nested/reset runtime acceptance remains open.

Current source connects default-off single-player roomscale body movement through
native whole-body queries, the checked native setter and actual-anchor origin
settlement. Default-off head-volume checks cover handheld and seated world queries,
with IPC10 head/eye clearance limits on cached frames; native exclusions can omit
the ridden vehicle itself. Optional immersive swimming preserves head-directed
joystick input as the default. Remote renderer native-unwind cleanup now explicitly
retires TLS, locks and owned scratch. Packaging checks matching compiled build-input
fingerprints and version/layout contracts instead of hardcoding an obsolete ABI.

Current PC source fixes squeeze-loss weapon-wheel cancellation using durable
per-hand epochs and separate producer admission. All60 Debug/Release groups,
UBSan and compiled/layout checks pass after correcting Release assertion coverage;
Astra/xhigh approved the bounded source follow-up. This is not physical vehicle
steering. See SQUEEZE_ADMISSION.md.

Full implementation is NOT complete: multiplayer roomscale body/origin settlement,
physical melee consumption/observer delivery, broader vehicle coverage, complete
remote-head lifetime/enablement and the final full-project review remain open.
See the current roomscale, head-volume and swimming records linked from the
continuation. Do not substitute test counts for these integrations.

## Historical checkpoint record

The following sections retain their original scope and conclusions. References
to “current”, “paused”, old ABI/test counts or absent scope images apply to those
historical checkpoints only.

## Unaccepted physical-melee worktree

The current working tree additionally contains physical-motion production and
logical-primary integration. It compiles and passes25 portable check groups,
but its native gesture/carry history is **NO-GO** and is not in a development
archive. A gesture-only native history high can become a false carry release;
passing producer checks do not repair that native interaction. The accepted
five-site join described below predates this physical extension.

Astra has rejected both a B0/E8-only stop guard (a reachable short recoil pulse
loses canonical release) and proposed bits2/3/4 metadata (bit2 is the grenade
command). Native AI-hearing notification/dynamic state replication also does
not supply the missing saw observer transition on the investigated paths.
Canonical native consumption, carry isolation and short-input observer delivery
must be resolved before source acceptance or packaging. See
PHYSICAL_NATIVE_DELIVERY_AND_BITS_GATE.md and
PHYSICAL_SAW_CONSUMPTION_REVIEW_BRIEF.md. The full immersive goal is paused;
roomscale body movement, scope images, broader vehicles and complete remote-head
lifetime/enablement remain unfinished. No runtime testing was performed.

## Latest accepted native query and primary source

The accepted source includes the Astra-approved bounded, inactive native model
triangle query adapter and the independently source-approved five-site native
firing join. Desktop startup and carried-object commands remain native; known
VR handheld consumers preserve the native operator/history/mapping path.
Client/server/host/loader builds, all23 offline groups
and actual linked primary-entry/ordinary-wrapper checks pass. This does not
implement body-follow movement or scope images. Public query lifecycle,
primitive contact, body geometry/placement and multiplayer settlement remain
roomscale gates. See ROOMSCALE_MODEL_QUERY_SOURCE_REVIEW.md and
ROOMSCALE_QUERY_OWNERSHIP_AUDIT.md. The immutable0.2.11 package is unchanged.

Latest source adds Astra-approved optional matching pre-draw opaque scope
observations through actual PS2.0/device/retained eye reads, preserving independent
geometry/UV and original native forwarding. All21 offline groups/full cross-build
and compiled ABI/cap/fixture checks pass. No image transaction is enabled and no
runtime is verified. See SCOPE_COLOR_READER_DESIGN_REVIEW.md and
scope-color-source-checks.json. The immutable0.2.11 package remains older.

The single-player baseline is packaged as0.1.0-dev. Current source implements multiplayer input/authority/remote weapon presentation, native menu ray input, comfortable UI, grip alignment, haptics, collision lasers, an opt-in native remote-head palette adapter and physical native gun P/view/depth adaptation. All products cross-compile and ten offline groups pass; Astra source/follow-up reviews approved the physical-gun adapter; the separate0.2.5-dev package records this checkpoint. Full immersive equivalence remains incomplete. No game, Windows host, Wine, OpenXR runtime or headset was executed. No installed game files/settings were modified.

## Real integration paths

The x86 proxy loads the system D3D9 implementation and intercepts CreateDevice, Reset and Present. It pins its owning DLL so detours remain mapped during graphics backend reloads. Native game hooks attach only after five exact SHA256 matches (four native dependencies/executable checks on the headless path). The x64 host owns the actual OpenXR session, predicted poses, action spaces and swapchains. IPC ABI 6 is fixed-width, pointer-free and identical on x86/x64 (83,887,512 bytes including two stereo slots and one classified mono menu buffer).

Native simulation calls identify the local puppet and inventory, maintain controller-origin/snap-turn calibration, and call the original hand-specific selection/dual-wield methods. A pending selection intent waits for native changeability rather than replacing native weapon policy. Weapon handles are resolved on the calling engine thread; player and base/sniper weapon deletion invalidate ownership. Aliased hands suppress VR firing rather than coupling triggers. Native fire queries for distinct current hand weapons receive independent states. Base/sniper shooting-placement and model-placement hooks use the same tracking transform and preserve native authored rotation; muzzle displacement is derived from camera-relative native flat-model/shot placement and bounded, so calibration does not retain an old absolute world position. The tracking body anchor bypasses native view/weapon animation but retains verified stance-height interpolation; the original animated camera is used only to derive authored model/shot offsets. Death and third-person rendering disable VR gameplay. This calculation still needs visual/gameplay verification.

The per-eye path invokes the native Render3D prepare/collect/execute/free transaction twice with one frozen game snapshot/body anchor and native reference camera and one immutable XR request. Its local weapon injection remains native. It substitutes the original WindowCanvas color/depth only; intermediate canvas targets remain native. A final desktop invocation restores native camera/render bookkeeping. An old D3D state block is deliberately not applied before that invocation, because it could desynchronize native shader/texture caches from the actual device.

Only full-size, non-MSAA 32-bit WindowCanvas views with the matching native depth format are classified. Each eye's final RT/depth identity must match its owned surfaces; failed binds and active non-null MRT slots reject the pair. Readback copies the native non-MSAA A8R8G8B8 or X8R8G8B8 color format to equal-format system memory, honoring row pitch and supplying opaque alpha. Native desktop color/depth aliases are redirected at bind/copy/readback/fill seams. Root-only view IDs separate per-eye native occlusion-query history; nested IDs remain native. The host accepts only complete same-request pairs and submits their actual eye poses/FOVs. Timeout/cancellation never frees a game-owned Rendering slot. A deferred completion must retire before another slot can be claimed. Input publication remains independent. Requests carry the native tracking epoch; early atomic invalidation and a snapshot-serialized Ready commit reject obsolete frames.

The host draws independent frozen controller-wheel quads with verified weapon names/ammo and a separate health/armor/weapon-ammo HUD. Explicit mono menu/loading frames come from the native backbuffer at Present and go to a screen quad, never a projection. Foreground-only menu input preserves keyboard navigation and dispatches controller ray motion/trigger through the native virtual640x480 mouse cursor. Pointer clicks carry their own input sample and exact displayed-frame identity. BGRA8 UNORM is preferred; RGBA8 UNORM is supported through an explicit red/blue conversion. No sRGB color conversion is assumed.

## Limits and open evidence

- Full child-command/postprocess classification is incomplete. The verified WindowCanvas binding and final-target checks support the narrow capture path; they do not prove every shadow/reflection/effect/HDR pass is correct. See RENDER_AUDIT.md. Only a complete, currently eligible pair can produce a world projection. An existing valid pair may be reused between captures.
- World-to-meter scale follows native camera/weapon transforms with scale 1; no headset calibration of physical size was performed. Horizontal head motion is bounded to0.75m, independently of vertical motion[-1.8,+1.0]m; both eyes and hands share the correction, preserving physical IPD and normal controller reach. Exceptional hand reach is bounded to1.3m from the physical head. These bounds are not wall collision or roomscale body physics.
- Experimental sampled head-lean fade is implemented and disabled by default (`HeadComfort.Enabled=0`). It uses native thick bullet-category queries, excludes the player/mechanism, and dims both world eye images from one exactly matched request/body/head result; host HUD/wheels remain visible. Missing/stale results do not dim. First-person/client query availability, moving-collider freshness, overlap and complete eye enclosure are unproved, so this is not continuous head protection. See HEAD_COMFORT_REVIEW_DISPOSITION.md.
- Continuous weapons share the verified native shooting-placement virtual family; this is static hook coverage, not proof of every weapon's firing path or accuracy. Native melee behavior is retained, not replaced with physical swings. Vehicle aim and immersive sniper scopes are outside this implementation.
- Native mission/conversation/boss/fade/zoom overlays are not all ported to headset layers. The small headset HUD exposes health, armor and current-hand ammo; the desktop retains native overlays.
- Stock first-person rendering and native multiplayer sessions are the current integration targets. Other custom native modules, cinematics, oversized drawports, MSAA and MRT rendering are unsupported. Multiplayer source is unverified in a session; teleport is excluded.
- CPU readback/upload may stall. The host no longer blocks XR frames waiting for game capture; it polls one pending transaction and may reuse a complete pair until the provisional 150 ms enqueue-age cutoff. Both image waits precede either write/release, and reacquiring a cached image hides that pair. Abandonment releases waited images without writes; a release failure leaves the pair invalid. Normal session stop retires existing acquisitions in all stereo/UI chains before EndSession, with bounded retries and host exit on failure. Logs distinguish accepted, submitted/reused, retired, expired, invalidated and image-wait counts. No performance or latency guarantee exists.
- Valve's wineopenxr bridge is a real Proton route, but headset runtime/container/prefix integration is not validated here. No runtime configuration was modified.

## Validation performed

- x86 proxy/adapter and x64 host/official loader compile and link using pinned MinHook/OpenXR dependencies.
- Offline checks cover independent release-gated triggers, tracking-loss/wheel cancellation, native forward-motion basis after snap turns, rigid transforms, stance-height sentinel rejection, body-motion-invariant muzzle offsets, head-bound IPD preservation, asymmetric projection and BGRA/RGBA channel conversion. Frame checks cover immutable pair identity, delayed Ready, cancellation ownership, epoch ordering/exhaustion, cached-index acquisitions and wait retries.
- Both cross-compilers emit identical IPC layout bytes; required decorated exports exist in the fingerprinted game. PE machine types and all proxy entry exports are checked. Static GCC/winpthreads linking eliminates additional MinGW runtime DLL dependencies.
- Terra reviewed native ABI/input/ownership; findings and dispositions are recorded in NATIVE_REVIEW.md. A second focused native canvas audit records binding facts in RENDER_AUDIT.md. Astra's xhigh design critique preceded implementation; focused anchor/transport amendments and final Terra integration reviews are recorded with dispositions. See COMPLETION_AUDIT.md.
- Package/installer checks operate on hashes and synthetic fixtures or a read-only preflight. They do not run the game. Artifact details: artifact-verification.json.

## Current multiplayer interval

Wire version4 uses native reliable full snapshots, consumption ACKs and bounded credit. The verified CSimulation/entity-manager boundary freezes every bound player before any weapon entity steps, and retires consumption after entities, scripts and physics. Original muzzle references are evaluated at native firing time; native inventory/ammo/cadence/damage remain authoritative. Remote tool subtrees are identified by native instance, filename ID and body ancestry, with affine deformation preserved and one frozen stereo snapshot. Head poses replicate as data; remote head-bone retargeting is implemented but opt-in/default-disabled pending complete native lifetime evidence. Relay presentation is lossy and stale data suppresses only affected hands. Final credit/tap corrections are integrated and checked offline.

## Native optic/head follow-up

Astra signed off on the shared muzzle dispatch: accepted VR snipers use the base native gun attachment once; rejected/nested contexts stay native and unretargeted. One validated local/authority context is retained through getter and retargeting, including post-getter handle/lifecycle guards. Explicit local laser snapshots use the same dispatch. Native zoom damage/callbacks and IPC6/wire4 are unchanged; helper tests do not establish runtime aiming equivalence.

The pinned owned-mesh audit proves a circular geometric cap candidate, not authored lens semantics. Astra selected native capture/textured apertures as the minimal next scope design, with explicit placement/cap, weapon-depth, color-stage and per-hand zoom-authority gates. Full scopes remain unimplemented. The remote head audit identifies native model-local skinning and instance/bone mapping, corrects file ordinal/runtime ID and bone-record indexing, and records retained-pointer lifetime plus conditional GPU/CPU consumption requirements. The subsequent0.2.4 checkpoint implements opt-in Head animation under the Astra-reviewed producer/lifetime guards; complete remote-head equivalence is still gated.

##0.2.4 head and remote-pair checkpoint

Added an explicitly opt-in/default-disabled native remote Head/owned-descendant renderer palette adapter. The producer refreshes temporary matrices before both CPU and GPU skinning; canonical animation matrices remain unchanged. Full XYZ/quaternion delta preserves native animation/stretch/shear. This is not anatomical IK or complete remote-avatar equivalence. Core main-thread/owner gates detect unsupported scheduling; unhooked native worker/model/deletion lifetime remains unproven, so shipped RemoteHeadTracking=0.

Remote hand presentation now shares immutable per-pair admission, local invalidation history, and binding/MP guards held through Ready. Astra caught and approved fixes for native-only fallback after remote threading failure, stale server revision composites, unrelated unskinned draw rejection and producer exception/reentrancy handling. Nine offline groups pass; x86 proxy/server and x64 host/loader build; IPC6/wire4 and four static internal entries are verified. No runtime/network session or installed-file mutation occurred. Other full-immersion requirements remain unfinished.


## Physical gun follow-up

Native owned XR gun rendering now uses the actual world execution raw projection, full view matrix and depth range, preserving the original by-value camera for authored model placement. This removes the stock gun-specific near/far and foreground depth compression from the admitted XR gun transaction. Successful tracked hand placement shares the same admission; lost/nonfinite placement suppresses draw. Exact caller gates and scoped nested suppression preserve native calls outside that transaction. Missing native restore is repaired through native callbacks/cache invalidation, and a sticky fault prevents stereo Ready. CPU cache and offline clip-depth checks do not prove actual GPU wall occlusion, near-face clipping or playability. Astra source/follow-up GO and packaging are recorded separately. Scope resource/section association and native overlay boundaries are statically traced, but immersive scopes and complete overlays remain unimplemented.

## Current0.2.6 checkpoint (supersedes historical status above)

Matching IPC7/wire6 host/game/server products include independent action activity/stream generations, fresh release/ACK admission, passive scope observation, corrected native alternative route, and Astra-approved mounted tracking/control integration. Seated full-6DOF rig uses native body/eye anchor, independent eyes and captured right-hand aim through original native controls. Native vehicle physics/firing/replication are preserved; remote riders share the native anchor. Wheels and handheld mutation/presentation are suppressed while mounted. Final cross-build, all11 offline groups and artifact/ABI verification pass;38 exported/5 internal seams. Source acceptance/details: MOUNTED_ADAPTER_SOURCE_GO.md.

Handheld lasers are implemented; actual native vehicle-muzzle lasers are unfinished. Roomscale body collision, native zoom/optic images, full native overlays, physical swing melee and default-enabled remote-head lifetime/support remain unfinished. All runtime/headset/network behavior remains unverified by user instruction. Earlier0.2.5 and other archives remain immutable.

## Current0.2.7 world-marker checkpoint

Native navigation beacons and world objective markers now draw inside each actual eye's completed Render3D/readback interval, using copied executed native view/full projection, original fade and original draw helpers. No retired-root activation, parent replay, listener mutation, new hook or protocol change. Native thread/frame/rider/world-info/drawport and actual surface/depth/viewport checks reject incomplete phases/pairs. Desktop/fallback remain native. Astra/xhigh bounded source GO, final full cross-build and all12 offline groups pass;38 exported/5 internal seams, IPC7/wire6 unchanged. See NATIVE_WORLD_OVERLAY_SOURCE_REVIEW.md and world-marker-source-checks.json. The new0.2.7 package includes this integration;0.2.6 predates it and remains immutable.

World markers do not implement native flat conversation/boss/death/score/fade panels. Investigation proves RenderOverlayFadings updates native timing and that transparent HUD capture needs a separately verified alpha path; per-eye native overlay replay is unsupported. Vehicle origin investigation is corrected: owned Cannon.mdl exists in All_PC_02.gro, but actual authored blast attachment/executor placement is not yet decoded/proven. These are evidence gates for remaining work, not implemented lasers/UI. Full immersive goal remains active and incomplete; no runtime launched.


## Current0.2.8 turret laser checkpoint

Implemented the Astra/xhigh-reviewed minimal native turret aiming laser in the existing query/sample/frozen-pair/draw path. The exact audited turret table2B39E8 supplies original selected attachment, explicit existence, original purpose1 world origin and reported shoot direction. Native main thread plus actual idle model scratch guards protect the destructive query boundary; active resource/config COW, model, rider/seat, action, attachment, mechanism and request changes reject stale sampling/publication/freezing. Collision excludes the actual vehicle mechanism; no vehicle sample reuse, gameplay/fire rewrite or protocol change. One right-side native mounted beam; both native fire commands stay original.

Astra design/source GO, full x86/x64 cross-build, all13 offline groups and refreshed artifact/compiled caller checks pass. Meaningful portable checks cover actual production pose/scratch/frozen-source helpers; they do not execute native callbacks, COW, missing-attachment lookup or native gameplay. Manual receiver/buffer provenance is recorded for actual compiled hashes. See NATIVE_VEHICLE_LASER_SOURCE_REVIEW.md and vehicle-laser-source-checks.json. New separate0.2.8 package; older archives immutable.

Full equivalence remains active: broader vehicle implementations, roomscale body collision, native zoom/optic images, complete flat overlays/full-field fades, physical swing melee and complete remote-head lifetime/enablement remain unfinished. Exact source acceptance does not prove query opportunity, moving freshness, appearance/performance or unknown unhooked-worker concurrency. No runtime/headset/network session or installed-game changes. Additional native UI inspection proves conversation Render mutates texts/time, and the current Ready commit precedes original flat overlay; any capture must preserve once-only native ownership and immutable correlated images. NATIVE_FLAT_UI_ONCE_OWNER_FOLLOWUP.md records evidence, not an implemented UI adapter.


## Accepted flat native UI geometry (2026-10-04)

Astra/xhigh gave bounded source GO for the flat panel transform and tests. The
adapter retains original source clipping via six transported halfspaces and
synthetic output clip-Z, while preserving flat eye X/Y/W. Added VR UI draws will
disable destination depth tests/writes for legibility; source fog or depth-dependent
shading is not admitted. Original program objects, UV/color, blending and native
callbacks remain the integration reference. Physical thickness, replacement
shaders and scratch masks were rejected.

Final production-helper checks pass 1658 boundary witnesses, all14 offline
groups and Windows x86/x64 compile-only checks. These verify geometry, not native
GPU draws, raster appearance, callback coverage or frame completion. The helper
is not yet connected; the next step is the once-only owner/draw/pair adapter,
actual compiled position semantics and pre-UI dimming/fade ordering. Full goal
remains active. No runtime or installed-game changes; immutable0.2.8 unchanged.
See NATIVE_FLAT_UI_CLIP_SOURCE_REVIEW.md and native-ui-clip-source-checks.json.


## Native UI cleanup and admission preparation (2026-10-04)

Astra/xhigh approved the bounded native-finally boundary and actual program,
adjusted-eye-projection and creation-thread/device gates. Main caught a mistaken
HLSL-route claim; the verified native assembly route let us delete a proposed
shader parser. Native SEH differs from MinGW DWARF2, so one small MSVC-target C
finally object isolates cleanup while retaining the surrounding toolchain. GNU
callbacks realign locally and contain direct errors to a failurebool; they never
rethrow across the foreign boundary. Native exceptions remain native. Future
resource ownership/reentry and reset-only failure policy still require integration.

Full cross-build/all14 offline groups and compiled ABI/artifact checks pass.
The actual final PE code, scope/funclet/IAT identity, four HIGHLOW relocations and
GNU callback alignment are checked; no exception, Windows code or game was run.
The installed ProtonHotfix32-bit API-set/schema/provider export chain was inspected
read-only. Runtime selection, unwinding, GPU appearance and performance remain
unverified. IPC7/wire6/version0.2.8 and immutable archives remain unchanged.

Complete UI is still not connected. Next extend the existing Rendering pair
through the once-only original brain/overlay owner, add closed-domain immediate
GPU duplication with explicit cleanup/state restoration, pre-UI alpha-preserving
dimming and frozen existing comfort geometry, then final presented-pair metadata.
Keep existing world-only behavior where the complete-UI owner is not admitted.
No new queue/renderer/protocol service. Full immersion goal remains active;
roomscale collision, scopes/native zoom, physical melee and remote-head lifetime/
enablement remain required. See NATIVE_FLAT_UI_NATIVE_BOUNDARY_REVIEW.md.


## Current0.2.9 connected native UI checkpoint

Astra/xhigh SOURCE GO closes the once-only native UI integration after four
corrected defects: raster point/line and point/wireframe-fill admission, retained
failed-unlock ownership/reset quarantine, and session loss during moved fallback
HUD upload. Original brain/overlay/fade callbacks run once, immediate admitted
filled triangle draws preserve native programs/resources/blend/alpha/order in
both existing eye RTs. Exact native fade calls cover whole eyes. Actual adjusted
P/source constants and viewport/scissor clipping supply flat physical panels;
source fog/stencil/userplanes and unsupported color output reject the pair.
Nonlockable/actual swapchain identity, creation/main thread, current hooked device
and raw-resource/frame generations gate compatibility. No new renderer, shader
parser, HUD state model, frame queue or XR layer.

Existing Rendering ownership now extends through original overlay AND enclosing
owner return; Ready publishes only both final images with matching immutable
geometry. Pre-UI world dimming preserves alpha via existing pitched readback/
UpdateSurface, then final export only makes alpha opaque. The existing35°/8°/
90°s comfort anchor updates once and freezes full-canvas panel size/aspect; stats
fallback is suppressed only for a final surviving completed UI pair, including
reuse and post-wait revalidation. Final loss/quit clears EVERY layer. Failed raw
restore/unlock halts adaptation until successful reset/new creation. Nativefinally
cleanup covers explicit resource and commit-lock ownership, not arbitrary crashes.

Full x86 proxy/server+x64host/official-loader cross-build/all14 portable groups,
refreshed41-export/5-internal-seam artifacts and native-finally ABI checks pass.
IPC8/wire6/version0.2.9 products must match; previous archives remain immutable.
NATIVE_FLAT_UI_CONNECTED_REVIEW.md and native-ui-connected-source-checks.json
record exact source/artifact evidence. Compiled/offline acceptance does not prove
native callbacks/GPU/foreign unwind/provider behavior, stock callback/device/thread
opportunity, appearance/deadlines/performance or runtime/headset compatibility.
No game, host, Wine, XR/server/network session, installed-game/config changes or
publication. Roomscale body collision, native zoom/optic images, physical swing
melee, broader vehicles and complete remote-head lifetime/enablement remain
required; full goal is active, teleport excluded.


## Current0.2.10 input and multiplayer checkpoint

Astra/xhigh SOURCE GO accepts native input interval/TLS retirement and exact
credit/discard ownership after three review fixes: allocating lifecycle lock
scope, transport handoff credit/ACK retention, and cached local neutral rearming.
The original simulation/entity/player callbacks and existing scheduler, token,
neutral gates and reliable protocol remain. Root interruption restores TLS,
invalidates existing tracking/intent epochs, retains actual inactive awaiting
slots for later normal discard, and excludes pre-interruption local samples by
sequence/time/session/reference/producer. Snapshot resets preserve that boundary.
No native send, game callback, allocation, new ACK queue or protocol on abort.

A related actual compiler warning exposed the relay Sample revision landing in
liveIntentEpoch[0], leaving presentationRevision0 and preventing admitted remote
VR presentation. Corrected exact aggregate positioning and made narrowing a
build error for both x86 products. Full remote-head lifetime/enablement is separate.

Final x86 proxy/server+x64 host/official-loader build, all14 portable groups,
artifact/native-finally ABI and pinned8-window static planning checks pass.
Direct scan completeness, indirect incoming paths and actual HDE continuation/
installed-hook behavior remain unknown. Exact evidence/dispositions:
NATIVE_INPUT_INTERVAL_REVIEW.md and native-input-source-checks.json. No game,
Windows program, host, Wine, XR/server/network session, installed-game/config/save
changes or publication. IPC8/wire6/version0.2.10; prior archives immutable.

This is a real input/replication source fix, not native zoom or optical image
integration. Native zoom producer remains zero. Next connect callback-owned
native zoom/predicates only after actual borrowed cleanup lifetime, operation
reentry and production-entry admission close; preserve native damage/timing/
scheduler and historical shot context. Full roomscale collision, optic images,
physical swing melee, broader vehicles and complete remote-head support remain
required. Full goal active; teleport excluded.


## Current0.2.11 native per-hand zoom checkpoint

Native zoom input is connected end to end: contextual per-hand OpenXR action,
later neutral admission, local snapshot and existing wire6 intent, actual
caller-selected sniper Step/Fire, original native held/Activate/Deactivate,
field-free conditional reads and original PutDown/Delete. Native flags, timers,
progress, damage, ammo, recoil and sound remain native. Existing active zoom
survives native cooldown4/recoil8; holster/bringup cannot activate. Right-only
selection leaves left intent intact. Exact right-delete cross-call preservation
uses current ephemeral owner/BC routing independently of older cleanup claims.

Astra/xhigh source GO follows six interaction fixes plus the reassociated-owner
routing correction. One bounded per-source provenance store remains outside
copied snapshots/authority, with cleanup-only lifetime and no live eviction.
Revoked effects cannot restart, but same-source native Stop remains available;
source deletion/reuse prevents stale outer accesses. External native activation
retains stock ownership and restoration, conservatively declining another
adapter takeover until source deletion. 128-record exhaustion declines new zoom.
No deferred pointers, raw native state writes, scheduler or protocol change.

Final x86 proxy/server and x64 host/official loader cross-build and all15 offline
groups pass. Actual linked entries/callback return ABI, native-finally compiled
shape,47 exported/11 internal seams and matching IPC8 layouts pass. Unchanged
pinned HDE32 host decoding accepts18 stolen+8 continuation instructions. These
checks do not execute native callbacks, MinHook installation, Windows or VR.
Exact review/evidence: SNIPER_BORROWED_ZOOM_SOURCE_REVIEW.md and
native-zoom-source-checks.json. No installed Bin/config/save changes.

This completes native zoom controls, not optical rendering. Native owner FOV
is shared; the surrounding XR world preserves runtime eye FOV. Magnified optic
images, roomscale body collision, physical swing melee, broader vehicles and
complete remote-head lifetime/enablement remain required. Teleport stays excluded.

## Scope source follow-up after0.2.11

Astra/xhigh SOURCE GO: existing native scope observer now uses one exact draw
selection to copy selected surface counts/channel descriptors alongside the
tracked full affine. A portable cap parser and private stock-asset verifier
preserve actual lens triangles and mirrored/stretch geometry. All16 offline
groups and full cross-build pass. This is diagnostic source/owned-byte evidence;
no live buffer reader or magnified lens renderer is enabled. `contentVerified`
remains false. The0.2.11 package is unchanged. See SCOPE_CAP_STAGE_REVIEW.md and
scope-geometry-source-checks.json. Full immersive equivalence remains unfinished.

The subsequent Astra/xhigh source follow-up admits actual live cap geometry at
the existing exact native DIP. It requires complete draw interception, current
physical raster/affine, exact static managed COM bindings and four owned-slice
hashes. Explicit native-finally lock ownership contains failed/indeterminate
unlock by stopping draws until process termination; managed-buffer uncertainty
is never reset-cleared. Recognized rejected Scope passes retire geometry for
that invocation; unrelated surfaces preserve it. `contentVerified` remains false.
All17 offline groups, full cross-build, owned cap, artifact and compiled ABI/
finally checks pass. This integrates geometry admission, not magnified images.
See SCOPE_GPU_GEOMETRY_REVIEW.md and scope-gpu-source-checks.json. No runtime
launched or installed files changed; immutable0.2.11 archive remains unchanged.
