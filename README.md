# Serious Sam 2 — OpenXR VR development mod

**PC development and runtime testing resumed under the user's 2026-10-07 authority.** The mod remains incomplete. Start with [current continuation](docs/CLOUD_CONTINUATION.md) and [implementation status](docs/IMPLEMENTATION_STATUS.md). The old handoff is historical; its pause/no-runtime restrictions are superseded. Native safety and privacy remain.

An original native adapter for the fingerprinted Steam Serious Sam 2 installation, with an x86 D3D9 proxy and an x64 OpenXR/D3D11 host. The interaction reference is Serious Sam VR: The First Encounter: tracked weapons in both hands, independent triggers, and a separate weapon wheel for each hand.

**Status: incomplete development build.** The private direct-scene harness now verifies Jungle scene bytes and captures sustained native world/UI stereo under simulated Monado/Proton. All seven controlled head poses produced request-correlated eye images and native camera response:0.1m translations and0.15rad yaw/pitch/roll. The native checkpoint passed82Debug and82assert-enabledRelease groups; subsequent offline collector changes passed their seven relevant consumer groups. Desktop terrain occlusion, hands/native dual wield, full immersive features, native Windows, multiplayer and real-headset acceptance remain open. See [rendering investigation](docs/PC_GAMEPLAY_READINESS.md), [Astra senior review](docs/RENDERING_SENIOR_REVIEW.md), [direct lab](docs/DIRECT_GAMEPLAY_LAB.md) and [required acceptance](docs/RUNTIME_ACCEPTANCE_MATRIX.md).

The current extension adds threshold-follow VR panels, native controller menu pointing, tracked native weapon placement, native-event haptics, collision lasers and multiplayer pose/weapon authority with remote weapon presentation and a dedicated server module. The **0.2.9** checkpoint connected native flat HUD/messages and full-field fades to both eye images through the original once-only native owner, with comfortable frozen panel geometry and conditional stats fallback. The current **0.2.11** source adds independent native sniper zoom controls and cleanup, including multiplayer shot context. It also retains the input/replication corrections from0.2.10. Astra approved these bounded source changes. Full immersive equivalence remains incomplete; teleport is excluded. See [UI source review](docs/NATIVE_FLAT_UI_CONNECTED_REVIEW.md), [input and multiplayer review](docs/NATIVE_INPUT_INTERVAL_REVIEW.md), [extension plan](docs/IMMERSIVE_PLAN.md) and [implementation status](docs/IMPLEMENTATION_STATUS.md).

Current source also contains default-off controlling-local single-player/multiplayer roomscale movement and
handheld/seated world head-volume protection. The latter uses post-simulation native queries,
rejects initial overlaps and constrains cached image reuse to the checked space
and age. Native mechanism exclusions can omit the ridden vehicle itself; this
is not cockpit-interior protection or a runtime-verified safety system.
Current IPC11 retains the earlier head-clearance receipt and requires matching game/host builds; do not mix
with older archives. See [head-volume integration](docs/HEAD_VOLUME_INTEGRATION.md)
and [MP origin/capture ownership](docs/MP_LOCAL_ORIGIN_CAPTURE.md).

## Implemented

Current cloud source now connects magnified per-hand scope images through native
pre-bloom scene capture and an ordered RGB-only lens pass. This supports a strict
stock-geometry, opaque PS2, direct non-MSAA UNORM-target subset; unsupported
render paths keep the native draw. Surrounding headset FOV and native zoom timing
stay unchanged. No game/headset runtime or visual/performance validation has been
performed. See [scope source integration](docs/SCOPE_SOURCE_INTEGRATION.md).
This does not update the historical development archive described below.

Current source uses IPC11 and multiplayer wire7. Matching host/game/server products preserve independent raw actions, acknowledged hand epochs and native shot/zoom context. Wire7 adds physical gesture fields separately from manual firing, release and zoom. The default-off unique-saw adapter now connects local, authoritative and remote-observer native consumers; short remote gestures are retained per hand until native consumption or explicit discard. Original ClientAction transport, manual history and native combat remain unchanged. See [melee source review](docs/PHYSICAL_MELEE_CONSUMPTION.md).

Earlier0.2.11 archives precede the current magnified scope integration above. Per-hand native zoom preserves original activation, timing, damage and cleanup; surrounding XR FOV stays unchanged. See [zoom source acceptance](docs/SNIPER_BORROWED_ZOOM_SOURCE_REVIEW.md) and [current scope integration](docs/SCOPE_SOURCE_INTEGRATION.md). Historical scope-stage/GPU-geometry documents describe their narrower original snapshots.

