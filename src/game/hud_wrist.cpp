// Wrist HUD element tagging (hud_wrist.hpp).
#include "hud_wrist.hpp"
#include "game.hpp"
#include <cstdint>
#include <cstdio>

namespace ss2vr::game::hud_wrist {
namespace {

// Left wrist: the player's own state (health, armor, lives, Netricsa's new
// message icon, air, power-ups) and the left hand's ammo. Right wrist: ammo,
// grenades and bombs, weapon combos, vehicle ammo, score. Everything else
// (messages, prompts, subtitles, boss bar, the Netricsa screen) stays head-mounted.
struct Element {
    const char *name;
    int side;
};
constexpr Element Elements[] = {
    {"CHealthSimpleHudElement", Left},       {"CArmorSimpleHudElement", Left},
    {"CLivesSimpleHudElement", Left},        {"CNetricsaIconHudElement", Left},
    {"CAirSimpleHudElement", Left},          {"CAdrenalineSimpleHudElement", Left},
    {"CFrozenSimpleHudElement", Left},       {"CLeftAmmoHudElement", Left},
    {"CSeriousDamageHudElement", Left},      {"CSeriousJumpHudElement", Left},
    {"CSeriousScoreHudElement", Left},       {"CSeriousSpeedHudElement", Left},
    {"CSeriousStrengthHudElement", Left},    {"CInvulnerabilityHudElement", Left},
    {"CInvisibilityHudElement", Left},       {"CAmmoHudElement", Right},
    {"CRightAmmoHudElement", Right},         {"CBombsSimpleHudElement", Right},
    {"CComboWeaponsSimpleHudElement", Right}, {"CVehicleAmmoSimpleHudElement", Right},
    {"CVehicleSimpleHudElement", Right},     {"CPointsSimpleHudElement", Right},
    {"CCounterSimpleHudElement", Right},     {"CTimeSimpleHudElement", Right},
};
constexpr unsigned Count = sizeof(Elements) / sizeof(Elements[0]);
static_assert(Count <= MaxElements);
constexpr unsigned RenderSlot = 0x28 / 4;

using Render = void(__thiscall *)(void *, void *, void *);
struct Patched {
    void **slot = nullptr;
    Render original = nullptr;
    uintptr_t table = 0;
};
Patched patched[Count]{};
thread_local int side = Head, index = -1;
thread_local const char *element = nullptr;
bool wristMode = true;
float wristScale = .36f;
// The engine's 2D vertex binding (a function pointer variable in Engine).
using Bind = void(__cdecl *)(int, const void *, int, int, int);
Bind *bindVariable = nullptr;
Bind originalBind = nullptr;
thread_local bool boundsValid = false;
thread_local float boundsMin[3], boundsMax[3];
void __cdecl bind(int slot, const void *data, int type, int bytes, int stride) {
    // Positions of a tagged element: float3 (0x87) or float2 (0x86).
    if (slot == 0 && side != Head && data && (type == 0x87 || type == 0x86) && stride >= (type == 0x87 ? 12 : 8) &&
        bytes >= stride) {
        const unsigned n = unsigned(bytes / stride) < 8192u ? unsigned(bytes / stride) : 8192u;
        const auto *bytesIn = static_cast<const uint8_t *>(data);
        for (unsigned i = 0; i < n; ++i) {
            const auto *f = reinterpret_cast<const float *>(bytesIn + size_t(i) * unsigned(stride));
            const float v[3]{f[0], f[1], type == 0x87 ? f[2] : 0.f};
            if (!(v[0] == v[0]) || !(v[1] == v[1]) || !(v[2] == v[2])) continue;
            if (!boundsValid) {
                for (unsigned k = 0; k < 3; ++k) boundsMin[k] = boundsMax[k] = v[k];
                boundsValid = true;
            } else
                for (unsigned k = 0; k < 3; ++k) {
                    if (v[k] < boundsMin[k]) boundsMin[k] = v[k];
                    if (v[k] > boundsMax[k]) boundsMax[k] = v[k];
                }
        }
    }
    originalBind(slot, data, type, bytes, stride);
}
// The variable may be set again by the graphics API (a reset): keep ours on it.
void keepBind() noexcept {
    if (!bindVariable || !*bindVariable || *bindVariable == &bind) return;
    originalBind = *bindVariable;
    *bindVariable = &bind;
}

void __fastcall render(void *self, void *, void *a, void *b) {
    const uintptr_t table = self ? *static_cast<uintptr_t *>(self) : 0;
    for (unsigned i = 0; i < Count; ++i) {
        if (!patched[i].original || patched[i].table != table) continue;
        const int previousSide = side;
        const char *previous = element;
        const int previousIndex = index;
        if (wristMode) keepBind();
        side = wristMode ? Elements[i].side : Head;
        element = Elements[i].name;
        index = int(i);
        boundsValid = false;
        patched[i].original(self, a, b);
        side = previousSide;
        element = previous;
        index = previousIndex;
        boundsValid = false;
        return;
    }
    // Not one of ours (cannot happen: only these tables point here).
}

bool write(void **slot, void *value) noexcept {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) return false;
    *slot = value;
    DWORD unused = 0;
    VirtualProtect(slot, sizeof(void *), old, &unused);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void *));
    return true;
}

} // namespace

bool install(HMODULE game, HMODULE engine) noexcept {
    if (!game) return false;
    if (engine && !bindVariable)
        bindVariable = reinterpret_cast<Bind *>(GetProcAddress(engine, "?_gfxBindVertexArray@SeriousEngine@@3P6AXJPBXW4GfxUsageType@1@JJ@ZA"));
    unsigned done = 0;
    for (unsigned i = 0; i < Count; ++i) {
        if (patched[i].original) { ++done; continue; }
        char symbol[160];
        std::snprintf(symbol, sizeof symbol, "??_7%s@SeriousEngine@@6B@", Elements[i].name);
        auto **table = reinterpret_cast<void **>(GetProcAddress(game, symbol));
        if (!table || !table[RenderSlot]) { log("Wrist HUD: element %s not found", Elements[i].name); continue; }
        const auto original = reinterpret_cast<Render>(table[RenderSlot]);
        if (original == reinterpret_cast<Render>(&render)) continue;
        patched[i] = {table + RenderSlot, original, reinterpret_cast<uintptr_t>(table)};
        if (!write(table + RenderSlot, reinterpret_cast<void *>(&render))) { patched[i] = {}; continue; }
        ++done;
    }
    log("Wrist HUD: %u of %u element renders tagged", done, Count);
    return done == Count;
}

void uninstall() noexcept {
    if (bindVariable && *bindVariable == &bind && originalBind) *bindVariable = originalBind;
    for (auto &p : patched)
        if (p.original && p.slot && write(p.slot, reinterpret_cast<void *>(p.original))) p = {};
}

int currentSide() noexcept { return side; }
int currentIndex() noexcept { return index; }
void configure(bool w, float sc) noexcept {
    wristMode = w;
    if (sc > .05f && sc < 2.f) wristScale = sc;
}
bool wrists() noexcept { return wristMode; }
float scale() noexcept { return wristScale; }
bool takeBounds(float (&minimum)[3], float (&maximum)[3]) noexcept {
    if (!boundsValid) return false;
    for (unsigned k = 0; k < 3; ++k) { minimum[k] = boundsMin[k]; maximum[k] = boundsMax[k]; }
    boundsValid = false;
    return true;
}
const char *currentElement() noexcept { return element; }

} // namespace ss2vr::game::hud_wrist
