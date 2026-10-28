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

## New: ammo readouts on every gun (2026-10-28)

The ammo screens now cover every gun that uses ammo, placed as Serious Sam
Fusion's VR weapons place theirs: on the gun's centre line at the rear of its
top, in front of the hand, facing you and tilted back, about 4.5 x 2.3 cm.

1. Hold the uzi, the minigun and the rocket launcher in turn, in either hand
   (both uzis when dual wielding): each gun's small screen shows your ammo as
   glowing seven-segment digits (green on the uzi, blue on the minigun's
   flip-up screen, amber on the rocket launcher). The digits sit on the screen
   in both eyes, move with the gun, read left to right in the left hand too,
   and are hidden by anything in front of the screen.
2. Hold each of the others, in either hand where the game allows it, and
   check the readout sits flat on its spot, reads left to right and is not
   cut by the gun:
   - Auto shotgun: rust-orange, on the rust slope at the top rear.
   - Double shotgun: brass, on the back of the breech between the hammers.
   - Grenade launcher: orange, on the carry handle's rear ramp below the
     warning label (its own screen still shows the charge).
   - Plasma rifle: teal, on the top deck just behind the sight hood.
   - Klodovik: magenta, a wrist display on top of Sam's forearm.
   - Cannon: red, on the sloped rear ring at the top of the barrel.
   - Serious Bomb: one orange digit (bombs left) on the bomb's forehead.
   - Revolvers: one silver-white digit standing on the top strap behind the
     barrel, the rounds left in the cylinder: 6 after a reload, one less per
     shot, back to 6 when the reload starts.
   - Sniper: orange, on the rear slope of the receiver below the scope
     (smaller: the receiver is narrow).
   - Beam gun: cyan, in the middle of its winged display.
   - Flamer: yellow, on the receiver's rear face behind the napalm tank.
   The chainsaw and the zap gun (unlimited ammo) have none.