- OpenXR six-DOF HMD and hand poses: lateral/vertical/forward translation plus pitch/yaw/roll, separate eye views and asymmetric projections. Deep crouches retain physical height; eyes and hands share one tracking correction. Physical translation is bounded; development-gated native body settlement supports the controlling local actor in SP/MP. Actual collision/convergence remains user acceptance.
- Physical native gun projection/view/depth matching for admitted XR draws, with failed placement/state restoration rejecting the stereo pair. Native gun models retain their authored animation/content, including hands/arms where the actual assembly provides them; a universal hand model, articulated fingers and full-body IK are not added.
- Mounted six-DOF head/hand rig follows the native seat body pose and eye height. Right-hand aim feeds the original vehicle look clamp/control path; left-stick controls and both fire commands remain native. Handheld wheels/equip/muzzle adaptation are suppressed while mounted; mount/seat changes reset the existing rig. Replicated riders use the same anchor and native vehicle RPCs. See [mounted source acceptance](docs/MOUNTED_ADAPTER_SOURCE_GO.md).
- Collision lasers start at the original native gun muzzle. Mounted turret beams use the native authored attachment/world placement and reported aim direction, exclude the actual vehicle mechanism and end at the native collision hit. Exact audited turret implementation only; final projectile scatter is not predicted. See [Astra source acceptance](docs/NATIVE_VEHICLE_LASER_SOURCE_REVIEW.md).
- Native dual wielding, independent per-weapon fire routing (including identical weapon types), hand-specific shooting/model placement, and native ammo/cooldown/damage behavior.
- Two grip-held wheels, stock weapon names, ammo counts and hover highlights. Release commits a selection; returning the stick to the center cancels. The native inventory validates selections, with a pending hand-specific intent during weapon cooldown.
- Left-stick movement, 30° right-stick snap turns, use/jump/sprint and recenter. Wheel opening, tracking/focus loss and weapon changes suppress fire until a trigger release.
- Native flat HUD, messages, player names, scores/death and other original overlay content render once on desktop, then admitted immediate draws appear on the comfortable panel in both eyes. Exact native fades cover each whole eye. The panel starts following after a 35° head turn, stops within 8°, and moves at at most 90°/s. Default full canvas is 2.2m wide at 2.2m distance; menu settings control its physical size. Native timing, localization and blend order remain original. Health/armor/per-hand ammo stats remain a fallback when the actually submitted pair lacks completed native UI. Unsupported output rejects the pair; point/line/wireframe/custom-program UI paths and device/thread compatibility remain explicit limits. See [source acceptance](docs/NATIVE_FLAT_UI_CONNECTED_REVIEW.md).
- A separately classified flat screen carries native menus/loading; left stick navigates, Use or a presented controller ray and trigger click native menu items; Use confirms, Menu goes back/pauses. Menu keys are sent only while the game is the foreground window.
- Native navigational beacons and world objective markers use each eye's actual view and full asymmetric projection, with the original native fade/content/draw helpers. Failed target or lifecycle checks reject the whole pair. See [Astra source acceptance](docs/NATIVE_WORLD_OVERLAY_SOURCE_REVIEW.md). Flat panels use the separately admitted once-only UI path above.
- Touch, Index, Vive and Microsoft motion-controller action profiles; exact mappings are in [host details](docs/HOST_STATUS.md).

Experimental head-volume protection can be enabled with `[HeadComfort] Enabled=1` in `Bin/SS2VR/SS2VR.ini`. Current source waits for a matching query attempt and makes obstructed or unavailable owned attempts opaque in both world views while preserving UI. Clear cached images must remain inside their admitted volume and lifetime. It remains disabled by default and runtime-unverified. See [current head-volume integration](docs/HEAD_VOLUME_INTEGRATION.md); the [earlier head comfort review](docs/HEAD_COMFORT_REVIEW_DISPOSITION.md) describes the superseded opportunistic ray implementation.

Native remote head bones are enabled by default inside admitted frozen VR stereo pairs. The adapter retains native animation and applies full XYZ/quaternion tracking about the body eye. Ordinary desktop observers retain native heads. Set `[Multiplayer] RemoteHeadTracking=0` to disable the adapter; actual multiplayer appearance and lifecycle still need your in-VR acceptance. This does not affect local 6DOF tracking. See [current ownership and coverage](docs/REMOTE_HEAD_RESOURCE_OWNERSHIP.md).

## Requirements and installation

Use the stock game's first-person view and the exact binaries listed in [installed-build.json](docs/installed-build.json). Other native builds are rejected. Windows 10/11 x64 needs an active OpenXR HMD runtime offering D3D11 and BGRA8 or RGBA8 UNORM, with at least four composition layers. No proprietary game files are included.

