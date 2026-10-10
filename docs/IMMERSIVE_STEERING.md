# Immersive steering: requirement and native evidence

## Current production controls and helicopter coverage

Production one-/two-hand Fighter/Saucer handle grabbing is now connected to
successfully submitted native stereo geometry and the borrowed mode2 heading
callback. See VEHICLE_GRIP_INTEGRATION_PLAN.md and NATIVE_HOVER_HEADING.md for
contact, continuity/rearm, source/build proof and runtime limits. These controls
are handles, not a literal steering wheel. The historical observer-only boundary
below predates this source integration; wider actual controls remain required.

The bounded Helicopter_Player investigation inspected its exact model, skeleton,
mechanism, referenced animation set, cockpit-debris model and directly referencing
puppet. Four LODs, all71 LOD0 connected components and cockpit cutaways were
inspected; six CRC-checked resources fully parsed with the existing evaluated
parser. Main plus five bones and nine animations did not establish a defensible
driver stick/yoke/wheel/handgrip. The exterior nose weapon bracket is not admitted
as a driver control. Seat is a null-model attachment, and cockpit debris belongs
to destruction rather than a live control attachment.

This is a finite negative identification, not proof that every possible control
is absent. Thirty-two immediate references remain unopened (debris, textures,
effects, sounds, material/shader and missile puppet); rider assets, world/loose
composition, loaded resource identity and occupied-seat/runtime animation are
uninspected. Exact resource/hash/reference inventories and private previews are
preserved locally. No guessed offset/control or game/editor run was introduced.

A separate bounded default-rider check fully parsed the player parameter resource
and all three stock SeriousSam model variants with the unchanged evaluated parser.
Each model has20 declared hand-parented weapon attachment slots, all with null
child configurations; no separate authored cockpit/control model was established
by this route. Owned bytes/CRC/hashes, parser seals and complete cached decode
equality passed normally and with Python optimization. This does not establish
loaded precedence, occupied rider identity, dynamically attached models, rider
mesh geometry or evaluated ride animations. Those limits remain separate from
the already inspected helicopter model; no new physical control is admitted.

## Virtual steering wheel — 2026-10-10

The owner chose a mod-made wheel for driven rides without authored controls,
textured from existing game art. `tools/blender/steering_wheel.py` builds the
mesh headlessly in Blender and maps it onto regions of the stock HoverFighter
atlas (riveted strip for the rim, panel for spokes, the round dial for the hub);
the game loads that texture at runtime through its own immediate
`resObtainResource` wrapper pattern, so no texture bytes ship. The wheel is drawn
after the lasers in each eye with native simple-shader vertex arrays, with depth.

Ride classes come from the Sam2Game vtable: wheeled/buggy/military car steer from
move X (wheel replaces stick X while held); aircraft, rolling ball, air bike and
hovercraft turn toward look heading (held wheel adds a yaw rate in the mounted
look clamp instead of hand aim). Fighter/Saucer keep authored handles; a
hovercraft-class ride waits 1.5 s for handle admission before showing the wheel.
Grabbing reuses the squeeze availability/re-arm rules of the handle grasp; each
hand keeps its own baseline, so joining/leaving never jumps, and the rim
self-centres on release. A private capture rendered it in both eyes on foot
(preview mode); grasp and steering are unit-tested only. Aircraft/rolling-ball
route choice and feel are user acceptance items.

## Current implementation boundary — 2026-10-10

The existing default-off [render observer](RIDE_RENDER_OBSERVATION.md) now joins
the occupied vehicle's Main/Seat frame to an actual indexed draw, selected LOD,
resource identity and actual Main palette slot. Its schema4 preserves core frames
when draw evidence is absent and permits different eye inventories. Publication
requires original success, all COM Releases, fresh owner/routing/generation checks
and normal native-finally completion. All four products rebuilt;82Debug and
82assert-enabledRelease groups pass. Astra/xhigh source GO, local routing verified,
backend unattested. No vehicle/runtime/input test or controls activation occurred.

Static resource evidence shows the Saucer handles belong to a15-bone surface;
whole-draw SingleAffine is unsuitable. Fighter's one-bone surface has2806 triangles,
exceeding existing2673-triangle idle storage. These are demonstrated narrow policy
incompatibilities. Reuse existing bound-buffer/program ownership with explicit
vehicle capacities and actual indexed Main-weighted subsets; do not widen ID2's
three-entry policy or assume serialized palette slot10 equals a live draw slot.

