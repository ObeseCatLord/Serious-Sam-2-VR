#include "roomscale_resource_gate.hpp"
#include "native_finally.hpp"
#include "native_memory.hpp"
#include "common/optional_query.hpp"
#include <array>
#include <cstring>

#if !defined(__GNUC__) || defined(__clang__) || !defined(__i386__) || !defined(__MINGW32__)
#error Optional native query gates require the verified GNU MinGW x86 target
#endif

namespace ss2vr::game {
namespace {
thread_local bool *unavailable = nullptr;
bool gatesQueued = false;
uintptr_t primitiveHullTable=0,modelHullTable=0,fluidHullTable=0,forceHullTable=0;
uintptr_t hullEngineBase=0;
struct InstalledGate { uintptr_t address=0,target=0; };
std::array<InstalledGate,26> installedGates{};
bool resourceGatesEnabled() noexcept {
    if(!gatesQueued)return false;
    for(const auto& gate:installedGates) {
        if(!gate.address||!gate.target||!readableMemory(reinterpret_cast<void*>(gate.address),5))return false;
        const auto* bytes=reinterpret_cast<const unsigned char*>(gate.address);
        int32_t relative=0;std::memcpy(&relative,bytes+1,4);
        if(bytes[0]!=0xe9||uint32_t(gate.address+5+uint32_t(relative))!=uint32_t(gate.target))return false;
    }
    return true;
}
}
// Called inside a full register/flags/FP save. No native call, allocation,
// exception, resource mutation, or output publication is permitted here.
// 0 forwards native instructions; 1 takes a normal cancellation suffix;
// 2 skips an originally clear replacement branch using its native destination.
extern "C" int __cdecl ss2vrResourceDecision(const uint8_t *resource, unsigned kind) noexcept {
    if (kind==3) {
        if (!unavailable) return static_cast<int>(OptionalQueryDecision::original);
        if (*unavailable) return static_cast<int>(OptionalQueryDecision::cancel);
        if (!resource) return static_cast<int>(optionalQueryTargetDecision(unavailable,false));
        bool supported=false;
        // PUSHAD's saved ESP points to the pushed flags word. The four native
        // cdecl arguments start one word above it, before the call instruction.
        const auto *args=reinterpret_cast<volatile const int32_t*>(resource+4);
        const int32_t handle=args[0];
        if (hullEngineBase && handle>0 && args[1]==0x93) {
            const auto table=*reinterpret_cast<volatile const uintptr_t*>(hullEngineBase+0x2e68e4);
            const int32_t count=*reinterpret_cast<volatile const int32_t*>(hullEngineBase+0x2e68e8);
            const uint64_t offset=uint64_t(uint32_t(handle)-1u)*12u;
            if (table && handle<=count && uint64_t(table)+offset+12u<=(uint64_t(1)<<32)) {
                const auto entry=table+static_cast<uintptr_t>(offset);
                supported=optionalQueryBufferReadable(
                    *reinterpret_cast<volatile const uint32_t*>(entry),
                    *reinterpret_cast<volatile const int32_t*>(entry+4),
                    *reinterpret_cast<volatile const int16_t*>(entry+8),
                    *reinterpret_cast<volatile const uint8_t*>(entry+10),args[2],args[3]);
            }
        }
        return static_cast<int>(optionalQueryTargetDecision(unavailable,supported));
    }
    if (kind==2) {
        if (!unavailable) return static_cast<int>(OptionalQueryDecision::original);
        if (*unavailable) return static_cast<int>(OptionalQueryDecision::cancel);
        const auto table=reinterpret_cast<uintptr_t>(resource);
        uint32_t target=0;
        if (table && table==primitiveHullTable) target=0x525b0;
        else if (table && table==modelHullTable) target=0x51430;
        else if (table && table==fluidHullTable) target=0x4fa20;
        else if (table && table==forceHullTable) target=0x111f80;
        // Read a slot only after recognizing the resident native table; an
        // unknown target may be arbitrary and is never dereferenced here.
        const bool supported=target && hullEngineBase &&
            *reinterpret_cast<volatile const uintptr_t*>(table+0x40)==hullEngineBase+target;
        return static_cast<int>(optionalQueryTargetDecision(unavailable,supported));
    }
    const bool pending=unavailable && !*unavailable && kind==0 &&
        (!resource || (*reinterpret_cast<volatile const uint8_t*>(resource+4)&1u));
    return static_cast<int>(optionalQueryDecision(unavailable,pending,kind));
}

#define STRING_(x) #x
#define STRING(x) STRING_(x)
// Slots in pushal: EDI0, ESI4, EBP8, savedESP12, EBX16, EDX20, ECX24, EAX28.
// The native instruction's FP stack and flags survive both continuations.
// Branch on the helper result BEFORE restoring the original flags. Two restore
// suffixes avoid modifying a native return address or relying on a fake RET.
#define RESOURCE_GATE(name, receiverSlot, kind) \
    extern "C" { void *name##_original=nullptr; void *name##_cancel=nullptr; void *name##_clear=nullptr; } \
    extern "C" __attribute__((naked,used)) void name() { __asm__( \
        "pushfl\n\tpushal\n\tcld\n\tmovl %esp,%ebp\n\tandl $-16,%esp\n\tsubl $512,%esp\n\t" \
        "fxsave (%esp)\n\tfninit\n\tfldcw (%esp)\n\tsubl $8,%esp\n\t" \
        "pushl $" STRING(kind) "\n\tpushl " STRING(receiverSlot) "(%ebp)\n\t" \
        "call _ss2vrResourceDecision\n\taddl $16,%esp\n\tcmpl $1,%eax\n\tje 1f\n\tcmpl $2,%eax\n\tje 2f\n\t" \
        "fxrstor (%esp)\n\tmovl %ebp,%esp\n\tpopal\n\tpopfl\n\tjmp *_" STRING(name) "_original\n\t" \
        "1: fxrstor (%esp)\n\tmovl %ebp,%esp\n\tpopal\n\tpopfl\n\tjmp *_" STRING(name) "_cancel\n\t" \
        "2: fxrstor (%esp)\n\tmovl %ebp,%esp\n\tpopal\n\tpopfl\n\tjmp *_" STRING(name) "_clear\n\t"); }

RESOURCE_GATE(resourceHullDispatch,20,2)
RESOURCE_GATE(resourceCollisionBuffer,12,3)
RESOURCE_GATE(resourceWorld,24,0)
RESOURCE_GATE(resourceModel,24,0)
RESOURCE_GATE(resourceConfiguration,0,0)
RESOURCE_GATE(resourceSkeleton,0,0)
RESOURCE_GATE(resourceMesh,4,0)
RESOURCE_GATE(resourceOverride,0,0)
RESOURCE_GATE(resourceChildOne,4,0)
RESOURCE_GATE(resourceChildTwo,4,0)
RESOURCE_GATE(resourceMaterialInherited,4,0)
RESOURCE_GATE(resourceMaterialDirect,4,0)
RESOURCE_GATE(resourcePrimitiveMaterial,4,0)
RESOURCE_GATE(resourceCollisionConfigOne,0,0)
RESOURCE_GATE(resourceCollisionConfigTwo,0,0)
RESOURCE_GATE(resourceCollisionConfigThree,0,0)
RESOURCE_GATE(resourceCollisionMeshOne,24,0)
RESOURCE_GATE(resourceCollisionConfigFour,0,0)
RESOURCE_GATE(resourceCollisionMeshTwo,0,0)
RESOURCE_GATE(resourceCollisionVertexResource,24,0)
RESOURCE_GATE(resourceCollisionMaterial,0,0)
RESOURCE_GATE(resourceMaterialReturn,0,1)
RESOURCE_GATE(resourceChildOneReturn,0,1)
RESOURCE_GATE(resourceChildTwoReturn,0,1)
RESOURCE_GATE(resourcePrepareReturn,0,1)
RESOURCE_GATE(resourceQueryReturn,0,1)

extern "C" {
void *resourceModelEarlyExit=nullptr, *resourcePrepareCleanup=nullptr;
void *resourceScratchReset=nullptr, *resourceQueryCleanup=nullptr;
void *resourceCollisionEarlyExit=nullptr;
}
extern "C" __attribute__((naked,used)) void resourceCancelModelPreparation() { __asm__(
    "leal -0x58(%ebp),%esp\n\tjmp *_resourceModelEarlyExit\n\t"); }
extern "C" __attribute__((naked,used)) void resourceCancelCollisionQuery() { __asm__(
    "leal -0xc0(%ebp),%esp\n\tjmp *_resourceCollisionEarlyExit\n\t"); }
extern "C" __attribute__((naked,used)) void resourceCancelMaterialGetter() { __asm__(
    "xorl %eax,%eax\n\tpopl %edi\n\tpopl %esi\n\tpopl %ebp\n\tret\n\t"); }
extern "C" __attribute__((naked,used)) void resourceCancelOuterPreparation() { __asm__(
    "addl $20,%esp\n\tjmp *_resourcePrepareCleanup\n\t"); }
extern "C" __attribute__((naked,used)) void resourceCancelQueryTail() { __asm__(
    "addl $12,%esp\n\txorl %esi,%esi\n\tcall *_resourceScratchReset\n\tjmp *_resourceQueryCleanup\n\t"); }

struct ResourceGateBinding {
    uint32_t rva, clearRva;
    std::array<uint8_t,6> prefix;
    uint8_t prefixSize;
    void *entry;
    void **original, **cancel, **clear;
    void *cancelEntry;
    uint32_t cancelRva;
};
#define BIND(name,rva,clearRva,bytes,size,abort,abortRva) \
    ResourceGateBinding{rva,clearRva,bytes,size,reinterpret_cast<void*>(&name), \
        &name##_original,&name##_cancel,&name##_clear,abort,abortRva}
#define BYTES(...) (std::array<uint8_t,6>{__VA_ARGS__})

bool queueRoomscaleResourceGates(HMODULE engine, RoomscaleInternalHook registrar) {
    if (!engine || !registrar || gatesQueued) return false;
    auto *base=reinterpret_cast<uint8_t*>(engine);
    auto *modelAbort=reinterpret_cast<void*>(&resourceCancelModelPreparation);
    auto *collisionAbort=reinterpret_cast<void*>(&resourceCancelCollisionQuery);
    auto *materialAbort=reinterpret_cast<void*>(&resourceCancelMaterialGetter);
    const std::array bindings{
        BIND(resourceCollisionBuffer,0xcba9f,0,BYTES(0xe8,0x3c,0x3e,0xfc,0xff),5,collisionAbort,0),
        BIND(resourceHullDispatch,0x2f9d1,0,BYTES(0xff,0x52,0x40,0x85,0xc0),5,nullptr,0x2f9de),
        BIND(resourceWorld,0x29114,0x29146,BYTES(0xf6,0x41,0x04,0x01,0x74,0x2c),6,nullptr,0x29160),
        BIND(resourceModel,0xda419,0xda439,BYTES(0xf6,0x41,0x04,0x01,0x74,0x1a),6,nullptr,0xda476),
        BIND(resourceConfiguration,0xe17a1,0xe17c2,BYTES(0xf6,0x47,0x04,0x01,0x74,0x1b),6,modelAbort,0),
        BIND(resourceSkeleton,0xe1806,0xe1827,BYTES(0xf6,0x47,0x04,0x01,0x74,0x1b),6,modelAbort,0),
        BIND(resourceMesh,0xe1a47,0xe1a68,BYTES(0xf6,0x46,0x04,0x01,0x74,0x1b),6,modelAbort,0),
        BIND(resourceOverride,0xe1d5e,0xe1d7f,BYTES(0xf6,0x47,0x04,0x01,0x74,0x1b),6,modelAbort,0),
        BIND(resourceChildOne,0xe1f56,0xe1f77,BYTES(0xf6,0x46,0x04,0x01,0x74,0x1b),6,modelAbort,0),
        BIND(resourceChildTwo,0xe1ff9,0xe201a,BYTES(0xf6,0x46,0x04,0x01,0x74,0x1b),6,modelAbort,0),
        BIND(resourceMaterialInherited,0xe6932,0xe6972,BYTES(0xf6,0x46,0x04,0x01,0x74,0x3a),6,materialAbort,0),
        BIND(resourceMaterialDirect,0xe6951,0xe6972,BYTES(0xf6,0x46,0x04,0x01,0x74,0x1b),6,materialAbort,0),
        BIND(resourcePrimitiveMaterial,0x528e4,0x52905,BYTES(0xf6,0x46,0x04,0x01,0x74,0x1b),6,nullptr,0x52918),
        BIND(resourceCollisionConfigOne,0xcb407,0xcb428,BYTES(0xf6,0x47,0x04,0x01,0x74,0x1b),6,collisionAbort,0),
        BIND(resourceCollisionConfigTwo,0xcb438,0xcb459,BYTES(0xf6,0x47,0x04,0x01,0x74,0x1b),6,collisionAbort,0),
        BIND(resourceCollisionConfigThree,0xcb46c,0xcb48d,BYTES(0xf6,0x47,0x04,0x01,0x74,0x1b),6,collisionAbort,0),
        BIND(resourceCollisionMeshOne,0xcb49b,0xcb4bb,BYTES(0xf6,0x41,0x04,0x01,0x74,0x1a),6,collisionAbort,0),
        BIND(resourceCollisionConfigFour,0xcb4c8,0xcb4e9,BYTES(0xf6,0x47,0x04,0x01,0x74,0x1b),6,collisionAbort,0),
        BIND(resourceCollisionMeshTwo,0xcb4f4,0xcb515,BYTES(0xf6,0x47,0x04,0x01,0x74,0x1b),6,collisionAbort,0),
        BIND(resourceCollisionVertexResource,0xcba64,0xcba84,BYTES(0xf6,0x41,0x04,0x01,0x74,0x1a),6,collisionAbort,0),
        BIND(resourceCollisionMaterial,0xcbef3,0xcbf14,BYTES(0xf6,0x47,0x04,0x01,0x74,0x1b),6,collisionAbort,0),
        BIND(resourceMaterialReturn,0xe1cc9,0,BYTES(0x89,0x46,0x1c,0x8b,0x45,0xc8),6,modelAbort,0),
        // Native no-child branches enter AFTER the call's argument cleanup.
        // Hook the common join so they cannot land inside MinHook's rel32 jump.
        // Cancellation rebuilds ESP from EBP; the already-cleaned args add no debt.
        BIND(resourceChildOneReturn,0xe1f9e,0,BYTES(0x8b,0x4d,0x10,0x8b,0x45,0xb4),6,modelAbort,0),
        BIND(resourceChildTwoReturn,0xe203e,0,BYTES(0x8b,0x45,0x18,0x8b,0x4d,0x08),6,modelAbort,0),
        BIND(resourcePrepareReturn,0xe2472,0,BYTES(0xa1,0x68,0xac,0x2e,0x10),5,reinterpret_cast<void*>(&resourceCancelOuterPreparation),0),
        BIND(resourceQueryReturn,0xda4e1,0,BYTES(0xe8,0x4a,0x39,0x00,0x00),5,reinterpret_cast<void*>(&resourceCancelQueryTail),0),
    };
    // Entire module fingerprint is the caller's obligation; exact prefixes
    // protect this instruction-level binding before any entry is queued.
    for (const auto &binding:bindings) {
        if (binding.rva==0xe2472) {
            uint32_t address=0;
            std::memcpy(&address,base+binding.rva+1,sizeof(address));
            if (base[binding.rva]!=0xa1 || address!=reinterpret_cast<uintptr_t>(base+0x2eac68)) return false;
        } else if (std::memcmp(base+binding.rva,binding.prefix.data(),binding.prefixSize)) return false;
    }
    // Only exact pinned native class tables are admitted. The native call still
    // owns geometry and filtering; unknown/dummy/derived tables cancel rather
    // than being silently skipped as if the optional path were unobstructed.
    hullEngineBase=reinterpret_cast<uintptr_t>(base);
    primitiveHullTable=reinterpret_cast<uintptr_t>(base+0x209268);
    modelHullTable=reinterpret_cast<uintptr_t>(base+0x2094b8);
    fluidHullTable=reinterpret_cast<uintptr_t>(base+0x209308);
    forceHullTable=reinterpret_cast<uintptr_t>(base+0x2093b8);
    resourceModelEarlyExit=base+0xe18a1;
    resourceCollisionEarlyExit=base+0xcbf5f;
    resourcePrepareCleanup=base+0xe263e;
    resourceScratchReset=base+0xdad90;
    resourceQueryCleanup=base+0xda4f9;
    static_assert(bindings.size()==installedGates.size());
    unsigned installed=0;
    for (const auto &binding:bindings) {
        *binding.cancel=binding.cancelEntry?binding.cancelEntry:base+binding.cancelRva;
        *binding.clear=binding.clearRva?base+binding.clearRva:nullptr;
        if (!registrar(engine,binding.rva,binding.entry,binding.original)) return false;
        installedGates[installed++]={reinterpret_cast<uintptr_t>(base+binding.rva),reinterpret_cast<uintptr_t>(binding.entry)};
    }
    gatesQueued=true;
    return true;
}
void resetRoomscaleResourceGatesAfterRemoval() noexcept {
    // The caller removes every queued detour, including partial registration,
    // and quiesces callers before this reset. No callback may remain active.
    gatesQueued=false;installedGates={};
    primitiveHullTable=modelHullTable=fluidHullTable=forceHullTable=hullEngineBase=0;
}
bool roomscaleResourceGatesUsable() noexcept { return resourceGatesEnabled(); }
bool roomscaleResourceScopeUsable() noexcept { return unavailable && !*unavailable; }
bool finishRoomscaleResourceScopeForCommit(bool &failed) noexcept {
    if (unavailable!=&failed || failed) return false;
    unavailable=nullptr;
    return true;
}
bool runRoomscaleResourceScope(bool &failed, DWORD thread, RoomscaleResourceBody body, void *context) noexcept {
    if (unavailable) { *unavailable=true; failed=true; return false; }
    if (!resourceGatesEnabled() || !thread || GetCurrentThreadId()!=thread || !body || failed) {
        failed=true; return false;
    }
    const bool completed=withNativeFinally([&] {
        unavailable=&failed;
        body(context);
    },[&](bool aborted) noexcept {
        if (aborted) failed=true;
        unavailable=nullptr;
    });
    return completed&&!failed;
}
} // namespace ss2vr::game
