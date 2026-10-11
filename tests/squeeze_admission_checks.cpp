#include "common/controls.hpp"
#include "common/frame_policy.hpp"
#include <cassert>
#include <limits>
#ifdef NDEBUG
#error Squeeze admission checks require active assertions
#endif
using namespace ss2vr;

// A latest-value squeeze consumer with the contract the grip consumers
// (steering wheel, ride grips, carry) follow: a fresh admitted press starts a
// hold, an epoch change or lost admission cancels it without a release, and
// only a release seen within the same epoch completes it (returning 2).
struct HoldConsumer {
    bool open = false, previousHold = false;
    uint32_t epoch = 0;
    int sample(const Input &input, unsigned hand, bool allowed) {
        if (epoch != input.wheelInputEpoch[hand]) {
            epoch = input.wheelInputEpoch[hand];
            open = previousHold = false;
        }
        const bool held = (input.buttons[hand] & Wheel) != 0;
        if (!epoch || !(input.wheelAdmissionMask & (1u << hand))) {
            open = previousHold = false;
            return -1;
        }
        if (!allowed) {
            open = false;
            previousHold = held;
            return -1;
        }
        int result = -1;
        if (held && !previousHold)
            open = true;
        else if (!held && previousHold && open) {
            open = false;
            result = 2;
        }
        previousHold = held;
        return result;
    }
};

int main() {
    const int ids[]{2, 5};
    SqueezeAdmission producer[2];
    HoldConsumer consumer[2];
    Input input{};
    input.axis[0][1] = input.axis[1][1] = 1;
    auto publish = [&](unsigned hand, SqueezeSample raw, bool allowed = true) {
        input.buttons[hand] = raw.down ? Wheel : 0;
        const bool admitted = producer[hand].sample(raw.sources, raw.valid, raw.down, allowed);
        input.wheelInputEpoch[hand] = producer[hand].epoch;
        input.wheelAdmissionMask &= ~(1u << hand);
        if (admitted) input.wheelAdmissionMask |= 1u << hand;
        ++input.sequence;
    };
    auto deliver = [&](unsigned hand, bool comfort = true) {
        return consumer[hand].sample(input, hand, comfort);
    };
    const auto released = squeezeSample(true, 0, false, 0);
    const auto held = squeezeSample(true, 1, false, 0);
    publish(0, released); assert(deliver(0) == -1);
    publish(0, held); assert(deliver(0) == -1 && consumer[0].open);

    // Negative control: latest-value low alone commits the old highlighted wheel.
    WeaponWheel old;
    old.update(true, 0, 1, ids, 2, true);
    assert(old.update(false, 0, 1, ids, 2, true) == 2);

    // Producer interruptions need not reach the consumer or successfully publish.
    const auto openEpoch = input.wheelInputEpoch[0];
    producer[0].sample(0, false, false, false);
    assert(producer[0].epoch != openEpoch); // Action loss alone must retire.
    publish(0, released);
    assert(input.wheelInputEpoch[0] != openEpoch);
    assert(deliver(0) == -1 && !consumer[0].open);
    assert(deliver(0) == -1); // Re-reading recovery cannot commit.
    publish(0, held); assert(deliver(0) == -1 && consumer[0].open);
    publish(0, released); assert(deliver(0) == 2 && !consumer[0].open);
    assert(deliver(0) == -1);

    // Drop interruption AND producer-observed release. Deliver only the next
    // admitted press: it opens a fresh wheel, never retains the old hold latch.
    publish(0, held); assert(deliver(0) == -1 && consumer[0].open);
    producer[0].invalidate();
    publish(0, released); // Deliberately omitted from consumer delivery.
    publish(0, held);
    assert(deliver(0) == -1 && consumer[0].open);
    publish(0, released); assert(deliver(0) == 2);

    SqueezeAdmission initialProducer;
    HoldConsumer initialConsumer;
    Input first{};
    first.axis[0][1] = 1;
    first.buttons[0] = Wheel;
    initialProducer.sample(1, true, true, true);
    first.wheelInputEpoch[0] = initialProducer.epoch;
    assert(initialConsumer.sample(first, 0, true) == -1 && !initialConsumer.open);
    initialProducer.sample(1, true, false, true); // Consumer misses release.
    assert(initialProducer.sample(1, true, true, true));
    first.wheelAdmissionMask = 1;
    assert(initialConsumer.sample(first, 0, true) == -1 && initialConsumer.open);

    // A held recovery remains unavailable until a genuine current release.
    publish(0, held); assert(deliver(0) == -1 && consumer[0].open);
    publish(0, squeezeSample(false, 0, false, 0)); assert(deliver(0) == -1);
    publish(0, held); assert(deliver(0) == -1 && !consumer[0].open);
    publish(0, held); assert(deliver(0) == -1 && !consumer[0].open);
    publish(0, released); assert(deliver(0) == -1);
    publish(0, held); assert(deliver(0) == -1 && consumer[0].open);

    // Source replacement with all cancelled samples skipped still retires old UI.
    publish(0, squeezeSample(false, 0, true, 0));
    assert(deliver(0) == -1 && !consumer[0].open);
    assert(squeezeSample(false, 0, true, .65f).down == false);
    assert(squeezeSample(false, 0, true, .6501f).down == true);
    assert(squeezeSample(true, 1, true, 0).down);
    assert(squeezeSample(true, 0, true, 1).down);
    assert(!squeezeSample(true, 2, false, 0).valid);
    assert(!squeezeSample(true, 1, true, std::numeric_limits<float>::quiet_NaN()).valid);
    assert(!squeezeSample(false, 0, true, 1.1f).valid);
    assert(squeezeSample(true, 0, false, std::numeric_limits<float>::quiet_NaN()).valid);

    // A consumer can miss a tracking/focus/recenter interruption even while
    // raw squeeze stays held and its source is unchanged.
    publish(0, released); deliver(0);
    publish(0, held); assert(deliver(0) == -1 && consumer[0].open);
    publish(0, held, false);
    const auto retiredEpoch = producer[0].epoch;
    publish(0, held, false);
    assert(producer[0].epoch == retiredEpoch); // No per-frame epoch churn.
    publish(0, released);
    assert(deliver(0) == -1 && !consumer[0].open);
    publish(1, released); deliver(1);
    publish(1, held); assert(deliver(1) == -1 && consumer[1].open);
    publish(0, held, false); deliver(0);
    assert(consumer[1].open); // Independent hand survives.
    assert(deliver(1, false) == -1 && !consumer[1].open); // Existing comfort cancellation.

    // Immutable frame identity includes admission and cancellation epoch.
    Input other = input;
    assert(sameInput(input, other));
    other.wheelAdmissionMask ^= 1;
    assert(!sameInput(input, other));
    other = input; ++other.wheelInputEpoch[1];
    assert(!sameInput(input, other));

    SqueezeAdmission exhausted;
    exhausted.epoch = UINT32_MAX;
    exhausted.invalidate();
    assert(!exhausted.sample(1, true, false, true) && exhausted.epoch == 0);
    assert(!exhausted.sample(1, true, true, true));
}