Loaded GPU bytes/program semantics, defensible grasp and simulation-time input
freshness remain open. The native mode2 look/processed movement route and mode3
sign-based steering remain distinct. Preserve joystick fallback, throttle/aim/fire,
physics and ClientAction while adding availability-loss rearm and smooth hand
transfer. This source association is not a completed physical steering feature.

Owner requirement, 2026-10-07: vehicles with steering wheels must support grabbing
and turning the wheel with either hand or both. Hand transfer should not cause a
steering jump. This feature is not implemented yet.

Current matrix contract: the existing render observer copies canonical matrices
that native DDE30 uses as draw-palette sources. Native bone placement removes
stored inverse bind from canonical P, so canonical is not an ordinary bone-local
frame. A physical-grip adapter should reuse the existing SingleAffine actual draw
palette, bound to occupied ride Main/resource/LOD and proven current surface
buffers/program, rather than a new attachment/bone query or an extra inverse bind.
Optional declared seat metadata remains separate and is not a new grip gate.
Earlier proposed-query notes below are historical; see the corrected current
contract in [RIDE_RENDER_OBSERVATION.md](RIDE_RENDER_OBSERVATION.md).

The [same-world render observation](RIDE_RENDER_OBSERVATION.md) now joins the
current operated hover ride to its renderable, model and selected Main cache.
It is passive/default-off and has not run in a vehicle. Installed controls,
seat-relative geometry and input-time freshness remain separate prerequisites;
the older getter-time receipt does not supply them.

## Existing input route to preserve

The mounted adapter already disables handheld weapon wheels while seated, keeps
seated movement in the vehicle's native input axes, and supplies controller aim
through the original look clamp. Steering should join the existing local input
producer and native ClientAction transport, not add a vehicle physics controller
or another network steering command. Throttle, braking, aim and firing remain
independent of the hand-grab calculation.

The pinned Sam2Game wheeled class retains EnforcePuppetMoveLook at1563D0 and
SetDriveSteerRatioAndLookDir at802C0. The former forwards raw move and look; the
latter stores move at8C and look atA4, preserving pitch adjustment and mode3.
The native wheeled step consumes X for steering and Z for driving.

There is an important limit: the inspected mode3 wheeled steering path selects
zero or a native joint limit based on the input sign and each road-wheel's flags.
It does not prove proportional steering from the input magnitude. A floating
input named "ratio" is insufficient evidence that a90-degree hand turn produces
a proportional front-wheel angle. Do not replace native physics or silently
advertise proportional handling. `tools/verify_vehicle_steering_native.py` checks
36 instructions, exports, class slots and the native joint target import against
the installed binary fingerprint. It installs no adapter.

## Geometry and control admission still needed

A road-wheel physics joint is not the driver's steering wheel. Wheeled class
identity alone does not prove a visible steering wheel, its center, rotation
axis, radius, driver seat, grip locations or current visual angle. Native model,
mechanism, skeleton and attachment metadata must establish those separately.
The local stock inventory is now complete:14 root archives,12,342 entries and
4,385 model/mesh/skeleton/mechanism/parameter entries with CRC, identifier-table
round-trip and serialized object-count checks. Astra/xhigh found no positively
identified driver-wheel geometry. This does not prove an unnamed wheel absent.
Center, axis, rim radius, wheel-to-seat transform and visual angle remain unknown.
No generic wheel placement or guessed model offset is installed.

Notable candidates are Sirius_Car_05/Car_Blue, Sirius_Car_11/transport_vehicle_open,
HoverFighter/Fighter_Final, FlyingSaucer/FlyingSaucer, RollerBall/RollerBall_Vehicle
and Helicopter/Helicopter_Player under the stock Models/Vehicles tree. Named
Seat/Console/CockPit metadata is not a proven attachment/transform. Car_Blue's
referencing parameters serialize aircraft parameters; its filename does not
establish a steerable wheeled ride. Eight serialized wheel-joint templates occur
in turret/shooter models, so template type alone does not identify road or driver
wheels. Catapult's Wheel/Wheels_Front/Wheels_Back names remain geometry candidates,
not driver controls. Runtime precedence, loose overrides, world-embedded content
and mod-subdirectory archives were outside this bounded inventory.

Next content proof: identify an actual visible driver wheel in a fingerprinted
loaded model, its surface/bone/attachment ownership, rim and non-spinning parent
frame relative to the driver seat. Then reuse the guarded native attachment
query with explicit success/current rider/model/seat ownership. The existing
turret-specific gate does not authorize other classes. Private inventory and
asset hashes remain outside public source; no assets/disassembly are published.

