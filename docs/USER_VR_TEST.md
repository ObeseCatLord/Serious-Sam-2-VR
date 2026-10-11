# Comprehensive user VR acceptance procedure

Preparation document: the full implementation is still in progress. This is not
a claim that every feature below is enabled or ready. The integration owner must
supply an exact build/package identity and list remaining disabled features before
asking for a final pass. Individual weapon-firing tests belong to the user in VR;
Codex must not continue those probes under the current instruction.

## Current candidate — 2026-10-10

- Version `0.2.11-dev.c6060831f136`, source fingerprint
  `c6060831f136f14818034c6f02630300b3eaf051e84c08d06fbecb0d77dcb806`, source commit
  `1394b84`, IPC11/wire7. The package is supplied privately; binaries are not published.
- Product SHA-256: `Bin/d3d9.dll` `0930bb00…1f59`, `Bin/SS2VRServer.dll`
  `0a266ded…8b69`, `Bin/SS2VR/ss2vr_host.exe` `20308ef0…c403`,
  `Bin/SS2VR/openxr_loader.dll` `bb011caa…bd1d`. Compare against the package
  `manifest.json`; the installer refuses mismatches.
- Game settings for VR: D3D renderer, windowed (`gfx_bFullScreen = 0`),
  `gfx_bHDRRendering = 0`, no MSAA (Options → Video, or
  `Content/SeriousSam2/Sam2.ini`). Eye resolution follows the headset's OpenXR
  recommendation automatically; `[Render] Scale` adjusts it and `[Weapons] Scale`
  sizes hand-held weapons (0 = automatic).
- Pre-handoff evidence (simulated Monado, fixed neutral pose, no input): with
  `[Roomscale]`, `[HeadComfort]` and `[Melee]` `Enabled=1`, fourteen consecutive
  fresh startups loaded Jungle, presented both native eyes with the visible world,
  dual weapons and lasers, head clearance Clear, and shut down cleanly. This is not
  a headset, motion, firing, vehicle or multiplayer result.
- For the full-feature pass set `Enabled=1` in `[Roomscale]`, `[HeadComfort]` and
  `[Melee]` of `Bin/SS2VR/SS2VR.ini` (all three default to 0), then restart.
  Repeat the world/tracking section once with them at 0 for comparison.
- Vehicles: Fighter/Saucer use their authored handles (`[Vehicles]ImmersiveGrips`);
  other driven vehicles use the virtual steering wheel (`[SteeringWheel]`). Stock
  SS2 has no modeled driver wheel, so the wheel is a mod-made model textured from
  the game's own HoverFighter texture. Teleport is excluded by design.
- Report the first failing step with `Bin/SS2VR.log`, `Bin/SS2VR/ss2vr_host.log`
  and the game's `Sam2.log`. A startup that hangs on loading should be reported
  with its log even if a retry succeeds.

Use a separate test profile/save and a private multiplayer session. Keep original
saves and settings backed up. Record game version, mod package/source fingerprint,
platform, runtime version, headset and controller models. Use the same scene/save
and settings when comparing failures. Save captures and logs locally.

## Platform and device coverage

Record each available cell separately; simulation does not pass a hardware cell.

| Platform | SteamVR + Steam Frame | Envision/Monado + Bigscreen Beyond |
| --- | --- | --- |
| Native Windows | Actual headset/controller pass required | Record actual runtime availability; do not assume this Linux setup exists on Windows |
| Linux/Proton | Actual headset/controller pass required | Actual headset and selected controllers required |

