#include "roomscale_query.hpp"
#include "native_finally.hpp"
#include "roomscale_resource_gate.hpp"
#include "native_memory.hpp"
#include <cstring>

namespace ss2vr::game {
static RoomscaleTriangleKernel originalTriangle=nullptr;
static RoomscalePrimitiveKernel originalPrimitive=nullptr;
static thread_local roomscale::QueryScope* activeScope=nullptr;
static uintptr_t triangleEntry=0,primitiveEntry=0;
static RoomscaleScopeFailure scopeFailure;
void resetRoomscaleScopeFailure() noexcept {scopeFailure={};}
RoomscaleScopeFailure roomscaleScopeFailure() noexcept {return scopeFailure;}
static void noteScopeFailure(const roomscale::QueryScope& scope,uint32_t kind,const roomscale::Ray& ray,
    float radius) noexcept {
    scopeFailure.kind=kind;scopeFailure.ray=ray;scopeFailure.radius=radius;
    scopeFailure.maxRayParameter=scope.maxRayParameter;scopeFailure.contactDepthBudget=scope.contactDepthBudget;
    scopeFailure.requireWholePathClear=scope.requireWholePathClear;
    scopeFailure.rejectInitialContact=scope.rejectInitialContact;
}
static bool installedKernel(uintptr_t entry,uintptr_t target) noexcept {
    if(!entry||!target||!readableMemory(reinterpret_cast<void*>(entry),5))return false;
    const auto* bytes=reinterpret_cast<const unsigned char*>(entry);
    int32_t relative=0;std::memcpy(&relative,bytes+1,4);
    return bytes[0]==0xe9 && uint32_t(entry+5+uint32_t(relative))==uint32_t(target);
}
bool roomscaleCollisionKernelsUsable() noexcept {
    return originalTriangle&&originalPrimitive&&
        installedKernel(triangleEntry,reinterpret_cast<uintptr_t>(&ss2vrRoomscaleTriangleQuery))&&
        installedKernel(primitiveEntry,reinterpret_cast<uintptr_t>(&ss2vrRoomscalePrimitiveQuery));
}

bool queueRoomscaleTriangleHook(HMODULE core,RoomscaleQueueHook registrar) {
    if (!core||!registrar||originalTriangle) return false;
    triangleEntry=reinterpret_cast<uintptr_t>(GetProcAddress(core,RoomscaleTriangleExport));
    return registrar(core,RoomscaleTriangleExport,reinterpret_cast<void*>(&ss2vrRoomscaleTriangleQuery),
                     reinterpret_cast<void**>(&originalTriangle)) && originalTriangle;
}
bool queueRoomscalePrimitiveHook(HMODULE core,RoomscaleQueueHook registrar) {
    if (!core||!registrar||originalPrimitive) return false;
    primitiveEntry=reinterpret_cast<uintptr_t>(GetProcAddress(core,RoomscalePrimitiveExport));
    return registrar(core,RoomscalePrimitiveExport,reinterpret_cast<void*>(&ss2vrRoomscalePrimitiveQuery),
                     reinterpret_cast<void**>(&originalPrimitive)) && originalPrimitive;
}
void resetRoomscaleTriangleHookAfterRemoval() noexcept { originalTriangle=nullptr; originalPrimitive=nullptr; triangleEntry=primitiveEntry=0; }

bool runRoomscaleModelQueryScope(roomscale::QueryScope& scope,DWORD recognizedSimulationThread,
                                RoomscaleQueryBody body,void* context) noexcept {
    if (activeScope) { activeScope->failed=true; scope.failed=true; return false; }
    if (!originalTriangle||!body||!roomscale::validScope(scope)||!recognizedSimulationThread||
        GetCurrentThreadId()!=recognizedSimulationThread||!roomscale::arithmeticSupported()) {
        scope.failed=true;
        return false;
    }
    // The scope and cleanup captures are above the native-finally frame. TLS is
    // changed only inside it. Native faults propagate; finally rejects the
    // scope/restores TLS on unwind. No borrowed native pointer or native cleanup.
    auto* previous=activeScope;
    const bool completed=withNativeFinally([&] {
        activeScope=&scope;
        body(context);
    },[&](bool aborted) noexcept {
        if (aborted) scope.failed=true;
        activeScope=previous;
    });
    return completed&&!scope.failed;
}

bool runRoomscaleCollisionScope(roomscale::QueryScope& scope,DWORD recognizedSimulationThread,
                               RoomscaleQueryBody body,void* context) noexcept {
    if (!roomscaleCollisionKernelsUsable()||!body||!scope.requireWholePathClear||!roomscale::validScope(scope)) {
        scope.failed=true;
        return false;
    }
    // This non-owning context lives above BOTH foreign-finally boundaries.
    // There is one sticky failure location, never a copied success snapshot.
    struct Context {
        roomscale::QueryScope &scope;
        DWORD thread;
        RoomscaleQueryBody body;
        void *payload;
        bool mathematicalScopeCompleted=false;
        static void __cdecl run(void *opaque) noexcept {
            auto &self=*static_cast<Context*>(opaque);
            self.mathematicalScopeCompleted=runRoomscaleModelQueryScope(
                self.scope,self.thread,self.body,self.payload);
        }
    } state{scope,recognizedSimulationThread,body,context};
    const bool resourcesCompleted=runRoomscaleResourceScope(scope.failed,recognizedSimulationThread,
                                                            Context::run,&state);
    return resourcesCompleted&&state.mathematicalScopeCompleted&&!scope.failed;
}

extern "C" __attribute__((force_align_arg_pointer,noinline))
float __cdecl ss2vrRoomscaleTriangleQuery(const roomscale::Ray& ray,const roomscale::Vector& a,
    const roomscale::Vector& b,const roomscale::Vector& c,const roomscale::Vector& normal,float radius) noexcept {
    // Initialization supplies the trampoline before queued enable. Removal must
    // quiesce all callers. No concurrent rebind or lazy native symbol lookup.
    if (!activeScope) return originalTriangle(ray,a,b,c,normal,radius);
    try {
        const bool before=activeScope->failed;++scopeFailure.calls;
        const float t=roomscale::dispatch(activeScope,originalTriangle,ray,a,b,c,normal,radius);
        if(!before&&activeScope->failed&&!scopeFailure.kind) {
            noteScopeFailure(*activeScope,1,ray,radius);
            scopeFailure.a=a;scopeFailure.b=b;scopeFailure.c=c;scopeFailure.normal=normal;scopeFailure.result=t;
        }
        return t;
    } catch (...) {
        activeScope->failed=true;
        return 0; // Contain GNU errors; native SEH remains the outer finally's job.
    }
}
extern "C" __attribute__((force_align_arg_pointer,noinline))
roomscale::PrimitiveInterval* __cdecl ss2vrRoomscalePrimitiveQuery(roomscale::PrimitiveInterval* out,
    const roomscale::Ray& ray,const roomscale::Primitive& shape,float radius) noexcept {
    if (!activeScope) return originalPrimitive(out,ray,shape,radius);
    try {
        const bool before=activeScope->failed;++scopeFailure.calls;
        auto* result=roomscale::dispatchPrimitive(activeScope,originalPrimitive,out,ray,shape,radius);
        if(!before&&activeScope->failed&&!scopeFailure.kind) {
            noteScopeFailure(*activeScope,2,ray,radius);scopeFailure.shape=shape;
            if(out){scopeFailure.entry=out->entry;scopeFailure.exit=out->exit;}
        }
        return result;
    } catch (...) {
        activeScope->failed=true;
        if (out) *out={roomscale::NativeMiss,-roomscale::NativeMiss};
        return out;
    }
}
} // namespace ss2vr::game