Croteam's [game description](https://www.croteam.com/serious-sam-2/) covers varied
vehicles and mountable animals. The publisher's [original manual, pages21–23](https://www.mogelpower.de/manuals/Serious_Sam_2_Handbuch.pdf)
also lists very different vehicle types, including a rollerball and aircraft.
These are background references, not evidence that a specific installed model
has a steering wheel. Match controls to the actual vehicle rather than applying
a car wheel to every ride.

The existing squeeze actions already supply weapon-wheel buttons while on foot.
Their current IPC bit does not distinguish an inactive action from a released
grip. Immersive grabbing needs positive action availability/generation and fresh
release-to-rearm evidence; do not use tracking recovery as an automatic regrab.
The recenter chord must interrupt/release any steering grasp safely.

## Bounded interaction behavior

- Acquire only near the positively identified wheel rim, with a fresh squeeze
  press after a known release. Either hand may acquire first.
- Compute hand motion in the wheel's non-spinning vehicle-relative frame, so
  vehicle translation/rotation is not mistaken for turning the wheel.
- With one hand, use that hand's angular motion around the wheel axis. With two,
  combine their incremental turn without cancellation from opposite grip points.
- Joining, leaving or exchanging hands must preserve the current commanded angle.
  New grips establish their own baseline instead of snapping to an absolute angle.
- Native limits remain authoritative. Reject nonfinite poses, singular radial
  projections, impossible jumps, stale samples and superseded ride/seat/rig owners.
- Loss of one tracked hand releases that grasp; a valid remaining hand can keep
  control. Losing both, leaving the seat, pausing or recentering retires the grasp
  and requires fresh release before reacquisition.
- Ordinary joystick steering remains usable when there is no admitted grasp.
  No input helper, fixture or compilation result constitutes native integration.

The inventory/review used no runtime. Subsequent PC startup tests are recorded
in PC_VALIDATION.md; no steering drive or headset/controller acceptance exists.

## Additional bounded installed-content coverage

Astra/xhigh subsequently indexed17 nested archives with8614 entries, inspected
14 additional metadata objects (eight models, six puppets) and exactly three
world dictionaries/type/reference collections. Loose Content filenames provided
no standalone candidate control geometry. This did not reopen the14 root archives.
No positive driver wheel was identified; this remains a bounded result, not proof
that unnamed or unopened geometry is absent.

InSamnity2's rideable SiriusCar_Ridable_Blue parameters identify aircraft puppet
and aircraft player controls, with a Seat referring to the SiriusCar_Blue model.
That model has Barrel01, Barrel02, EnterPoint, Engine and Seat child references,
an aircraft joint template and a seat-local transform. This establishes a concrete
seat/model chain for an aircraft-class rideable car; it does not identify a driver
wheel or authorize grabbing at the seat transform. Its City PartII world refers
to this puppet and model. The Car.bmf mesh was not reopened in this scope.

Cart wheel/hinge metadata remained unlinked to a driver seat. Renovation's portal
wheel had Open/Close animation and kinematic-body metadata, with no driver
association. FlyingSaucer Console and dropship Cockpit names did not establish a
seat-linked independent control. Scooter_Air was hovercraft parameters and
RollingBall_Player was rolling-ball parameters.

The three collections were InSamnity2 Sirius City PartII, Renovation Sirius City
PartI and Renovation Kronor Compound, inspected at dictionary/type/reference level
without a world entity census. Remaining unopened nested entries include2178
models,588 puppets,46 mechanisms,127 controllers,160 BMFs,44 skeletons and33 worlds;
counts include duplicate logical paths. Runtime resource precedence, the actual
seated instance and baked unnamed control geometry remain unresolved. A positively
identified control mesh/bone and its physical pivot/axis relative to the seat are
still required. No steering geometry or offset has been guessed or installed.


## Completed nested metadata follow-up — 2026-10-08

The subsequent collector searched all2,997 selected nested metadata entries:
2,186 models,594 puppets,46 mechanisms,127 controllers and44 skeletons. It read
177,186,181 bytes through CRC-checked archive reads and retained40,172 names and
167,017 type declaration headers. This supersedes the earlier unopened *metadata*
counts; it does not establish geometry or exhaustive serialized type coverage.
The collector excludes pointer/template and external type declarations and does
not resolve numeric object/member transforms. No driver control was positively
identified. Numeric geometry in160 BMFs and36 worlds remains uninspected; three
of those worlds previously received only dictionary/type/reference inspection.
Counts include duplicate paths;12 noncontributing nested archives were not
independently manifested in the collected JSON.

