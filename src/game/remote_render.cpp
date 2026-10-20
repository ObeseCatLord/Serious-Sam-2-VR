#include "native_memory.hpp"
#include <cstdio>
#include "native_finally.hpp"
#include "remote_render.hpp"
#include "common/idle_projection_source.hpp"
#include "common/build_contract.hpp"

#include "common/model_tree.hpp"
#include "common/head_palette.hpp"
#include "common/arm_ik.hpp"
#include "common/palette_provenance.hpp"
#include "common/presentation_identity.hpp"
#include "common/frame_policy.hpp"
#include "common/winproc.hpp"
#include "game.hpp"
#include "multiplayer.hpp"
#include "native_tracking.hpp"
#include "scope_observer.hpp"
#include "idle_observer.hpp"
#include <array>
#include <atomic>
#include <cstring>
#include <span>
#include <optional>
#include <vector>

extern "C" const ss2vr::BuildContract ss2vrBuildContract;

namespace ss2vr::game::remote_render {
namespace {

constexpr size_t MaxBindings = 18;
constexpr size_t MaxModelRecords = 2048;
constexpr size_t LeftWeaponHandle = 0x804;
constexpr size_t RightWeaponHandle = 0x800;
constexpr size_t WeaponId = 0xb4;
constexpr size_t WeaponPropertiesResource = 0x64;

using StringId = uint32_t *(__cdecl *)(uint32_t *, const char *);
using VoidCdecl = void(__cdecl *)();
using HandleResolve = void *(__cdecl *)(uint32_t);
using PointerHandle = uint32_t(__cdecl *)(void *);
using IntThis = int(__thiscall *)(void *);
using ModelRenderable = void *(__thiscall *)(void *);
using ModelInstance = void *(__thiscall *)(void *);
using WeaponTool = void *(__thiscall *)(void *, int);
using ToolFileStemId = uint32_t *(__thiscall *)(void *, uint32_t *);
using ChildName = uint32_t *(__cdecl *)(uint32_t *, const void *);
using ChildOffset = Pose *(__cdecl *)(Pose *, const void *);
using ChildInstance = void *(__cdecl *)(const void *);

// Engine's render-list item. Only world is ever written, after a complete
// portable subtree retarget succeeds.
struct NativeModelRecord {
    uint32_t unused;
    int32_t parent;
    uint8_t beforeWorld[0x1c];
    Matrix34 world;
    void *instance;
    void *descriptor;
    uint8_t tail[0x0c];
};
static_assert(offsetof(NativeModelRecord, parent) == 0x04);
static_assert(offsetof(NativeModelRecord, world) == 0x24);
static_assert(offsetof(NativeModelRecord, instance) == 0x54);
static_assert(offsetof(NativeModelRecord, descriptor) == 0x58);
static_assert(sizeof(NativeModelRecord) == 0x68);

struct Binding {
    uint32_t playerHandle = 0;
    uint32_t handHandle[2]{};
    int16_t nativeId[2]{-1, -1};
    uint32_t toolId[2]{};
    bool handEligible[2]{};
    void *bodyModelInstance = nullptr;
    RiderIdentity rider;
    multiplayer::Sample sample;
};

struct FrozenBinding {
    Binding binding;
    Pose body;
    bool bodyValid = false;
};

template <class T> bool symbol(HMODULE module, const char *name, T &out) {
    out = loadProc<T>(module, name);
    return out != nullptr;
}

static VoidCdecl originalModelPass = nullptr, originalPalettePass = nullptr;
static void(__cdecl *originalAnimationEnd)(void *) = nullptr;
static uint32_t idleAnimationName = 0;
static uint32_t headName = 0;
// Arm IK (networked VRIK): bone names per side (0 left, 1 right): upper arm,
// forearm, hand of the player skeletons (Models/Player/*).
static uint32_t armNames[2][3]{};
// How the model pass moved each remote weapon this pair (world delta, desired x
// native^-1), so the palette pass moves that hand's bones the same way.
struct ArmWeaponDelta {
    uint32_t owner = 0;
    void *body = nullptr;
    Matrix34 delta{};
    bool valid = false;
};
static std::array<std::array<ArmWeaponDelta, 2>, MaxBindings> armWeaponDeltas{};
static uint32_t scopeName = 0, scopeBoneName = 0;
static bool headTrackingEnabled = false;
static bool rideObservationEnabled = false;
static bool rideGripEnabled=false,rideDiagnosticEnabled=false;
static uint32_t mainName = 0, seatName = 0;
static StringId stringId = nullptr;
static int(__cdecl *isMainThread)() = nullptr;
static const uint32_t *invalidId = nullptr;
static thread_local bool paletteReentrant = false, paletteInvalidated = false;
static HandleResolve resolve = nullptr;
static PointerHandle pointerHandle = nullptr;
static IntThis isLocal = nullptr, isAlive = nullptr;
static ModelRenderable getModelRenderable = nullptr;
static ModelInstance getModelInstance = nullptr;
static WeaponTool getWeaponTool = nullptr;
static ToolFileStemId toolFileStemId = nullptr;
static ChildName getChildName = nullptr;
static ChildOffset getChildOffset = nullptr;
static ChildInstance getChildInstance = nullptr;
static uintptr_t engineBase = 0;
static uintptr_t seatGameBase = 0;
using ProjectionSlots = void(__cdecl *)(const int32_t *);
using ProjectionFog = void *(__cdecl *)(void *);
static ProjectionSlots originalProjectionSlots=nullptr;
static ProjectionFog originalProjectionFog=nullptr;
static uintptr_t projectionShaderBase=0;
static std::atomic<bool> ready = false;
static SRWLOCK bindingLock = SRWLOCK_INIT;
static std::array<Binding, MaxBindings> bindings;
static std::array<FrozenBinding, MaxBindings> frozen;
static std::atomic<bool> pairInvalid = false;
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static std::atomic<uint32_t> pairOwner = 0; // bindingLock protects the transaction bank.
static uint32_t nextPairOwner = 0; // Same lock; exhaustion never reuses a token.
static constinit thread_local uint32_t currentFrozenOwner = 0;
static constinit thread_local bool presentationSuppressed = false;
static std::atomic<DWORD> simulationThread = 0; // observations establish native object ownership.
static DWORD pairThread = 0;
static thread_local bool frozenPair = false;
static thread_local bool reentrant = false, modelInvalidated = false, presentationBusy = false;
// One optional observation in the EXISTING frozen bank. Native addresses are
// equality tokens; every dereference starts with a fresh render-owner query.
static RideRenderIdentity frozenRide;
static IdleConfigIdentity frozenRideConfig;
static std::array<RideRenderFrameCopy,2> rideFrames;
static std::array<std::array<RideMainDrawCopy,RideMainDrawCapacity>,2> rideMainDraws;
static std::array<uint32_t,2> rideMainDrawCounts{},rideGpuAttempts{};
static uint32_t rideGpuSessionAttempts=0;
static std::array<std::array<RideDrawGpuCopy,RideMainDrawCapacity>,2> rideMainGpu;
static std::array<bool,2> rideMainDrawOverflow{};
static std::array<bool,2> rideFrameSeen{};
static std::atomic<bool> rideObservationDeclined=false;
static std::atomic<unsigned> rideObservationRows=0;

// All sampling is integer/raw-bit only. In particular, no model/raster query or
// reference arithmetic occurs between native matrix production bookends.
// Disable vectorisation and loop-to-memcpy conversion to keep this inspectable.
__attribute__((noinline,target("general-regs-only"),optimize("no-tree-vectorize","no-tree-loop-distribute-patterns")))
static void captureProjectionSnapshot(IdleProjectionSnapshot &out) noexcept {
    uint16_t control;
    __asm__ volatile("fnstcw %0":"=m"(control)::"memory");
    out.control=control;
    out.flags=*reinterpret_cast<const volatile uint32_t *>(engineBase+0x2e5bdc);
    out.modelRecord=*reinterpret_cast<const volatile uint32_t *>(engineBase+0x2eab50);
    out.drawRecord=*reinterpret_cast<const volatile uint32_t *>(engineBase+0x2eab48);
    const auto *model=reinterpret_cast<const volatile uint32_t *>(engineBase+0x2e63d8);
    const auto *view=reinterpret_cast<const volatile uint32_t *>(engineBase+0x2e6408);
    const auto *projection=reinterpret_cast<const volatile uint32_t *>(engineBase+0x2e6438);
    const auto *vp=reinterpret_cast<const volatile uint32_t *>(engineBase+0x2e64f0);
    const auto *mvp=reinterpret_cast<const volatile uint32_t *>(engineBase+0x2e6530);
    for(unsigned i=0;i<12;++i) {out.model[i]=model[i];out.view[i]=view[i];}
    for(unsigned i=0;i<16;++i) {
        out.projection[i]=projection[i];out.cachedVP[i]=vp[i];out.cachedMVP[i]=mvp[i];
    }
}
static void __cdecl projectionSlots(const int32_t *slots) {
    const auto caller=reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    const uint32_t source=idleProjectionSlotsSource(projectionShaderBase,caller,reinterpret_cast<uintptr_t>(slots));
    if(ownsNativeThread())observeIdleProjectionOpportunity(0,source);
    auto *owner=ownsNativeThread()?idleProjectionOwner():nullptr;
    const bool raw=source==2 && ownsNativeThread();
    if(raw && (!owner || owner->nativeId==2))owner=idleRawProjectionOwner();
    const bool selected=owner && source && (source==1 || owner->nativeId==13 || (raw && owner->nativeId==2));
    const bool entered=selected && owner->projectionProbe.enterHelper();
    bool returned=false;
    withNativeFinally([&] {originalProjectionSlots(slots);returned=true;},[&](bool aborted) noexcept {
        if(entered)owner->projectionProbe.leaveHelper(aborted);
    });
    // Finish all owner lookups/finally callbacks BEFORE the pre-production
    // sample. There is no callback or floating arithmetic from sample to return.
    if(!entered || !returned)return;
    auto *current=raw && owner->nativeId==2?idleRawProjectionOwner():idleProjectionOwner();
    if(current!=owner) {owner->projectionProbe.invalidate();return;}
    auto *sample=owner->projectionProbe.begin(source);
    if(sample)captureProjectionSnapshot(*sample);
}
static void *__cdecl projectionFog(void *out) {
    const auto caller=reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    const uint32_t source=idleProjectionFogSource(projectionShaderBase,caller);
    // For the selected call this is the first observer operation after the
    // native, call-free producer interval. No floating interpretation occurs.
    IdleProjectionSnapshot sample;
    if(source)captureProjectionSnapshot(sample);
    auto *owner=source && ownsNativeThread()?idleProjectionOwner():nullptr;
    // The post-production integer snapshot above precedes all bookkeeping.
    if(ownsNativeThread())observeIdleProjectionOpportunity(1,source);
    const bool raw=source==2 && ownsNativeThread();
    if(raw && (!owner || owner->nativeId==2))owner=idleRawProjectionOwner();
    const bool pending=owner && (source==1 || owner->nativeId==13 || (raw && owner->nativeId==2)) && owner->projectionProbe.enterFog(source);
    void *result=nullptr;
    withNativeFinally([&] {
        result=originalProjectionFog(out);
        if(pending) {
            auto *current=raw && owner->nativeId==2?idleRawProjectionOwner():idleProjectionOwner();
            owner->projectionProbe.end(sample,result==out && current==owner);
        }
    },[&](bool aborted) noexcept {
        if(pending)owner->projectionProbe.leaveFog(aborted);
    });
    return result; // Preserve the native hidden-output pointer return exactly.
}

static void producerAbort() noexcept {
    modelInvalidated=paletteInvalidated=true;
    // A native-only bank is normally a valid fallback. An interrupted producer
    // is different: retire the whole pair, including a bank with no remotes.
    invalidateFrozenPresentationOwner(pairOwner,pairInvalid,currentFrozenOwner,currentFrozenOwner);
    nativeUiFault();
}

// The role query happens before binding ownership. Native callbacks below may
// unwind through MSVC frames; GNU destructors alone cannot retire these locks.
template<bool Exclusive=false,class Body,class Cleanup>
static bool withPresentationBindings(Body&& body,Cleanup&& cleanup) noexcept {
    static_assert(std::is_nothrow_invocable_v<Cleanup&,bool>);
    static_assert(std::is_trivially_destructible_v<std::remove_reference_t<Body>>);
    static_assert(std::is_trivially_destructible_v<std::remove_reference_t<Cleanup>>);
    if(presentationBusy) {
        // Decline nested adapter reads before reacquiring nonrecursive locks.
        producerAbort();
        return false;
    }
    multiplayer::PresentationReadGuard guard(false);
    bool held=false;presentationBusy=true;
    return withNativeFinally([&] {
        if constexpr(Exclusive) AcquireSRWLockExclusive(&bindingLock);
        else AcquireSRWLockShared(&bindingLock);
        held=true;guard.acquire();
        body(guard);
    },[&](bool aborted) noexcept {
        cleanup(aborted);
        if(aborted)producerAbort();
        guard.release();
        if(held) {
            held=false;
            if constexpr(Exclusive) ReleaseSRWLockExclusive(&bindingLock);
            else ReleaseSRWLockShared(&bindingLock);
        }
        presentationBusy=false;
    });
}

static uint32_t read32(const void *object, size_t offset) {
    uint32_t value = 0;
    std::memcpy(&value, static_cast<const uint8_t *>(object) + offset, sizeof(value));
    return value;
}

static int nativeWeaponId(void *weapon) {
    int value = -1;
    if (weapon)
        std::memcpy(&value, static_cast<uint8_t *>(weapon) + WeaponId, sizeof(value));
    return value;
}

static uint32_t nativeHandHandle(void *player, unsigned hand) {
    return read32(player, hand == 0 ? LeftWeaponHandle : RightWeaponHandle);
}

static bool validNativeId(int value) {
    return value >= 0 && value < int(WeaponCount) && value != 14;
}

static bool validSample(const Binding &binding, const multiplayer::Sample &sample) {
    if (!sample.negotiated || !sample.valid || !sample.avatar || !sample.incarnation ||
        sample.avatar != binding.playerHandle || sample.incarnation != binding.sample.incarnation ||
        GetTickCount64() - sample.receivedMs > network::MaxPoseAgeMs)
        return false;
    return true;
}

static bool bodyAnchor(void *player, const RiderIdentity &rider, Pose &body) {
    return player && nativeTrackingAnchor(player, body, &rider);
}

static bool currentBinding(const Binding &binding, const multiplayer::Sample &sample, void *&player,
                           const multiplayer::PresentationReadGuard *guard = nullptr) {
    player = binding.playerHandle ? resolve(binding.playerHandle) : nullptr;
    if (!player || !nativeRiderCurrent(player, binding.rider))
        return false;
    const bool local = isLocal(player);
    if (!nativeRiderCurrent(player, binding.rider))
        return false;
    const bool alive = !local && isAlive(player);
    if (!nativeRiderCurrent(player, binding.rider) || local || !alive)
        return false;
    auto renderable = getModelRenderable(player);
    if (!nativeRiderCurrent(player, binding.rider) || !renderable)
        return false;
    const auto modelInstance = getModelInstance(renderable);
    if (!nativeRiderCurrent(player, binding.rider) || modelInstance != binding.bodyModelInstance)
        return false;
    if (!guard)
        return validSample(binding, sample);
    const auto now = guard->sample(binding.playerHandle);
    if (!sample.presentationRevision || sample.presentationRevision != now.presentationRevision ||
        !samePresentationIdentity(binding.playerHandle, sample.incarnation, sample.pose, now.avatar,
                                  now.incarnation, now.pose, now.negotiated, now.valid))
        return false;
    for (unsigned hand = 0; hand < 2; ++hand)
        if (binding.rider.handheld() && binding.handEligible[hand] &&
            nativeHandHandle(player, hand) != binding.handHandle[hand])
            return false;
    return true;
}

static bool compatibleBinding(const Binding &a, const Binding &b) {
    return a.sample.presentationRevision && a.sample.presentationRevision == b.sample.presentationRevision &&
           a.playerHandle == b.playerHandle && a.bodyModelInstance == b.bodyModelInstance && a.rider == b.rider &&
           samePresentationIdentity(a.playerHandle, a.sample.incarnation, a.sample.pose, b.sample.avatar,
                                    b.sample.incarnation, b.sample.pose, b.sample.negotiated, b.sample.valid) &&
           !std::memcmp(a.handHandle, b.handHandle, sizeof(a.handHandle)) &&
           !std::memcmp(a.nativeId, b.nativeId, sizeof(a.nativeId)) &&
           !std::memcmp(a.toolId, b.toolId, sizeof(a.toolId)) &&
           !std::memcmp(a.handEligible, b.handEligible, sizeof(a.handEligible));
}
static bool validatePair(const multiplayer::PresentationReadGuard &guard) {
    bool admitted = false;
    for (const auto &entry : frozen)
        admitted |= entry.bodyValid;
    if (!eligiblePresentationPair(frozenPresentationOwnerMatches(pairOwner.load(std::memory_order_acquire),
                                                               currentFrozenOwner,currentFrozenOwner),
                                  pairThread == GetCurrentThreadId(), admitted,
                                  pairInvalid.load(std::memory_order_acquire), [] { return checkNativeThread(); }))
        return false;
    for (const auto &entry : frozen) {
        if (!entry.bodyValid)
            continue;
        void *player = nullptr;
        if (!currentBinding(entry.binding, entry.binding.sample, player, &guard)) {
            pairInvalid.store(true, std::memory_order_release);
            return false;
        }
    }
    return frozenPresentationOwnerMatches(pairOwner.load(std::memory_order_acquire),
                                          currentFrozenOwner,currentFrozenOwner)&&
        (!admitted||!pairInvalid.load(std::memory_order_acquire));
}

static bool eligibleHand(const Binding &binding, const multiplayer::Sample &sample, void *player, unsigned hand) {
    return binding.rider.handheld() && binding.handEligible[hand] && binding.handHandle[hand] &&
           nativeHandHandle(player, hand) == binding.handHandle[hand] && validNativeId(binding.nativeId[hand]) &&
           sample.pose.nativeWeaponId[hand] == binding.nativeId[hand] &&
           (sample.pose.validMask & (1u << (hand + 1))) && finite(sample.pose.grip[hand]);
}

static bool lineageContainsBody(std::span<const ModelRecord> records, size_t root, void *body) {
    for (size_t i = 1; i < records.size(); ++i) {
        if (records[i].instance != reinterpret_cast<uintptr_t>(body))
            continue;
        bool contains = false;
        if (!ancestorContains(records, root, i, contains))
            return false;
        if (contains)
            return true;
    }
    return false;
}

static bool descendantOf(std::span<const ModelRecord> records, size_t child, size_t root) {
    bool contains = false;
    return ancestorContains(records, child, root, contains) && contains;
}

static bool selectRoot(std::span<const NativeModelRecord> native, std::span<const ModelRecord> records,
                       const Binding &binding, unsigned hand, size_t &root, Pose &offset) {
    size_t found = records.size();
    for (size_t index = 1; index < native.size(); ++index) {
        const void *descriptor = native[index].descriptor;
        uint32_t name = 0;
        Pose candidateOffset;
        if (!descriptor)
            continue;
        getChildName(&name, descriptor);
        if(modelInvalidated)return false;
        getChildOffset(&candidateOffset, descriptor);
        if(modelInvalidated)return false;
        if(name!=binding.toolId[hand])continue;
        const auto child=getChildInstance(descriptor);
        if(modelInvalidated)return false;
        if (native[index].instance != child ||
            !lineageContainsBody(records, index, binding.bodyModelInstance))
            continue;
        if (found != records.size())
            return false; // Descriptor/name collisions must not select an arbitrary tool.
        found = index;
        offset = candidateOffset;
    }
    if (found == records.size() || !finite(offset))
        return false;
    root = found;
    return true;
}

struct ModelPassStorage {
    std::vector<ModelRecord> records,changed;
};
static void postModelPassBody(const multiplayer::PresentationReadGuard& presentationGuard,
                              ModelPassStorage& storage) {
    if (!hooksReady.load(std::memory_order_acquire) || !ready.load(std::memory_order_acquire))
        return;
    if (frozenPair && !validatePair(presentationGuard))
        return;
    if (!checkNativeThread() || modelInvalidated)
        return;

    NativeModelRecord *native = nullptr;
    int32_t count = 0, capacity = 0;
    std::memcpy(&native, reinterpret_cast<void *>(engineBase + 0x2eac24), sizeof(native));
    std::memcpy(&count, reinterpret_cast<void *>(engineBase + 0x2eac28), sizeof(count));
    std::memcpy(&capacity, reinterpret_cast<void *>(engineBase + 0x2eac20), sizeof(capacity));
    if (!native || count < 1 || count > int(MaxModelRecords) || capacity < count || native[0].instance)
        return;

    std::array<FrozenBinding, MaxBindings> bank{};
    if (frozenPair) {
        bank = frozen;
    } else {
        for (size_t i = 0; i < bindings.size(); ++i)
            bank[i].binding = bindings[i];
    }

    auto& records=storage.records;
    records.reserve(size_t(count));
    for (int32_t i = 0; i < count; ++i)
        records.push_back({native[i].parent, reinterpret_cast<uintptr_t>(native[i].instance), native[i].world});
    auto& changed=storage.changed;changed=records;
    std::array<size_t, MaxBindings * 2> selected{};
    size_t selectedCount = 0;

    for (const auto &entry : bank) {
        const Binding &binding = entry.binding;
        if (!binding.playerHandle || !binding.bodyModelInstance)
            continue;
        void *player = nullptr;
        multiplayer::Sample sample = binding.sample;
        if (!frozenPair) {
            player = resolve(binding.playerHandle);
            if(modelInvalidated)return;
            if (!player)
                continue;
            sample = presentationGuard.sample(binding.playerHandle);
        }
        const bool current=currentBinding(binding, sample, player, frozenPair ? &presentationGuard : nullptr);
        if(modelInvalidated)return;
        if (!current)continue;
        Pose body;
        if (frozenPair) {
            if (!entry.bodyValid)
                continue;
            body = entry.body;
        } else {
            const bool anchored=bodyAnchor(player,binding.rider,body);
            if(modelInvalidated)return;
            if(!anchored)continue;
        }
        for (unsigned hand = 0; hand < 2; ++hand) {
            if (!eligibleHand(binding, sample, player, hand))
                continue;
            size_t root = 0;
            Pose offset;
            const bool selectedRoot=selectRoot(std::span<const NativeModelRecord>(native,size_t(count)),
                                                records,binding,hand,root,offset);
            if(modelInvalidated)return;
            if(!selectedRoot)continue;
            bool collision = false;
            for (size_t i = 0; i < selectedCount; ++i)
                collision |= root == selected[i] || descendantOf(records, root, selected[i]) ||
                             descendantOf(records, selected[i], root);
            if (collision)
                continue;
            Matrix34 desiredRigid = affineMultiply(matrix(compose(body, sample.pose.grip[hand])), matrix(offset));
            Matrix34 desired;
            if (!retainNativeStretch(records[root].world, desiredRigid, desired) ||
                !retargetModelSubtree(std::span<ModelRecord>(changed), root, desired))
                continue;
            selected[selectedCount++] = root;
            if (frozenPair && currentFrozenOwner) {
                Matrix34 inverse;
                auto &stored = armWeaponDeltas[size_t(&entry - bank.data())][hand];
                stored.valid = affineInverse(records[root].world, inverse);
                if (stored.valid) stored = {currentFrozenOwner, binding.bodyModelInstance,
                                            affineMultiply(desired, inverse), finiteMatrix(affineMultiply(desired, inverse))};
            }
        }
    }

    if(modelInvalidated)return; // A nested native producer can replace every captured span.
    for (size_t i = 0; i < records.size(); ++i)
        if (std::memcmp(&records[i].world, &changed[i].world, sizeof(Matrix34)))
            native[i].world = changed[i].world;
}

static void postModelPass() {
    if (presentationSuppressed) return;
    // Storage lives above the foreign unwind frame and is explicitly retired.
    std::optional<ModelPassStorage> storage(std::in_place);
    withPresentationBindings([&](const auto& guard) {postModelPassBody(guard,*storage);},
                             [&](bool) noexcept {storage.reset();});
}

// Engine renderer arrays, read only after DDE30 has finished reallocating/copying.
struct NativeMeshRecord { int32_t owner, first, count; uint8_t tail[0x14]; };
struct NativeDrawEntry { int32_t mesh, first, count; uint8_t tail[0x14]; };
struct NativeBone { int32_t owner, parent; uint8_t evaluated[0x1c]; void *definition; };
static_assert(sizeof(NativeMeshRecord) == 0x20);
static_assert(sizeof(NativeDrawEntry) == 0x20);
static_assert(sizeof(NativeBone) == 0x28 && offsetof(NativeBone, definition) == 0x24);
constexpr size_t MaxNativeBones = 8192, MaxNativeEntries = 8192, MaxPaletteMatrices = 32768;

template<class T> static bool rendererArray(size_t arrayRva, size_t maximum, std::span<T> &out,
                                            bool writable = false) {
    T *pointer = nullptr;
    int32_t capacity = 0, count = 0;
    std::memcpy(&capacity, reinterpret_cast<void *>(engineBase + arrayRva), 4);
    std::memcpy(&pointer, reinterpret_cast<void *>(engineBase + arrayRva + 4), 4);
    std::memcpy(&count, reinterpret_cast<void *>(engineBase + arrayRva + 8), 4);
    if (count < 0 || size_t(count) > maximum || capacity < count || count > INT32_MAX / int32_t(sizeof(T)))
        return false;
    if (count && !readableMemory(pointer, size_t(count) * sizeof(T), writable))
        return false;
    out = {pointer, size_t(count)};
    return true;
}
static void paletteFault() {
    if (frozenPair)
        pairInvalid.store(true, std::memory_order_release);
}
struct HeadPassStorage {
    std::vector<uintptr_t> instances;
    std::vector<PaletteModelRange> models;
    std::vector<PaletteMeshRange> meshRanges;
    std::vector<PaletteDrawRange> drawRanges;
    std::vector<Matrix34> worlds,changed,next;
    std::vector<int32_t> drawOwners;
    std::vector<PaletteBone> boneViews;
    std::vector<uint8_t> ownersUsed;
};
// Networked VRIK: each arm of a remote VR player's body reaches for that
// player's tracked hand. A hand holding a weapon follows it (the bones move as
// the model pass moved the weapon, so the hand keeps its native hold); an
// empty hand reaches its tracked grip. Bones stay native when anything needed
// is missing (unknown skeleton, untracked hand, unreadable bone data).
static bool poseArms(const FrozenBinding &entry, size_t bankIndex, size_t owner, std::span<const NativeBone> bones,
                     std::span<const PaletteBone> boneViews, std::span<const PaletteMap> mappings,
                     std::span<const int32_t> drawOwners, std::span<const Matrix34> worlds,
                     std::vector<Matrix34> &palette) {
    if (!armNames[1][2] || owner >= worlds.size() || palette.size() != mappings.size()) return false;
    void *evaluated = nullptr;
    Matrix34 *canonical = nullptr;
    int32_t canonicalCount = 0;
    std::memcpy(&evaluated, reinterpret_cast<void *>(engineBase + 0x2eab68), 4);
    if (!readableMemory(evaluated, 0x28)) return false;
    std::memcpy(&canonical, static_cast<uint8_t *>(evaluated) + 0x20, 4);
    std::memcpy(&canonicalCount, static_cast<uint8_t *>(evaluated) + 0x24, 4);
    if (canonicalCount < int32_t(bones.size()) || size_t(canonicalCount) > MaxNativeBones ||
        !readableMemory(canonical, size_t(canonicalCount) * sizeof(Matrix34)))
        return false;
    Matrix34 inverseWorld;
    if (!affineInverse(worlds[owner], inverseWorld)) return false;
    const auto &pose = entry.binding.sample.pose;
    bool any = false;
    for (unsigned side = 0; side < 2; ++side) {
        if (!(pose.validMask & (1u << (side + 1))) || !finite(pose.grip[side])) continue;
        // The side's three bones on this body, as posed model-space frames:
        // canonical (skinning) x bind, the bind being the inverse of the
        // definition's matrix at +0x48 (V477: joint spacing = bone lengths).
        size_t index[3]{bones.size(), bones.size(), bones.size()};
        for (size_t b = 0; b < boneViews.size(); ++b)
            if (boneViews[b].owner == int32_t(owner) && boneViews[b].named)
                for (unsigned k = 0; k < 3; ++k)
                    if (boneViews[b].name == armNames[side][k]) index[k] = b;
        Matrix34 frame[3];
        bool ok = true;
        for (unsigned k = 0; k < 3 && ok; ++k) {
            ok = index[k] < bones.size() && bones[index[k]].definition &&
                 readableMemory(static_cast<const uint8_t *>(bones[index[k]].definition) + 0x48, sizeof(Matrix34));
            Matrix34 inverseBind, bind;
            if (ok) std::memcpy(&inverseBind, static_cast<const uint8_t *>(bones[index[k]].definition) + 0x48, sizeof(Matrix34));
            ok = ok && affineInverse(inverseBind, bind);
            if (ok) frame[k] = affineMultiply(canonical[index[k]], bind);
            ok = ok && finiteMatrix(frame[k]);
        }
        if (!ok) continue;
        // Target hand frame in model space.
        const auto &weapon = armWeaponDeltas[bankIndex][side];
        Matrix34 handTarget;
        bool oriented = false;
        if (weapon.valid && weapon.owner == currentFrozenOwner && weapon.body == entry.binding.bodyModelInstance) {
            handTarget = affineMultiply(inverseWorld, affineMultiply(weapon.delta, affineMultiply(worlds[owner], frame[2])));
            oriented = true;
        } else {
            // Empty hand: the wrist 7 cm behind the tracked grip point, native hand orientation.
            const Pose grip = compose(entry.body, pose.grip[side]);
            const Vec3 wrist = grip.p + rotate(normalize(grip.q), Vec3{0, 0, .07f});
            handTarget = frame[2];
            const Vec3 local = matrixApply(inverseWorld, wrist);
            handTarget.m[3] = local.x; handTarget.m[7] = local.y; handTarget.m[11] = local.z;
        }
        if (!finiteMatrix(handTarget)) continue;
        ArmDeltas deltas;
        if (!solveArm({frame[0], frame[1], frame[2]}, matrixTranslation(handTarget), oriented ? &handTarget : nullptr, deltas))
            continue;
        // Bone levels on this body: 3 under the hand, 2 under the forearm, 1 under the upper arm.
        std::vector<uint8_t> level(bones.size(), 0);
        for (size_t b = 0; b < bones.size(); ++b) {
            size_t node = b;
            for (size_t hops = 0; hops <= bones.size() && node < bones.size(); ++hops) {
                if (boneViews[node].owner != int32_t(owner)) break;
                if (node == index[2]) { level[b] = 3; break; }
                if (node == index[1]) { level[b] = 2; break; }
                if (node == index[0]) { level[b] = 1; break; }
                if (boneViews[node].parent < 0) break;
                node = size_t(boneViews[node].parent);
            }
        }
        for (size_t i = 0; i < mappings.size(); ++i) {
            const auto &map = mappings[i];
            if (map.bone < 0 || size_t(map.bone) >= bones.size() || map.draw < 0 || size_t(map.draw) >= drawOwners.size() ||
                drawOwners[size_t(map.draw)] != int32_t(owner) || !level[size_t(map.bone)])
                continue;
            const unsigned l = level[size_t(map.bone)];
            const Matrix34 moved = affineMultiply(l == 3 ? deltas.hand : l == 2 ? deltas.lower : deltas.upper, palette[i]);
            if (!finiteMatrix(moved)) return false;
            palette[i] = moved;
            any = true;
        }
    }
    return any;
}
static void postPaletteBody(const multiplayer::PresentationReadGuard& presentationGuard,
                            HeadPassStorage& storage) {
    if (!frozenPair || !hooksReady.load(std::memory_order_acquire) || !ready.load(std::memory_order_acquire))
        return;
    if (!checkNativeThread() || !validatePair(presentationGuard)) {
        paletteFault();
        return;
    }
    const auto bank = frozen; // Captured before native stereo production, once per pair.
    // A body is posed when its head moved or a hand is tracked (arm IK).
    const auto posed = [](const FrozenBinding &entry) {
        const auto &pose = entry.binding.sample.pose;
        return entry.bodyValid && (((pose.validMask & 1) && !identityTrackingDelta(pose.head)) || (pose.validMask & 6));
    };
    bool eligible = false;
    for (const auto &entry : bank)
        eligible |= posed(entry);
    if (!eligible)
        return;
    std::span<NativeModelRecord> records;
    std::span<NativeMeshRecord> meshes;
    std::span<NativeDrawEntry> draws;
    std::span<NativeBone> bones;
    std::span<PaletteMap> mappings;
    std::span<Matrix34> palette;
    if (!rendererArray(0x2eac20, MaxModelRecords, records) || records.empty() || records[0].instance) {
        paletteFault();
        return;
    }
    auto& instances=storage.instances;
    for (const auto &record : records)
        instances.push_back(reinterpret_cast<uintptr_t>(record.instance));
    std::array<int32_t, MaxBindings> bodyOwners;
    bodyOwners.fill(-1);
    bool relevantBody = false;
    for (size_t i = 0; i < bank.size(); ++i) {
        const auto &entry = bank[i];
        if (!posed(entry))
            continue;
        bodyOwners[i] = paletteBodyOwner(instances, reinterpret_cast<uintptr_t>(entry.binding.bodyModelInstance));
        if (bodyOwners[i] == -2) { paletteFault(); return; }
        relevantBody |= bodyOwners[i] >= 0;
    }
    if (!relevantBody)
        return; // Unrelated static/no-morph models have no head-specific requirements.
    if (!rendererArray(0x2eac70, MaxPaletteMatrices, mappings)) { paletteFault(); return; }
    if (mappings.empty())
        return; // Native unskinned/empty-LOD draw; canonical evaluation may be null.
    if (!rendererArray(0x2eac30, MaxNativeEntries, meshes) ||
        !rendererArray(0x2eac50, MaxNativeEntries, draws) ||
        !rendererArray(0x2eac60, MaxNativeBones, bones) ||
        !rendererArray(0x2eac90, MaxPaletteMatrices, palette, true) || palette.size() < mappings.size()) {
        paletteFault();
        return;
    }
    auto& models=storage.models;
    auto& meshRanges=storage.meshRanges;
    auto& drawRanges=storage.drawRanges;
    auto& worlds=storage.worlds;
    for (const auto &record : records) {
        int32_t first = 0, count = 0;
        std::memcpy(&first, reinterpret_cast<const uint8_t *>(&record) + 8, 4);
        std::memcpy(&count, reinterpret_cast<const uint8_t *>(&record) + 0xc, 4);
        models.push_back({first, count});
        worlds.push_back(record.world);
    }
    for (const auto &mesh : meshes) meshRanges.push_back({mesh.owner, mesh.first, mesh.count});
    for (const auto &draw : draws) drawRanges.push_back({draw.mesh, draw.first, draw.count});
    auto& drawOwners=storage.drawOwners;
    if (!paletteDrawOwners(models, meshRanges, drawRanges, mappings, drawOwners)) {
        paletteFault();
        return;
    }
    auto& boneViews=storage.boneViews;
    for (const auto &bone : bones) {
        uint32_t name = 0;
        if (bone.definition) {
            if (!readableMemory(bone.definition, 4)) { paletteFault(); return; }
            std::memcpy(&name, bone.definition, 4);
        }
        boneViews.push_back({bone.owner, bone.parent, name, bone.definition != nullptr});
    }
    auto& changed=storage.changed;auto& next=storage.next;
    changed.assign(palette.begin(),palette.begin()+mappings.size());
    auto& ownersUsed=storage.ownersUsed;ownersUsed.assign(records.size(),0);
    bool changedHead = false;
    for (size_t i = 0; i < bank.size(); ++i) {
        const auto &entry = bank[i];
        if (bodyOwners[i] < 0)
            continue;
        const size_t owner = size_t(bodyOwners[i]);
        if (ownersUsed[owner]) { paletteFault(); return; }
        ownersUsed[owner] = 1;
        const auto &pose = entry.binding.sample.pose;
        if ((pose.validMask & 1) && !identityTrackingDelta(pose.head)) {
            const auto result = retargetHeadPalette(boneViews, mappings, drawOwners, worlds, int32_t(owner), headName,
                                                    entry.body, pose.head, changed, next);
            if (result == HeadPaletteResult::Invalid) { paletteFault(); return; }
            if (result == HeadPaletteResult::Changed) {
                changedHead = true;
                changed.swap(next);
            }
        }
        changedHead |= poseArms(entry, i, owner, bones, boneViews, mappings, drawOwners, worlds, changed);
    }
    if (!changedHead)
        return; // Headless/unmapped LOD preserves native draw, even without evaluation.
    if (paletteInvalidated)
        return; // Outer invocation faults late nesting; no adapter writes have happened.
    void *evaluated = nullptr;
    Matrix34 *canonical = nullptr;
    int32_t canonicalCount = 0;
    std::memcpy(&evaluated, reinterpret_cast<void *>(engineBase + 0x2eab68), 4);
    if (!readableMemory(evaluated, 0x28)) { paletteFault(); return; }
    std::memcpy(&canonical, static_cast<uint8_t *>(evaluated) + 0x20, 4);
    std::memcpy(&canonicalCount, static_cast<uint8_t *>(evaluated) + 0x24, 4);
    if (canonicalCount < 0 || size_t(canonicalCount) < bones.size() || size_t(canonicalCount) > MaxNativeBones ||
        !disjointMatrixStorage(reinterpret_cast<uintptr_t>(palette.data()), mappings.size(),
                               reinterpret_cast<uintptr_t>(canonical), size_t(canonicalCount))) {
        paletteFault();
        return;
    }
    // Atomic adapter phase: all validation/allocation precedes every write.
    if (paletteInvalidated)
        return;
    for (size_t i = 0; i < mappings.size(); ++i)
        if (std::memcmp(&palette[i], &changed[i], sizeof(Matrix34)))
            palette[i] = changed[i];
}
static void postPalette() {
    // Head writes are restricted to the admitted frozen native render extent.
    // Ordinary desktop draws do not enter binding reads or recapture an anchor.
    if (!frozenPair || presentationSuppressed) return;
    std::optional<HeadPassStorage> storage(std::in_place);
    withPresentationBindings([&](const auto& guard) {postPaletteBody(guard,*storage);},
                             [&](bool) noexcept {storage.reset();});
}
static ScopeSurfaceLayout scopeSurfaceLayout(const uint8_t *bytes) {
    ScopeSurfaceLayout layout;
    std::memcpy(&layout.triangles, bytes+8, 4);
    std::memcpy(&layout.vertices, bytes+0xc, 4);
    const size_t offsets[]{0x20,0x18,0x38,0x40};
    for (size_t i=0;i<layout.channels.size();++i) {
        layout.channels[i].format = bytes[offsets[i]];
        layout.channels[i].buffer = bytes[offsets[i]+1];
        std::memcpy(&layout.channels[i].offset,bytes+offsets[i]+4,4);
    }
    return layout;
}
static ScopeRasterStatus readScopeRaster(void *instance, Matrix34 &affine, ScopeSurfaceLayout &layout) {
    affine = {}; layout = {};
    if (!instance || !hooksReady.load(std::memory_order_acquire) || !ready.load(std::memory_order_acquire) ||
        !ownsNativeThread() || paletteInvalidated) return ScopeRasterStatus::Rejected;
    NativeModelRecord *model = nullptr;
    NativeDrawEntry *draw = nullptr;
    const uint8_t *surface = nullptr;
    int32_t softwarePosition = 0;
    std::memcpy(&model,reinterpret_cast<void *>(engineBase+0x2eab50),4);
    std::memcpy(&draw,reinterpret_cast<void *>(engineBase+0x2eab48),4);
    std::memcpy(&surface,reinterpret_cast<void *>(engineBase+0x2eab44),4);
    std::memcpy(&softwarePosition,reinterpret_cast<void *>(engineBase+0x2c7f88),4);
    if (!readableMemory(model,sizeof(*model)) || !readableMemory(surface,4)) return ScopeRasterStatus::Rejected;
    const void *drawSurface = nullptr;
    uint32_t name = 0;
    std::memcpy(&name,surface,4);
    if (model->instance != instance || name != scopeName) return ScopeRasterStatus::Unrelated;
    if (softwarePosition != -1 || !readableMemory(draw,sizeof(*draw)) || !readableMemory(surface,0x130) ||
        draw->count != 1 || draw->first < 0) return ScopeRasterStatus::Rejected;
    std::memcpy(&drawSurface,reinterpret_cast<const uint8_t *>(draw)+0x18,4);
    if (drawSurface != surface) return ScopeRasterStatus::Rejected;
    std::span<Matrix34> palette;
    if (!rendererArray(0x2eac90,MaxPaletteMatrices,palette) || size_t(draw->first) >= palette.size()) return ScopeRasterStatus::Rejected;
    const Matrix34 candidate = affineMultiply(model->world,palette[size_t(draw->first)]);
    Matrix34 inverse;
    if (!finiteMatrix(model->world) || !finiteMatrix(palette[size_t(draw->first)]) ||
        !affineInverse(candidate,inverse)) return ScopeRasterStatus::Rejected;
    affine = candidate;
    layout = scopeSurfaceLayout(surface);
    return ScopeRasterStatus::Observed; // No native pointer or index escapes this synchronous read.
}
struct ScopePassStorage {
    std::vector<uintptr_t> instances;
    std::vector<PaletteModelRange> modelRanges;
    std::vector<Matrix34> worlds;
    std::vector<PaletteMeshRange> meshRanges;
    std::vector<PaletteDrawRange> drawRanges;
    std::vector<uint32_t> surfaceNames;
    std::vector<PaletteBone> boneViews;
};
static void observeLocalScopeBody(ScopePassStorage& storage) {
    ScopeDrawBinding binding;
    if (!hooksReady.load(std::memory_order_acquire) || !ready.load(std::memory_order_acquire) ||
        !ownsNativeThread() || !currentScopeDraw(binding) || paletteInvalidated)
        return;
    std::span<NativeModelRecord> records;
    std::span<NativeMeshRecord> meshes;
    std::span<NativeDrawEntry> draws;
    std::span<NativeBone> bones;
    std::span<PaletteMap> mappings;
    std::span<Matrix34> palette;
    if (!rendererArray(0x2eac20, MaxModelRecords, records) || records.empty() || records[0].instance ||
        !rendererArray(0x2eac30, MaxNativeEntries, meshes) ||
        !rendererArray(0x2eac50, MaxNativeEntries, draws) ||
        !rendererArray(0x2eac60, MaxNativeBones, bones) ||
        !rendererArray(0x2eac70, MaxPaletteMatrices, mappings) ||
        !rendererArray(0x2eac90, MaxPaletteMatrices, palette))
        return;
    auto& instances=storage.instances;
    auto& modelRanges=storage.modelRanges;
    auto& worlds=storage.worlds;
    for (const auto &record : records) {
        int32_t first = 0, count = 0;
        std::memcpy(&first, reinterpret_cast<const uint8_t *>(&record) + 8, 4);
        std::memcpy(&count, reinterpret_cast<const uint8_t *>(&record) + 0xc, 4);
        modelRanges.push_back({first, count});
        instances.push_back(reinterpret_cast<uintptr_t>(record.instance));
        worlds.push_back(record.world);
    }
    auto& meshRanges=storage.meshRanges;
    auto& drawRanges=storage.drawRanges;
    auto& surfaceNames=storage.surfaceNames;
    for (const auto &mesh : meshes)
        meshRanges.push_back({mesh.owner, mesh.first, mesh.count});
    for (const auto &draw : draws) {
        const void *surface = nullptr;
        uint32_t name = 0;
        std::memcpy(&surface, reinterpret_cast<const uint8_t *>(&draw) + 0x18, 4);
        if (!readableMemory(surface, 0x130))
            return;
        std::memcpy(&name, surface, 4);
        drawRanges.push_back({draw.mesh, draw.first, draw.count});
        surfaceNames.push_back(name);
    }
    auto& boneViews=storage.boneViews;
    for (const auto &bone : bones) {
        uint32_t name = 0;
        if (bone.definition) {
            if (!readableMemory(bone.definition, 4))
                return;
            std::memcpy(&name, bone.definition, 4);
        }
        boneViews.push_back({bone.owner, bone.parent, name, bone.definition != nullptr});
    }
    ScopeDrawSelection selected;
    const ScopePaletteView view{instances, modelRanges, meshRanges, drawRanges, surfaceNames,
                                boneViews, mappings, worlds, palette};
    if (paletteInvalidated || !selectScopeDraw(view, reinterpret_cast<uintptr_t>(binding.modelInstance),
                                               scopeName, scopeBoneName, selected)) return;
    // Reuse the exact association selected by the affine producer. Do not
    // search by name again or retain native indices/pointers beyond this call.
    const void *surface = nullptr;
    std::memcpy(&surface, reinterpret_cast<const uint8_t *>(&draws[size_t(selected.draw)]) + 0x18, 4);
    if (!readableMemory(surface, 0x130)) return;
    const auto bytes = static_cast<const uint8_t *>(surface);
    uint32_t currentName = 0;
    std::memcpy(&currentName, bytes, 4);
    if (currentName != scopeName) return;
    const auto layout = scopeSurfaceLayout(bytes);
    if (!paletteInvalidated) recordScopeObservation(binding, selected.affine, layout);
}
static void observeLocalScope() {
    std::optional<ScopePassStorage> storage(std::in_place);
    withNativeFinally([&] {observeLocalScopeBody(*storage);},[&](bool aborted) noexcept {
        storage.reset();
        if(aborted)producerAbort();
    });
}
static bool idleConfig(void *instance,IdleConfigIdentity &out,
                       uintptr_t expectedVtable=engineBase+0x2095b4) {
    out={};
    if(!readableMemory(instance,0x2c))return false;
    uint32_t configuration=0,vtable=0;
    std::memcpy(&configuration,static_cast<const uint8_t*>(instance)+0x18,4);
    if(!readableMemory(reinterpret_cast<void*>(configuration),0x14))return false;
    std::memcpy(&vtable,reinterpret_cast<void*>(configuration),4);
    if(!expectedVtable || vtable!=expectedVtable)return false;
    out.configuration=configuration;
    std::memcpy(&out.file,reinterpret_cast<void*>(configuration+0xc),4);
    std::memcpy(&out.resource,reinterpret_cast<void*>(configuration+0x10),4);
    return true; // Numeric current CResource identity, not a historical byte join.
}
struct IdleRasterStorage {
    PaletteOwnerScratch paletteScratch;
    std::vector<PaletteModelRange> models;
    std::vector<PaletteMeshRange> meshes;
    std::vector<PaletteDrawRange> draws;
    std::vector<int32_t> owners;
    std::vector<ModelRecord> tree;
};
struct RidePaletteBookend {
    RideRenderFrameCopy frame;
    std::array<uint32_t,3> modelArray{},boneArray{},cacheRows{};
    uint32_t boneFirst=0,boneCount=0,canonicalCount=0,skeleton=0,skeletonFlags=0;
    uint32_t lodCount=0,lods=0,lod=0,definitions=0,definitionCount=0;
    bool operator==(const RidePaletteBookend &) const = default;
};
__attribute__((noinline,noclone)) static bool rideObservationOwnerCurrent(uint32_t owner,
        RideReadPhase phase=RideReadPhase::Drawing) {
    // Native thread ownership and TLS/atomic token precede EVERY ordinary bank
    // read. A foreign producer may overlap freezePair's bank initialization.
    if(!hooksReady.load(std::memory_order_acquire) || !ready.load(std::memory_order_acquire) ||
       !rideObservationEnabled ||
       !ownsNativeThread() || !owner || !rideReadPhaseCurrent(frozenPair,phase) ||
       !frozenPresentationOwnerMatches(pairOwner.load(std::memory_order_acquire),currentFrozenOwner,owner) ||
       presentationSuppressed || paletteInvalidated || pairInvalid.load(std::memory_order_acquire))return false;
    return pairThread==GetCurrentThreadId();
}
// Copy raw words after native DDE30 production. No reference arithmetic, native
// evaluation, allocation or virtual resource load occurs in this observer.
__attribute__((noinline,optimize("no-tree-vectorize","no-tree-loop-distribute-patterns")))
// Bounded trace: the first time each reason declines a ride palette copy.
static bool ridePaletteFail(unsigned reason) {
    static uint64_t seen=0;
    if(reason<64 && !(seen&(1ull<<reason))) {seen|=1ull<<reason;log("Ride palette declined reason=%u",reason);}
    return false;
}
static bool copyRidePaletteBookend(RidePaletteBookend &out,bool &selected) {
    selected=false;
    if(!rideObservationOwnerCurrent(currentFrozenOwner) || !rideObservationEnabled ||
       !frozenRide.valid() || rideObservationDeclined)return ridePaletteFail(1);
    out={}; // Even compiler-generated clearing stays behind owner admission.
    selected=true; // Losing the captured actor/model owner declines the bank.
    auto &frame=out.frame;
    if(!copyNativeRideRenderIdentity(frozenRide.player,frame.identity) || frame.identity!=frozenRide)return ridePaletteFail(2);
    void *instance=reinterpret_cast<void*>(frame.identity.instance);
    if(!readableMemory(instance,0x2c))return ridePaletteFail(3);
    std::memcpy(&frame.evaluated,reinterpret_cast<void*>(engineBase+0x2eab68),4);
    uint32_t linked=0,evaluatedOwner=0;
    std::memcpy(&linked,static_cast<uint8_t*>(instance)+0x28,4);
    if(!frame.evaluated || frame.evaluated!=linked) {selected=false;return ridePaletteFail(4);}
    if(!readableMemory(reinterpret_cast<void*>(frame.evaluated),0x38))return ridePaletteFail(5);
    std::memcpy(&evaluatedOwner,reinterpret_cast<void*>(frame.evaluated+0x18),4);
    if(evaluatedOwner!=frame.identity.instance)return ridePaletteFail(6);
    selected=true; // Subsequent failed association invalidates a provisional copy.
    std::span<NativeModelRecord> records;std::span<NativeBone> bones;
    if(!rendererArray(0x2eac20,MaxModelRecords,records) || records.empty() || records[0].instance ||
       !rendererArray(0x2eac60,MaxNativeBones,bones))return ridePaletteFail(7);
    std::memcpy(out.modelArray.data(),reinterpret_cast<void*>(engineBase+0x2eac20),12);
    std::memcpy(out.boneArray.data(),reinterpret_cast<void*>(engineBase+0x2eac60),12);
    size_t owner=0;unsigned matches=0;
    for(size_t i=1;i<records.size();++i)
        if(reinterpret_cast<uintptr_t>(records[i].instance)==frame.identity.instance) {owner=i;++matches;}
    if(matches!=1)return ridePaletteFail(8);
    frame.modelRecord=uint32_t(owner);
    const auto *model=reinterpret_cast<const uint8_t*>(&records[owner]);
    std::memcpy(&out.boneFirst,model+0x18,4);
    std::memcpy(&out.boneCount,model+0x1c,4);
    std::memcpy(&out.lod,model+0x5c,4);
    if(!out.boneFirst || !out.boneCount || out.boneFirst>bones.size() ||
       out.boneCount>bones.size()-out.boneFirst)return ridePaletteFail(9);
    IdleConfigIdentity config;
    if(!idleConfig(instance,config) || config!=frozenRideConfig ||
       !readableMemory(reinterpret_cast<void*>(config.configuration),0x34))return ridePaletteFail(10);
    frame.configuration=config.configuration;frame.file=config.file;frame.resource=uint32_t(config.resource);
    std::memcpy(frame.stretch.data(),instance,12);
    std::memcpy(&out.skeleton,reinterpret_cast<void*>(config.configuration+0x30),4);
    if(!readableMemory(reinterpret_cast<void*>(out.skeleton),0x38))return ridePaletteFail(11);
    std::memcpy(&out.skeletonFlags,reinterpret_cast<void*>(out.skeleton+4),4);
    std::memcpy(&out.lodCount,reinterpret_cast<void*>(out.skeleton+0x30),4);
    std::memcpy(&out.lods,reinterpret_cast<void*>(out.skeleton+0x34),4);
    if((out.skeletonFlags&1) || !out.lodCount || out.lodCount>64 || !out.lods || out.lod<out.lods ||
       (out.lod-out.lods)%0x20 || (out.lod-out.lods)/0x20>=out.lodCount ||
       !readableMemory(reinterpret_cast<void*>(out.lods),size_t(out.lodCount)*0x20))return ridePaletteFail(12);
    std::memcpy(&out.definitions,reinterpret_cast<void*>(out.lod+0xc),4);
    std::memcpy(&out.definitionCount,reinterpret_cast<void*>(out.lod+0x10),4);
    if(!out.definitionCount || out.definitionCount>MaxNativeBones || !out.definitions ||
       !readableMemory(reinterpret_cast<void*>(out.definitions),size_t(out.definitionCount)*0x78))return ridePaletteFail(13);
    std::memcpy(&frame.matrices,reinterpret_cast<void*>(frame.evaluated+0x20),4);
    std::memcpy(&out.canonicalCount,reinterpret_cast<void*>(frame.evaluated+0x24),4);
    std::memcpy(out.cacheRows.data(),reinterpret_cast<void*>(frame.evaluated+0x2c),12);
    if(evaluatedOwner!=frame.identity.instance || out.canonicalCount<bones.size() ||
       out.canonicalCount>MaxNativeBones || !frame.matrices ||
       out.cacheRows[2]!=records.size() || out.cacheRows[0]<out.cacheRows[2] || !out.cacheRows[1] ||
       !readableMemory(reinterpret_cast<void*>(out.cacheRows[1]),records.size()*8) ||
       !readableMemory(reinterpret_cast<void*>(frame.matrices),size_t(out.canonicalCount)*sizeof(Matrix34)))return ridePaletteFail(14);
    uint32_t cacheLod=0;
    std::memcpy(&cacheLod,reinterpret_cast<void*>(out.cacheRows[1]+owner*8+4),4);
    if(cacheLod!=out.lod)return ridePaletteFail(15);
    unsigned mains=0,seats=0;
    for(size_t i=out.boneFirst;i<size_t(out.boneFirst)+out.boneCount;++i) {
        const auto &bone=bones[i];const auto definition=reinterpret_cast<uintptr_t>(bone.definition);
        if(bone.owner!=int32_t(owner) || definition<out.definitions ||
           (definition-out.definitions)%0x78 || (definition-out.definitions)/0x78>=out.definitionCount)return ridePaletteFail(16);
        uint32_t name=0;std::memcpy(&name,bone.definition,4);
        if(name==mainName) {++mains;frame.mainBone=uint32_t(i);frame.boneDefinition=uint32_t(definition);}
        if(name==seatName) {++seats;frame.seatBone=uint32_t(i);frame.seatDefinition=uint32_t(definition);}
    }
    if(mains!=1 || seats!=1 || frame.mainBone==frame.seatBone)return false;
    // Global NativeBone index, exactly the native DDE97/DDEA5 lookup, not the
    // model-relative index. All reads remain within the selected cache bounds.
    std::memcpy(frame.world.data(),model+0x24,48);
    std::memcpy(frame.main.data(),reinterpret_cast<void*>(frame.matrices+frame.mainBone*48),48);
    std::memcpy(frame.seat.data(),reinterpret_cast<void*>(frame.matrices+frame.seatBone*48),48);
    frame.skeleton=out.skeleton;frame.lod=out.lod;frame.definitions=out.definitions;
    frame.definitionCount=out.definitionCount;frame.boneFirst=out.boneFirst;frame.boneCount=out.boneCount;
    frame.canonicalCount=out.canonicalCount;frame.cacheRows=out.cacheRows[1];frame.cacheRowCount=out.cacheRows[2];
    return true;
}
struct RideAttachmentBookend {
    RideAttachmentCopy copy;
    std::array<uint32_t,3> seatArray{},childArray{};
    // Complete membership/selection/flatness inputs. Numeric tokens only.
    std::array<std::array<uint32_t,4>,32> seats{};
    std::array<std::array<uint32_t,6>,32> children{};
    bool operator==(const RideAttachmentBookend &) const = default;
};
__attribute__((noinline,optimize("no-tree-vectorize","no-tree-loop-distribute-patterns")))
static bool copyRideAttachmentBookend(const RidePaletteBookend &frame,RideAttachmentBookend &out) {
    if(!rideObservationOwnerCurrent(currentFrozenOwner))return false;
    RideRenderIdentity current;
    if(!copyNativeRideRenderIdentity(frame.frame.identity.player,current) || current!=frame.frame.identity)return false;
    out={};
    auto &copy=out.copy;
    void *ride=resolve(current.ride);
    if(!seatGameBase || !readableMemory(ride,0x480))return false;
    std::memcpy(&copy.parameter,static_cast<uint8_t*>(ride)+0x47c,4);
    if(!readableMemory(reinterpret_cast<void*>(copy.parameter),0x1a0))return false;
    std::memcpy(&copy.parameterFlags,reinterpret_cast<void*>(copy.parameter+4),4);
    // Native access would replace a pending shared resource. Its old array is
    // not the native selection even if two readable copies happen to agree.
    if(copy.parameterFlags&1)return false;
    std::memcpy(out.seatArray.data(),reinterpret_cast<void*>(copy.parameter+0x194),12);
    const auto count=out.seatArray[2],data=out.seatArray[1];
    if(!count || count>out.seats.size() || out.seatArray[0]>INT32_MAX || out.seatArray[0]<count || !data ||
       !readableMemory(reinterpret_cast<void*>(data),size_t(count)*4))return false;
    unsigned selected=0;
    for(size_t i=0;i<count;++i) {
        auto &row=out.seats[i];std::memcpy(&row[0],reinterpret_cast<void*>(data+i*4),4);
        if(!readableMemory(reinterpret_cast<void*>(row[0]),12))return false;
        std::memcpy(row.data()+1,reinterpret_cast<void*>(row[0]),12);
        if(row[1]!=seatGameBase+0x297a74)return false;
        if(row[2]==current.seat) {++selected;copy.seatData=row[0];copy.attachment=row[3];}
    }
    if(selected!=1 || !invalidId || copy.attachment==*invalidId)return false;
    if(!readableMemory(reinterpret_cast<void*>(current.instance),0x28))return false;
    std::memcpy(&copy.childState,reinterpret_cast<void*>(current.instance+0x24),4);
    if(!readableMemory(reinterpret_cast<void*>(copy.childState),0x1c))return false;
    std::memcpy(out.childArray.data(),reinterpret_cast<void*>(copy.childState+0x10),12);
    copy.childArray=out.childArray[1];copy.childCount=out.childArray[2];
    if(!copy.childCount || copy.childCount>out.children.size() || !copy.childArray ||
       !readableMemory(reinterpret_cast<void*>(copy.childArray),size_t(copy.childCount)*4))return false;
    selected=0;
    for(size_t i=0;i<copy.childCount;++i) {
        auto &row=out.children[i];std::memcpy(&row[0],reinterpret_cast<void*>(copy.childArray+i*4),4);
        if(!readableMemory(reinterpret_cast<void*>(row[0]),0x50))return false;
        std::memcpy(row.data()+1,reinterpret_cast<void*>(row[0]),12);
        std::memcpy(&row[4],reinterpret_cast<void*>(row[0]+0x40),4); // embedded configuration
        std::memcpy(&row[5],reinterpret_cast<void*>(row[0]+0x4c),4); // embedded children
        if(row[4] || row[5])return false; // Prove this actual query tree is flat.
        if(row[1]==copy.attachment) {
            ++selected;copy.descriptor=row[0];copy.childFlags=row[2];copy.parentName=row[3];
            std::memcpy(copy.pose.data(),reinterpret_cast<void*>(row[0]+0xc),28);
            std::memcpy(copy.scale.data(),reinterpret_cast<void*>(row[0]+0x28),12);
        }
    }
    uint32_t parentName=0;
    std::memcpy(&parentName,reinterpret_cast<void*>(frame.frame.seatDefinition),4);
    if(selected!=1 || copy.parentName!=parentName)return false; // IDENT, not global index.
    std::span<NativeModelRecord> records;
    if(!rendererArray(0x2eac20,MaxModelRecords,records) || records.size()!=frame.frame.cacheRowCount)return false;
    selected=0;
    for(size_t i=1;i<records.size();++i) {
        if(reinterpret_cast<uintptr_t>(records[i].descriptor)!=copy.descriptor)continue;
        if(records[i].parent!=int32_t(frame.frame.modelRecord) || records[i].unused!=frame.frame.seatBone ||
           reinterpret_cast<uintptr_t>(records[i].instance)!=copy.descriptor+0x28)return false;
        ++selected;copy.childRecord=uint32_t(i);
    }
    if(selected>1)return false;
    copy.childRecordPresent=selected;copy.mapped=true;
    return true;
}
__attribute__((noinline)) static void observeRidePalette() {
    if(!rideObservationOwnerCurrent(currentFrozenOwner) || !rideObservationEnabled ||
       !frozenRide.valid() || rideObservationDeclined)return;
    const int eye=nativeWorldEye();
    if(eye < -1 || eye > 1) {rideObservationDeclined=true;return;}
    const size_t index=eye<0?0:size_t(eye);
    RidePaletteBookend before,after;
    bool selected=false,afterSelected=false;
    if(!copyRidePaletteBookend(before,selected)) {
        if(selected)rideObservationDeclined=true;
        return; // Other model production is ordinary visibility absence.
    }
    RideAttachmentBookend attachmentBefore,attachmentAfter;
    const bool mapped=copyRideAttachmentBookend(before,attachmentBefore) &&
                      copyRideAttachmentBookend(before,attachmentAfter) && attachmentBefore==attachmentAfter;
    if(rideFrameSeen[index] || !copyRidePaletteBookend(after,afterSelected) || before!=after) {
        rideObservationDeclined=true;return;
    }
    // Optional metadata failure never suppresses the original Main/Seat copy.
    if(mapped)before.frame.attachment=attachmentBefore.copy;
    rideFrames[index]=before.frame;rideFrameSeen[index]=true;
}
// Bounded trace: the first time each reason declines a ride handle frame.
static bool rideGripFrameFail(unsigned reason) {
    static uint64_t seen=0;
    if(reason<64 && !(seen&(1ull<<reason))) {seen|=1ull<<reason;log("Ride grip frame declined reason=%u",reason);}
    return false;
}
static bool buildRideGripFrame(const Request &request,const RideGripRig &rig,RideGripFrame &out) {
    out={};const RideDrawGpuCopy *selected[2]{};const RideMainDrawCopy *draw[2]{};
    for(unsigned eye=0;eye<2;++eye) {
        if(rideMainDrawOverflow[eye])return rideGripFrameFail(1);
        for(unsigned i=0;i<rideMainDrawCounts[eye];++i) {
            const auto &d=rideMainDraws[eye][i];const auto &g=rideMainGpu[eye][i];
            if(!rideSurfaceSupported(d.layout))continue;
            // The same mesh can be drawn again in one eye (another pass); the
            // first draw whose copy completed stands for it.
            if(selected[eye] || !g.copied || !g.handles.copied || !g.handlePositionAgrees)continue;
            selected[eye]=&g;draw[eye]=&d;
        }
        if(!selected[eye])return rideGripFrameFail(3);
    }
    const auto &left=*selected[0],&right=*selected[1];
    if(left.handles.profile!=right.handles.profile || draw[0]->identity!=draw[1]->identity ||
       draw[0]->configuration!=draw[1]->configuration || draw[0]->file!=draw[1]->file ||
       draw[0]->resource!=draw[1]->resource || !draw[0]->identity.parameter ||
       rig.rider.player!=draw[0]->identity.player || rig.rider.ride!=draw[0]->identity.ride ||
       rig.rider.seat!=draw[0]->identity.seat || !validNativeBodyPose(rig.anchor))return rideGripFrameFail(4);
    RideGripFrame next;next.request=request;next.rig=rig;next.render=draw[0]->identity;
    next.configuration=draw[0]->configuration;next.file=draw[0]->file;next.resource=draw[0]->resource;
    if(rideFrames[0].stretch!=rideFrames[1].stretch)return rideGripFrameFail(5);
    next.stretch=rideFrames[0].stretch;
    next.parameter=next.render.parameter;next.profile=left.handles.profile;
    for(unsigned hand=0;hand<2;++hand) {
        const auto &a=left.handles.handles[hand],&b=right.handles.handles[hand];
        if(a.vertices!=b.vertices || a.indices!=b.indices)return rideGripFrameFail(6);
        auto &surface=next.surfaces[hand];surface.vertices=a.vertices;surface.indices=a.indices;
        for(unsigned v=0;v<a.vertices;++v) {
            const auto p=left.handleWorld[hand][v],q=right.handleWorld[hand][v],delta=p-q;
            if(!std::isfinite(dot(delta,delta)) || dot(delta,delta)>.0005f*.0005f)return rideGripFrameFail(7);
            surface.positions[v]=rotate(inverse(rig.anchor.q),p-rig.anchor.p);
            const auto local=surface.positions[v];
            if(!std::isfinite(local.x)||!std::isfinite(local.y)||!std::isfinite(local.z))return rideGripFrameFail(8);
        }
    }
    next.valid=true;out=next;return true;
}
// Called only after retained-bank admission; this stack contains copied values,
// and its allocation must not move ahead of the publisher's owner gate.
__attribute__((noinline)) static void publishRideGripFrame(const Request &request,const RideGripRig &rig) {
    RideGripFrame frame;
    if(buildRideGripFrame(request,rig,frame))recordRideGripFrame(frame);
}
__attribute__((noinline)) static void publishRideObservation(uint32_t owner,bool stereo,
        const Request *request=nullptr,const RideGripRig *rig=nullptr) {
    // endEye retires native scratch use before stereo Ready. It does not retire
    // the bank owner; only this copied-value publication uses the retained phase.
    if(!rideObservationOwnerCurrent(owner,RideReadPhase::Retained) || !rideObservationEnabled || rideObservationDeclined ||
       !frozenRide.valid() || !rideFrameSeen[0])return;
    if(stereo && !rideFrameSeen[1]) {
        // Single-pass stereo draws the world (and the ride) once for both eyes:
        // that one observation stands for the second eye as well.
        rideFrames[1]=rideFrames[0];rideFrameSeen[1]=true;
        rideMainDraws[1]=rideMainDraws[0];rideMainDrawCounts[1]=rideMainDrawCounts[0];
        rideMainDrawOverflow[1]=rideMainDrawOverflow[0];rideMainGpu[1]=rideMainGpu[0];
    }
    if(rideGripEnabled && stereo && request && rig)publishRideGripFrame(*request,*rig);
    if(!rideDiagnosticEnabled || rideObservationRows.load(std::memory_order_relaxed)>=32)return;
    const unsigned row=rideObservationRows.fetch_add(1,std::memory_order_relaxed);
    if(row>=32)return;
    if(stereo) {
        auto a=rideFrames[0].attachment,b=rideFrames[1].attachment;
        a.childRecordPresent=b.childRecordPresent=0;a.childRecord=b.childRecord=0;
        if(a!=b) {rideFrames[0].attachment={};rideFrames[1].attachment={};}
    }
    for(unsigned eye=0;eye<(stereo?2u:1u);++eye) {
        const auto &frame=rideFrames[eye];const auto &id=frame.identity;
        const auto &a=frame.attachment;
        log("Lab ride render schema=5 source=%.*s row=%u bank=%u eye=%d player=%u brain=%u ride=%u seat=%u class=%x renderableHandle=%u renderable=%u instance=%u cfg=%u file=%u resource=%u modelRecord=%u evaluated=%u matrices=%u mainBone=%u definition=%u seatBone=%u seatDefinition=%u attachmentMapped=%u mainDrawCount=%u mainDrawOverflow=%u resourceClaim=0 seatClaim=0 graspClaim=0 steeringClaim=0",
            64,ss2vrBuildContract.sourceFingerprint.data(),row,owner,stereo?int(eye):-1,id.player,id.brain,id.ride,id.seat,id.classRva,id.renderableHandle,
            id.renderable,id.instance,frame.configuration,frame.file,frame.resource,frame.modelRecord,
            frame.evaluated,frame.matrices,frame.mainBone,frame.boneDefinition,frame.seatBone,frame.seatDefinition,unsigned(a.mapped),rideMainDrawCounts[eye],unsigned(rideMainDrawOverflow[eye]));
        log("Lab ride render binding schema=5 row=%u bank=%u eye=%d skeleton=%u lod=%u definitions=%u definitionCount=%u boneFirst=%u boneCount=%u canonicalCount=%u cacheRows=%u cacheRowCount=%u",
            row,owner,stereo?int(eye):-1,frame.skeleton,frame.lod,frame.definitions,frame.definitionCount,
            frame.boneFirst,frame.boneCount,frame.canonicalCount,frame.cacheRows,frame.cacheRowCount);
        const auto emit=[&](const char *kind,const std::array<uint32_t,12> &m) {
            log("Lab ride render matrix schema=5 row=%u bank=%u eye=%d kind=%s words=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x",
                row,owner,stereo?int(eye):-1,kind,m[0],m[1],m[2],m[3],m[4],m[5],m[6],m[7],m[8],m[9],m[10],m[11]);
        };
        emit("modelWorld",frame.world);emit("MainCanonical",frame.main);emit("SeatCanonical",frame.seat);
        if(a.mapped) {
            log("Lab ride render attachment schema=5 row=%u bank=%u eye=%d parameter=%u parameterFlags=%u seatData=%u attachment=%u childState=%u childArray=%u childCount=%u descriptor=%u parentName=%u childFlags=%u childRecordPresent=%u childRecord=%u childWorldAvailable=0 flatTree=1",
                row,owner,stereo?int(eye):-1,a.parameter,a.parameterFlags,a.seatData,a.attachment,a.childState,
                a.childArray,a.childCount,a.descriptor,a.parentName,a.childFlags,a.childRecordPresent,a.childRecord);
            log("Lab ride render attachmentPose schema=5 row=%u bank=%u eye=%d pose=%08x,%08x,%08x,%08x,%08x,%08x,%08x scale=%08x,%08x,%08x",
                row,owner,stereo?int(eye):-1,a.pose[0],a.pose[1],a.pose[2],a.pose[3],a.pose[4],a.pose[5],a.pose[6],
                a.scale[0],a.scale[1],a.scale[2]);
        }
        for(uint32_t ordinal=0;ordinal<rideMainDrawCounts[eye];++ordinal) {
            const auto &d=rideMainDraws[eye][ordinal];
            const auto &g=rideMainGpu[eye][ordinal];
            log("Lab ride render mainDraw schema=5 row=%u bank=%u eye=%d ordinal=%u model=%u draw=%u surface=%u instance=%u name=%u bone=%u definition=%u cfg=%u file=%u resource=%u lod=%u paletteFirst=%u paletteCount=%u localMainSlot=%u topology=%u base=%d minimum=%u vertices=%u start=%u primitives=%u buffersClaim=0 positionProgramClaim=0 graspClaim=0 steeringClaim=0 originalSucceeded=1 cleanupCurrent=1 gpuCopied=%u",
                row,owner,stereo?int(eye):-1,ordinal,d.modelRecord,d.drawRecord,d.surface,d.instance,d.surfaceName,d.bone,d.definition,
                d.configuration,d.file,d.resource,d.lod,d.paletteFirst,d.paletteCount,d.localMainSlot,
                d.api.topology,d.api.base,d.api.minimum,d.api.vertices,d.api.start,d.api.primitives,unsigned(g.copied));
            const auto &c=d.layout.channels;
            log("Lab ride render mainDrawLayout schema=5 row=%u bank=%u eye=%d ordinal=%u vertices=%d triangles=%d positions=%u,%u,%u indices=%u,%u,%u weights=%u,%u,%u localIndices=%u,%u,%u",
                row,owner,stereo?int(eye):-1,ordinal,d.layout.vertices,d.layout.triangles,
                unsigned(c[0].format),unsigned(c[0].buffer),c[0].offset,unsigned(c[1].format),unsigned(c[1].buffer),c[1].offset,
                unsigned(c[2].format),unsigned(c[2].buffer),c[2].offset,unsigned(c[3].format),unsigned(c[3].buffer),c[3].offset);
            const auto emitDraw=[&](const char *kind,const std::array<uint32_t,12> &m) {
                log("Lab ride render mainDrawMatrix schema=5 row=%u bank=%u eye=%d ordinal=%u kind=%s words=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x",
                    row,owner,stereo?int(eye):-1,ordinal,kind,m[0],m[1],m[2],m[3],m[4],m[5],m[6],m[7],m[8],m[9],m[10],m[11]);
            };
            emitDraw("modelWorld",d.world);emitDraw("actualPalette",d.actualPalette);
            if(g.copied) {
                log("Lab rideHandleProof schema=1 source=%.*s row=%u bank=%u eye=%d ordinal=%u profile=%u geometryCopied=%u vertices=%u,%u positionAgrees=%u graspClaim=0 steeringClaim=0",
                    64,ss2vrBuildContract.sourceFingerprint.data(),row,owner,stereo?int(eye):-1,ordinal,
                    unsigned(g.handles.profile),unsigned(g.handles.copied),
                    g.handles.copied?g.handles.handles[0].vertices:0,g.handles.copied?g.handles.handles[1].vertices:0,
                    unsigned(g.handlePositionAgrees));
                std::array<char,325> hashes{};
                constexpr char hex[]="0123456789abcdef";
                for(unsigned i=0;i<5;++i) {
                    if(i)hashes[i*65-1]=',';
                    for(unsigned j=0;j<32;++j) {
                        hashes[i*65+j*2]=hex[g.hashes[i][j]>>4];
                        hashes[i*65+j*2+1]=hex[g.hashes[i][j]&15];
                    }
                }
                const auto &v=g.inputs;const auto &p=v.positions,&l=v.localIndices,&w=v.weights,&u=v.uv;
                log("Lab ride render mainDrawGpu schema=5 row=%u bank=%u eye=%d ordinal=%u programWords=%u constantRows=%u declarationElements=%u inputLayout=%u positions=%u,%u,%u,%u localIndices=%u,%u,%u,%u weights=%u,%u,%u,%u uv=%u,%u,%u,%u indexObject=%u declarationObject=%u shaderObject=%u vertexDesc=%u,%u,%u,%u,%u indexDesc=%u,%u,%u,%u,%u softwarePositions=0 hashes=%s",
                    row,owner,stereo?int(eye):-1,ordinal,g.programWords,g.constantRows,g.declarationElements,g.inputLayout,
                    uint32_t(p.object),p.offset,p.stride,p.frequency,uint32_t(l.object),l.offset,l.stride,l.frequency,
                    uint32_t(w.object),w.offset,w.stride,w.frequency,uint32_t(u.object),u.offset,u.stride,u.frequency,
                    uint32_t(v.indexObject),g.declarationObject,g.shaderObject,
                    v.vertex.size,v.vertex.usage,v.vertex.pool,v.vertex.format,v.vertex.fvf,
                    v.index.size,v.index.usage,v.index.pool,v.index.format,v.index.fvf,hashes.data());
                const auto emitWords=[&](const char *kind,std::span<const uint32_t> values) {
                    for(size_t offset=0;offset<values.size();offset+=64) {
                        std::array<char,577> words{};
                        const size_t count=std::min(size_t(64),values.size()-offset);
                        for(size_t i=0;i<count;++i)std::snprintf(words.data()+i*9,10,"%08x%s",values[offset+i],i+1<count?",":"");
                        log("Lab ride render mainDrawGpuWords schema=5 row=%u bank=%u eye=%d ordinal=%u kind=%s offset=%u words=%s",
                            row,owner,stereo?int(eye):-1,ordinal,kind,unsigned(offset),words.data());
                    }
                };
                std::array<uint32_t,130> declaration{};
                for(unsigned i=0;i<g.declarationElements;++i) {
                    const auto e=g.declaration[i];declaration[2*i]=(uint32_t(e.stream)<<16)|e.offset;
                    declaration[2*i+1]=(uint32_t(e.type)<<24)|(uint32_t(e.method)<<16)|(uint32_t(e.usage)<<8)|e.usageIndex;
                }
                emitWords("program",std::span(g.program).first(g.programWords));
                emitWords("constants",std::span(g.constants).first(g.constantRows*4));
                emitWords("declaration",std::span(declaration).first(g.declarationElements*2));
            }

        }
    }
}
enum class IdleNativeDrawPolicy { SingleAffine,Id2Palette,RideMainMapping };
static bool readIdleNativeDraw(void *instance,IdleRasterCopy &out,IdleRasterStorage &storage,
                               IdleNativeDrawPolicy policy,IdlePaletteCopy *companion=nullptr,
                               int32_t requestedMain=-1,RideMainDrawCopy *rideDraw=nullptr,IdlePaletteBoundaryCopy *diagnostic=nullptr) {
    out={};
    const auto fail=[&](uint32_t step) noexcept {if(diagnostic)diagnostic->reader=step;return false;};
    if(diagnostic) {
        diagnostic->reader=0;diagnostic->valid=1;
        diagnostic->root=uint32_t(reinterpret_cast<uintptr_t>(instance));
        diagnostic->render=diagnostic->evaluated=diagnostic->linked=0;
        diagnostic->cacheOwner=diagnostic->canonicalCount=diagnostic->matrices=0;
        diagnostic->software=0;
    }
    if(companion)*companion={};
    if(!instance || !hooksReady.load(std::memory_order_acquire) || !ready.load(std::memory_order_acquire) ||
       !ownsNativeThread() || paletteInvalidated)return fail(1);
    std::span<NativeModelRecord> records;std::span<NativeMeshRecord> meshes;
    std::span<NativeDrawEntry> draws;std::span<NativeBone> bones;
    std::span<PaletteMap> maps;std::span<Matrix34> palette;
    if(!rendererArray(0x2eac20,MaxModelRecords,records) || !rendererArray(0x2eac30,MaxNativeEntries,meshes) ||
       !rendererArray(0x2eac50,MaxNativeEntries,draws) || !rendererArray(0x2eac60,MaxNativeBones,bones) ||
       !rendererArray(0x2eac70,MaxPaletteMatrices,maps) || !rendererArray(0x2eac90,MaxPaletteMatrices,palette))return fail(2);
    uintptr_t modelPointer=0,drawPointer=0,surfacePointer=0;int32_t softwarePosition=0;
    std::memcpy(&modelPointer,reinterpret_cast<void*>(engineBase+0x2eab50),4);
    std::memcpy(&drawPointer,reinterpret_cast<void*>(engineBase+0x2eab48),4);
    std::memcpy(&surfacePointer,reinterpret_cast<void*>(engineBase+0x2eab44),4);
    std::memcpy(&softwarePosition,reinterpret_cast<void*>(engineBase+0x2c7f88),4);
    // Exact membership before dereferencing either current native pointer.
    const auto member=[](uintptr_t pointer,auto span,size_t &index) {
        const auto base=reinterpret_cast<uintptr_t>(span.data());
        if(!pointer || pointer<base || (pointer-base)%sizeof(span[0]))return false;
        index=(pointer-base)/sizeof(span[0]);return index<span.size();
    };
    size_t modelIndex=0,drawIndex=0,root=0;unsigned roots=0;
    if(softwarePosition!=-1) {
        if(diagnostic) {diagnostic->software=softwarePosition;diagnostic->valid|=32;}
        return fail(3);
    }
    if(!member(modelPointer,records,modelIndex))return fail(17);
    if(!member(drawPointer,draws,drawIndex))return fail(18);
    for(size_t i=0;i<records.size();++i)if(records[i].instance==instance) {root=i;++roots;}
    if(roots!=1 || !root)return fail(4);
    auto &models=storage.models;auto &meshRanges=storage.meshes;
    auto &drawRanges=storage.draws;auto &owners=storage.owners;auto &tree=storage.tree;
    for(const auto &r:records) {int32_t first=0,count=0;
        std::memcpy(&first,reinterpret_cast<const uint8_t*>(&r)+8,4);
        std::memcpy(&count,reinterpret_cast<const uint8_t*>(&r)+0xc,4);
        models.push_back({first,count});tree.push_back({r.parent,reinterpret_cast<uintptr_t>(r.instance),r.world});}
    for(const auto &m:meshes)meshRanges.push_back({m.owner,m.first,m.count});
    for(const auto &d:draws)drawRanges.push_back({d.mesh,d.first,d.count});
    if(diagnostic) {
        diagnostic->render=uint32_t(storage.tree[modelIndex].instance);diagnostic->valid|=2;
    }
    bool descendant=false;
    if(!ancestorContains(tree,modelIndex,root,descendant) || !descendant ||
       !paletteDrawOwners(models,meshRanges,drawRanges,maps,owners,storage.paletteScratch) || owners[drawIndex]!=int32_t(modelIndex))return fail(5);
    const auto &draw=draws[drawIndex];const auto &model=records[modelIndex];
    uintptr_t declaredSurface=0;std::memcpy(&declaredSurface,reinterpret_cast<const uint8_t*>(&draw)+0x18,4);
    if(surfacePointer!=declaredSurface || !readableMemory(reinterpret_cast<void*>(surfacePointer),0x130) ||
       draw.first<0)return fail(6);
    if(policy==IdleNativeDrawPolicy::SingleAffine) {
        // Existing admission is deliberately count-one, including its original
        // mapping, configuration, finite affine and invertibility checks.
        if(draw.count!=1 || size_t(draw.first)>=maps.size() || size_t(draw.first)>=palette.size())return fail(7);
        const auto mapping=maps[size_t(draw.first)];
        if(mapping.draw!=int32_t(drawIndex) || mapping.bone<0 || size_t(mapping.bone)>=bones.size() ||
           bones[size_t(mapping.bone)].owner!=int32_t(modelIndex) ||
           !readableMemory(bones[size_t(mapping.bone)].definition,4) ||
           !idleConfig(instance,out.rootConfig) || !idleConfig(model.instance,out.renderConfig))return fail(8);
        out.affine=affineMultiply(model.world,palette[size_t(draw.first)]);
        Matrix34 inverse;if(!finiteMatrix(model.world) || !finiteMatrix(palette[size_t(draw.first)]) ||
                            !affineInverse(out.affine,inverse))return fail(9);
        out.factors.model=model.world;out.factors.local=palette[size_t(draw.first)];
        out.factors.paletteIndex=uint32_t(draw.first);out.factors.modelCopied=true;
        out.bone=mapping.bone;
        std::memcpy(&out.boneName,bones[size_t(mapping.bone)].definition,4);
    } else if(policy==IdleNativeDrawPolicy::RideMainMapping) {
        // Observe one Main map in a potentially multi-bone draw. No invented
        // whole-draw affine and no change to either weapon policy above/below.
        uint32_t slot=0;
        if(!rideDraw || !selectRideMainMapping(maps,palette.size(),int32_t(drawIndex),draw.first,draw.count,
            requestedMain,[&](int32_t bone) {
                return size_t(bone)<bones.size() && bones[size_t(bone)].owner==int32_t(modelIndex) &&
                    readableMemory(bones[size_t(bone)].definition,4);
            },slot) || !idleConfig(instance,out.rootConfig) || !idleConfig(model.instance,out.renderConfig))return fail(10);
        const size_t index=size_t(draw.first)+slot;
        out.bone=requestedMain;
        std::memcpy(&out.boneName,bones[size_t(requestedMain)].definition,4);
        rideDraw->paletteFirst=uint32_t(draw.first);rideDraw->paletteCount=uint32_t(draw.count);
        rideDraw->localMainSlot=slot;
        rideDraw->definition=uint32_t(reinterpret_cast<uintptr_t>(bones[size_t(requestedMain)].definition));
        std::memcpy(rideDraw->world.data(),model.world.m,48);
        std::memcpy(rideDraw->actualPalette.data(),palette[index].m,48);
    } else {
        if(!companion || draw.count<1 || draw.count>int32_t(IdlePaletteCopy::MaxPalette) ||
           !validPaletteSpan(draw.first,draw.count,maps.size()) ||
           !validPaletteSpan(draw.first,draw.count,palette.size()))return fail(11);
        auto &p=*companion;
        for(int32_t i=0;i<draw.count;++i) {
            const auto mapping=maps[size_t(draw.first)+size_t(i)];
            if(mapping.draw!=int32_t(drawIndex) || mapping.bone<0 || size_t(mapping.bone)>=bones.size() ||
               bones[size_t(mapping.bone)].owner!=int32_t(modelIndex) ||
               !readableMemory(bones[size_t(mapping.bone)].definition,4))return fail(12);
            auto &entry=p.entries[size_t(i)];
            entry.draw=mapping.draw;entry.bone=mapping.bone;entry.owner=int32_t(modelIndex);
            entry.definition=uint32_t(reinterpret_cast<uintptr_t>(bones[size_t(mapping.bone)].definition));
            std::memcpy(&entry.name,bones[size_t(mapping.bone)].definition,4);
        }
        if(!idleConfig(instance,out.rootConfig) || !idleConfig(model.instance,out.renderConfig) ||
           !readableMemory(instance,0x2c))return fail(13);
        uint32_t linked=0,cacheOwner=0;
        std::memcpy(&p.evaluated,reinterpret_cast<void*>(engineBase+0x2eab68),4);
        std::memcpy(&linked,static_cast<uint8_t*>(instance)+0x28,4);
        if(diagnostic) {diagnostic->evaluated=p.evaluated;diagnostic->linked=linked;diagnostic->valid|=4;}
        if(!p.evaluated || p.evaluated!=linked || !readableMemory(reinterpret_cast<void*>(p.evaluated),0x28))return fail(14);
        std::memcpy(&cacheOwner,reinterpret_cast<void*>(p.evaluated+0x18),4);
        std::memcpy(&p.matrices,reinterpret_cast<void*>(p.evaluated+0x20),4);
        std::memcpy(&p.canonicalCount,reinterpret_cast<void*>(p.evaluated+0x24),4);
        if(diagnostic) {
            diagnostic->cacheOwner=cacheOwner;diagnostic->canonicalCount=p.canonicalCount;
            diagnostic->matrices=p.matrices;diagnostic->valid|=8;
        }
        if(cacheOwner!=reinterpret_cast<uintptr_t>(instance) || p.canonicalCount<bones.size() ||
           p.canonicalCount>MaxNativeBones || !p.matrices ||
           !readableMemory(reinterpret_cast<void*>(p.matrices),size_t(p.canonicalCount)*48))return fail(15);
        p.first=uint32_t(draw.first);p.count=uint32_t(draw.count);
        p.mapCount=uint32_t(maps.size());p.paletteCount=uint32_t(palette.size());p.modelCount=uint32_t(records.size());
        p.mappingAddress=uint32_t(reinterpret_cast<uintptr_t>(maps.data()));
        p.paletteAddress=uint32_t(reinterpret_cast<uintptr_t>(palette.data()));
        std::memcpy(p.world.data(),model.world.m,48);
        for(unsigned i=0;i<p.count;++i) {
            auto &entry=p.entries[i];
            std::memcpy(entry.canonical.data(),reinterpret_cast<void*>(p.matrices+uint32_t(entry.bone)*48),48);
            std::memcpy(entry.palette.data(),palette[size_t(p.first)+i].m,48);
        }
        // A multi-mapping draw has no single bone or invented affine.
        if(draw.count==1) {out.bone=p.entries[0].bone;out.boneName=p.entries[0].name;}
    }
    out.layout=scopeSurfaceLayout(reinterpret_cast<const uint8_t*>(surfacePointer));
    out.modelRecord=uint32_t(modelIndex);out.drawRecord=uint32_t(drawIndex);out.surface=uint32_t(surfacePointer);
    out.projectionModelAddress=uint32_t(modelPointer);out.projectionDrawAddress=uint32_t(drawPointer);
    out.instance=uint32_t(reinterpret_cast<uintptr_t>(model.instance));
    std::memcpy(&out.surfaceName,reinterpret_cast<void*>(surfacePointer),4);
    if(companion) {
        companion->metadata=IdleSubmissionMetadata::copy(out);companion->copied=true;
        if(!companion->bounded())return fail(16);
    }
    if(paletteInvalidated)return fail(32);
    return true;
}
static void observeIdleQuery(void *queue,uintptr_t caller) {
    ScopeDrawBinding binding;IdleDrawIdentity identity;IdleWeaponTrace *trace=nullptr;
    if(!currentIdleDraw(binding,identity,trace))return;
    trace->callbacks|=IdleWeaponTrace::QuerySeen;
    uint32_t active=0,instance=0,entries=0;
    int32_t count=0;IdleConfigIdentity config;
    std::memcpy(&active,reinterpret_cast<void*>(engineBase+0x2d92a4),4);
    if(!nativeIdleQueryBorrow(caller,engineBase+0xddded,active,reinterpret_cast<uintptr_t>(queue))) {
        trace->reject(IdleWeaponTrace::Rejection::QueryBorrow,IdleWeaponTrace::checks({queue!=nullptr,caller==engineBase+0xddded,active==reinterpret_cast<uintptr_t>(queue)}));return;
    }
    if(!readableMemory(queue,0xc)) {trace->reject(IdleWeaponTrace::Rejection::QueryMemory);return;}
    std::memcpy(&instance,static_cast<const uint8_t*>(queue)+8,4);
    if(instance!=reinterpret_cast<uintptr_t>(binding.modelInstance))return; // Unrelated child query stays native.
    trace->callbacks|=IdleWeaponTrace::QueryRootSeen;
    if(trace->nativeId==2)trace->projectionOpportunities.mark(trace->projectionOpportunities.rootQuery);
    if(!idleConfig(binding.modelInstance,config)) {
        trace->reject(IdleWeaponTrace::Rejection::QueryConfig);return;
    }
    std::memcpy(&entries,reinterpret_cast<void*>(engineBase+0x2d92c4),4);
    std::memcpy(&count,reinterpret_cast<void*>(engineBase+0x2d92c8),4);
    if(!trace->event(identity,config,true,count))return;
    if(!readableMemory(reinterpret_cast<void*>(entries),size_t(count)*0x20)) {trace->reject(IdleWeaponTrace::Rejection::QueryEntries);return;}
    for(unsigned i=0;i<trace->contributors;++i) {
        IdleAnimationValue value;
        std::memcpy(value.contribution.data(),reinterpret_cast<void*>(entries+i*0x20),0x20);
        const auto animation=value.contribution[7];
        if(!readableMemory(reinterpret_cast<void*>(animation),0x10)) {trace->reject(IdleWeaponTrace::Rejection::QueryAnimationMemory);return;}
        std::memcpy(value.header.data(),reinterpret_cast<void*>(animation),0x10);
        if(value.header[0]!=idleAnimationName) {trace->rejectAnimationName(i,idleAnimationName,value);return;}
        if(!trace->animation(i,value))return;
    }
    ScopeDrawBinding currentBinding;IdleDrawIdentity current;IdleWeaponTrace *same=nullptr;
    if(!currentIdleDraw(currentBinding,current,same) || same!=trace || current!=identity)
        trace->reject(IdleWeaponTrace::Rejection::QueryCurrent);
}
static void __cdecl animationEnd(void *queue) {
    const auto caller=reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    IdleWeaponTrace *entered=nullptr;
    withNativeFinally([&] {
        ScopeDrawBinding binding;IdleDrawIdentity identity;
        currentIdleDraw(binding,identity,entered);
        observeIdleQuery(queue,caller);
        originalAnimationEnd(queue);
    },[&](bool aborted) noexcept {
        if(aborted) {if(entered)entered->reject(IdleWeaponTrace::Rejection::QueryAbort);producerAbort();}
    });
}
// Lab (SS2VR_LAB_BONE_DUMP=1): the renderer's canonical evaluated bone
// matrices (+0x2eab68 -> +0x20) for bones named Hand/Arm/R_Hand/R_LowerArm/
// R_UpperArm, with their owner model's world, to check what space they are in.
static void labBoneDump() {
    static const bool on = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_BONE_DUMP", v, 2) == 1 && v[0] == L'1'; }();
    static unsigned lines = 0;
    if (!on || lines >= 60 || !stringId) return;
    static uint32_t names[5]{};
    static const char *labels[5]{"Hand", "Arm", "R_Hand", "R_LowerArm", "R_UpperArm"};
    if (!names[0]) for (unsigned i = 0; i < 5; ++i) stringId(&names[i], labels[i]);
    std::span<NativeModelRecord> records;
    std::span<NativeBone> bones;
    if (!rendererArray(0x2eac20, MaxModelRecords, records) || !rendererArray(0x2eac60, MaxNativeBones, bones) || bones.empty())
        return;
    void *evaluated = nullptr;
    Matrix34 *canonical = nullptr;
    int32_t canonicalCount = 0;
    std::memcpy(&evaluated, reinterpret_cast<void *>(engineBase + 0x2eab68), 4);
    if (!readableMemory(evaluated, 0x28)) return;
    std::memcpy(&canonical, static_cast<uint8_t *>(evaluated) + 0x20, 4);
    std::memcpy(&canonicalCount, static_cast<uint8_t *>(evaluated) + 0x24, 4);
    if (canonicalCount < int32_t(bones.size()) || !readableMemory(canonical, size_t(canonicalCount) * sizeof(Matrix34))) return;
    for (size_t b = 0; b < bones.size() && lines < 60; ++b) {
        uint32_t name = 0;
        if (!bones[b].definition || !readableMemory(bones[b].definition, 4)) continue;
        std::memcpy(&name, bones[b].definition, 4);
        for (unsigned k = 0; k < 5; ++k)
            if (name == names[k]) {
                const auto &m = canonical[b].m;
                const int32_t o = bones[b].owner;
                const Matrix34 w = o >= 0 && size_t(o) < records.size() ? records[size_t(o)].world : Matrix34{};
                ++lines;
                log("Lab bone %s index=%zu owner=%d parent=%d canonical t=%.4f,%.4f,%.4f r=%.3f,%.3f,%.3f/%.3f,%.3f,%.3f/%.3f,%.3f,%.3f world t=%.3f,%.3f,%.3f",
                    labels[k], b, o, bones[b].parent, m[3], m[7], m[11], m[0], m[1], m[2], m[4], m[5], m[6],
                    m[8], m[9], m[10], w.m[3], w.m[7], w.m[11]);
                if (lines < 12 && readableMemory(bones[b].definition, 0x80)) {
                    float f[32];
                    std::memcpy(f, bones[b].definition, sizeof(f));
                    char text[600]; size_t at = 0;
                    for (unsigned j = 0; j < 32 && at < sizeof(text) - 16; ++j)
                        at += size_t(std::snprintf(text + at, sizeof(text) - at, " %.4g", double(f[j])));
                    log("Lab bone %s definition:%s", labels[k], text);
                    const uint8_t *e = reinterpret_cast<const uint8_t *>(&bones[b]) + 8;
                    float ev[7]; std::memcpy(ev, e, sizeof(ev));
                    log("Lab bone %s evaluated=%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f", labels[k], ev[0], ev[1], ev[2], ev[3], ev[4], ev[5], ev[6]);
                }
            }
    }
}
// The Serious Bomb split (setSplitModel): the left arm's bones of that model
// move so its Palm_Left sits at the left controller, turned by the rotation
// between the two controllers. Set by the bomb's own placement each render: a
// stale one (the bomb gone, nothing placed in the right hand since) whose
// freed instance address another model reused must not move that model's arm.
static struct { uintptr_t instance = 0; Pose left{}; Quat right{0, 0, 0, 1}; bool valid = false; uint64_t tick = 0; } splitModel;
static uint32_t splitArmName = 0, splitPalmName = 0;
static void applySplitModel() {
    if (!splitModel.valid || !splitModel.instance || !splitArmName || !finite(splitModel.left) ||
        GetTickCount64() - splitModel.tick > 250)
        return;
    std::span<NativeModelRecord> records;
    std::span<NativeMeshRecord> meshes;
    std::span<NativeDrawEntry> draws;
    std::span<NativeBone> bones;
    std::span<PaletteMap> mappings;
    std::span<Matrix34> palette;
    if (!rendererArray(0x2eac20, MaxModelRecords, records) || records.empty() ||
        !rendererArray(0x2eac70, MaxPaletteMatrices, mappings) || mappings.empty() ||
        !rendererArray(0x2eac30, MaxNativeEntries, meshes) || !rendererArray(0x2eac50, MaxNativeEntries, draws) ||
        !rendererArray(0x2eac60, MaxNativeBones, bones) ||
        !rendererArray(0x2eac90, MaxPaletteMatrices, palette, true) || palette.size() < mappings.size())
        return;
    size_t owner = records.size();
    for (size_t i = 0; i < records.size(); ++i)
        if (reinterpret_cast<uintptr_t>(records[i].instance) == splitModel.instance) { owner = i; break; }
    if (owner == records.size()) return;
    void *evaluated = nullptr;
    Matrix34 *canonical = nullptr;
    int32_t canonicalCount = 0;
    std::memcpy(&evaluated, reinterpret_cast<void *>(engineBase + 0x2eab68), 4);
    if (!readableMemory(evaluated, 0x28)) return;
    std::memcpy(&canonical, static_cast<uint8_t *>(evaluated) + 0x20, 4);
    std::memcpy(&canonicalCount, static_cast<uint8_t *>(evaluated) + 0x24, 4);
    if (canonicalCount < int32_t(bones.size()) || size_t(canonicalCount) > MaxNativeBones ||
        !readableMemory(canonical, size_t(canonicalCount) * sizeof(Matrix34)))
        return;
    size_t arm = bones.size(), palm = bones.size();
    std::vector<uint32_t> names(bones.size(), 0);
    for (size_t b = 0; b < bones.size(); ++b) {
        if (!bones[b].definition || !readableMemory(bones[b].definition, 4)) continue;
        std::memcpy(&names[b], bones[b].definition, 4);
        if (bones[b].owner == int32_t(owner) && names[b] == splitArmName) arm = b;
        if (bones[b].owner == int32_t(owner) && names[b] == splitPalmName) palm = b;
    }
    if (arm == bones.size() || palm == bones.size() ||
        !readableMemory(static_cast<const uint8_t *>(bones[palm].definition) + 0x48, sizeof(Matrix34)))
        return;
    Matrix34 inverseBind, bind;
    std::memcpy(&inverseBind, static_cast<const uint8_t *>(bones[palm].definition) + 0x48, sizeof(Matrix34));
    if (!affineInverse(inverseBind, bind)) return;
    const Matrix34 &world = records[owner].world;
    Matrix34 inverseWorld;
    if (!affineInverse(world, inverseWorld)) return;
    const Vec3 palmModel = matrixTranslation(affineMultiply(canonical[palm], bind));
    const Vec3 palmWorld = matrixApply(world, palmModel);
    // World delta: turn about the palm by the controllers' relative rotation,
    // then move the palm to the left controller.
    const Quat turn = normalize(multiply(normalize(splitModel.left.q), inverse(normalize(splitModel.right))));
    const Matrix34 delta = affineMultiply(matrix(Pose{{0, 0, 0, 1}, splitModel.left.p - palmWorld}),
                                          rotationAbout(turn, palmWorld));
    const Matrix34 modelDelta = affineMultiply(inverseWorld, affineMultiply(delta, world));
    if (!finiteMatrix(modelDelta)) return;
    // Bones under Arm_Left on this model.
    std::vector<uint8_t> moved(bones.size(), 0);
    for (size_t b = 0; b < bones.size(); ++b) {
        size_t node = b;
        for (size_t hops = 0; hops <= bones.size() && node < bones.size(); ++hops) {
            if (bones[node].owner != int32_t(owner)) break;
            if (node == arm) { moved[b] = 1; break; }
            if (bones[node].parent < 0) break;
            node = size_t(bones[node].parent);
        }
    }
    std::vector<PaletteModelRange> models;
    std::vector<PaletteMeshRange> meshRanges;
    std::vector<PaletteDrawRange> drawRanges;
    std::vector<int32_t> drawOwners;
    for (const auto &record : records) {
        int32_t first = 0, count = 0;
        std::memcpy(&first, reinterpret_cast<const uint8_t *>(&record) + 8, 4);
        std::memcpy(&count, reinterpret_cast<const uint8_t *>(&record) + 0xc, 4);
        models.push_back({first, count});
    }
    for (const auto &mesh : meshes) meshRanges.push_back({mesh.owner, mesh.first, mesh.count});
    for (const auto &draw : draws) drawRanges.push_back({draw.mesh, draw.first, draw.count});
    if (!paletteDrawOwners(models, meshRanges, drawRanges, mappings, drawOwners)) return;
    if (paletteInvalidated) return;
    for (size_t i = 0; i < mappings.size(); ++i) {
        const auto &map = mappings[i];
        if (map.bone < 0 || size_t(map.bone) >= bones.size() || map.draw < 0 || size_t(map.draw) >= drawOwners.size() ||
            drawOwners[size_t(map.draw)] != int32_t(owner) || !moved[size_t(map.bone)])
            continue;
        const Matrix34 next = affineMultiply(modelDelta, palette[i]);
        if (finiteMatrix(next)) palette[i] = next;
    }
}
static void observeIdlePalette() {
    ScopeDrawBinding binding;IdleDrawIdentity identity;IdleWeaponTrace *trace=nullptr;
    if(paletteInvalidated || !currentIdleDraw(binding,identity,trace))return;
    trace->callbacks|=IdleWeaponTrace::PaletteSeen;
    if(trace->nativeId==2)trace->projectionOpportunities.mark(trace->projectionOpportunities.palette);
    std::span<NativeModelRecord> records;
    if(!rendererArray(0x2eac20,MaxModelRecords,records)) {trace->reject(IdleWeaponTrace::Rejection::PaletteRecords);return;}
    unsigned matches=0;const NativeModelRecord *selected=nullptr;
    for(const auto &record:records)if(record.instance==binding.modelInstance) {++matches;selected=&record;}
    IdleConfigIdentity config;
    uint32_t evaluated=0,linked=0,owner=0,matrices=0;
    int32_t count=0;
    if(matches!=1 || !idleConfig(binding.modelInstance,config)) {trace->reject(IdleWeaponTrace::Rejection::PaletteRootConfig);return;}
    std::memcpy(&evaluated,reinterpret_cast<void*>(engineBase+0x2eab68),4);
    std::memcpy(&linked,static_cast<const uint8_t*>(binding.modelInstance)+0x28,4);
    if(!readableMemory(reinterpret_cast<void*>(evaluated),0x28)) {trace->reject(IdleWeaponTrace::Rejection::PaletteEvaluated);return;}
    std::memcpy(&owner,reinterpret_cast<void*>(evaluated+0x18),4);
    std::memcpy(&matrices,reinterpret_cast<void*>(evaluated+0x20),4);
    std::memcpy(&count,reinterpret_cast<void*>(evaluated+0x24),4);
    trace->notePaletteOwnershipFailure(identity,config,evaluated,linked,owner,
                                      uint32_t(reinterpret_cast<uintptr_t>(binding.modelInstance)),count);
    if(!trace->palette(identity,config,evaluated==linked && owner==reinterpret_cast<uintptr_t>(binding.modelInstance),count))return;
    if(!readableMemory(reinterpret_cast<void*>(matrices),size_t(count)*sizeof(Matrix34))) {trace->reject(IdleWeaponTrace::Rejection::PaletteMatrices);return;}
    Vec3 stretch;std::memcpy(&stretch,binding.modelInstance,sizeof(Vec3));
    if(!trace->pose(selected->world,stretch,{reinterpret_cast<const Matrix34*>(matrices),size_t(count)}))return;
    ScopeDrawBinding currentBinding;IdleDrawIdentity current;IdleWeaponTrace *same=nullptr;
    IdleConfigIdentity currentConfig;
    if(paletteInvalidated || !currentIdleDraw(currentBinding,current,same) || same!=trace || current!=identity ||
       !idleConfig(currentBinding.modelInstance,currentConfig) || currentConfig!=config)trace->reject(IdleWeaponTrace::Rejection::PaletteCurrent);
}
static void __cdecl palettePass() {
#ifdef _MSC_VER
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
#else
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif
    const bool wasActive=paletteReentrant;
    IdleWeaponTrace *entered=nullptr;
    withNativeFinally([&] {
        ScopeDrawBinding binding;IdleDrawIdentity identity;
        currentIdleDraw(binding,identity,entered);
        if(wasActive && entered)entered->reject();
        withFreshNativePalette(caller == engineBase + 0xe2e06, paletteReentrant, paletteInvalidated,
                               [] { originalPalettePass(); }, [] {
                                   if (headTrackingEnabled) postPalette();
                                   labBoneDump();
                                   applySplitModel();
                                   // Independent of remote-player presence and head option.
                                   // Unsupported observations stay native; an aborted extent
                                   // retires the pair through its native-finally cleanup.
                                   observeLocalScope();
                                   observeIdlePalette();
                                   observeRidePalette();
                               }, [] { if (headTrackingEnabled) paletteFault(); });
    },[&](bool aborted) noexcept {
        if(aborted && entered)entered->reject(IdleWeaponTrace::Rejection::PaletteAbort);
        retireNativePaletteInvocation(wasActive,aborted,paletteReentrant,paletteInvalidated);
        if(aborted)producerAbort();
    });
}

static void __cdecl modelPass() {
    const bool wasActive=reentrant;
    withNativeFinally([&] {
        withFreshNativePalette(true,reentrant,modelInvalidated,
            [] {originalModelPass();},[] {postModelPass();},[] {paletteFault();});
    },[&](bool aborted) noexcept {
        retireNativePaletteInvocation(wasActive,aborted,reentrant,modelInvalidated);
        if(aborted)producerAbort();
    });
}

static void clearBinding(uint32_t handle) {
    AcquireSRWLockExclusive(&bindingLock);
    for (auto &binding : bindings)
        if (!handle || binding.playerHandle == handle)
            binding = {};
    // Keep the immutable bank until commit; invalidity survives desktop restore.
    for (const auto &entry : frozen)
        if (pairOwner.load(std::memory_order_acquire) && entry.bodyValid && (!handle || entry.binding.playerHandle == handle))
            pairInvalid.store(true, std::memory_order_release);
    // Foreign lifecycle callers never read the main-thread observation fields.
    // Conservatively decline any active observation while holding the bank lock.
    if(pairOwner.load(std::memory_order_acquire))rideObservationDeclined=true;
    ReleaseSRWLockExclusive(&bindingLock);
}

} // namespace

void setSplitModel(uintptr_t instance, const Pose &left, Quat right, bool valid) noexcept {
    splitModel.instance = instance;
    splitModel.left = left;
    splitModel.right = right;
    splitModel.valid = valid && instance;
    splitModel.tick = GetTickCount64();
}
bool initialize(HMODULE engine, HMODULE core, HMODULE sam, HookInstallerRva install, bool enableHeadTracking,
                bool observeRideControl,bool produceGrips) {
    if (ready.load(std::memory_order_acquire))
        return true;
    if (!engine || !core || !sam || !install)
        return false;
    bool ok = true;
#define S(module, name, out) ok = symbol(module, name, out) && ok
    S(core, "?thrIsThisMainThread@SeriousEngine@@YAHXZ", isMainThread);
    S(core, "?_st_idInvalid@SeriousEngine@@3UInvalidIdent@1@B", invalidId);
    S(core, "?strConvertStringToID@SeriousEngine@@YA?AVIDENT@1@PBD@Z", stringId);
    S(core, "?hvHandleToPointer@SeriousEngine@@YAPAXK@Z", resolve);
    S(core, "?hvPointerToHandle@SeriousEngine@@YAKPAX@Z", pointerHandle);
    S(sam, "?IsLocal@CPuppetEntity@SeriousEngine@@QAEHXZ", isLocal);
    S(sam, "?IsAlive@CPuppetEntity@SeriousEngine@@UAEHXZ", isAlive);
    S(sam, "?GetModelRenderable@CPuppetEntity@SeriousEngine@@UAEPAVCModelRenderable@2@XZ", getModelRenderable);
    S(engine, "?GetModelInstance@CModelRenderable@SeriousEngine@@QAEPAVCModelInstance@2@XZ", getModelInstance);
    S(engine, "?mdlGetChildName@SeriousEngine@@YA?AVIDENT@1@PBVCModelConfigChild@1@@Z", getChildName);
    S(engine, "?mdlGetChildOffset@SeriousEngine@@YA?AVQuatVect@1@PBVCModelConfigChild@1@@Z", getChildOffset);
    S(engine, "?mdlGetChildInstance@SeriousEngine@@YAPAVCModelInstance@1@PAVCModelConfigChild@1@@Z", getChildInstance);
    S(sam, "?GetWeaponTool@CBaseWeaponEntity@SeriousEngine@@QAEPAVCCharacterTool@2@W4PlayerHand@2@@Z", getWeaponTool);
#undef S
    toolFileStemId = reinterpret_cast<ToolFileStemId>(reinterpret_cast<uintptr_t>(sam) + 0x1fd4f0);
    engineBase = reinterpret_cast<uintptr_t>(engine);
    seatGameBase = reinterpret_cast<uintptr_t>(sam);
    if (!ok || !toolFileStemId ||
        !install(engine, 0xdbc90, reinterpret_cast<void *>(modelPass), reinterpret_cast<void **>(&originalModelPass)) ||
        !originalModelPass)
        return false;
    if (enableHeadTracking) {
        stringId(&headName, "Head");
        if (!invalidId || headName == *invalidId)
            return false;
        static const char *const arms[2][3]{{"L_UpperArm", "L_LowerArm", "L_Hand"}, {"R_UpperArm", "R_LowerArm", "R_Hand"}};
        for (unsigned side = 0; side < 2; ++side)
            for (unsigned k = 0; k < 3; ++k) stringId(&armNames[side][k], arms[side][k]);
    }
    stringId(&scopeName, "Scope");
    stringId(&splitArmName, "Arm_Left");
    stringId(&splitPalmName, "Palm_Left");
    stringId(&scopeBoneName, "Sniper");
    // The ride's Main and Seat bones (names to match) are needed by the grips
    // too: set only for the lab ride diagnostic, no vehicle handle was ever admitted.
    if(observeRideControl || produceGrips) {
        stringId(&mainName,"Main");stringId(&seatName,"Seat");
        if(!invalidId || mainName==*invalidId || seatName==*invalidId || mainName==seatName)return false;
    }
    if (!invalidId || scopeName == *invalidId || scopeBoneName == *invalidId ||
        !install(engine, 0xdde30, reinterpret_cast<void *>(palettePass),
                 reinterpret_cast<void **>(&originalPalettePass)) || !originalPalettePass)
        return false;
    headTrackingEnabled = enableHeadTracking; // Writes require a frozen stereo pair.
    rideObservationEnabled = observeRideControl || produceGrips;
    rideDiagnosticEnabled=observeRideControl;rideGripEnabled=produceGrips;
    if(idleProbeWeaponSupported(selectedIdleProbeWeapon())) {
        stringId(&idleAnimationName,"Idle");
        if(idleAnimationName==*invalidId ||
           !install(engine,0xbbf0,reinterpret_cast<void*>(animationEnd),
                    reinterpret_cast<void**>(&originalAnimationEnd)) || !originalAnimationEnd)return false;
        const auto shaders=GetModuleHandleW(L"Shaders.dll");
        if(shaders) {
            HMODULE pinned=nullptr;
            if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                    reinterpret_cast<LPCWSTR>(shaders),&pinned) || pinned!=shaders ||
               !nativeModuleFingerprint(shaders,"de7652d61f5af2ec8b218b902d09820b3ef76c1d796f23d66a31ea35374fd642") ||
               !readableMemory(reinterpret_cast<void *>(engineBase+0x2e5bdc),4) ||
               !readableMemory(reinterpret_cast<void *>(engineBase+0x2e63d8),0x198) ||
               !readableMemory(reinterpret_cast<void *>(engineBase+0x2eab48),12) ||
               reinterpret_cast<uintptr_t>(GetProcAddress(engine,"?shaGetFogFactors@SeriousEngine@@YA?AVVector4f@1@XZ"))!=engineBase+0x770b0)
                return false;
            const auto sb=reinterpret_cast<uintptr_t>(shaders);
            const auto *table=reinterpret_cast<const int32_t *>(sb+0x2834c);
            if(!readableMemory(table,12) || table[0]!=23 || table[1]!=7 || table[2]!=8)return false;
            const auto *polyTable=reinterpret_cast<const int32_t *>(sb+0x27ccc);
            if(!readableMemory(polyTable,12) || polyTable[0]!=21 || polyTable[1]!=5 || polyTable[2]!=6)return false;
            projectionShaderBase=sb;
            if(!install(shaders,0x6920,reinterpret_cast<void *>(projectionSlots),
                       reinterpret_cast<void **>(&originalProjectionSlots)) || !originalProjectionSlots ||
               !install(engine,0x770b0,reinterpret_cast<void *>(projectionFog),
                       reinterpret_cast<void **>(&originalProjectionFog)) || !originalProjectionFog)return false;
            log("Lab native projection bookends installed source=1 slotsReturn=f4ff fogReturn=fc8a; source=2 slotsReturn=856b fogReturn=8ca7 ID13-only");
        } else log("Lab native projection bookends unavailable: Shaders.dll not loaded; no late hook installation");
    }
    ready.store(true, std::memory_order_release);
    return true;
}