Desktop participants in mixed multiplayer do not need a headset. Every participant
and the server need matching mod products. A mod handshake does not take over
desktop controls; accepted tracked poses establish VR gameplay ownership. Mixed
gameplay acceptance remains open; admitted mono remote-head rendering is connected
in source. See
[mixed desktop/VR status](docs/MIXED_DESKTOP_VR.md).

Set **USE COMBO WEAPONS → YES** in the stock single-player or cooperative settings. Independent identical weapons require the game's combo capability and valid native dual state; its fallback mode couples the guns. Patch resources permit custom combinations, but archive filenames alone do not prove the loaded setting. See [native combo evidence](docs/NATIVE_COMBO_CAPABILITY_AUDIT.md).

Set a desktop resolution with **both dimensions at most 2048** and disable MSAA. Use a 32-bit color mode. A modest window resolution such as 1280×720 reduces CPU transfer cost; the native drawport size currently determines each eye's texture size. The allocator preserves the native depth format and uses separate non-MSAA eye targets. CPU readback can stall the game; performance has not been measured.

The historical extension archive is `dist/ss2vr-0.2.11-dev.zip`; it predates current IPC10 source and remains unchanged. Current packaging derives a `-dev.<source-fingerprint>` name and refuses stale or mixed game/server/host builds. No current finished-device-test package is available yet. For a matching development package, extract it and use Python 3:

```sh
python install.py install --game "/path/to/Serious Sam 2" --dry-run
python install.py install --game "/path/to/Serious Sam 2"
```

From this repository, pass the fresh directory printed by `tools/package.py` as the installer’s `--package` argument. The installer checks all five game fingerprints and every package hash, refuses collisions with `Bin/d3d9.dll`, `Bin/SS2VR`, `Bin/SS2VRServer.dll` or `Content/SS2VR.mod`, and writes a receipt. It never overwrites another mod. Removal checks that all payload files still match:

```sh
python install.py uninstall --game "/path/to/Serious Sam 2"
```

Only mod-owned files are removed. Added logs remain. This task created a separate package and did not deploy into the installed game's Bin folder or alter its settings.

Both Windows and Linux through Proton are required targets. See [platform setup and verification limits](docs/PLATFORM_SUPPORT.md).

## Linux / Proton

The products are Windows PE binaries and the host must inherit the game's Proton prefix. The game starts its host through CreateProcess; do not run it in an unrelated Wine prefix. Steam launch options need the proxy override:

```sh
WINEDLLOVERRIDES="d3d9=n,b" PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 %command%
```

