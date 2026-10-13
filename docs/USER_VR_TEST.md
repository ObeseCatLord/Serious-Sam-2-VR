# User VR test procedure

Hardware acceptance is done by the user in VR. The simulated-headset lab covers
rendering, frame rate and captures but cannot press real controllers, so every
section below that involves your hands is still open. Mark each item passed,
failed, unavailable or not enabled; unavailable is not passed.

## Current candidate

- Build: see `Bin/SS2VR/install-receipt.json` (`version`) in the game folder; the
  last line of this section is updated with each installed candidate.
- Settings in the installed `Bin/SS2VR/SS2VR.ini`: `[Roomscale] Enabled=1`,
  `[Melee] Enabled=0` (the chainsaw runs on the trigger, no swing gestures),
  `[HeadComfort] Enabled=0` (no blackouts). Leave the rest at defaults for the
  first pass.
- Game settings: D3D renderer, windowed (`gfx_bFullScreen = 0`), no HDR
  (`gfx_bHDRRendering = 0`), no MSAA. Eye resolution follows the headset's OpenXR
  recommendation; `[Render] Scale` adjusts it.
- Bigscreen Beyond 2e via Envision/Monado: close WayVR (wlx-overlay) for the test;
  it competes for frames. The lab measured 75 Hz at 3561x3561 per eye on this PC.

Report the first failing step with `Bin/SS2VR.log`, `Bin/SS2VR/ss2vr_host.log` and
the game's `Sam2.log`. The `Perf` lines (game, every 5 s) and `Host perf` lines
(host) record frame rate and frame cost; note roughly when a problem happened.

## New: VR options and foveated rendering (2026-10-13, late)

- Open a weapon wheel (light trackpad press) and release on the new bubble with
  three sliders: the VR options panel opens in front of you. Move between rows
  with a stick, change a value with the stick sideways (or a trigger / A),
  close with B or the menu button. Changes are saved at once to
  `Bin/SS2VR/SS2VR.user.ini` (it overrides `SS2VR.ini` and survives updates;
  delete it to return to the defaults).
- Try "Foveated rendering": Eye-tracked follows your eyes when the runtime
  offers eye tracking (XR_EXT_eye_gaze_interaction or, on Steam Frame,
  XR_META_foveation_eye_tracked; `ss2vr_host.log` says which; otherwise it
  stays off and the row says so), Fixed keeps
  the sharp region at the view centre. The periphery should only look softer,
  with no seam at the sharp region's edge. Note the `Perf` lines with it on
  and off.
- Also check: snap/smooth turning and turn speed, lasers on/off and haptic
  strength apply without restarting; render scale changes the image size.
- Single pass got exact (sky, smoke, moon, waterfall and the sun's bloom glow
  match two-pass).

## Changed after the second headset test (2026-10-13)

- Both eyes now come from one render of the world (single-pass stereo):
  nothing should show through terrain or walls in either eye, and the frame
  rate should be higher. If an eye looks wrong, say whether it goes away with
  `[Render] SinglePass=0`.
- Weapon wheel: a chosen weapon stays chosen.
- Turrets: aim with the right stick (like the mouse on the desktop); the right
  trigger fires, A leaves.
- Lasers are translucent; a hand without tracking shows no laser; the uzi
  beam starts under the barrel.

## Fixed after the first headset test (2026-10-12)

Check these first; they were reported broken:

- Menus, the pause menu, loading screens and cutscenes appear on the flat
  screen (they were black). Pause with left B (Index); resuming must not tilt
  the view (mouse look is now ignored while VR input is live).
- No blackouts during play: a dropped frame keeps the last image up instead of
  going black. If the world still freezes or blacks out, note the weapon and
  what you did; the log now names the cause (`Native eye projection rejected
  ... at=`).
- Index controls: holding the controller no longer jumps or grabs (grab is a
  firm grip squeeze, jump is right B); a light trackpad press opens that hand's
  weapon wheel; right stick click throws a grenade (zooms with the sniper).
- Right eye: nothing shows through terrain or walls in the right eye only.
- Turrets: look at the turret and press A; the HUD prompt says "Press A to
  enter". Inside, the right hand aims (laser), the right trigger fires, the left
  trigger alt-fires, A leaves.
- Shotguns: your hand sits on the auto shotgun's pistol grip and on the double
  shotgun's stock behind the hammers.