Confirm the actual per-hand interaction profile in the host log. Frame has a
documented Touch fallback, but an Index profile is not proof of Frame bindings.
Beyond's controller model determines its bindings. Use the current package's
[control table](../README.md#controls) and [profile bindings](HOST_STATUS.md#controller-bindings).

## World rendering and tracking

1. Launch the supplied direct-gameplay test scene. World geometry must surround
   you at the headset's field of view. A rectangular menu panel is appropriate
   for menus; two small flat gameplay panels fail this test.
2. Look at a nearby solid object with a distant object behind it. Alternate
   closing each eye: depth and occlusion must agree, with binocular disparity.
   Inspect terrain, vegetation and the waterfall separately in both eyes.
3. Keep the controllers still. Move your head left/right, up/down and forward/back
   without turning; nearby and distant objects must show different parallax.
   Then test yaw, pitch and roll independently. The world must remain stable.
4. Turn with the right stick, move with the left stick, recenter, pause/resume,
   change level and load a test save. Check for stale frames, double movement,
   eye disagreement, wrong scale or an origin jump after each transition.
5. Approach a wall with physical head movement and ordinary joystick movement.
   Test the enabled body/head collision behavior, corners, stairs and low roofs.
   Do not count an intentionally disabled collision feature as a pass.

Body settlement and head-volume protection are separate development options:
`[Roomscale]Enabled=1` requests body movement; `[HeadComfort]Enabled=1` requests
the world-clearance guard and can make obstructed or unavailable world views
opaque while retaining UI. Both default to0. The head path also admits supported
seats, but native filtering may exclude the ridden mechanism, so cockpit-interior
collision is not established. Record these settings and each outcome separately.

## Hands, aiming and native dual wield — user-operated firing

1. Check each weapon's physical grip, size and orientation against its controller.
   Move one hand while the other is stationary. Weapon identity, aiming and laser
   must remain independent. Verify whether the build actually includes hands;
   do not infer a hand model from a floating weapon or controller pointer.
2. Check laser aiming on every available non-scoped weapon; it must work without
   entering scope zoom. Trace lasers across nearby walls and distant terrain. They should originate
   at the native muzzle and terminate on the closest eligible surface. They do
   not promise the final random projectile spread or penetration path.
   Include a long-barrel weapon: its laser must begin at its muzzle rather than
   at a fixed distance from the controller. Repeat after turning or moving the
   body, charging where applicable and switching either hand.
3. For every combination the stock game actually permits, record left/right
   weapon identities and native eligibility. Unsupported combinations remain
   unsupported. A two-hand grip on one weapon is a separate test.
4. Fire left alone, right alone, alternating, and both simultaneously. Release
   each independently; no firing may persist after release. Include tap, hold
   and release for native charged weapons. Confirm actual impacts follow that
   hand's aim, not head gaze or the other hand.
5. Exercise native ammo, cooldown and reload only where applicable. Include the
   initial Zap's charge/release behavior and reload cycle; infinite ammo does
   not mean absence of reload. Compare questionable behavior against stock.
6. Switch either hand, equip/unequip, run out of applicable ammo, use an item,
   pause, lose/regain tracking, die and respawn while pressing and after releasing.
   A held control must not become a new press merely because tracking returns.
7. Test each eligible scoped hand: enter/release zoom, turn the other hand,
   fire, switch that hand and interrupt tracking. Magnified image, muzzle and
   zoom ownership must belong to the same weapon; surrounding world FOV stays normal.
8. The unique-saw physical adapter is a default-off development option. For a
   separate test pass enable `[Melee]Enabled=1` in the private test copy; use
   matching wire7 clients/server and the same setting on participating native
   consumers. After selecting or replacing a saw, hold still before testing motion. The
   first physical swing must work without a trigger-prime cycle. Unknown/copy
   state is never guessed low; the first gesture issues a real native press. Test both hands where native eligibility permits, with
   ordinary and flipped button mapping; coupled/alternative targets stay native.
9. Check saw contacts/misses, native cadence, manual firing coexistence, short
   moving→still transitions, held-query cooldown skips, switching and carried
   objects. Try source loss/recovery, death/respawn and re-equipping after a swing.
   No carried object may be thrown by physical motion. Verify each receiving MP
   role independently admits first use after fresh quiet and rearms after actual
   stops. Include copied-active state and compare behavior with a native manual press.

For the physical-saw pass, also interrupt grip tracking briefly while keeping the
trigger released, then restore tracking with the controller in a slightly different
position. Test each hand separately. Recovery must not create a new physical
gesture, even if the game missed the loss frame. Hold still to establish a fresh
quiet baseline before making the next intentional swing. Test native stop/release
separately: rejecting a recovered gesture does not prove that an earlier native
press was released. Do not modify native manual history to prime this test.

## Wheels, menus and comfort

During the later hardware pass, recenter while looking up/down and with a tilted
head. Keep the same horizontal facing direction: head pitch/roll must remain
tracked without introducing a sideways camera or hand offset. Also recenter while
looking vertically, then return to the horizon; the previous horizontal reference
must remain stable. Repeat on foot and in supported seats, separately from firing.


1. Hold each hand's grip: slowly spinning 3D weapon models (about 9.5 cm long,
   lit like the scene) and four bubbles must appear on a ring around that hand,
   upright and facing you. Reach onto a weapon (it grows and its disc glows; the
   centre card names it and its ammo; empty weapons get a red rim) and release
   to equip it. Return to the ring centre before release to cancel; a deflected
   stick must still pick by direction. Open both wheels; models, cards and hover
   feedback must be readable and independently actionable. Release on each
   bubble: Netricsa opens, the game menu opens as with Menu, and (single player
   only) quick save then quick load act once each. Report any weapon that shows
   a flat icon instead of a model (the log names it), a model that is black,
   off-centre, oversized or flickering, or icons that are upside down or boxed.
2. Lose squeeze availability/tracking while a wheel is open, then restore it
   while still squeezing. It must cancel and require a real release/rearm.
3. Navigate the native menu using the pointer and buttons. The visible cursor
   must match the hit location; pause, back and confirmation must work without
   a desktop mouse. Holding a trigger across menu entry must not confirm an item.
4. Read HUD, messages, scores, player names, death screens and fades. At default
   settings, turn less than 35 degrees: the panel should stay anchored. Turn
   farther: it should follow smoothly, stop near an 8-degree residual and remain
   level. Check recenter, sitting/standing and physical position changes.
5. Adjust panel size/distance within supported settings. Record clipped text,
   unreadable labels, intrusive placement or uncomfortable following separately.