void observePlayer(void *player) {
    if (!checkNativeThread()) {
        clearBinding(0);
        return;
    }
    if (!ready.load(std::memory_order_acquire) || !player) {
        invalidatePlayer(player);
        return;
    }
    Binding next;
    next.playerHandle = pointerHandle(player);
    if (!next.playerHandle || !readNativeRider(player, next.rider)) {
        invalidatePlayer(player);
        return;
    }
    const bool local = isLocal(player);
    const bool alive = !local && isAlive(player);
    if (!nativeRiderCurrent(player, next.rider) || local || !alive) {
        invalidatePlayer(player);
        return;
    }
    next.sample = multiplayer::presentation(player);
    if (!validSample(next, next.sample)) {
        invalidatePlayer(player);
        return;
    }
    auto renderable = getModelRenderable(player);
    if (!nativeRiderCurrent(player, next.rider) || !renderable) {
        invalidatePlayer(player);
        return;
    }
    next.bodyModelInstance = getModelInstance(renderable);
    if (!nativeRiderCurrent(player, next.rider) || !next.bodyModelInstance) {
        invalidatePlayer(player);
        return;
    }
    if (next.rider.handheld()) {
        for (unsigned hand = 0; hand < 2; ++hand) {
            if (!nativeRiderCurrent(player, next.rider)) {
                invalidatePlayer(player);
                return;
            }
            uint32_t handle = nativeHandHandle(player, hand);
            void *weapon = handle ? resolve(handle) : nullptr;
            if (!weapon || !read32(weapon, WeaponPropertiesResource))
                continue;
            int nativeId = nativeWeaponId(weapon);
            void *tool = getWeaponTool(weapon, int(hand));
            if (!nativeRiderCurrent(player, next.rider)) {
                invalidatePlayer(player);
                return;
            }
            uint32_t toolId = 0;
            const bool named = tool && toolFileStemId(tool, &toolId);
            if (!nativeRiderCurrent(player, next.rider)) {
                invalidatePlayer(player);
                return;
            }
            if (!validNativeId(nativeId) || !named || next.sample.pose.nativeWeaponId[hand] != nativeId || !toolId)
                continue;
            next.handHandle[hand] = handle;
            next.nativeId[hand] = int16_t(nativeId);
            next.toolId[hand] = toolId;
            next.handEligible[hand] = true;
        }
    }
    if (!nativeRiderCurrent(player, next.rider)) {
        invalidatePlayer(player);
        return;
    }
    AcquireSRWLockExclusive(&bindingLock);
    Binding *destination = nullptr;
    for (auto &binding : bindings)
        if (binding.playerHandle == next.playerHandle) {
            destination = &binding;
            break;
        }
    if (!destination)
        for (auto &binding : bindings)
            if (!binding.playerHandle) {
                destination = &binding;
                break;
            }
    if (destination) {
        for (const auto &entry : frozen)
            if (pairOwner.load(std::memory_order_acquire) && entry.bodyValid && entry.binding.playerHandle == next.playerHandle &&
                !compatibleBinding(entry.binding, next))
                pairInvalid.store(true, std::memory_order_release);
        *destination = next;
    }
    ReleaseSRWLockExclusive(&bindingLock);
}