## World rendering and tracking

1. Start a level. The world surrounds you at the headset's field of view, in
   stereo, with correct scale. Menus, loading screens, cutscenes, movies and
   Netricsa appear on a flat virtual screen; gameplay returns to full stereo.
2. Close each eye in turn while looking at near and far objects: depth and
   occlusion must agree. Check terrain, vegetation and water in both eyes.
3. Move your head left/right, up/down, forward/back, then yaw, pitch and roll:
   parallax must be right and the world steady. Note any judder, stale frames,
   double images or eye disagreement.
4. Smooth turn with the right stick (snap turn: `[Comfort] SnapTurn=1`), move
   with the left stick (forward follows your head), recenter (both grips + left
   stick click), pause/resume, change level and load a save.
5. Roomscale: walk physically to a wall, a corner, stairs and a low roof, and
   also push with the stick. Note clipping, pushing back, or jumps of the origin.

## Single-pass stereo

Both eyes come from one world render (`[Render] SinglePass=1`). Look for
anything that appears only in one eye, sits at the wrong depth there, shows
through walls or terrain, or flickers between eyes, and say whether it goes
away with `SinglePass=0`. The old right-eye replay (`EyeReplay`) is now off by
default because it could lose world geometry in the right eye.

## Weapons

1. Grip size and orientation feel right for every weapon; hands are the same
   size on every weapon (the revolvers are the base). Klodovik and the Serious
   Bomb were enlarged; say if any weapon still looks too big or too small.
2. Lasers start at each muzzle and hit the nearest surface. They follow that
   hand only, including while turning, moving and switching.
3. Fire each hand alone, alternately and together; nothing keeps firing after
   release. Try charged weapons (tap, hold, release), ammo, cooldown and reload
   where the weapon has them, and dual wield combinations the game allows.
4. Chainsaw: hold the trigger near an enemy; it must cut, and stop on release.
5. Sniper: hold that hand's stick click (trackpad on Vive/Index-style profiles)
   to zoom. The scope lens shows a 4x view that stays registered to the rifle
   while you move it; the image is smooth (no sparkle). You do not need your eye
   exactly behind the scope. The world around the scope keeps its normal field
   of view. Release, switch hands, fire while zoomed, and lose tracking while
   zoomed. Watch the frame rate while zoomed (about +2 ms per frame in the lab).
6. Switch, equip, run out of ammo, pause, lose/regain tracking, die and respawn
   while holding and after releasing: a held button must never become a new
   press when tracking returns.

## Wheels, pickup and menus

1. Hold a hand's wheel button (see the controls table in the README and
   `docs/HOST_STATUS.md` for your controller): weapon models appear on a ring
   ahead of that hand, with four bubbles on the outer side. Point at or reach
   into a weapon and release to equip; point back at the centre to cancel. The
   bubbles open Netricsa, the game menu, quick save and quick load.
2. Grip on a takeable object picks it up into that hand; releasing throws or
   drops it. Away from objects, on foot, a grip jumps.
3. Menus: the pointer hits what you aim at; triggers held into a menu never
   confirm an item.
4. HUD: health, armour, ammo, messages and scores are readable on the floating
   panel. Turn less than about 35 degrees and it stays put; turn more and it
   follows smoothly. Compare text sharpness with what you remember; the HUD is
   now drawn once and placed on the panel, which may look very slightly softer.
5. Fades (level start, death) cover the whole view, and the HUD is not drawn on
   top of a full fade.

## Vehicles and swimming

Enter and exit vehicles and seats. Fighter and Saucer use their cockpit handles;
other driven vehicles show a virtual steering wheel (squeeze the rim and turn;
release recentres). Joystick steering still works. Swim with head-directed
movement. Note anything that jumps, sticks or steers wrongly.

## Multiplayer (optional)

Every player needs the same mod build. Try a listen host with a VR client, a VR
host with a desktop client, and a dedicated server. Players must see each other's
head, hands and weapons move coherently, and shots must come from the right hand.

## Reporting

For each failure note the build, level/save, controller, left or right hand or
eye, what you did, what you expected and what happened. Keep the three logs.
After quitting, the game and `ss2vr_host.exe` should be gone.

Installed candidate: commit `2b6d00f`, version `0.2.11-dev.92b377c36234` (2026-10-13).