3. Fire: the count drops with each shot (both uzis show the shared bullets).
4. "Ammo screens" on the VR options menu's Interface page (`[Weapons]
   AmmoScreens`) turns them all off and on at once.

## Changed after the water, cursor, wheel and controller report (2026-10-27, night)

1. Water that changed with the head: the water's see-through/reflective blend
   (its fresnel term) was worked out once per corner of its large triangles
   from the head's position, so straight-edged light and dark wedges slid
   across it as you moved. It is now worked out for every pixel from each
   eye's own view, with the game's own fresnel strength.
2. Choppy cursor in the pause menu: the paused game ran unpaced (hundreds of
   frames a second, most of them never shown), so the menu and the world
   behind it updated unevenly. Menus in VR (title, pause, Netricsa) now draw
   one frame per headset frame. Lab: pause menu 55 fps, every frame shown;
   title about 47 fps.
3. Weapon wheel names are capitalized: Zap Gun, Quick Save, Quick Load, Game
   Menu, VR Options, Left Hand / Right Hand.
4. Index menu prompts show which hand: A and B with a small R (right A
   selects, right B goes back), and the menus' X and Y as A and B with a small
   L (left A, left B). They follow the controllers in hand right away.
5. Steam Frame: its A, B, X and Y are all on the right controller, like a
   gamepad, so the game's Xbox prompts are used as they are: A uses (select),
   B opens the right wheel (back), X reloads (the menus' X), Y opens Netricsa
   (the menus' Y). Left D-pad up opens the left wheel, down reloads.
6. The Colt's laser turns off while its reload animation plays and comes back
   when it ends.
7. Quick save: the game's "cheats enabled" flag (left by the lab that made the
   KINGSBURG save; it is what adds the pause menu's "cheats" entry) is cleared.
   Your previous quick save is kept in the archive's backups.
8. The grey "microphone enabled" box is not from the game or the mod: it is a
   desktop notification shown in the headset by the WayVR overlay (Envision's
   wlx-overlay-s), like the other notification bubbles. Turn notifications off
   in WayVR's settings, or silence the desktop notification, to remove it.

## Changed after the cubemap, cursor, Netricsa, title and pop-in report (2026-10-27)

1. Title screen: shown at 16:9 on a wider panel, exactly as on the desktop. The
   title art is drawn for 16:9 whatever the screen, so the earlier 4:3 panel
   squeezed it every time (the sun badge and the A button were taller than
   wide). Pause menus over a level keep their 4:3 panel.
2. Menu cursor: the menu picture is now copied every frame (it was every 33 ms,
   an uneven rhythm against the pause menu's 19 ms frames), and at the title
   screen the game's 33 fps menu limit is raised to 90 in VR.
3. Netricsa works like the pause menu: the world stays live behind its panel
   and follows your head, at full frame rate. Scroll, page and close as before.
4. Draw distance: `[Render] DrawDistance` in `SS2VR.ini` (default 2, range 1 to
   4) multiplies the distances at which the game lowers model detail, culls
   small objects and draws grass and clutter, starting from your video
   settings. 1 is the game as configured. Your own values are put back before
   the game saves its settings.
5. Water reflections that moved with the head: the reflection was looked up
   once per corner of the water's (large) triangles and blended between them,
   so its sky/tree boundaries were straight lines that slid across the water as
   the view changed. It is now worked out for every pixel. In the lab the
   straight-edged sky triangles are gone, and the reflection stays with the
   scene when the head turns or moves sideways.
6. New save `KINGSBURG` in your profile's load list: the start of Kingsburg
   with the weapons and ammo of that point, including the minigun and the
   rocket launcher (not marked as cheated). No Kingsburg save had ever been written to the save
   folder, so there was nothing to restore.

## Changed after the title, water, sticks and pause report (2026-10-25, evening)

1. Title screen: it is laid out for the 4:3 menu panel when the game starts in
   VR, instead of keeping the 16:9 desktop layout squeezed into 4:3. In levels
   the HUD and cutscenes keep their 16:9 band as before.
2. Pause menu over the live world:
   - Full frame rate. The game slows itself to 33 fps while a menu is up; with
     the world live behind the pause menu that limit is lifted (the lab went
     from 30 ms to 19 ms frames).
   - Dark outlines and borders inside the menu no longer let the world show
     through. Only the black around the menu panel is see-through.
3. Hoverbike and UFO handles:
   - They turn with the steering, whether you steer with the right stick or
     with your hands.
   - They are not held when you board. Squeeze a grip to take hold of them:
     they light up faintly, and tilting your hands steers (right hand lower
     turns right). Squeeze again to let go; then only the stick steers.
4. The circular saw's engine loop (a constant motor-like hum) is stopped when
   no saw is in your hands, including when a save restores it. Every sound
   file the game plays is now listed once in `Bin/SS2VR.log` ("Sound file ...")
   so any other stray loop can be named.
5. Water reflections are a still, basic picture of the surroundings:
   - The reflection map is rendered once where you stand, from scenery about
     as far away as the sphere it is mapped on (in the lab 15-24 m and beyond):
     sky, far terrain and big landmarks. Nearby trees, banks and props are left
     out, so nothing in the reflection slides as you move your head.
   - It no longer updates live. Walking more than 4 m away renders a new one in
     the background and switches to it in one go; no flicker or half-updated
     reflection.
   - Turning your head, reflections stay put in the world. Moving it, the
     reflected sky shifts a little, as on real water.
   - Exact mirror reflections: VR options > Render > Water mirror (frame rate cost).

## Changed after the hoverbike, menus and weapon wheel report (2026-10-25)

1. Weapon wheel, styled after Serious Sam Fusion's VR wheel:
   - Each weapon has an ammo ring that empties clockwise from 12 o'clock as its
     ammo runs out. The ring is green, turns yellow below two thirds and red
     below one third. Weapons without ammo (chainsaw, zap gun, Colt) have a
     blue ring.
   - Weapons that can't fire are greyed out: grey disc, grey icon, no 3D model.
     That includes a double shotgun with a single shell. Hovering one shows a
     red glow and "Out of ammo". Releasing on one selects nothing.
   - The ring's centre shows the hovered weapon's ammo icon and count in the
     game's HUD font. With nothing hovered, it shows the weapon in that hand.
   - The weapon's name now sits on a card above the ring.
   - The weapon in that hand has a halo in the hand's laser colour.
   - The wheel's colours were also fixed. The menu, save and load bubbles now
     show their intended colours (purple menu, green save, orange load).
2. Hoverbike and UFO: the right stick turns them again. Both hands now steer
   like a wheel (right hand on the right, left on the left): tilt your hands to
   turn. You don't need to grab anything, and there's no floating wheel.
3. Menus: A selects and B goes back. Loading screens say "Press A or a trigger
   to continue". A or B skips the title screen's attract video.
4. Netricsa: the left stick scrolls, the right stick changes page, A uses, and
   B or the menu button closes.
5. The pause menu floats in front of the live game world: look around while
   paused and the world turns with your head. Single player pauses as on the
   desktop; in multiplayer the game runs on for everyone, as the game's own
   pause menu does. The HUD and lasers are hidden while it's open. Netricsa
   still floats in front of the last game view.
6. The title screen uses a 4:3 image. Cutscenes are no longer cut off at the
   top and bottom.
7. Desktop play (no headset): a "VR options" button sits at the bottom of the
   game window under the game's menus. It opens the same options panel as a
   window: click a tab, click the arrows or use the mouse wheel over a value,
   and press Esc or Close to close it. Sound and voice chat settings can be
   changed there. Settings that only apply in VR are greyed out, with grey
   arrows. In VR, the pause menu over a level has no "VR options" button
   (the Proton runtime can't show it there); use the wheel's options bubble.

## Changed after the junkyard water and match report (2026-10-24)

1. Held objects (the match, crates, balls) are held rigidly at your hand. The
   game's carry is a spring attached to your body: it shook around the hand
   and pulled on you, so jumps were short. Now:
   - jumping while carrying is as high as without (lab: 1.62 m holding a
     50 kg crate, 1.58 m empty);
   - throws take your fastest recent hand motion, including the wrist flick,
     and give the whole object that speed and spin (lab: the crate flew
     6.9 m).

   Please check in Giant Junkyard:
   - the match stays steady in your hand;
   - you can climb the mushrooms holding it;
   - it flies when thrown.

   Your log will say "Grip carry taken ... mass=... parts=..." for each grab.
2. Water reflections are back to the cubemap by default (`[Render]
   WaterMirror=0`; VR options, Graphics, Water reflections: Cubemap). It no
   longer moves with your head:
   - The captured surroundings were treated as a 40 m sphere centred on your
     eyes, so the sphere moved whenever your head did.
   - Now the sphere is anchored to a fixed point in the world. It moves only
     after you walk about 4 m away.
   - Its size follows what is around you, measured with the game's own
     collision rays: near walls and giant junk make it small, open ground
     makes it large.

   Turn your head and step sideways by the water: reflections should stay put
   in the world. Very close objects can still look slightly too far away.
   "Mirror" renders the world mirrored for each eye (exact at any distance,
   costs two extra renders); try it once for comparison.

## New: VR options menu (2026-10-23)

The VR options panel is now a tabbed menu: Movement, Graphics, Sound, Voice
chat and Interface.

1. Open it two ways:
   - a "VR options" button under the game's own menus (the main menu and the
     pause menu), so it works before a level is loaded;
   - the weapon-wheel bubble, as before.
2. Point either controller at it and pull the trigger:
   - on a tab to switch pages;
   - on a row's left or right arrow to step its value down or up (clicking the
     rest of the row steps it up);
   - on Reset page or Close.

   Pointing at a row selects it and shows what it does at the bottom. Without
   pointing: the stick moves between the tabs, rows and buttons and changes
   values; the Use button (Index right B, Touch A) steps the selected row; the
   Menu button (Index left B) closes the panel. Right A no longer closes it.
3. New settings:
   - Walk direction: Head, Left hand or Right hand (`[Comfort] MoveDirection`).
     With a hand, the left stick walks where that controller points; with the
     controller pointing straight up or down, or untracked, it follows your
     head.
   - Snap turn angle: 15 to 90 degrees (`[Comfort] SnapTurnDegrees`).
4. Also in the menu now: arm-stroke swimming, vehicle turn rate, water reflections (mirrored world or
   surroundings) and their resolution, scope magnification, showing the game on
   the monitor, spatial audio, sound occlusion, and every voice chat setting
   (hear others, microphone mode and device, sensitivity, boost, voice and radio
   volume, proximity range, talking indicators), plus HUD, lasers and menu size.
5. Everything applies at once, in a level or in the menus, except spatial audio:
   its row turns amber and says a restart is needed. Instant mounting, creature
   hand aim, ride guns following head pitch and grabbing vehicle controls are
   not in the menu; they stay on by default and remain `SS2VR.ini` options
   (`[Rides]`, `[Vehicles] ImmersiveGrips`). The HUD
   (wrist or head) and its size now also switch without a restart.
6. Please check:
   - walking with Walk direction set to each hand;
   - picking your microphone from the device list;
   - that the click which closes the panel neither fires nor clicks the game's
     menu.

## Changed after the cutscene and carry report (2026-10-23)

1. Multiplayer voice keeps working through intermissions and cutscenes (you hear
   others where you last stood while the camera is cinematic).
2. The end-of-load prompt names a VR control: "Press B or a trigger to continue"
   on Index (A or a trigger on Touch), the buttons that do continue.
3. Carried physics objects ease into your hand instead of overshooting: in
   Land of Giants, carry the match to the TNT. Throws are scaled to the game's
   own gravity (about 2.9x Earth's): toss a basketball at the Jungle court hoop.
4. Serious Bomb: the forearms match the gloves (only their length shrinks).
5. Rolling ball: no floating wheel; it rolls where you look and the right stick
   turns you.
6. Water reflections (Branchester, any level with water): reflections are now a
   mirrored render of the world (`[Render] WaterMirror=1`, the default). Turn
   your head and step sideways next to the water: reflected walkways, trunks and
   sky should stay put in the world, with depth beyond the surface. Compare once
   with `WaterMirror=0` (the old cube) and note the frame rate with each; the
   mirror costs one extra world render while water is in view
   (`WaterMirrorScale` sets its size).
7. Swamp start (canted displays): turn around and compare the eyes on distant
   trees. Engine scissors now follow each eye's frustum. If the eyes still
   differ, try `[Render] SinglePass=0` once: if that cures it, single pass is
   the cause.
8. Not a bug: `Sam2.log`'s "Started simulation ... in N seconds" for a level
   change counts the time the "press to continue" screen waited for you.

## Sprint (2026-10-22)

1. Click the left stick while walking forward: you sprint (the game's own sprint,
   1.5x). It stays on while the stick points roughly forward from where you walk,
   including after turning with the right stick or with your head turned.
2. Stop, strafe or back up: sprint switches off; click again to sprint.
3. Clicking both sticks together still recentres and does not toggle sprint.
4. Sprinting needs the game's Enable sprinting option, which is off by default:
   the mod turns it on in single player and on a VR server.

## Changed after the turret, wrist and prison report (2026-10-21)

1. Turrets aim where you look. The right stick turns your view and the turret
   together (up to the turret's limit). The laser leaves the gun barrel along
   its axis, so it stays on the line of fire at the top of the arc too.
2. Use (switches, the prison's detonator, doors, pickups, "Press Use" prompts)
   now looks where your head looks instead of level ahead of your body.
3. Wrist watches sit on top of the wrists like EDuke32 VR's (behind the
   controller, along the forearm, tilted toward your eyes), without the dark
   backgrounds. Health and the left ammo no longer cut each other in half, and
   each weapon's ammo shows on the wrist of the hand holding it.
4. The grenade in your hand is the same size as a thrown one.
5. The weapon wheel opens further out and a little higher; a laser from the
   hand points at the item to choose. The weapon lasers have no dot at the end.
6. Water reflections keep their place: nearby trees, rocks and crates reflect at
   their own distance instead of sliding with your head.

## New after the vehicle and scope report (2026-10-20)

1. Hoverbike (Road to Ursul save): no steering wheel. Squeeze both grips on the
   bike's own red-knob handles and twist your hands. Held twisted, the bike keeps
   turning, like a control stick (`[Vehicles] GripTurnRateDegrees`, 0 for 1:1
   handlebars). The right stick does not turn a ride that has handles. Look up and
   down: the guns pitch with your head (`[Rides] HeadPitchAim`). The laser leaves
   the gun muzzle. The view should stay steady while riding.
2. Sniper rifle, in either hand: the scope shows a live 4x picture without zooming
   (`[Scope] AlwaysLive`, `IdleMagnification`). Zooming uses the game's own zoom.
   Thin crosshair lines cross the lens through where the shot lands. Both eyes
   should see it (single-pass stereo stays on).
3. Serious Bomb: the gloves match the other weapons' hands, with short forearms
   instead of arms reaching back to your face.
4. Wrist HUD (`[HUD] Mode=1`, or "HUD" in the VR options menu). Turn your left
   wrist toward you to see health, armor and lives on a watch face, and the right
   wrist for ammo, grenades and score. Messages, prompts and Netricsa stay in front
   of you.
   "Head-mounted" in the options menu brings back the old HUD. "Wrist HUD size"
   scales the watches.
5. Spatial sound (`[Audio] Spatial`): the game now plays through OpenAL Soft with
   headphone HRTF, and your head is the listener. Turn your head and sounds should
   stay put around you. Sounds behind walls should be muffled
   (`[Audio] Occlusion`). `Sam2.log` should say `Sfx API: OpenAL` and `Device:
   OpenAL Soft`. If sound is missing, set `Spatial=0` and report the
   `Spatial audio:` lines in `Bin/SS2VR.log`.

6. Hand grenades (2026-10-20, later): the right stick click now always puts the
   grenade in your right hand (its weapon is put away until you throw or click
   again). Throw it with the right hand; it flies where your left hand aims (the
   left laser's dot, or where the left controller points).

## Dedicated server (2026-10-22)

A VR dedicated server now works with today's Steam (lab: a simulated-VR client
joined a server on an ARM host under FEX + Wine, negotiated VR, and its poses
and voice reached the server). What it needs:

1. `DedicatedServer.exe +mod SS2VR +port <port> +lan 0`, run from `Bin`. The
   game's UDP ports (port and port+1) must be reachable from the internet.
2. A current Windows `steamclient.dll`, `tier0_s.dll` and `vstdlib_s.dll` in
   `Bin` (Steamworks SDK Redist, app 1007): the game's own copy only knows
   retired Steam servers and never logs on.
3. Join from the server list, or with `+connect <server SteamID>`.

Every player needs their own Steam account: Steam networking holds one
session per account, so a second game on the same account drops the first.
Players and the server all need this build (it authenticates with current
Steam session tickets; the game's own ticket check is refused by Steam).

## Voice chat (multiplayer, 2026-10-19)

Players who all run SS2VR can talk. The server must run SS2VR too: a listen
host with the mod, or a dedicated server with `SS2VRServer.dll`. The server
relays the voices; players without the mod hear nothing and see nothing extra.

- Voices come from where each player is (left/right by direction, softer with
  distance) and fade to a centred "radio" voice beyond 24 m, so distant
  teammates stay audible.
- The microphone is for chat only: it is sent to the other players and never
  controls the game (no voice commands; the weapon wheel and every other
  control work exactly as before).
- Microphone (VR options panel, or `[Voice] Microphone` in `SS2VR.ini`): Open
  mic (default; sends while you speak), Hand to mouth (hold either controller
  at your mouth to talk), or Off. The microphone only opens while you are in a
  multiplayer session with another SS2VR player.
- A green ring floats over a player who is talking; a small one sits at your
  left hand while you are sending. `[Voice] Indicators=0` hides them.
- Voice volume is on the VR options panel too. `[Voice] InputDevice` picks a
  microphone by name; the names are listed in `Bin/SS2VR.log` ("Voice capture
  device"). Empty uses the system default input.
- To check: both players talk in turn; each should hear the other from the
  right side; with "Hand to mouth", nothing is sent until a controller is at
  the mouth. Report with both players' `Bin/SS2VR.log` ("Voice" lines).

## Changed after the ride and weapon report (2026-10-18)

- Getting on a vehicle no longer throws you off at once: Use (B) held from
  boarding or leaving counts again only after you release it.
- Weapon picks no longer switch straight back (after a ride or a loaded save):
  an empty weapon left in neither hand kept making the game re-equip your
  favourite weapon.
- Rolling ball: the view keeps its own heading and follows the ball's centre;
  the ball rolls where you look; the right stick turns you as on foot.
- Dinosaurs: aim with your right hand. A laser from the hand shows where shots
  land (the dinosaur aims its shots at the laser's dot); the right stick turns
  you and the left stick walks where you face. `[Rides] HandAimCreatures=0`
  restores stick aiming.

## Changed after the sixth headset test (2026-10-14, late night)

- No more tilt after a judder: on foot the view takes only the game's heading
  (its camera shake or swimming pitch tilted the whole world).
- Sky and clouds sit at infinity in stereo (they had a few pixels of parallax).
- Menus show completely (top and bottom were cut off); the cursor stays under
  the pointer.
- Boxes come into your hand and stay there (they ended above your head).
- Index: right A jumps, right B is Use.
- Riding: the camera is smoothed (bob and sway removed, no lag on steady
  movement or turns); the ride turns only while you push the right stick.
- Turrets and other rides: a laser shows where the gun aims; hold a grip (or
  both) and move your hands to swing the gun like a real mount (hands right,
  muzzle left); the stick still works.

## Changed after the fifth headset test (2026-10-14, night)

- Sideways or tilted world: the mod now uses the headset system's fixed play
  space (STAGE) instead of a space the runtime may set from wherever the
  headset was lying when the game started or tracking came back.
- Recentre: click both sticks together (or both grips + left stick click);
  keep holding 2 s while looking straight ahead to also level the view. The
  grenade is now taken when you release the right stick click.
- Fewer stutters: a frame with one bad layer pose is no longer thrown away
  whole, and a newly arrived frame is not hidden while the channel is busy.

## Changed after the fourth headset test (2026-10-14, evening)

- Riding (dinosaur and every other ride): the view no longer bobs or pitches
  with the ride; it keeps the ride's heading only (aircraft still pitch and
  roll). Pushing the left stick forward goes straight; the right stick turns
  the ride while you push it and your right hand aims up and down. Turrets
  are unchanged (stick only).
- Menus: the game's cursor is now under your pointer everywhere (it drifted
  away from the centre up and down).
- Grabbing boxes: squeeze the grip (Index: a firm squeeze, the light trackpad
  press is the wheel) with your hand on or near the box, or point that hand at
  a box up to 3 m away and squeeze: it comes to your hand. Release to throw.
- Colt: the laser now runs from its emitter along the game's own beam (it sat
  to the left).
- Tilted view (e.g. after taking the headset off while paused): hold both
  grips and click the left stick, keep holding about 2 s while looking
  straight ahead; the view is levelled to your head.
- Water: the reflection is looked up by the reflected view direction from each
  eye, so it stays fixed in the world and no longer follows your head.

## Changed after the third headset test (2026-10-14)

- Colours: the picture should no longer look pale or washed out (the eye
  images are now handed to the runtime as sRGB).
- No black flashes: switching weapons, snap turning or recentring keeps the
  last image up instead of a black frame; level fade-ins no longer flicker.
- Hand grenades: right stick click puts a grenade in your other hand (that
  hand's gun is put away); throw it with a throwing motion of that hand. It
  flies toward where your gun hand's laser points, with the game's own arc.
  Click again to put it away.
- Turrets: only the right stick aims them, and they stop when you let go of
  the stick (no more turning on their own, no hand aiming).
- Circular saw: no laser; your hand is on its rear loop handle.
- Colt: its laser starts at the emitter under the barrel, along the game's
  own beam.
- All weapons are 15% smaller.
- Hit-direction arrows and blood marks are no longer drawn.
- Menus: the game's selection sits exactly under the pointer dot (it used to
  drift outward toward the edges).
- Water: the reflection is looked up per eye from a steady camera, so it sits
  at a distance instead of looking painted on the water and swimming with your
  head. Say whether it looks right.
- Multiplayer: other players see your arms reach to your tracked hands (and
  your head turn) on your character.

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
- With one controller switched off, the other hand's gun still fires from its
  muzzle and its laser points where it aims; switching weapons on the wheel no
  longer leaves the old weapon firing too.
- A weapon held alone (minigun, sniper, ... with combo weapons off): squeeze the
  other hand's grip under the barrel to hold it with both hands; it aims along
  your two hands until you let go.
- Shots leave the barrels: zap gun between its prongs, double shotgun, uzi
  and sniper from their barrel tips (the uzi's laser from the tube below).
- Lasers are see-through like the game's own uzi laser; a hand without
  tracking shows no laser; the uzi beam starts at the emitter tube under the
  barrel and runs along the game's own beam.

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
A VR player's character reaches with both arms to that player's hands (arm IK);
watch for twisted elbows or hands that do not meet the weapon.

## Reporting

For each failure note the build, level/save, controller, left or right hand or
eye, what you did, what you expected and what happened. Keep the three logs.
After quitting, the game and `ss2vr_host.exe` should be gone.

Installed candidate: commit `9192578`, version `0.2.11-dev.62d6f37eb521` (2026-10-28).