void noteSimulationThread() {
    if (!ready.load(std::memory_order_acquire))
        return;
    if (!isMainThread || !isMainThread()) {
        simulationThread.store(MAXDWORD, std::memory_order_release);
        pairInvalid.store(true, std::memory_order_release);
        return;
    }
    const DWORD current = GetCurrentThreadId();
    DWORD unknown = 0;
    simulationThread.compare_exchange_strong(unknown, current, std::memory_order_acq_rel);
    checkNativeThread();
}
bool checkNativeThread() {
    const DWORD owner = simulationThread.load(std::memory_order_acquire);
    if (owner == GetCurrentThreadId() && isMainThread && isMainThread())
        return true;
    if (owner) {
        simulationThread.store(MAXDWORD, std::memory_order_release);
        pairInvalid.store(true, std::memory_order_release);
    }
    return false;
}
bool ownsNativeThread() {
    const DWORD owner = simulationThread.load(std::memory_order_acquire);
    return scopeObservationThread(owner, GetCurrentThreadId(), isMainThread && isMainThread());
}
bool copyModelConfigurationStretch(void *instance,uint32_t expectedConfigurationVtable,
                                   IdleConfigIdentity &identity,Vec3 &stretch) {
    identity={};stretch={};
    // The caller supplies its pinned native configuration type and owns the
    // borrow. Headless authority must not depend on renderer initialization.
    if(!expectedConfigurationVtable)return false;
    IdleConfigIdentity before,after;
    Vec3 first,last;
    if(!idleConfig(instance,before,expectedConfigurationVtable))return false;
    std::memcpy(&first,instance,sizeof(first));
    if(!idleConfig(instance,after,expectedConfigurationVtable))return false;
    std::memcpy(&last,instance,sizeof(last));
    if(before!=after || std::memcmp(&first,&last,sizeof(first)) ||
       !std::isfinite(first.x) || !std::isfinite(first.y) || !std::isfinite(first.z))return false;
    identity=before;stretch=first;return true;
}
bool copyIdleRaster(void *instance,IdleRasterCopy &out) {
    std::optional<IdleRasterStorage> storage(std::in_place);bool observed=false;
    // Owners live above this frame. GNU allocation failure declines diagnostics
    // locally; foreign unwind retires every vector before crossing the caller.
    withNativeFinally([&] {observed=readIdleNativeDraw(instance,out,*storage,IdleNativeDrawPolicy::SingleAffine);},[&](bool aborted) noexcept {
        storage.reset();if(aborted)observed=false;
    });
    return observed;
}
bool copyIdlePalette(void *instance,IdlePaletteCopy &out,IdlePaletteBoundaryCopy *diagnostic) {
    std::optional<IdleRasterStorage> storage(std::in_place);bool observed=false;
    withNativeFinally([&] {
        IdleRasterCopy metadata;
        observed=readIdleNativeDraw(instance,metadata,*storage,IdleNativeDrawPolicy::Id2Palette,&out,-1,nullptr,diagnostic);
    },[&](bool aborted) noexcept {storage.reset();if(aborted){observed=false;if(diagnostic){*diagnostic={};diagnostic->wrapper=99;}}});
    if(!observed)out={};
    return observed;
}
// Cheap pre-check for the per-draw probe: a ride main draw can only be observed
// while a frozen ride pair is current on this thread (same first checks as below).
bool rideDrawPossible() noexcept {
    return rideObservationOwnerCurrent(currentFrozenOwner) && frozenRide.valid() && !rideObservationDeclined;
}
__attribute__((noinline)) bool copyCurrentRideMainDraw(RideMainDrawCopy &out) {
    if(!rideObservationOwnerCurrent(currentFrozenOwner))return false;
    out={};
    const int eye=nativeWorldEye();
    if(eye < -1 || eye > 1 || !frozenRide.valid() || rideObservationDeclined)return false;
    const size_t index=eye<0?0:size_t(eye);
    if(!rideFrameSeen[index])return false;
    std::optional<IdleRasterStorage> storage(std::in_place);bool copied=false;
    withNativeFinally([&] {
        RidePaletteBookend before,after;bool selected=false,afterSelected=false;
        if(!copyRidePaletteBookend(before,selected))return;
        IdleRasterCopy raster;
        if(!readIdleNativeDraw(reinterpret_cast<void*>(before.frame.identity.instance),raster,*storage,
             IdleNativeDrawPolicy::RideMainMapping,nullptr,int32_t(before.frame.mainBone),&out))return;
        out.bank=currentFrozenOwner;out.eye=eye;out.identity=before.frame.identity;
        out.configuration=raster.renderConfig.configuration;out.file=raster.renderConfig.file;
        out.resource=uint32_t(raster.renderConfig.resource);out.modelRecord=raster.modelRecord;
        std::memcpy(&out.lod,reinterpret_cast<void*>(raster.projectionModelAddress+0x5c),4);
        out.drawRecord=raster.drawRecord;out.surface=raster.surface;out.surfaceName=raster.surfaceName;
        out.instance=raster.instance;out.bone=uint32_t(raster.bone);out.layout=raster.layout;
        if(raster.rootConfig!=raster.renderConfig ||
           !rideMainDrawLinked(out,before.frame) ||
           !copyRidePaletteBookend(after,afterSelected) || before!=after ||
           !rideMainDrawLinked(out,rideFrames[index]))return;
        copied=true;
    },[&](bool aborted) noexcept {storage.reset();if(aborted)copied=false;});
    if(!copied)out={};
    return copied;
}
static bool rideDrawBankMatches(const RideMainDrawCopy &copy) noexcept {
    if(copy.eye < -1 || copy.eye > 1 || copy.eye!=nativeWorldEye() || rideObservationDeclined)return false;
    const size_t index=copy.eye<0?0:size_t(copy.eye);
    return rideFrameSeen[index] && copy.identity==frozenRide && rideMainDrawLinked(copy,rideFrames[index]);
}
__attribute__((noinline)) bool rideMainDrawCurrent(const RideMainDrawCopy &copy) noexcept {
    if(!rideObservationOwnerCurrent(copy.bank))return false;
    return rideDrawBankMatches(copy);
}
__attribute__((noinline)) bool claimRideGpuAttempt(const RideMainDrawCopy &copy) noexcept {
    if(!rideObservationOwnerCurrent(copy.bank))return false;
    if(!rideDrawBankMatches(copy) || !rideSurfaceSupported(copy.layout))return false;
    const size_t index=copy.eye<0?0:size_t(copy.eye);
    if(rideGripEnabled && copy.eye>=0) {
        if(rideGpuAttempts[index])return false;
        ++rideGpuAttempts[index];return true;
    }
    if(!rideDiagnosticEnabled)return false;
    return chargeRideGpuAttempt(rideGpuAttempts[index],rideGpuSessionAttempts,
        rideObservationRows.load(std::memory_order_relaxed)>=32);
}
__attribute__((noinline)) void recordRideMainDraw(const RideMainDrawCopy &copy,const RideDrawGpuCopy *gpu) noexcept {
    if(!rideObservationOwnerCurrent(copy.bank))return;
    if(!rideDrawBankMatches(copy))return;
    const size_t index=copy.eye<0?0:size_t(copy.eye);
    auto &count=rideMainDrawCounts[index];
    if(count==RideMainDrawCapacity) {rideMainDrawOverflow[index]=true;return;}
    rideMainDraws[index][count]=copy;
    rideMainGpu[index][count].copied=false;
    if(gpu && gpu->copied)rideMainGpu[index][count]=*gpu;
    ++count;
}
uint32_t idlePaletteProjectionSequence(const IdleProjectionProbe &probe,const IdlePaletteCopy &copy) noexcept {
    if(!idleProjectionConfigured())return 0;
    IdleProjectionSnapshot now;
    captureProjectionSnapshot(now);
    return probe.currentSequence(now,copy.metadata.modelAddress,copy.metadata.drawAddress,copy.world);
}
bool idleProjectionConfigured() noexcept {
    return ready.load(std::memory_order_acquire) && projectionShaderBase &&
        originalProjectionSlots && originalProjectionFog;
}
ScopeRasterStatus copyScopeRaster(void *instance, Matrix34 &affine, ScopeSurfaceLayout &layout) {
    return readScopeRaster(instance,affine,layout);
}
void invalidatePlayer(void *player) {
    if (!checkNativeThread()) {
        clearBinding(0);
        return;
    }
    if (!player || !pointerHandle)
        return;
    clearBinding(pointerHandle(player));
}