Astra's bounded review identified one concrete next content gate: the rideable
Sirius car model's referenced `Sirius_Car_05/Meshes/Car.bmf`. A dedicated read-only
investigation now examines that mesh and its immediate model/seat chain. Runtime
resource precedence, a visible driver's control and its pivot/axis/rim remain
unproved. Physical steering remains unimplemented until positive geometry is
established. Vehicle gameplay testing belongs to the user.


## Pinned car mesh inspection — 2026-10-08

The exact referenced `Sirius_Car_05/Meshes/Car.bmf` was subsequently decoded and
privately inspected in shaded and unobscured wire views. Its one LOD contains
415 vertices and374 triangles across three surfaces. Index/attribute buffers
have complete nonoverlapping coverage, valid indices and finite attributes;
calculated bounds match the authored bounds. Every vertex binds solely to the
body bone. No identifiable driver-wheel rim/column or defensible control pivot
was found in this mesh. Main independently verified its pinned bytes/hash and
inspected the preview. This is a bounded geometry result, not absence across all
vehicles/content or proof of active runtime resource precedence.

The immediate rideable-car chain remains aircraft parameters. Its mod Seat child
is an authored attachment marker with null model configuration; it contributes
no control mesh. The stock model has no such Seat child. Neither body bind
translation nor Seat translation is steering geometry. No offset/grab adapter is
installed. The remaining finite edge is an actual visible driver control tied to
a native vehicle/seat. The user has been asked for that vehicle/level identity;
other implementation continues meanwhile. Meshes/previews stay private.

## Second bounded geometry inspection: Car11

Astra/xhigh inspected the exact stock
`Sirius_Car_11/transport_vehicle_open.mdl`, SHA-256
`8231808e24329220f510f2c508df28fe094f04020b724beb1b50d775d3d2fc42`.
Its14 serialized objects/87,871 body bytes roundtrip exactly; all immediate
geometry is embedded in this one resource. One mesh/LOD/section contains1,592
vertices and1,603 triangles across MainBody_01 and Engine_01. Complete finite
position/normal/UV and index buffers reproduce the declared bounds; all nine
connected components and complete/isolate-surface previews were inspected.
Main independently verified member size/CRC/hash, null skeleton, empty child
container and render-object reference, and viewed both complete/component images.

No driver control was positively identified. The ring-like components belong to
the engine surface; they are not a driver-wheel certificate. This resource has
no skeleton, child/Seat attachment, animation or mechanism container. Zero
external mesh/model/skeleton dependencies remain unopened in this immediate
chain. Materials, shaders, textures, collision interpretation, parent/world
composition, loaded precedence and actual seat/gameplay semantics remain
uninspected. Other candidate models also retain their previous coverage limits.
This finite result does not prove every SS2 scene lacks a steering wheel.

No pivot/axis/radius/seat association or physical grab control is justified by
these bytes. A positive actual wheel/vehicle identity remains necessary. Private
resource data, previews and reports stay local; no game/input/vehicle test ran.
Local Astra/xhigh turn tags were verified, with backend routing unattested.

## FlyingSaucer immediate geometry inspection — 2026-10-09

Astra inspected the exact stock FlyingSaucer model, its Console destruction-debris
model and the directly referencing HoverSaucer operator-seat parameters. Main read
the full report, independently checked all three member sizes/CRCs/hashes and
inspected the decoded cockpit/seat preview. Across the three resources, 128 objects
and all four mesh LODs passed structural, finite-value, index and bounds checks.
This does not establish runtime resource precedence or current rider identity.

Two mirrored L-shaped handles are visible immediately in front of the authored
operator seat. Each has 35 vertices and 30 triangles. Their one-hot palette entry
resolves to Main, without an independent handle bone. The seat selects its own
Seat palette entry; authored operator/seat references agree. Console's external
model is destruction debris, not a live attached driver control. These are actual
alternative control candidates, not a driver-wheel or actuation-axis certificate.

All intact immediate geometry is embedded and inspected. Twenty-five dependencies
remain unopened, including the animation set, three other debris models and
materials/effects/projectiles. The next narrow inspection is the exact referenced
animation set's Main/Seat effects. Native actuation, live seat/model attachment,
control frame and input semantics remain separate proofs before grasp admission.
No wheel, pivot, axis, grip offset or control adapter is guessed or installed.
Private geometry, images and reports remain local. No vehicle/runtime/input test
ran. Local Astra/xhigh routing tags were independently checked; backend unattested.

## FlyingSaucer animation follow-up