This is an installation recipe, not verified Linux support. Valve supplies a [wineopenxr D3D11 bridge](https://github.com/ValveSoftware/Proton/blob/proton_11.0/wineopenxr/openxr_loader.c), and the installed Proton Hotfix contains its x64 manifest. A native OpenXR runtime and its socket/manifest must be accessible inside Steam's container; the Windows loader also needs the prefix's registered wineopenxr runtime. Start the chosen runtime before the game. No prefix or headset-runtime configuration was changed here. `VR_OVERRIDE`/xrizer alone configures OpenVR applications; this mod calls OpenXR directly.

## Controls

The comprehensive hardware acceptance procedure is being prepared in
[USER_VR_TEST.md](docs/USER_VR_TEST.md). Several immersive features remain unfinished;
the checklist is not a readiness claim. Individual weapon-firing tests are now
reserved for the user in VR while development continues on the remaining scope.

| Action | Input |
|---|---|
| Fire | Trigger on that weapon's hand |
| Sniper zoom | Hold that hand’s stick/trackpad click; Vive right uses its contextual Jump action. Release after equip, wheel or tracking loss before pressing again |
| Select weapon | Hold that hand's grip, point its stick/trackpad at a sector, release grip |
| Cancel wheel | Center the stick before releasing grip |
| Move | Left stick/trackpad, relative to the calibrated heading plus snap turn |
| Turn | Right stick/trackpad left/right, 30° per deflection |
| Steer (vehicles without handles) | Squeeze a grip on the virtual wheel rim and turn it; release to recenter |
| Recenter | Hold both grips and click the left stick/trackpad; release triggers before firing again |
| Use / jump / menu / sprint | Profile-specific buttons listed in HOST_STATUS.md |

Weapons retain native gameplay. A default-off unique-saw gesture adapter is source-reviewed for SP and matching modded MP local/authority/observer roles. After equip/tracking changes, fresh quiet motion arms the first swing. It issues a real native press without guessing initial low; actual stops require later quiet before rearming. Actual contact, cadence and multiplayer behavior need user acceptance. See [melee consumption](docs/PHYSICAL_MELEE_CONSUMPTION.md). Fighter/Saucer one-/two-hand handle grabbing uses submitted native geometry and the native mode2 heading callback. Other driven vehicles get a virtual steering wheel (no stock vehicle models one): squeeze the rim with either or both hands and turn it; cars take it as steering and hover/air vehicles as a turn rate. The wheel mesh is generated with Blender (`tools/blender/steering_wheel.py`) and textured from the stock HoverFighter texture loaded from your game files at runtime. Physical handling, roomscale and remote-head source integration require user-operated acceptance. Immersive scope source code now supports the bounded subset documented above; broader compatibility and runtime behavior remain unverified. Mounted tracking/control integration is implemented; vehicle runtime behavior remains unverified. Multiplayer replication is implemented in source and has not been run in a network session. Teleport is excluded. Native flat overlays now have a source-reviewed headset path; stock callback coverage, raster appearance and performance remain unverified. Netricsa/cinematics and unsupported output retain their explicit compatibility limits. Cinematic views and complex postprocessing are not verified. The host may reuse its last complete stereo pair with the original poses/FOV while its epoch, focus, tracking and age remain valid. Without an eligible complete pair it submits no world projection; no two-copy mono stereo fallback exists. The provisional age cutoff is 150 ms from request enqueue, not a latency guarantee.

## Build and repository

Linux tools: CMake ≥3.24, Git, Python 3, native C++ compiler, Clang with the i686 Windows MSVC target, and i686/x86_64 GNU MinGW-w64 C/C++ cross-compilers/binutils. The game compiler must provide native Windows TLS (for example GCC 16 configured with `--enable-tls`, plus binutils ≥2.44); CMake checks an actual compiled TLS probe. Clang is used only for the small native-finally object, not as a replacement game compiler. The small native-finally C object isolates native SEH cleanup while the surrounding C++ remains on MinGW. CMake fetches pinned MinHook/OpenXR revisions onto the project drive. `python tools/build.py` builds both products and runs only native offline checks. Optional `--minhook-source` and `--openxr-source` accept matching local checkouts. `python tools/package.py` verifies exported compiled source/version/layout contracts before staging and derives the manifest ABI from them. Its default output includes the build-input fingerprint; existing outputs are refused. Choose `--output` for another fresh directory. Install the Python dependencies in `tools/requirements.txt` before verification/packaging. See [package identity checks](docs/PACKAGE_IDENTITY.md).

`python tools/verify_zoom_entry_abi.py` inspects actual linked native zoom entries and callback return conventions; `verify_sniper_predicate_sites.py --game ..` checks pinned native boundaries and HDE decoder acceptance. Neither executes Windows or game code. `python tools/verify_weapon_abi.py` inspects the compiled MinGW x86 gun-hook calling conventions and unchanged camera forwarding. `python tools/verify_world_marker_abi.py` checks native marker caller shape, supplemented by the manual provenance audit for accepted artifact hashes. `python tools/verify_artifacts.py --game ..` checks final PE architectures, exports/imports, both IPC layouts and installed hook symbols. It needs pefile (`python -m pip install -r tools/requirements.txt`). No verification tool launches the game, host or headset runtime.

[AGENTS.md](AGENTS.md) records task constraints and engineering rules. [Research](docs/RESEARCH.md), the [Astra review](docs/ASTRA_REVIEW.md), [disposition](docs/REVIEW_DISPOSITION.md) and [final plan](docs/FINAL_PLAN.md) record the design. [Completion audit](docs/COMPLETION_AUDIT.md) and [MODLOG.md](MODLOG.md) record implementation and offline evidence. Original source is MIT; dependency notices and AI contribution disclosure are in [THIRD_PARTY.md](THIRD_PARTY.md).

## Current multiplayer source

Current source uses the source-reviewed native firing operator and held-input
path, preserving mapping, blocking and history. Desktop startup and carried
objects retain native commands. This follow-up is newer than the immutable
0.2.11 archive. See [Astra source acceptance](docs/PRIMARY_JOIN_SOURCE_REVIEW.md).

The game proxy supports listen-host/client VR through native RPCs. Dedicated authority loads `SS2VRServer.dll` through `Content/SS2VR.mod`; a build containing those files is required. Run the native server with `+mod SS2VR` (without `.mod`) plus its usual `+level`, `+port` and session options. Every VR client and the authority need matching mod products. Gameplay damage/ammunition/cadence stay native. Unknown or stock authority does not negotiate VR weapon input. Authoritative aim updates are limited by20Hz and the reliable round trip; remote presentation expires after200ms. No dedicated server or network session was launched here.
