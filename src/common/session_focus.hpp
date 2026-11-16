#pragma once
#include "protocol.hpp"
namespace ss2vr {
// Input::focused. In both non-zero states the runtime shows this application:
// the head is tracked and the world is rendered. Only InputFocused carries the
// controllers. A runtime menu over the application (the SteamVR dashboard)
// takes input focus and leaves the session visible: the world stays up and
// every control of the sample is unavailable. Rendering tests the value for
// non-zero; whatever acts on a control needs inputFocused.
enum SessionFocus : uint32_t { SessionHidden = 0, InputFocused = 1, SessionVisible = 2 };
constexpr uint32_t sessionFocus(bool visible, bool focused) {
    return focused ? InputFocused : visible ? SessionVisible : SessionHidden;
}
inline bool inputFocused(const Input &input) { return input.focused == InputFocused; }
// The head, identity and stream generations of a sample without its controls:
// unavailable hands and actions (not released ones), centred sticks, no buttons.
inline Input withoutControls(Input input) {
    for (unsigned h = 0; h != 2; ++h) {
        input.hand[h] = input.grip[h] = {};
        input.handValid[h] = input.gripValid[h] = 0;
        input.trigger[h] = input.axis[h][0] = input.axis[h][1] = 0;
        input.buttons[h] = 0;
    }
    input.primaryActiveMask = input.zoomActiveMask = input.zoomDownMask = input.wheelAdmissionMask = 0;
    return input;
}
// What the game may act on: a sample without input focus never moves, turns,
// fires or clicks, whatever its fields hold.
inline Input admittedInput(const Input &input) { return inputFocused(input) ? input : withoutControls(input); }
} // namespace ss2vr
