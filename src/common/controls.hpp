#pragma once
#include "math.hpp"
#include "weapon_wheel.hpp"
namespace ss2vr {
// Input provenance only: native gameplay still owns firing and zoom. A lost
// action or changed logical stream cannot reuse an earlier neutral witness.
struct ActionStream {
    uint32_t generation = 1;
    bool active = false;
    void invalidate() {
        generation = generation && generation != UINT32_MAX ? generation + 1 : 0;
        active = false;
    }
    bool sample(bool available) {
        if (active && !available)
            invalidate();
        active = available && generation != 0;
        return active;
    }
};
struct PrimaryActionSample {
    uint32_t generation = 0;
    bool active = false;
    float value = 0;
};
// An out-of-domain runtime value is action loss, never a clamped physical
// release or press. Preserve the stream boundary even if publication skips
// the invalid sample and the consumer next sees a recovered held trigger.
inline PrimaryActionSample primaryActionSample(ActionStream &stream, bool available, float value) {
    const bool active = stream.sample(available && std::isfinite(value) && value >= 0.f && value <= 1.f);
    return {stream.generation, active, active ? value : 0.f};
}
// One producer owns squeeze release admission. The epoch makes cancellation
// survive latest-value publication/coalescing; consumers only compare identity.
struct SqueezeSample {
    uint32_t sources = 0;
    bool valid = false, down = false;
};
// Index grips report squeeze force (a resting hand reads ~0), so they take a
// lower threshold than curl-style analog grips.
inline SqueezeSample squeezeSample(bool clickActive, uint32_t clickDown, bool analogActive, float value,
                                   float threshold = .65f) {
    return {(clickActive ? 1u : 0u) | (analogActive ? 2u : 0u),
            (!clickActive || clickDown <= 1) && (!analogActive ||
                (std::isfinite(value) && value >= 0.f && value <= 1.f)),
            (clickActive && clickDown == 1) ||
                (analogActive && std::isfinite(value) && value > threshold)};
}
struct SqueezeAdmission {
    uint32_t epoch = 1, sources = 0;
    bool eligible = false, armed = false;
    void invalidate() {
        epoch = epoch && epoch != UINT32_MAX ? epoch + 1 : 0;
        sources = 0;
        eligible = armed = false;
    }
    bool sample(uint32_t sourceMask, bool valuesValid, bool down, bool allowed) {
        const bool current = sourceMask && valuesValid && allowed;
        if (sourceMask != sources || (eligible && !current)) {
            invalidate();
        }
        sources = sourceMask;
        eligible = current && epoch != 0;
        if (!eligible) {
            armed = false;
            return false;
        }
        if (!down) armed = true; // Positive, currently available release only.
        return armed;
    }
};
inline bool primaryActionEligible(const Input &input, unsigned hand) {
    return hand < 2 && (input.primaryActiveMask & (1u << hand)) && input.primaryInputGeneration[hand];
}
// A causal sample boundary, not another control gate. Existing gates must see
// a new eligible release; rereading cached neutral cannot arm after interruption.
struct InputSampleBoundary {
    uint64_t session = 0, reference = 0, sequence = 0, tickMs = 0;
    uint32_t producer = 0;
    bool permits(const Input &input, uint64_t now, uint32_t currentProducer) const {
        return !tickMs || (input.tickMs > tickMs && input.tickMs <= now &&
            ((currentProducer != producer || input.session != session || input.reference != reference) ||
             input.sequence > sequence));
    }
};
inline int nativeCommandQuery(float value, float previous, int mode, bool compatible) {
    if (!compatible)
        return 0; // Both halves of an edge belong to the same native transaction.
    const bool active = value > 0;
    return mode == 0 ? active : mode == 1 ? (active && previous <= 0) : (!active && previous > 0);
}
inline bool primaryCommandHistoryCompatible(const Input &previous, const Input &current, unsigned hand,
                                             uint32_t previousEpoch, uint32_t currentEpoch, bool handheld) {
    return primaryActionEligible(previous, hand) && primaryActionEligible(current, hand) &&
           previous.primaryInputGeneration[hand] == current.primaryInputGeneration[hand] &&
           (!handheld || previousEpoch == currentEpoch);
}
inline bool recenterHeld(const Input &input) {
    return ((input.buttons[0] | input.buttons[1]) & Button::Recenter) != 0;
}
// Same horizontal convention for native bindings and an admitted local input
// adapter: right/up/backward, with calibrated snap/smooth turn applied once.
inline Vec3 horizontalStickMovement(const Input &input, bool blocked, float turn) {
    const float x=input.axis[0][0], y=input.axis[0][1];
    if (blocked || !std::isfinite(turn) || !std::isfinite(x) || !std::isfinite(y) ||
        std::abs(x)>1.f || std::abs(y)>1.f) return {};
    return rotate(yaw(turn), Vec3{x,0,-y});
}
// Coordinates and trigger belong to the same submitted XR input sample.
// Newer input may invalidate focus/tracking, but cannot supply another click.
// The game republishes an unchanged menu every ~33 ms while the host presents
// at display rate, so the presented frame normally trails the newest copy by a
// frame or two. The menu identity (interaction generation) pins the layout; a
// short bounded lag keeps clicks working, older or future frames are rejected.
inline constexpr uint64_t MenuPointerFrameLag = 8;
inline bool menuPointerEligible(const MenuPointer &pointer, const Input &input, uint64_t menuSequence,
                                uint32_t generation, uint64_t now) {
    return pointer.active && pointer.hand < 2 && pointer.inputSequence && pointer.menuSequence &&
           pointer.menuSequence <= menuSequence && menuSequence - pointer.menuSequence <= MenuPointerFrameLag &&
           generation &&
           pointer.interactionGeneration == generation && input.focused && input.headValid &&
           input.handValid[pointer.hand] && primaryActionEligible(input, pointer.hand) &&
           pointer.primaryInputGeneration == input.primaryInputGeneration[pointer.hand] &&
           !recenterHeld(input) && pointer.session == input.session &&
           pointer.reference == input.reference && pointer.inputSequence <= input.sequence &&
           pointer.tickMs <= now && input.tickMs <= now && now - pointer.tickMs < 150 &&
           now - input.tickMs < 150 && std::isfinite(pointer.trigger) && pointer.trigger >= 0 &&
           pointer.trigger <= 1 && std::isfinite(pointer.u) && std::isfinite(pointer.v) && pointer.u >= 0 &&
           pointer.u <= 1 && pointer.v >= 0 && pointer.v <= 1;
}
// The native menu cursor position (640x480 menu space) under a pointer hit at
// (u, v) of the published menu image. That image is the 2D band the game
// letterboxes into its canvas (gfx_fAspectRatio), but the cursor's 640x480
// spans the whole canvas: V482, canvas 1344x1512, band 1344x754, a hit at
// v 0.30 drew the cursor at 0.11 (twice as far from the centre). The hit is
// mapped into the canvas (band centred), so the cursor sits under the pointer.
struct MenuCursor { float x, y; };
inline MenuCursor menuCursor(float u, float v, uint32_t imageWidth = 0, uint32_t imageHeight = 0,
                             uint32_t canvasWidth = 0, uint32_t canvasHeight = 0) {
    float sx = 1, sy = 1;
    if (imageWidth && imageHeight && canvasWidth && canvasHeight) {
        const float image = float(imageWidth) / float(imageHeight), canvas = float(canvasWidth) / float(canvasHeight);
        if (canvas < image) sy = canvas / image; // Letterboxed band (taller canvas).
        else sx = image / canvas;                // Pillarboxed band (wider canvas).
    }
    const auto clamp01 = [](float t) { return t < 0 ? 0.f : t > 1 ? 1.f : t; };
    return {640.f * clamp01(.5f + (u - .5f) * sx), 480.f * clamp01(.5f + (v - .5f) * sy)};
}
// Both stick clicks are a chord only when pressed together (the second within
// WindowMs of the first): the left click is held to sprint, and a right click
// meanwhile (grenade, sniper zoom) recentred the view. A stick that was part of
// a chord stays a plain stick until released: letting go of the left first
// left the right's click to take a grenade on its release.
struct StickChord {
    static constexpr uint64_t WindowMs = 200;
    uint64_t pressedAt[2]{};
    bool down[2]{}, held = false, spent[2]{};
    bool update(bool left, bool right, uint64_t now) {
        const bool clicks[2]{left, right};
        for (unsigned h = 0; h < 2; ++h) {
            if (clicks[h] && !down[h]) pressedAt[h] = now;
            down[h] = clicks[h];
            if (!clicks[h]) spent[h] = false;
        }
        if (!left || !right) held = false;
        else if (!held && !spent[0] && !spent[1])
            held = (pressedAt[0] > pressedAt[1] ? pressedAt[0] - pressedAt[1] : pressedAt[1] - pressedAt[0]) <= WindowMs;
        return held;
    }
};
// Recentre: both grips plus the left stick click, or both stick clicks
// together. Holding either chord 2 s also levels the view (host).
inline void applyRecenterChord(Input &input, bool clicked, StickChord &sticks, uint64_t now) {
    const bool grips = (input.buttons[0] & Wheel) && (input.buttons[1] & Wheel) && clicked;
    const bool both = sticks.update((input.buttons[0] & Sprint) != 0, (input.buttons[1] & Sprint) != 0, now);
    if (input.focused && (grips || both)) {
        for (unsigned h = 0; h < 2; ++h) sticks.spent[h] = sticks.spent[h] || (input.buttons[h] & Sprint) != 0;
        input.buttons[0] = (input.buttons[0] & ~Sprint) | Recenter;
        input.buttons[1] &= ~Sprint;
        // Keep physical trigger and grip state: neither a fake trigger release
        // nor a fake new grip press may be synthesized when the chord ends.
    }
    for (unsigned h = 0; h < 2; ++h)
        if (sticks.spent[h]) input.buttons[h] &= ~Sprint;
}
// Observe raw input independently of gated intent. Hysteresis in [.2,.65]
// preserves the latch, but is never evidence of a new physical release.
inline bool samplePrimaryNeutral(float value, bool eligible, bool newSample,
                                 bool &physicalDown, uint32_t &releasedSerial) {
    if (!eligible || !newSample || !std::isfinite(value))
        return false;
    if (value > .65f)
        physicalDown = true;
    else if (value < .2f) {
        physicalDown = false;
        if (releasedSerial != UINT32_MAX)
            ++releasedSerial;
        return true;
    }
    return false;
}
struct TriggerGate {
    bool armed = false, down = false;
    bool update(float value, bool allowed) {
        if (!allowed || !std::isfinite(value)) {
            armed = false;
            down = false;
            return false;
        }
        if (value < .2f) {
            armed = true;
            down = false;
        } else if (armed && value > .65f)
            down = true;
        return down;
    }
};
// The existing neutral gate with per-weapon sample provenance. Re-reading an
// old neutral after equip, action loss or a blocked wheel cannot arm zoom.
struct HeldZoomInput {
    TriggerGate gate;
    InputSampleBoundary boundary;
    uint32_t weapon = 0, rig = 0, action = 0, session = 0, reference = 0, producer = 0;
    uint64_t sampledSequence = 0;
    bool sample(const Input &input, unsigned hand, bool allowed, uint32_t nativeWeapon,
                uint32_t trackingGeneration, uint32_t inputProducer, uint64_t now) {
        if (hand >= 2) return false;
        const bool changed = weapon != nativeWeapon || rig != trackingGeneration ||
            action != input.zoomInputGeneration[hand] || session != input.session ||
            reference != input.reference || producer != inputProducer;
        weapon = nativeWeapon; rig = trackingGeneration; action = input.zoomInputGeneration[hand];
        session = input.session; reference = input.reference; producer = inputProducer;
        allowed = allowed && nativeWeapon && trackingGeneration && action && input.session &&
            input.focused && input.headValid && input.handValid[hand] &&
            (input.zoomActiveMask & (1u << hand)) && input.tickMs <= now;
        if (changed || !allowed) {
            gate = {};
            boundary = {input.session, input.reference, input.sequence, now, inputProducer};
            sampledSequence = input.sequence;
            return false;
        }
        if (!boundary.permits(input, now, inputProducer)) return false;
        if (sampledSequence != input.sequence) {
            sampledSequence = input.sequence;
            return gate.update((input.zoomDownMask & (1u << hand)) ? 1.f : 0.f, true);
        }
        return gate.down;
    }
};
// The host's source button is the exact SAME logical action sampled for zoom.
// Consume only that action on an admitted native sniper hand; another hand or
// a distinct jump/use action remains available even before zoom has rearmed.
inline uint32_t contextualSniperButtons(const Input &input, unsigned hand, bool nativeSniper) {
    if (hand >= 2) return 0;
    const uint32_t source = input.zoomSourceButton[hand];
    const bool known = source == Sprint || source == Jump;
    return input.buttons[hand] & ~(nativeSniper && known ? source : 0u);
}
struct PressGate {
    bool armed = false, down = false;
    bool pressed(bool held, bool allowed) {
        if (!allowed) {
            armed = down = false;
            return false;
        }
        if (!held) {
            armed = true;
            down = false;
            return false;
        }
        bool result = armed && !down;
        down = true;
        return result;
    }
};
enum class MenuAction { None, Back, Confirm, Up, Down, Left, Right };
struct MenuNavigation {
    PressGate back, use;
    TriggerGate trigger;
    bool triggerDown = false, axisArmed = false, axisDown = false;
    bool wasMenu = false;
    uint32_t primaryGeneration[2]{};
    MenuAction sample(const Input &input, bool menuVisible, bool allowed, bool pointerMode = false) {
        if (!allowed || menuVisible != wasMenu) {
            back = {};
            use = {};
            trigger = {};
            triggerDown = axisArmed = axisDown = false;
        }
        wasMenu = menuVisible;
        for (unsigned hand = 0; hand != 2; ++hand)
            if (primaryGeneration[hand] != input.primaryInputGeneration[hand]) {
                primaryGeneration[hand] = input.primaryInputGeneration[hand];
                trigger = {};
                triggerDown = false;
            }
        const auto buttons = input.buttons[0] | input.buttons[1];
        // The game shows gamepad prompts (A select, B back): the physical A and
        // B do that. Index (it has a jump button): right A jumps, right B uses;
        // Touch: right A uses, right B opens the wheel. Menu goes back too.
        const bool jumpProfile = (buttons & JumpBound) != 0;
        const bool aButton = jumpProfile ? (input.buttons[1] & Jump) != 0 : (buttons & Use) != 0;
        const bool bButton = jumpProfile ? (input.buttons[1] & Use) != 0 : (input.buttons[1] & WheelOpen) != 0;
        // B is Back only on a menu (in play it is Use or the wheel); Menu also pauses.
        bool goBack = back.pressed((buttons & Menu) != 0 || (bButton && menuVisible), allowed);
        bool confirm = use.pressed(aButton, allowed && menuVisible);
        const bool leftActive = primaryActionEligible(input, 0), rightActive = primaryActionEligible(input, 1);
        bool fire = trigger.update(std::max(leftActive ? input.trigger[0] : 0.f,
                                           rightActive ? input.trigger[1] : 0.f),
                                   allowed && menuVisible && !pointerMode && (leftActive || rightActive));
        confirm = confirm || (fire && !triggerDown);
        triggerDown = fire;
        MenuAction direction = MenuAction::None;
        const float x = input.axis[0][0], y = input.axis[0][1];
        if (allowed && menuVisible && std::isfinite(x) && std::isfinite(y)) {
            float extent = std::max(std::abs(x), std::abs(y));
            if (extent < .3f) {
                axisArmed = true;
                axisDown = false;
            } else if (extent > .75f && axisArmed && !axisDown) {
                direction = std::abs(y) >= std::abs(x) ? (y > 0 ? MenuAction::Up : MenuAction::Down)
                                                       : (x > 0 ? MenuAction::Right : MenuAction::Left);
                axisDown = true;
            }
        } else
            axisArmed = axisDown = false;
        // At most one native key tap per presentation; no truncated simultaneous taps.
        if (goBack)
            return MenuAction::Back;
        if (confirm)
            return MenuAction::Confirm;
        return direction;
    }
};
// The grip jumps on foot: an admitted fresh squeeze jumps while held, unless
// that hand carries (or is starting to carry) an object. A squeeze held across
// admission loss, a carry or a vehicle ride never jumps until pressed again.
struct GripJump {
    bool previous = false, jumping = false;
    bool update(bool down, bool admitted, bool onFoot, bool carries) {
        const bool pressed = down && !previous;
        previous = down;
        if (!down || !admitted || !onFoot || carries)
            jumping = false;
        else if (pressed)
            jumping = true;
        return jumping;
    }
};
// Producer gate for the hold-to-open wheel button. A button lost or disallowed
// while held is not a release: the hand stays blocked (WheelOpen withheld,
// blockedWheels set) until the button is seen released again, so a consumer
// that coalesces samples can never commit a selection on the loss itself.
struct WheelOpenGate {
    bool down = false, blocked = false;
    // Returns whether this hand's wheel is blocked; `down` is the published bit.
    bool sample(bool available, bool pressed, bool allowed) {
        if (!available || !allowed) {
            blocked = blocked || down;
            down = false;
            return blocked;
        }
        if (blocked && !pressed)
            blocked = false;
        down = pressed && !blocked;
        return blocked;
    }
};
// Per-hand wheel on its WheelOpen button. A fresh press opens a ring ahead of
// the hand; pointing at (or reaching to) an item, or a deflected thumbstick,
// hovers it and release commits it. Releasing over nothing cancels. Native
// code validates the choice.
struct WeaponWheel {
    bool open = false, previousHold = false;
    int hover = -1;    // WheelUi encoding: weapon list index or wheelBubbleHover().
    int selected = -1; // Weapon id committed by this update, else -1.
    WheelBubble action = WheelBubble::None; // Bubble committed by this update.
    int hoverSlot = -1;
    float outer = 1; // Bubble side: +1 right hand, -1 left hand.
    WheelRing ring;
    void cancel() {
        open = false;
        hover = hoverSlot = -1;
        selected = -1;
        action = WheelBubble::None;
        ring.anchored = false;
    }
    int sample(const Input &input, unsigned hand, const int *ids, uint32_t count, bool allowed,
               uint32_t bubbles = 0) {
        if (hand >= 2) { cancel(); return -1; }
        outer = hand ? 1.f : -1.f;
        return update((input.buttons[hand] & WheelOpen) != 0, input.axis[hand][0], input.axis[hand][1],
                      ids, count, allowed && !(input.blockedWheels & (1u << hand)), wheelPointer(input, hand),
                      bubbles);
    }
    int update(bool held, float x, float y, const int *ids, uint32_t count, bool allowed,
               const WheelPointer &pointer = {}, uint32_t bubbles = 0) {
        selected = -1;
        action = WheelBubble::None;
        if (!allowed || !std::isfinite(x) || !std::isfinite(y) || count > WeaponCount) {
            cancel();
            previousHold = held;
            return -1;
        }
        if (held && !previousHold) {
            open = true;
            hoverSlot = -1;
            // Anchored where the hand is now; without a pose only the stick hovers.
            ring.anchored = pointer.valid;
            ring.frame = pointer.valid ? wheelRingFrame(pointer) : Pose{};
            ring.count = 0;
        }
        if (open) {
            WheelSlot previous[WheelSlotCount];
            const uint32_t previousCount = ring.count;
            std::copy_n(ring.slot, previousCount, previous);
            ring.layout(ids, count, bubbles & WheelAllBubbles, outer);
            bool moved = ring.count != previousCount;
            for (uint32_t i = 0; !moved && i != ring.count; ++i)
                moved = ring.slot[i].weapon != previous[i].weapon || ring.slot[i].bubble != previous[i].bubble;
            if (moved)
                hoverSlot = -1; // Inventory changed under the ring; never keep a moved slot.
            const bool stick = std::sqrt(x * x + y * y) >= WheelStickDeadZone;
            hoverSlot = stick ? ring.pickStick(x, y, hoverSlot)
                      : pointer.valid ? ring.pick(pointer.hand, pointer.aim.p, pointer.aim.q, true, hoverSlot) : -1;
            hover = -1;
            if (hoverSlot >= 0) {
                const auto &slot = ring.slot[hoverSlot];
                hover = slot.bubble != WheelBubble::None ? wheelBubbleHover(slot.bubble) : slot.index;
            }
        }
        if (!held && previousHold && open) {
            if (hoverSlot >= 0 && uint32_t(hoverSlot) < ring.count) {
                const auto &slot = ring.slot[hoverSlot];
                if (slot.bubble != WheelBubble::None)
                    action = slot.bubble;
                else if (slot.index >= 0 && uint32_t(slot.index) < count && ids[slot.index] == slot.weapon)
                    selected = slot.weapon;
            }
            open = false;
            hover = hoverSlot = -1;
            ring.anchored = false;
        }
        previousHold = held;
        return selected;
    }
};
} // namespace ss2vr
