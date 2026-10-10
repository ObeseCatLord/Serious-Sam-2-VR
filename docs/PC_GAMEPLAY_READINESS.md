# Native gameplay VR investigation — 2026-10-07

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

This record is active work, not completed VR acceptance. Genuine gameplay requires
separate native world images for both eyes, tracked head translation and rotation
in the native camera, correct asymmetric FOV and coherent hands/origin/body collision.
A menu quad, duplicated desktop gameplay, a synthetic image or successful xrEndFrame
alone does not meet that requirement. Teleport remains excluded; multiplayer remains
required. Preserve native D3D9 CPU readback into the separate D3D11 OpenXR host.

## Current post-repair native result

Run190429 on compiled source725425de9c8dd4315399e65bf7c28c353bc1390e176747935b4477a4f2d0eb48
captured all seven actual native pose pairs and500 neutral complete native requests.
Both-eye cameras, complete world/UI and successful projection are correlated to
those exact image requests. Translation measures0.1m in each axis; yaw/pitch/roll
measure0.15rad. Static foreground rock/middle road/distant arch patches show
horizontal eye displacements22/7/1pixels under the matching symmetric eye FOVs,
consistent with depth-dependent parallax. Actual images were inspected; the
corrected right-eye terrain/waterfall occlusion remains coherent. Native shutdown
was observed, with no cleanup errors. Desktop captures failed and remain unaccepted.
This simulated Proton/fsync result is proper native world stereo/6DoF evidence;
body/origin collision, physical hand/model alignment, full features, real devices,
Windows and multiplayer acceptance remain open. Historical records below retain
the failures that led to this result.

## Observed source issue and incremental fix

The admitted native additional swapchain replaces the unsuitable implicit16x16
buffer at the existing capture boundary. Menu color readback is independent of
stereo depth allocation. A new readiness cycle was found during integration:
tracking update requires rendererReady, but stereo allocation was gated by a
menu classification that depended on tracking gameplay readiness. Stereo capacity
is now attempted independently. menuVisible uses positive native menu presence;
inactive/stale tracking is no longer a reason to project desktop gameplay as a menu.
This is a confirmed draft-source bug, not a proven cause of the user's screenshot,
whose binary/run provenance has not been established.

The existing native Render3D eye loop, eye camera/FOV hooks and two-view OpenXR
projection layer remain in place. Source presence is not runtime proof. All three
backbuffer consumers use the admitted chain, with explicit native-finally owners
and reset generation retirement. Scope/reset/nested lifecycle recovery remains
unaccepted; do not generalize the ordinary pistol-test review to those routes.

## Actual observations

The actual proxy launched the actual x64 host under Proton Hotfix
hotfix-20260828-ptr-x86_64 against isolated simulated Monado25.1.0
v25.1.0-710-g735e29e4e. NVIDIA RTX4090, D3D11 via DXVK3.1, BGRA format87,
three swapchain images, FOCUSED and WMR interaction profiles were observed.
A prior native menu transport segment copied the actual1280x720 game menu through
D3D9 CPU readback; private CPU and exact game images were visually inspected.
That segment recorded4808 successful native menu-quad frames. It is menu transport
acceptance only, with no actual-device claim.

A later draft package (archive SHA256
92c9a6298e6f6b57289f9daa460c3b3fa7b20a42ac9142848cdf8f6c38483bff;
compiled source fingerprint
9a641a0455fb5153e7168e1b1701ab8158f065f2b3c41e88d7ee40b107defcd8)
initialized native stereo targets and both host eye swapchains1280x720. Exact-window
capture showed real first-person outdoor pistol gameplay. Native menu presence
cleared and gameplay readiness became1. However the host recorded zero completed
world pairs, with requests filling/retiring. A15-second read-only Ready-slot capture
found no pair. This is an unresolved gameplay VR result, not a stereo pass.

The exact game process was stopped at the boundary; the host subsequently exited.
Private logs and images were preserved. Bounded source diagnostics now distinguish
Render3D admission, request acquisition, target validation and completion. All60
Debug build/check groups passed both source snapshots. No equivalent current-source
Release, lifecycle fault, network or real-device acceptance is claimed.

