#include "menu.hpp"
#include "common/controls.hpp"
#include "game.hpp"
#include <cstring>
#include <limits>

namespace ss2vr::game::menus {
namespace {
using MenuPoll = void(__cdecl *)();
static MenuPoll originalPoll = nullptr;
static int(__cdecl *interactive)() = nullptr;
static void(__cdecl *motion)(float, float) = nullptr;
static void(__cdecl *press)(int, int) = nullptr;
static void(__cdecl *release)(int) = nullptr;
static uintptr_t base = 0;
static SRWLOCK stateLock = SRWLOCK_INIT;
static uintptr_t capturedMenu = 0;
static uint32_t generation = 0;
static bool generationExhausted = false;
static std::atomic<DWORD> pollThread{0};
static TriggerGate trigger;
static bool down = false;
static unsigned selectedHand = 2;
static uint32_t session = 0, reference = 0, pointerGeneration = 0;
static uint32_t actionGeneration = 0;
static std::atomic<int> queuedKey{0};
static int heldKey = 0;
static uint32_t read32(uintptr_t address) {
    uint32_t result;
    std::memcpy(&result, reinterpret_cast<void *>(address), sizeof(result));
    return result;
}
static float readFloat(uintptr_t address) {
    float result;
    std::memcpy(&result, reinterpret_cast<void *>(address), sizeof(result));
    return result;
}
static uintptr_t current() {
    return base ? uintptr_t(read32(base + 0x40a270)) : 0;
}
static bool nativePointerReady() {
    float sensitivity = readFloat(base + 0x3fe200);
    // Driven through the native dispatcher, not OS input: window focus is not required.
    return interactive() && read32(base + 0x3fe214) && std::isfinite(sensitivity) &&
           sensitivity > .0001f && std::isfinite(readFloat(base + 0x3fe20c)) &&
           std::isfinite(readFloat(base + 0x3fe210));
}
static void __cdecl poll() {
    originalPoll(); // Preserve the normal keyboard, mouse and menu event ordering.
    if (!hooksReady.load(std::memory_order_acquire) || !channel.shared)
        return;
    pollThread = GetCurrentThreadId();
    // Sam2Game input mode (RVA 3FDFE8): 1 keyboard, 2 gamepad. It only selects
    // menu/loading prompts (A/B/X/Y icons and "press A" text), so a live VR
    // session shows the game's own controller prompts instead of PC keys.
    // Player input rewrites it every gameplay poll; fire-button flip and aiming
    // use inpUsingGamepad() directly and are not affected.
    if (vrInputLive()) {
        auto *mode = reinterpret_cast<volatile int32_t *>(base + 0x3fdfe8);
        if (*mode == 1 || *mode == 2) *mode = 2;
    }
    // Native menu keys: press on one poll, release on the next.
    if (heldKey) {
        release(heldKey);
        heldKey = 0;
    }
    if (const int key = queuedKey.exchange(0)) {
        if (current() && interactive()) {
            press(key, 0);
            heldKey = key;
        }
    }
    MenuPointer pointer;
    Input input;
    bool sourceEligible = false;
    {
        Lock lock(channel);
        if (!lock) {
            trigger = {};
            if (down) {
                release(0x1f);
                down = false;
            }
            return;
        }
        pointer = channel.shared->pointer;
        input = channel.shared->latest;
        sourceEligible = menuPointerEligible(pointer, input, channel.shared->menu.sequence,
                                             channel.shared->menu.interactionGeneration, GetTickCount64());
    }
    uintptr_t menu = current();
    AcquireSRWLockShared(&stateLock);
    bool sameMenu = menu && menu == capturedMenu && pointer.interactionGeneration == generation;
    ReleaseSRWLockShared(&stateLock);
    bool allowed = sameMenu && sourceEligible && nativePointerReady();
    if (selectedHand != pointer.hand || session != input.session || reference != input.reference ||
        pointerGeneration != pointer.interactionGeneration || actionGeneration != pointer.primaryInputGeneration) {
        allowed = false;
        selectedHand = pointer.hand;
        session = input.session;
        reference = input.reference;
        pointerGeneration = pointer.interactionGeneration;
        actionGeneration = pointer.primaryInputGeneration;
    }
    float sensitivity = readFloat(base + 0x3fe200);
    float x = readFloat(base + 0x3fe20c), y = readFloat(base + 0x3fe210);
    allowed =
        allowed && std::isfinite(sensitivity) && sensitivity > .0001f && std::isfinite(x) && std::isfinite(y);
    bool held = trigger.update(pointer.trigger, allowed);
    if (down && !held) {
        // Native release clears its global mouse-held state and routes through
        // the current menu. Never retain a down state after focus/target loss.
        release(0x1f);
        down = false;
    }
    if (!allowed)
        return;
    // Native Sam2Game cursor motion integrates deltas in a 640x480 menu space.
    // Supply the delta to its own dispatcher, not direct cursor/global writes.
    motion((pointer.u * 640.f - x) / sensitivity, (pointer.v * 480.f - y) / sensitivity);
    if (held && !down) {
        press(0x1f, 0);
        down = true;
    }
}
} // namespace
bool active() {
    bool result = hooksReady.load(std::memory_order_acquire) && current() != 0;
    if (!result) {
        AcquireSRWLockExclusive(&stateLock);
        capturedMenu = 0;
        ReleaseSRWLockExclusive(&stateLock);
    }
    return result;
}
uint32_t captureGeneration() {
    if (!active() || pollThread != GetCurrentThreadId())
        return 0;
    uintptr_t menu = current();
    AcquireSRWLockExclusive(&stateLock);
    if (menu != capturedMenu) {
        capturedMenu = menu;
        if (!generationExhausted && generation != UINT32_MAX)
            ++generation;
        else {
            generation = 0;
            generationExhausted = true;
        }
    }
    uint32_t result = interactive() ? generation : 0;
    ReleaseSRWLockExclusive(&stateLock);
    return result;
}
bool pointerAvailable() {
    if (!active() || !channel.shared)
        return false;
    MenuPointer pointer;
    Input input;
    bool sourceEligible = false;
    {
        Lock lock(channel);
        if (!lock)
            return false;
        pointer = channel.shared->pointer;
        input = channel.shared->latest;
        sourceEligible = menuPointerEligible(pointer, input, channel.shared->menu.sequence,
                                             channel.shared->menu.interactionGeneration, GetTickCount64());
    }
    AcquireSRWLockShared(&stateLock);
    bool same = current() == capturedMenu && pointer.interactionGeneration == generation;
    ReleaseSRWLockShared(&stateLock);
    return same && sourceEligible && nativePointerReady();
}
bool queueKey(int key) {
    if (!key || !active())
        return false;
    queuedKey.store(key);
    return true;
}
bool initialize(HMODULE sam, HookInstaller install) {
    base = reinterpret_cast<uintptr_t>(sam);
    // These internal cdecl contracts are audited against the fingerprinted
    // Sam2Game: input poll 192550, cursor 19E210, press/release 19E0E0/19E170,
    // current-menu IsInteractive adapter 2300F0 (vtable +48).
    interactive = reinterpret_cast<decltype(interactive)>(base + 0x2300f0);
    motion = reinterpret_cast<decltype(motion)>(base + 0x19e210);
    press = reinterpret_cast<decltype(press)>(base + 0x19e0e0);
    release = reinterpret_cast<decltype(release)>(base + 0x19e170);
    return install(sam, 0x192550, reinterpret_cast<void *>(poll), reinterpret_cast<void **>(&originalPoll));
}
} // namespace ss2vr::game::menus