uint32_t freezePair(uint32_t localPlayer) {
    uint32_t issued=0;
    if(currentFrozenOwner || pairOwner.load(std::memory_order_acquire) || presentationBusy ||
       presentationSuppressed || !ownsNativeThread())return 0;
    withPresentationBindings<true>([&](const auto& guard) {
        if(currentFrozenOwner || pairOwner.load(std::memory_order_acquire))return;
        issued=nextFrozenPresentationOwner(nextPairOwner);
        if(!issued)return;
        currentFrozenOwner=issued;
        frozen = {};
        frozenRide={};frozenRideConfig={};rideFrames={};rideFrameSeen={};rideObservationDeclined=false;
        rideMainDraws={};rideMainDrawCounts={};rideMainDrawOverflow={};rideGpuAttempts={};
        for(auto &eye:rideMainGpu)for(auto &g:eye)g.copied=false;
        pairThread = GetCurrentThreadId();
        pairInvalid.store(false, std::memory_order_release);
        pairOwner.store(issued,std::memory_order_release);
        if(rideObservationEnabled && localPlayer) {
            RideRenderIdentity identity,after;IdleConfigIdentity config;
            if(copyNativeRideRenderIdentity(localPlayer,identity) &&
               idleConfig(reinterpret_cast<void*>(identity.instance),config) &&
               copyNativeRideRenderIdentity(localPlayer,after) && after==identity) {
                frozenRide=identity;frozenRideConfig=config;
            }
            static unsigned traced=0;
            if(traced<6 && identity.ride) {++traced;log("Ride render frozen=%d config=%08x",int(frozenRide.valid()),config.configuration);}
        }
        // Native model/handle access stays on the established simulation thread.
        // A bank with no admissible remote players is a valid native-only pair.
        if (ready.load(std::memory_order_acquire) && checkNativeThread()) {
            const uint64_t now = GetTickCount64();
            for (size_t i = 0; i < bindings.size(); ++i) {
                auto &entry = frozen[i];
                entry.binding = bindings[i];
                auto sample = guard.sample(entry.binding.playerHandle);
                if (!sample.negotiated || !sample.valid || !sample.avatar || !sample.incarnation ||
                    sample.avatar != entry.binding.playerHandle ||
                    sample.incarnation != entry.binding.sample.incarnation || now < sample.receivedMs ||
                    now - sample.receivedMs > network::MaxPoseAgeMs) {
                    entry = {};
                    continue;
                }
                entry.binding.sample = sample;
                void *player = nullptr;
                entry.bodyValid = currentBinding(entry.binding, sample, player, &guard) &&
                                  bodyAnchor(player, entry.binding.rider, entry.body);
                if (!entry.bodyValid)
                    entry = {};
            }
        }
    },[&](bool aborted) noexcept {
        if(aborted)retireFrozenPresentationOwner(pairOwner,pairInvalid,currentFrozenOwner,frozenPair,issued);
    });
    if(frozenPresentationOwnerMatches(pairOwner.load(std::memory_order_acquire),currentFrozenOwner,issued))return issued;
    retireFrozenPresentationOwner(pairOwner,pairInvalid,currentFrozenOwner,frozenPair,issued);
    return 0;
}