Capture by compositor active-window selection proved unreliable on this desktop;
subsequent captures target the positively owned SS2 X window ID. Synthetic controller
rays can override desktop menu cursor navigation; bounded events directed only at
the owned Monado debug window changed the actual sampled controller pose. Those are
simulation input observations, not hardware controls or head/camera acceptance.

## Astra review disposition

An Astra Max attempt returned a capacity error and no review. The retry explicitly
selected gpt-6-astra/xhigh and inspected the fixed source diff. Configured current-turn
metadata reports those settings; independent effective-backend introspection was
unavailable. Its GO covers only ordinary single-player pistol world observation.

| Recommendation | Disposition |
| --- | --- |
| Keep narrow actual-chain owner and CPU transport | Adopted; existing render/IPC architecture retained. |
| Break stereo readiness/classification cycle | Adopted; capability allocation precedes tracking admission. |
| Observe existing per-eye camera/FOV/native-render route | Adopted; no render rewrite or flat fallback. |
| Require visible near/far parallax and translation/rotation response | Adopted; still unaccepted. |
| Exclude scopes, reentrant Reset and nested recovery from this test GO | Adopted; separate cleanup/runtime gates remain open. |

Next: resolve the exact blocking native render gate, then inspect actual left/right
world images and simulated lateral/vertical/forward head translation plus yaw,
pitch and roll. Keep hardware SteamVR/Steam Frame and Monado/Beyond acceptance,
Windows-native and Proton, multiplayer, melee and vehicles separate and unfinished.

## Direct-scene probe outcome and active gates

The no-menu +level route reached Jungle loading through the actual native stream.
The successful restored-position receipt was 10,562,049 bytes, SHA256
106f9e287110988720cc717aaaa22190cafcf90fb184fd5a83e9d467d93165df.
A fresh private prefix fixed the failed native display enumeration without modifying
the old prefix or global monitor settings. Private OpenVR bootstrap configuration
was necessary for Proton to process the existing xrizer override before wineopenxr.
The actual host reached FOCUSED, format87, both1280x720 swapchains and remote
simulated Index profiles; these differ from the earlier WMR simulation.

A later run reached local Jungle simulation with online interface0 and observed
the native null-interface shutdown wrapper. Thirty advancing actual input snapshots
matched the commanded neutral head pose. The first eye camera hook executed, but
world completion was0 despite admitted targets and UI ownership. No Ready world
pair was captured and xr_world remained0. The new bounded eye-stage diagnostics
will distinguish projection admission, target restoration and postlude failures.
A native press-key loading screen was observed; automatic dismissal remains under
investigation, so this is not yet a fully repeatable direct-gameplay acceptance.

The user reports trees and a waterfall visible through terrain in desktop output.
That output has not been established as stock, and VR remains affected until tested.
Exact location is unspecified. Compare an identical scene/camera in stock and
modded desktop and each eye; inspect depth size/format, clear/test/write state,
opaque versus alpha-tested/transparent ordering and restoration between passes.
Keep terrain, vegetation and waterfall enabled. The regression remains open.

The subsequent root-view trace found exact first failure: prepared depth0..1,
executed depth0..0.899999976, with callback, view, projection and near endpoint
checks passing. The current strict far-endpoint equality rejects the native
executed segment. Native partition evidence and the narrow correction are under
Astra review; no predicate has been relaxed. A modded desktop image was captured
automatically at the neutral baseline. No matching stock-renderer or accepted
eye occlusion comparison is available yet.

That diagnostic package archive SHA256 is
9748e1bba2677d3d46daa6396eab3ec149680a9e42f8b84d19605b22d82b0a94;
compiled source fingerprint
1ed2a5d581e2c19f2c1af0612b878acee1a3e71114669551629ed4718c1734c5.
All60 current offline Debug groups passed. Source review was bounded normal-path
Astra/xhigh; serving-backend/effective-setting introspection remains unavailable.
No full exception, reset, scope, hardware or network GO is claimed.

Loading-gate clarification: later record inspection found no
loading_continue_messages_posted flag and no positive loading_ready observation
in runs143525/144056. Gameplay was reached without main-agent menu navigation,
but automatic native Enter consumption was not verified. The dismissal source
remains unknown; the native reader's predicate is being diagnosed. Do not repeat
the earlier claim that a harness-posted Enter was observed.

## Exact native partition and repeatable loading results