6. Watch an in-level cutscene, a pre-rendered movie (do not use
   `+sam_bSkipMovies 1`), Netricsa, a loading screen and the death view. Each must
   appear on the same flat virtual screen as menus, not black eyes or a stuck
   world frame, and return to stereo gameplay when control returns. Trigger/Use
   confirm and Menu goes back (skip a movie, close Netricsa); the pointer works
   only in real menus. After death, trigger or Use must reach the native
   respawn/reload prompt.

Optional native UI aid: if this pinned game build exposes the documented menu
gadget grid (`men_bRenderGadgetGrid` and X/Y spacing controls), compare its visible
lines with pointer selection across the panel. Use only the test profile, record
prior values and restore them afterward. Grid availability or appearance is not
a passing pointer test; visible selection must match the chosen native item.

## Swimming, seats and vehicles

Test walking, sprint/jump/use and head-directed swimming. Test optional arm pulls
only if enabled. Enter/exit each supported vehicle and seat, aim, use native
throttle/steering fallback and check head/hand anchoring through seat changes.
Fighter and Saucer use their real cockpit handles. Every other driven vehicle
(cars, buggy, aircraft, rollerball, air bike, hover scooter) shows the virtual
steering wheel (`[SteeringWheel]`, default on) about 0.4 m in front of and below
your seated eyes; recenter first if it sits in the wrong place. Check that it
looks like stock SS2 metalwork (HoverFighter texture) in both eyes and stays fixed
to the vehicle while driving. Squeeze on the rim with the left hand, right hand
and both; turn it both ways: cars steer, hover/air vehicles turn steadily while
the wheel is held off-centre. Release: the wheel recentres and steering stops.
Grab with one hand, add the other and release the first: no jump. Lose and
regain tracking while squeezing: it must not regrab until you release and
squeeze again. Joystick steering must still work without holding the wheel.
Record vehicle names, which steering route they used, and adjust `OffsetX/Y/Z`,
`TiltDegrees`, `FullLockDegrees` or `TurnRateDegrees` if needed. Teleport is
outside scope.

For optional arm pulls, use `[Swimming]Immersive=1` only in the separate test
copy; the default is0. Keep the movement stick centered in water. Briefly lose
and regain either controller's grip tracking at a different hand position, then
repeat with both controllers. Recovery must not inject a pull-stroke control
input; existing native momentum may continue. After a fresh tracked baseline,
an intentional pull should work again. Compare with arm pulls disabled and check
that head-directed joystick swimming still works. These checks distinguish a
recovery-generated input from normal native buoyancy and movement.

## Multiplayer with other mod users

Record the exact game/mod build and each test profile's network bandwidth and
auto-aim options before connecting. The published native controls include
`cli_iMaxBPS`/`cli_iMaxBPSOut`; availability and defaults vary by game build.
Keep these variables consistent across the two host-role runs and retain the
original profile. Do not change global network/security settings. An optional
GameDig status query is availability evidence only and is not required to pass
mixed desktop/VR gameplay acceptance.

Use matching client/server products and a private session. Test listen-host local,
remote client and dedicated-server roles. Observe both directions with another
mod user: head/hands/weapons, body motion, supported dual wield, scopes, melee,
vehicle seats and native hit results. Include at least one ordinary gun
(such as Auto SG) on the dedicated server: its native model reference must be
available there, and shots must follow the correct hand with working ammo/cadence.
A missing model/calibration admission must be recorded as a failure, not a pass.
 Include physical head displacement mixed
with joystick movement, walls/support movement, packet delay where available,
death/respawn, reconnect and level changes. Remote body correction must not cause
detached hands or reset local physical travel. For saw gestures, include a short swing followed immediately by stillness while
another player observes. A newer low pose must not erase the edge; a consumed
released edge must not keep the observer firing during later intervals. Where
controlled delay is available, test duplicates/reordering/expiry, with no old
swing after stop/rearm. Test manual+physical motion together and each hand
independently. Pose packets or counters alone
do not pass this test: another player must see coherent native gameplay.

Also test mixed desktop/VR sessions with the mod installed for every player.
The desktop participant must launch without a headset and retain native desktop
rendering, mouse/keyboard movement, aiming, firing and inventory controls. Test a
desktop listen host with a VR client, a VR listen host with a desktop client, and
both clients on the matching dedicated server. Verify both directions: desktop
players see coherent VR head/hands/weapons, while VR players see native desktop
player presentation and actions. Include reconnect, death/respawn and level
changes. Do not count VR-only sessions as mixed-mode acceptance.

## Reporting and recovery

For every failure record build, scene/save, platform/runtime/controllers, role,
left/right eye, exact action sequence and expected/observed result. Keep the
original logs and a short capture locally; note whether stock reproduces it.
After normal quit, verify owned game/host processes exit and your original
profile/save/settings remain unchanged. Report startup, tracking-loss and runtime
restart behavior separately from successful gameplay. Mark each row passed,
failed, unavailable or not yet enabled; unavailable is not passed.