bool commitPair(Slot &slot, const Request &request, bool localEligible, uint32_t owner,const RideGripRig *gripRig) {
    // Both remote locks remain held through Ready; local caller holds snapshotLock.
    bool committed=false;
    withPresentationBindings([&](const auto& guard) {
        committed=commitNativeFrame(slot,request,localEligible &&
            frozenPresentationOwnerMatches(pairOwner.load(std::memory_order_acquire),currentFrozenOwner,owner) &&
            validatePair(guard));
        if(committed)publishRideObservation(owner,true,&request,gripRig);
    },[&](bool) noexcept {
        invalidateFrozenPresentationOwner(pairOwner,pairInvalid,currentFrozenOwner,owner);
    });
    return committed;
}

void useFrozenPair(bool enabled) {
    frozenPair = enabled;
}
uint32_t beginMonoPresentation(uint32_t localPlayer) {
    if((!headTrackingEnabled && !rideObservationEnabled) || !hooksReady.load(std::memory_order_acquire) ||
       !ready.load(std::memory_order_acquire) || !ownsNativeThread())return 0;
    const uint32_t owner=freezePair(localPlayer);
    if(owner)frozenPair=true;
    return owner;
}
void completeMonoPresentation(uint32_t owner,bool completed) {
    if(completed)publishRideObservation(owner,false);
}
void retirePresentation(uint32_t owner) noexcept {
    retireFrozenPresentationOwner(pairOwner,pairInvalid,currentFrozenOwner,frozenPair,owner);
}
bool suppressNestedPresentation() noexcept {
    const bool previous=presentationSuppressed;
    presentationSuppressed=true;
    invalidateFrozenPresentationOwner(pairOwner,pairInvalid,currentFrozenOwner,currentFrozenOwner);
    return previous;
}
void restorePresentationSuppression(bool previous) noexcept {
    presentationSuppressed=previous;
}
bool presentationSuppressionCurrent() noexcept {
    return presentationSuppressed;
}
void invalidatePresentationForReset(bool activeDraw) noexcept {
    // A reset may return to a suspended original world draw. Keep BOTH
    // adapters suppressed until that draw's outer finally restores its scope.
    // A reset outside a draw has no such finally and must not latch suppression.
    presentationSuppressed=frozenPresentationResetSuppressed(presentationSuppressed,activeDraw,currentFrozenOwner);
    invalidateFrozenPresentationOwner(pairOwner,pairInvalid,currentFrozenOwner,currentFrozenOwner);
}

} // namespace ss2vr::game::remote_render