Run145306 verified guarded loading continuation using the same-source x86 GUI
observer: exact CMSLoading table29F148, ready1, Enter down/up posted, then actual
Jungle simulation. The earlier x64 module snapshot failed with
ERROR_PARTIAL_COPY299; a zero reader result was not evidence of a nonready menu.
Runs143525/144056 still have unknown dismissal provenance.

Native background collection writes the parent root depth range0..float32
0x3F666666 (Engine15EF50/15EF60); its child uses that boundary..1. The local
correction admits only this bit-exact executed root partition after the existing
root/caller/eye ownership checks. Generic executed-view equality remains strict.
Adjacent floats, other ranges, wrong eyes and NaN are rejected by checks.
All60 current Debug groups passed; no equivalent current Release or fault-lifecycle
acceptance is claimed. The bounded Astra/xhigh review admitted an ordinary probe.

Run145753 completed both native world eyes and accepted one fresh pair: left
FNV2adac751f9aa9098, rightb37a135fb1fc7c57, with1,596,793 differing RGBA bytes.
The host recorded six successful world submissions, comprising one fresh pair and
five reuses. This is distinct-image transport evidence, not visible parallax or
six independent native renders. No eye images were captured by the strict
same-pose readiness gate, and no translated/rotated camera acceptance follows.

Run150535 reproduced the remaining UI rejection after successful world rendering.
The native overlay owner matched Sam2Game+EB85D. The first fault mapped to a shared
indexed-draw callsite, which cannot distinguish desktop draw failure, captured
state rejection or unsupported topology/fill. Bounded scalar reason labels now
preserve the first rejection and log only at normal owner finish. Original draw
count, query order, offscreen behavior and failure gates remain unchanged. The
repeatable run observed native scene identity and null-interface shutdown, with
no cleanup errors. Sustained stereo, all six pose axes and the terrain-occlusion
comparison remain open.

## Actual sustained native world/UI and seven-pose result

Run154502 completed verified native Jungle loading and guarded continuation.
The narrow TEXCOORD0 contract correction admits the actual native builtin position
stream; original desktop draw and two eye replays complete without the previous
UI fault. The independent Astra/xhigh native check confirmed the common backend
mapping for all six builtin variants against exact Engine/GfxD3D fingerprints.

The harness observed300 distinct neutral complete world/UI pairs before capture.
The final exact-request assessment found341 neutral pairs with both native
cameras and matching successful projection submission; the host total2214
world submissions includes reuse and is not a fresh-pair count. All seven pose
image pairs were captured with exact request/session/reference/tracking identity,
complete UI, successful matching projection and approximately64mm eye separation.
Private contact-sheet inspection shows native game-world imagery for both eyes
and changes corresponding to translation and yaw/pitch/roll. No flat gameplay
quad was accepted.

Measured native camera-center translation for commanded0.1m local axes:
X=(-0.025039,0,-0.096810)m, Y=(0,0.100000,0)m,
Z=(0.096817,0,-0.025040)m. Rotation for each commanded0.15rad yaw/pitch/roll
was approximately8.594degrees. Centers stay fixed during these rotation probes.
The original world orientation explains the rotated X/Z axes. This is actual
simulated native game-camera response, not a fixture-only submission.

Exact source fingerprint:
765bd1bc0379da095f814cc790f46e9935c0ff38486391ce5a635c541ed7b7c4.
Private package archive SHA256:
ee53e34e5081d525277fa27b2556aecc532e2bf6453ee84114bf66abbe29f068.
Public source base was b654b74a with recorded local diff. All61 Debug groups
passed. Native scene SHA remains106f9e287110988720cc717aaaa22190cafcf90fb184fd5a83e9d467d93165df.
Normal null-interface shutdown was observed and cleanup errors were empty.

This establishes the bounded ordinary single-player simulated rendering path.
Image quality and terrain/vegetation occlusion need the stock-renderer reference.
Physical hands/weapons and independently fired native dual wield, scopes,
roomscale/body collision, lifecycle failures, multiplayer, Windows-native and
actual Frame/Beyond hardware are not accepted by this result. Full-mod acceptance
remains false. The preceding attempt154001 stopped before scene/host readiness
after an owned-focus-helper failure; its missing scene/shutdown evidence is not
a post-fix UI or stereo failure. Future helper failures preserve private stderr.
