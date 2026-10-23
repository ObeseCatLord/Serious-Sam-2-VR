# Serious Sam 2 OpenXR VR — active instructions

Always refer to the user as **ObeseCatLord** (see [CLAUDE.md](CLAUDE.md)).

## Resumed — 2026-10-10

The user resumed development after the pause recorded in
[the takeover handoff](docs/TAKEOVER_HANDOFF_2026_10_10.md) ("continue working on
the VR mod until it's done"). The black-world and startup-stall blockers are
resolved; see the current [status](docs/IMPLEMENTATION_STATUS.md).

## Authority and scope

The user authorizes PC development, tool setup, additional sessions and online
multiplayer development. Multiplayer with other mod users is required. Preserve
Windows and Linux/Proton targets, SteamVR + Steam Frame and Envision/Monado +
Bigscreen Beyond. Beyond controller profile depends on the actual controllers.
Teleport is excluded.

Non-scoped weapons require proper per-hand laser aiming. Mixed desktop/VR
multiplayer is required when every participating player has the mod installed.
Desktop players must retain native mouse/keyboard input and rendering without
requiring a headset; VR pose admission must not suppress their native actions.

Complete native stereo world rendering, tracked head translation/rotation and
independent hands; native dual wield, per-hand weapon wheels/lasers/scopes;
comfortable threshold-follow menus/HUD; coherent roomscale/body collision;
vehicles, physical melee and multiplayer body/weapon/head presentation. Do not
substitute narrower scope or call inactive helpers completed features.

Current user authority supersedes historical root-only/no-runtime/pause and
publication-hold instructions. The user explicitly approved public source and
sanitized reverse-engineering notes for ObeseCatLord/ss2-vr. Binary/release
publication is outside that source-only approval. Historical instructions are
preserved byte-for-byte in [the archive](docs/AGENTS_HISTORY_2026_10_09.md);
use this active file and current user messages for authority.

## Latest attachment scope — 2026-10-10

The user requires practical controller gun attachment and laser aiming. Detailed
winning-animation/cache attribution and certified anatomical grasp references are
optional research, not ordinary attachment prerequisites. This supersedes stricter
historical alignment gates below. Reuse calibratedGrip/native placement/muzzle
transport; preserve native lifetime, melee release and multiplayer ownership checks.
Do not describe practical calibration as a certified anatomical measurement.

## Workspace and ownership

Use this isolated sanitized development checkout. Preserve the old installed-game
checkout, old ancestry, rejected melee patch, user changes, installation,
settings/profiles/saves and unique evidence from attempted private fixtures. Never merge/reset old
email-bearing history into this checkout. Cloud source edits stopped to avoid
competing writers. One original integration/runtime/publication owner remains.

The four reviewed parallel commits are already integrated; see
[PARALLEL_INTEGRATION](docs/PARALLEL_INTEGRATION.md). The untracked transfer handoff
is preserved coordination evidence, not a patch to apply again. Do not stage,
edit or delete it as routine source work.

Before substantive changes read current [status](docs/IMPLEMENTATION_STATUS.md),
[continuation](docs/CLOUD_CONTINUATION.md), relevant exact gate documents,
FINAL_PLAN.md, RESEARCH.md and MODLOG.md. Historical HANDOFF.md is not a pause
instruction. Current private handoff/actual worktree take precedence over stale
status entries. Preserve historical evidence when correcting it.

## Architecture and native boundaries

Preserve Serious Engine 2 rendering, simulation, inventory/ammo/cooldowns,
damage/projectiles, saves, AI and native physics. Keep classic D3D9 CPU readback
into the separate x64 D3D11 OpenXR host. No shared GPU-handle transport rewrite.
Prefer the smallest adapter at a demonstrated incompatibility; compare it with
any proposed parallel implementation before changing architecture.

No invented native fields/functions/offsets, fake stereo, headset-as-mouse aim,
shared weapon aim or duplicate gameplay state machines. Verify native names,
ABI/addresses and module fingerprints from actual source/owned binaries. Unknown
means unknown. Reopen designs when evidence contradicts them or new layers
multiply state/ownership policy. Rendering must not advance simulation; gameplay
mutation stays on the simulation thread.

Never activate handoff/unaccepted-melee.patch: it can throw carried objects.
Native160/348 remain manual-only. Read current melee/native gates rather than
priming Low, release or native history. Native packet counters, queue growth and
correction return alone do not prove actor-property application/ownership.

Physical driver controls require positively established model/seat/control
geometry. A road-wheel joint is not a driver's wheel. Keep exact inspected and
uninspected coverage; no guessed offsets or car wheels on unrelated mounts.
One-/two-hand grabbing must permit smooth transfer. Preserve joystick fallback,
native throttle/aim/fire/physics and original ClientAction transport. Squeeze
availability loss requires explicit release/rearm; inactive is not released.

## Current runtime authorization

The user will primarily test shooting, vehicles, hardware and multiplayer. Do not
run individual weapon-firing probes. Finish source implementation and provide a
comprehensive runnable user test procedure when ready.

The user explicitly allows necessary PRIVATE NEUTRAL captures for the
startup/idle-geometry blocker, up to six minutes per fresh sealed fixture.
No desktop input/focus, movement, firing, switching, zoom, vehicle, headset or
network probes in that exception. Startup may restore/focus only the exact owned
private SS2 window and send one guarded loading Enter down/up pair. Existing
repeated private display/prerequisite checks are also authorized. Do not ask
again for the same scope or revive exhausted historical one-capture limits.

The user subsequently stated: "I allow all captures you could need." This
supersedes the pending Auto Shotgun capture question and extends necessary
private neutral weapon-reference captures to other weapons, including verified
single-player grant/equip/save preparation. The earlier sniper-specific authority
is historical, not a current ID restriction. Verify each native route before use,
preserve normal profiles/saves, and retain the private-window/no-desktop-interference
and no-firing/movement limits. It does not authorize cheats in online sessions
or replace user-operated firing, vehicle, headset or multiplayer acceptance.

An explicitly selected failed-startup diagnostic may collect a normal minidump
from the separate owned observer, retaining exact executable/creation identity.
Use the system provider only for that command; no live debugger attachment,
full-memory dump, native gameplay call, security or global runtime change.
Keep all dumps/stacks private. Provider success and process liveness do not prove
usable stacks, coherent state or thread resumption. Interrupted capture still
requires exact helper/game/private-prefix retirement and reported survivor checks.
Exclude existing world/simulation evidence independently of full VR readiness.

Use the reviewed isolated lab/prefix and per-process simulated Monado setup,
fixed neutral poses/zero controls, exact products/settings/profiles/scene and
source/tool seals, bounded readiness/deadlines and exact owned-process cleanup.
Attempted fixtures must never be rerun/resealed, including failures. The user
subsequently requested cleanup to control disk usage. Verified duplicate archives
in retired agent-created fixtures may be consolidated into a private read-only
store, retaining exact content hashes, original metadata and a retirement ledger.
Keep logs, captures, receipts, saves, profiles, settings and all unique evidence.
Do not link archived content to the original installation or mutable lab files;
new runnable fixtures must receive independent copies. Cleanup is limited to
owned generated data and does not authorize removal of user or original files.
Before creating more lab copies, check free space and retire superseded owned
storage when its unique evidence is preserved. Keep active fixtures independent;
do not accumulate another full archive set for each historical attempt.
A collection-readiness rejection after clean scene/shutdown is not a startup
crash. Keep native/Windows runtime manifests distinct and preserve global
runtime/security/credential settings. No automatic WinBoat passthrough changes.

Earlier brief game focus and simulated Monado questions were explicitly answered;
they are not pending permission questions. General online development authority
remains. Lab-only single-player initialization suppression must never become a
production multiplayer restriction.

## Verification and completion

Gameplay VR requires distinct native per-eye WORLD views, head translation and
rotation driving native cameras, correct FOV/parallax, coherent hands/origin/body
collision. Menu quads, duplicate flat gameplay or synthetic-only submission do
not satisfy it. See [runtime acceptance](docs/RUNTIME_ACCEPTANCE_MATRIX.md) and
[gameplay readiness](docs/PC_GAMEPLAY_READINESS.md).

Preserve actual native dual-wield eligibility/mechanics. User acceptance covers
per-hand identity/pose/aim, independent press/release/no stuck triggers,
simultaneous fire, switch/equip changes, native ammo/reload, per-hand scope
ownership, interruption/death/respawn and multiplayer. Hand annotations/content,
transform agreement, controller-origin/grasp semantics and physical release are
separate evidence. Never infer alignment from a root near a controller, borrow
another draw/eye's cache association or widen tolerance for a passing result.

Report implemented, built, static, simulated-runtime and actual-device evidence
separately. Keep unknown compatibility explicit. Source scaffolds, counters and
mocks are not completion. Avoid repeated exhausted broad audits; an unresolved
gate needs a concrete next source/probe operation or precise missing input.

## Builds and reviews

C++20; explicit x86 native calling conventions; pointer-free fixed-width IPC;
bounded waits; pinned dependencies/licenses. Build x86 game/server and x64
host/official loader. Rebuild ALL products after src/cmake/CMakeLists changes
before packaging. Check current compiled fingerprints and IPC11/wire7 layouts.
Historical IPC10 builds/fixtures retain their recorded identities; never mix products.
Pure offline-tool changes require their meaningful consumer/regression checks
and product-contract verification, not an invented native rebuild requirement.

Use the existing Python dependency environment for native verifiers/builds:
PYTHONPATH must name the checkout's deps/python by absolute path, because CTest
changes working directory; set PYTHONDONTWRITEBYTECODE=1. Run appropriate checks; do not
repeat broader passing checks without a new change or unresolved concern.

Use the main agent or Terra for substantive investigation and implementation.
All delegated reviews require explicit gpt-6-astra at xhigh or a supported Astra
Max/Ultra setting with agent_type default; fixed-model roles can override requested
settings. Never use a fixed Terra reviewer, Sol or Spark for reviews.
Reverify effective local model/effort tags on every review turn, including reused
or resumed agents. Earlier tags do not establish current routing. If a resume
cannot explicitly select Astra, spawn a fresh gpt-6-astra/xhigh reviewer instead.
Report unavailable Astra instead of substituting models.
State independent backend-attestation limitations. Before delegation specify objective, exact scope,
read-only/disjoint write ownership, output/evidence bounds and failure path.
Workers must not revert others. Main retains architecture/integration and public
conclusions. Close completed agents; periodically inspect subagent-audit results
privately and avoid overlapping/high-fanout work. Routine deterministic preflight
checks do not require another design review when the reviewed procedure is
unchanged; verify exact bytes/paths/seals instead.

## Preservation and publication

No proprietary assets/binaries/raw disassembly, captures, credentials or sensitive
telemetry in public source. Keep analysis/captures/packages private. Stage mod
files reversibly with backups and refuse unrelated collisions. Use exact owned
PIDs; no broad process killing, destructive cleanup/reset/force-push, security or
credential change without specific user intent.

Actions stays DISABLED and workflow files ABSENT. Before each source push verify
complete raw ancestry with tools/verify_publication_history.py HEAD; all reachable
author AND committer emails must be strictly empty. Use anonymous project names
and explicit empty emails for commits; never trust publishing-tool defaults or
limit the gate to the tip/revision range. Inspect staged source/messages for
private contents separately. Preserve old email ancestry outside this repo.