The exact referenced animation set has now been inspected: 51 objects, 40 envelopes
and 84 keys passed byte/record checks. Fly's Main/Seat keys are constant and match
local bind transforms. Fire_Left/Fire_Right have constant Main keys and omit Seat.
Only child Blades/Engine rotations vary. There is no handle-specific or morph
channel in this set, nor an unopened external dependency within it. These static
facts do not establish runtime selection, blending, missing-channel fallback or a
live handle-to-seat frame. Independent handle actuation and native control mapping
remain unproved. No vehicle or input probe ran; all curves/resources stay private.

## Fighter_Final bounded cockpit inspection — 2026-10-09

Astra/xhigh inspected the exact stock HoverFighter/Fighter_Final model and its
three directly referencing operator-seat puppets. Main read the full report,
independently verified all four member sizes/CRCs/hashes, and inspected the cockpit
preview.130serialized objects, all three embedded mesh LODs,222,732buffer bytes,
exact bounds and all31LOD0components passed the bounded checks.

Two mirrored L-shaped cockpit handgrips sit ahead of the authored Seat. Each has
25vertices/30triangles and uses the sole Main skin palette, with no independent
handle bone or morph. The Seat attachment contributes no model geometry. Embedded
Loading changes the authored Main/Seat relationship under local replacement
interpretation; a fixed bind-pose handle-to-seat frame is not established for
runtime. These are geometry-positive handles, not a certified steering wheel or
actuator axis.23referenced resources remain unopened; none is a missing intact
mesh/skeleton/animation dependency. Runtime scheduling/blending, resource
precedence, current rider/model ownership and native hovercraft input semantics
remain unproved. No pivot/grip offset, wheel or grab adapter is installed.

This does not reopen the completed car/saucer inventories. Keep joystick/native
aim/physics/ClientAction unchanged. Private geometry/resources/previews remain
local; no vehicle or input test ran. Local Astra/xhigh routing was verified;
independent backend attestation was unavailable.

## Hovercraft native control mode — 2026-10-09

Astra/xhigh resolved the two inspected handle-bearing families' native input
route without repeating their geometry census. The exact HoverSaucer and three
HoverFighter parameter resources encode moving abilities41, including horizontal
strafe bit8, and turning preference1. Horizontal-strafe admission precedes that
preference: native EnforcePuppetMoveLook158810 selects the mode2 setter80690.
This differs from the previously inspected wheeled mode3 sign/limit route.

Serialized SHovercraftPuppetParams resolves to CHovercraftPuppetParams. Native
CHovercraftPuppetEntity vtable2A8558 and CHovercraftCharacterEntity vtable2B8420
share EnforcePuppetMoveLook158810, LerpToDesiredVelocityAndLook158BE0 and
CalculateMovingAndRotatingRatio930A0. This verifies shared dispatch when these
parameters are installed; it does not identify a live world entity/subclass.

The original ClientAction/distributor preserves look, raw movement and processed
movement. Mode2 uses processed movement; the native ratio helper converts it
into the craft frame. At aligned headings, X requests lateral translation and
-Z propulsion. Native look yaw requests body heading, with native speed limits,
banking and physical joints retained. Raw X is neither a cockpit-wheel angle nor
a direct yaw-rate command. A wheel-to-X mapping for these families is rejected.

Neither this route nor the prior asset work establishes an independently animated
handle or pivot. The remaining source edge is a bounded passive observation of
local operated-seat/entity identity, installed parameter/model association, mode
and active abilities, then an evaluated Main/Seat frame bound to that same render
instance. Fighter Loading still prevents using fixed bind transforms as live
geometry. Any eventual grab gesture must use the existing mounted-input/native
ClientAction route, preserve joystick/throttle/aim/fire and require fresh squeeze
release/rearm and smooth hand transfer. No physical control adapter is installed.

The owned native fingerprint, reflection/dispatch/instruction/import boundaries
and exact scalar resource spans passed100private static checks. Main read the
completed report and retains private raw evidence outside source. Current local
Astra/xhigh tags were verified; backend identity remains unattested. No vehicle,
input, game, headset or network runtime ran. These facts are a native route
boundary, not full vehicle source or runtime acceptance.

## Borrowed mode observation source implemented

The optional [ride-control scalar observer](RIDE_CONTROL_OBSERVATION.md) now
copies mode/abilities at the existing original clamp boundary, with exactly-once
forwarding and native-finally cleanup. Parameter and renderable values remain
equality tokens. It neither identifies installed resources nor supplies an
evaluated control frame, and has not run in a vehicle. The next association must
join the current native-owned ride, renderable and model instance; pointer
matching across frames is insufficient. Physical grabbing remains unimplemented.
