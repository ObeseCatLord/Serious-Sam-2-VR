#include "common/hand_wheel.hpp"
#include <cassert>
#include <cstdio>
using namespace ss2vr;
int main() {
    float d = 1;
    // Level hands: no turn.
    assert(handWheelDeflection({-.2f, 1, -.4f}, {.2f, 1, -.4f}, d) && d == 0);
    // Right hand lower: turn right (positive), left hand lower: left.
    assert(handWheelDeflection({-.2f, 1.15f, -.4f}, {.2f, .85f, -.4f}, d) && d > .5f);
    assert(handWheelDeflection({-.2f, .85f, -.4f}, {.2f, 1.15f, -.4f}, d) && d < -.5f);
    // Inside the dead zone (about 5 degrees): nothing.
    assert(handWheelDeflection({-.2f, 1.01f, -.4f}, {.2f, .99f, -.4f}, d) && d == 0);
    // Past full lock: 1.
    assert(handWheelDeflection({-.1f, 1.3f, -.4f}, {.1f, .7f, -.4f}, d) && d == 1);
    // Any facing: the same tilt gives the same turn.
    assert(handWheelDeflection({-.4f, 1.15f, -.2f}, {-.4f, .85f, .2f}, d) && d > .5f);
    // Hands together or one above the other: no wheel.
    assert(!handWheelDeflection({0, 1, -.4f}, {.05f, .7f, -.4f}, d) && d == 0);
    std::puts("hand wheel ok");
}
