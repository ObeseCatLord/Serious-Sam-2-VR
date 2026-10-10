# Fighter/Saucer handle steering — user-operated acceptance

This focused supplement to [USER_VR_TEST.md](USER_VR_TEST.md) is a test procedure,
not a test result. No gameplay, headset or multiplayer pass is recorded here.
The integration owner must provide the final candidate identity and eligible
vehicle/seat list before the user starts. Source/offline checks do not establish
physical alignment, comfort or actual native control behavior.

## Setting and admitted coverage

The sample INI explicitly declares the existing code default:

```ini
[Vehicles]
ImmersiveGrips=1
```

Use a separate test profile/save and read its actual INI: overrides may differ.
Restart the VR host/game after changing the setting. Preserve original settings
and saves. Repeat the fallback comparison with `ImmersiveGrips=0` in that test
copy, then restore the recorded test value and restart.

The admitted handle profiles are Fighter and Saucer. Current positive resource
coverage is three Fighter variants and Saucer, constrained by matched submitted
handle geometry and the current operated mode 2 seat/control path. A vehicle name
alone does not guarantee admission. Record the exact variant, model and seat
supplied by the owner; other models, modes, seats and resource substitutions are
unsupported until separately established. These are hover handles. Literal
driver wheels, helicopter controls and universal vehicle coverage remain open.

The gesture changes relative native look heading: one hand uses grip orientation
yaw; two hands use the horizontal span between their grip positions. It retains
native heading clamp, throttle, aim/fire, physics and ClientAction transport.
Heading is coupled to native look; independent yaw aiming, a rotating visual wheel
and proportional road-wheel motion are not established. See
[VEHICLE_GRIP_INTEGRATION_PLAN.md](VEHICLE_GRIP_INTEGRATION_PLAN.md) and
[NATIVE_HOVER_HEADING.md](NATIVE_HOVER_HEADING.md) for the source evidence and limits.

## Preparation and acquisition

Record candidate source/product identity, scene/save, effective setting, vehicle/
variant/seat, platform/runtime/headset/controllers, actual per-hand interaction
profile and network role. Test Fighter and Saucer separately; repeat each supplied
eligible variant/seat. Use matching mod products for a private multiplayer pass.
Device/runtime combinations that are unavailable remain unavailable.

Here **Low** means a newly sampled, available squeeze release with valid current
tracking and control admission; **Down** means a subsequent squeeze press.
Unavailable input and cached neutral are not Low. Fully release the squeeze while
tracking is healthy after mounting or recovering, let the new release be sampled,
then press again. Touch, Index and native Valve Frame suggestions use analog
`squeeze/value` (Down above 0.65); Vive and Microsoft Motion suggestions use
`squeeze/click`. Confirm the runtime's actual binding; suggested paths alone do
not certify hardware. Frame's native suggestion requires its advertised extension;
fallback/remapping and Beyond's actual controllers need separate acceptance.
See [HOST_STATUS.md#controller-bindings](HOST_STATUS.md#controller-bindings).

1. With both squeezes released, confirm ordinary mounted aim and joystick/
   trackpad movement. Keep movement axes centered for the initial grab checks.
2. Test each hand alone: release, position its tracked grip near a visible handle,
   then squeeze. Source acquisition requires contact within 8 cm of an actual
   submitted handle triangle. This is a source threshold, not measured headset
   alignment. Initial acquisition should not introduce a heading jump. Turn the
   controller gently left/right in yaw; observe vehicle heading and native look.
3. Release and press well away from both handles, then move onto a handle while
   still squeezing. It must not acquire automatically. Release near the handle
   with healthy tracking, then press again to acquire.
4. Acquire one hand, then release/press the second hand near the other unoccupied
   handle. Also test both hands acquiring from a fresh released state. Move the
   horizontal hand span gently to turn; twisting one controller alone is not the
   two-hand gesture. Record missed grabs, wrong direction and unstable contact.

## Transfer, cancellation and heading wrap

1. Test left → both → right and right → both → left. Add the second hand before
   releasing the first; repeat while gently turning. Joining/leaving a hand should
   rebase to the returned native heading without a transfer-induced jump. Release
   both, confirm fallback, then reacquire with a fresh Low/Down sequence.
2. Briefly interrupt one controller's grip tracking while two hands are held.
   The still-valid hand may continue after rebasing. Restore the lost hand while
   still squeezing: it must not silently rejoin. Release and press it again near
   an unoccupied handle. Repeat with the other hand and with both hands lost.
3. Separately exercise squeeze availability loss where the runtime allows it,
   head tracking loss, pause/resume, recenter, mount/dismount, vehicle/seat/model
   changes, save/level reload and runtime/session interruption. After cancellation,
   recovery while held must not restore the old grasp. Obtain a new eligible
   release then press near the current handle. Mark unavailable interruption cases
   explicitly; do not infer them from another loss type.
4. Test a nearly vertical one-hand pointing axis and a nearly vertical/coincident
   two-hand span. The source rejects a singular horizontal heading reference;
   recovery requires release/rearm, without an invented turn or retained grasp.
5. Where the scene and physical reach permit, steer across the heading wrap in
   both directions and reverse across it. Use a scene landmark to recognize
   crossing the same orientation after a turn; record heading values only if
   existing diagnostics expose them. Repeat transfers near the crossing and
   gradual multiple-turn input where feasible. Check for sudden reversal, a large
   jump, unexpected waking or lost control. Native angular targets wrap modulo a
   turn; whole-turn winding and identical native wake bookkeeping are not promised.

## Fallback and native actions

With grips released, check joystick/trackpad fallback, native mounted aim,
throttle/lateral movement and user-operated fire. Compare with the separate
`ImmersiveGrips=0` run in the same scene. With an admitted grip, repeat native
throttle, lateral movement, pitch aiming and fire, and verify those actions remain
usable through one-/two-hand transfer. Mode 2 horizontal movement requests lateral
translation; do not interpret it as a road-wheel steering ratio. Loss of handle
admission must leave the ordinary mounted controls available when their own
tracking/input prerequisites recover. A held grip does not promise independent
heading control from both gesture and fallback at once.

Repeat relevant rows as private listen-host local, remote client and dedicated-
server client, with matching products and another mod user observing vehicle
motion and native actions. Successful local grabs do not pass multiplayer rows.
Repeat available Windows and Linux/Proton/device cells separately.

## Results and remaining limits

For each row record **passed**, **failed**, **unavailable** or **not enabled**, with
exact action sequence, expected/observed behavior and whether stock reproduces
it. Keep logs/captures private. Report acquisition, transfer, cancellation/rearm,
wrap and fallback separately, including the disabled-setting comparison.

Actual contact/draw availability, the 100 ms submitted-source age budget's
usability, GPU/perceptual alignment, native wake effects, comfort and vehicle/
multiplayer behavior remain untested by this document. Missing literal wheels
and wider controls are implementation gaps, not user acceptance passes. Do not
count an unavailable/disabled row or simulated fixture as real-device acceptance.
