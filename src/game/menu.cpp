#include "menu.hpp"
#include "common/controls.hpp"
#include "game.hpp"
#include <cstring>
#include <limits>
#include <vector>
#include <cstdio>

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
static std::atomic<int> queuedCommand{0};
static int heldCommand = 0;
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
// The game instance's process queue (game+0x11C): its front process is a movie
// when its vtable is the movie process's (Sam2Game 0x2E2488).
static bool readable(uintptr_t at, size_t bytes) {
    MEMORY_BASIC_INFORMATION m{};
    return at && VirtualQuery(reinterpret_cast<void *>(at), &m, sizeof m) && m.State == MEM_COMMIT &&
           !(m.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && at + bytes <= uintptr_t(m.BaseAddress) + m.RegionSize;
}
static bool nativePointerReady() {
    float sensitivity = readFloat(base + 0x3fe200);
    // Driven through the native dispatcher, not OS input: window focus is not required.
    return interactive() && read32(base + 0x3fe214) && std::isfinite(sensitivity) &&
           sensitivity > .0001f && std::isfinite(readFloat(base + 0x3fe20c)) &&
           std::isfinite(readFloat(base + 0x3fe210));
}
// Bounded runtime evidence for menu input; a user log shows why a selection
// did or did not reach the native dispatcher.
static void logCommand(int command, bool ready) {
    static unsigned count = 0;
    if (count < 64) {
        ++count;
        const uintptr_t instance = read32(base + 0x403350);
        log("Menu command 0x%02x dispatched=%d menu=%d interactive=%d okEnabled=%u", command, ready,
            current() != 0, current() ? interactive() : 0, instance ? read32(instance + 0x24) : 0u);
    }
}
static void logPointer(bool same, bool source, bool ready, bool held, uintptr_t menu, uint32_t pointerGeneration,
                       uint32_t publishedGeneration, uint64_t pointerSequence, uint64_t menuSequence) {
    static unsigned count = 0, previous = ~0u;
    const unsigned state = unsigned(same) | unsigned(source) << 1 | unsigned(ready) << 2 | unsigned(held) << 3;
    if (state == previous || count >= 64) return;
    previous = state;
    ++count;
    log("Menu pointer sameMenu=%d source=%d nativeReady=%d cursor=%u held=%d menu=%08x captured=%08x "
        "generation=%u pointerGeneration=%u published=%u pointerFrame=%llu menuFrame=%llu", same, source, ready,
        read32(base + 0x3fe214), held, unsigned(menu), unsigned(capturedMenu), generation, pointerGeneration,
        publishedGeneration, static_cast<unsigned long long>(pointerSequence), static_cast<unsigned long long>(menuSequence));
}
// A withdrawn pointer (generation 0) or one for the current menu identity.
static bool sameMenuGeneration(const MenuPointer &pointer) {
    return !pointer.interactionGeneration || pointer.interactionGeneration == generation;
}
static void __cdecl poll() {
    originalPoll(); // Preserve the normal keyboard, mouse and menu event ordering.
    if (!hooksReady.load(std::memory_order_acquire) || !channel.shared)
        return;
    pollThread = GetCurrentThreadId();
    applyEyeCanvas(); // Frame-start main-thread point, like the native window-resize event.
    // Sam2Game input mode (RVA 3FDFE8): 1 keyboard, 2 gamepad. It only selects
    // menu/loading prompts (A/B/X/Y icons and "press A" text), so a live VR
    // session shows the game's own controller prompts instead of PC keys.
    // Player input rewrites it every gameplay poll; fire-button flip and aiming
    // use inpUsingGamepad() directly and are not affected.
    if (vrInputLive()) {
        auto *mode = reinterpret_cast<volatile int32_t *>(base + 0x3fdfe8);
        if (*mode == 1 || *mode == 2) *mode = 2;
    }
    // Native menu commands: press on one poll, release on the next.
    if (heldCommand) {
        release(heldCommand);
        heldCommand = 0;
    }
    if (const int command = queuedCommand.exchange(0)) {
        const bool ready = current() && interactive();
        logCommand(command, ready);
        if (ready) {
            press(command, 0);
            heldCommand = command;
        }
    }
    MenuPointer pointer;
    Input input;
    bool sourceEligible = false;
    uint32_t publishedGeneration = 0;
    uint64_t menuSequence = 0;
    uint32_t imageWidth = 0, imageHeight = 0;
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
        publishedGeneration = channel.shared->menu.interactionGeneration;
        menuSequence = channel.shared->menu.sequence;
        imageWidth = channel.shared->menu.width;
        imageHeight = channel.shared->menu.height;
        sourceEligible = menuPointerEligible(pointer, input, menuSequence, publishedGeneration, GetTickCount64());
    }
    uintptr_t menu = current();
    AcquireSRWLockShared(&stateLock);
    bool sameMenu = menu && menu == capturedMenu && pointer.interactionGeneration == generation;
    ReleaseSRWLockShared(&stateLock);
    const bool nativeReady = menu && nativePointerReady();
    bool allowed = sameMenu && sourceEligible && nativeReady;
    if (selectedHand != pointer.hand || session != input.session || reference != input.reference ||
        pointerGeneration != pointer.interactionGeneration || actionGeneration != pointer.primaryInputGeneration) {
        allowed = false;
        selectedHand = pointer.hand;
        session = input.session;
        reference = input.reference;
        pointerGeneration = pointer.interactionGeneration;
        actionGeneration = pointer.primaryInputGeneration;
    }
    // Lab (SS2VR_LAB_MENU_CURSOR=u,v;u,v;...): the pointer at each listed hit of
    // the menu image in turn (3 s each), whatever the controllers do, to see
    // where the game draws its cursor for a known hit (with SS2VR_LAB_MENU_DUMP).
    static const std::vector<std::pair<float, float>> labPoints = [] {
        std::vector<std::pair<float, float>> points;
        char v[256]{};
        if (GetEnvironmentVariableA("SS2VR_LAB_MENU_CURSOR", v, sizeof v))
            for (char *at = v; *at;) {
                float u = 0, w = 0;
                if (std::sscanf(at, "%f,%f", &u, &w) == 2) points.push_back({u, w});
                char *next = std::strchr(at, ';');
                if (!next) break;
                at = next + 1;
            }
        return points;
    }();
    if (!labPoints.empty() && menu && nativeReady) {
        const auto &point = labPoints[(GetTickCount64() / 3000) % labPoints.size()];
        pointer.u = point.first; pointer.v = point.second;
        allowed = true;
        static uint64_t logged = 0;
        if (GetTickCount64() - logged > 1000) {
            logged = GetTickCount64();
            log("Lab menu cursor u=%.3f v=%.3f image=%ux%u", point.first, point.second, imageWidth, imageHeight);
        }
    }
    float sensitivity = readFloat(base + 0x3fe200);
    float x = readFloat(base + 0x3fe20c), y = readFloat(base + 0x3fe210);
    allowed =
        allowed && std::isfinite(sensitivity) && sensitivity > .0001f && std::isfinite(x) && std::isfinite(y);
    // The host withdraws the pointer for an XR frame that did not submit the
    // menu quad (upload in flight, dropped frame). Hold the click state across
    // such a short gap on the same menu instead of releasing a press mid-click.
    static uint64_t lastAllowed = 0;
    const uint64_t now = GetTickCount64();
    if (allowed)
        lastAllowed = now;
    else if (menu && menu == capturedMenu && sameMenuGeneration(pointer) && now - lastAllowed < 150)
        return;
    bool held = trigger.update(pointer.trigger, allowed);
    if (menu) logPointer(sameMenu, sourceEligible, nativeReady, held, menu, pointer.interactionGeneration,
                         publishedGeneration, pointer.menuSequence, menuSequence);
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
    // The menu image is the whole canvas (scaled to 4:3, bridge), as is the
    // cursor's space: the mapping is proportional.
    const MenuCursor target = menuCursor(pointer.u, pointer.v);
    motion((target.x - x) / sensitivity, (target.y - y) / sensitivity);
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
bool movieOnTop() {
    if (!base) return false;
    const uintptr_t game = readable(base + 0x403350, 4) ? read32(base + 0x403350) : 0;
    const uintptr_t queue = readable(game + 0x11c, 4) ? read32(game + 0x11c) : 0;
    const uintptr_t front = readable(queue, 4) ? read32(queue) : 0;
    return readable(front, 4) && read32(front) == base + 0x2e2488;
}
bool worldHidden() {
    return base && vrInputLive() && levelActive() && current() && !movieOnTop();
}
// Sam2Game 19E010: whether a menu on the stack hides the world (cdecl, no args).
static int(__cdecl *originalHidesWorld)() = nullptr;
static int __cdecl hidesWorld() {
    const bool hide = worldHidden();
    static unsigned traced = 0;
    static int previous = -1;
    if (int(hide) != previous && traced < 16) {
        previous = int(hide); ++traced;
        log("Menu hides world=%d (vr=%d level=%d menu=%d movie=%d)", int(hide), int(vrInputLive()), int(levelActive()),
            int(current() != 0), int(movieOnTop()));
    }
    if (hide) {
        clearCanvasTarget(); // The last world frame would stay behind the menu otherwise.
        return 1;
    }
    return originalHidesWorld();
}
bool queueCommand(int command) {
    // A movie in front never reads menu commands; one queued now would fire on
    // the menu after it (a stray selection). The bridge posts a key instead.
    if (movieOnTop()) { queuedCommand.store(0); return false; }
    if (!command || !active())
        return false;
    queuedCommand.store(command);
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
    if (!install(sam, 0x19e010, reinterpret_cast<void *>(hidesWorld), reinterpret_cast<void **>(&originalHidesWorld)))
        log("Menu world hiding hook unavailable: the pause menu shows over its flat world copy");
    return install(sam, 0x192550, reinterpret_cast<void *>(poll), reinterpret_cast<void **>(&originalPoll));
}
} // namespace ss2vr::game::menus
