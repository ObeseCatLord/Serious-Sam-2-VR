#include "single_pass.hpp"
#include "scope_gpu.hpp"
#include "scope_observer.hpp"
#include "idle_observer.hpp"
#include "game.hpp"
#include "native_finally.hpp"
#include "remote_render.hpp"
#include "common/scope_buffer_layout.hpp"
#include "common/scope_lock.hpp"
#include "common/scope_program.hpp"
#include "common/scope_color_program.hpp"
#include "common/scope_draw_order.hpp"
#include "common/scope_shader_constants.hpp"
#include "common/scope_position_program.hpp"
#include "common/ride_position.hpp"
#include <bcrypt.h>
#include <atomic>
#include <algorithm>
#include <optional>

namespace ss2vr::game {
static_assert(D3DDECLTYPE_FLOAT2 == 1 && D3DDECLTYPE_FLOAT3 == 2 && D3DDECLTYPE_UBYTE4N == 8 && D3DDECLTYPE_UNUSED == 17);
static_assert(D3DDECLUSAGE_TEXCOORD == 5 && D3DFMT_VERTEXDATA == 100 && D3DFMT_INDEX16 == 101);
static_assert(D3DPOOL_MANAGED == 1 && D3DPT_TRIANGLELIST == 4 && MAXD3DDECLLENGTH == 64);
// Fatal native-buffer ownership uncertainty lasts for this process. Managed
// resources survive Reset; neither generation changes nor deviceLost retire it.
static std::atomic<bool> fatalDraw{false};
static constexpr std::array colorStates{
    D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ZFUNC,D3DRS_ALPHABLENDENABLE,
    D3DRS_ALPHATESTENABLE,D3DRS_STENCILENABLE,D3DRS_FOGENABLE,D3DRS_COLORWRITEENABLE,
    D3DRS_FILLMODE,D3DRS_CULLMODE,D3DRS_SCISSORTESTENABLE,D3DRS_CLIPPLANEENABLE,
    D3DRS_DEPTHBIAS,D3DRS_SLOPESCALEDEPTHBIAS,D3DRS_CLIPPING,D3DRS_SRGBWRITEENABLE};
struct BoundColor {
    IDirect3DPixelShader9 *shader = nullptr;
    IDirect3DSurface9 *target = nullptr, *depth = nullptr, *extra = nullptr;
    IUnknown *identity[5]{}; // Actual PS/RT/depth; retained expected eye RT/depth.
    D3DVIEWPORT9 viewport{};
    RECT scissor{};
    std::array<DWORD,colorStates.size()> states{};
    std::array<std::array<float,4>,6> clipPlanes{};
    UINT width=0,height=0;               // The eye's (logical) size.
    UINT surfaceWidth=0,surfaceHeight=0; // The bound surfaces' (single pass: the pass target).
    bool stereo=false;                   // A single-pass draw for both eyes.
    bool valid=false;
};
struct BoundInputs {
    IDirect3DVertexDeclaration9 *declaration = nullptr;
    IDirect3DVertexBuffer9 *vertex[4]{};
    IDirect3DIndexBuffer9 *index = nullptr;
    IDirect3DVertexShader9 *shader = nullptr;
    std::array<std::array<float,4>,2> uvRows{};
    IUnknown *identity[7]{}; // Position/localindices/UV/weights/index/declaration/shader.
    ScopeBufferInputs values;
    std::array<ScopeDeclarationElement,MAXD3DDECLLENGTH+1> elements{};
    UINT count = 0;
    BoundColor color;
    std::array<std::array<float,4>,256> constants{};
    UINT constantCount = 0;
};
struct ProbeOwner {
    bool busy = false;
    uint64_t generation = 0;
    ScopeRasterObservation raster;
    IdleGeometryCopy idle;
    IdleWeaponTrace *idleTrace=nullptr;
    int idleNativeId=-1; // Copied before AddRef; never selected again after COM callbacks.
    IdleWeaponTrace *submissionOwner=nullptr;
    unsigned submissionSlot=IdleSubmissionTrace::NoSlot;
    unsigned paletteApiSlot=IdleWeaponTrace::MaxPaletteApiPayloads;
    BoundInputs bindings[3]; // Original/pre/post observations; one existing cleanup owner.
    bool streamReentered=false;
    RideMainDrawCopy rideBefore{},rideAfter{};
    RideDrawGpuCopy rideGpu;bool rideGpuAttempt=false,rideGpuMatched=false;
    RideCameraCopy rideCamera{};bool rideCameraCaptured=false;
    bool rideReentered=false,rideOriginalReturned=false,rideMatched=false,rideReady=false;
    ScopeLockLedger lock;
    IDirect3DVertexBuffer9 *lockedVertex = nullptr;
    IDirect3DIndexBuffer9 *lockedIndex = nullptr;
    void *mapped = nullptr;
    std::array<uint8_t,IdleGeometryStorageVertices*12> positions{};
    std::array<uint8_t,std::max(IdleGeometryStorageTriangles,RideGeometryTriangles)*6> indices{};
    std::array<uint8_t,IdleGeometryStorageVertices*4> weights{}, localIndices{};
    std::array<uint8_t,IdleGeometryStorageVertices*8> uv{};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::array<uint8_t,1024> hashObject{};
    ScopeCapGeometry cap;
    std::array<uint32_t,ScopeProgramMaxWords> program{};
    std::array<uint32_t,ScopeProgramMaxWords> colorProgram{};
    std::array<std::array<float,4>,2> uvRows{};
    IDirect3DDevice9 *device = nullptr; // Invocation reference survives early frame retirement.
    IDirect3DTexture9 *image = nullptr;
    IDirect3DBaseTexture9 *originalTexture = nullptr, *restoredTexture = nullptr;
    IDirect3DPixelShader9 *restoredShader = nullptr;
    IDirect3DPixelShader9 *imageShader = nullptr;
    ScopeSourceView sourceView;
    ScopeShaderConstants imageConstants;
    ScopeShaderConstants eyeConstants[2]; // Single pass: each eye's lens constants.
    bool stereoImage = false;
    size_t programWords = 0;
    std::array<std::array<float,4>,7> pixelConstants{};
    std::array<DWORD,10> sampler{};
    bool transaction = false, split = false, imageChanged = false, imageRestoreFailed = false;
};
static thread_local ProbeOwner probe;
static bool restoreImageState(IDirect3DDevice9 *) noexcept;
template<class T> static void release(T *&p) noexcept {
    T *owned = p; p = nullptr;
    if (owned) owned->Release();
}
static void releaseBindings(BoundInputs &b) noexcept {
    for (auto &id:b.color.identity) release(id);
    release(b.color.shader); release(b.color.target); release(b.color.depth); release(b.color.extra);
    b.color={};
    for (auto &identity : b.identity) release(identity);
    release(b.declaration);
    for (auto &v : b.vertex) release(v);
    release(b.index);
    release(b.shader);
    b.uvRows={};
    b.values={}; b.count=0; b.elements={}; // No stale object comparison keys survive this owner.
}
static void fatal() noexcept {
    fatalDraw.store(true,std::memory_order_release);
    scopeGpuFault();
    retireScopeGeometry(true);
}
bool scopeGpuForwardingAllowed() noexcept {
    if (probe.lock.outstanding()) fatal(); // Includes reentry while Lock/Unlock is indeterminate.
    return !fatalDraw.load(std::memory_order_acquire);
}
static bool unlock() noexcept {
    if (!probe.lock.beginUnlock()) return false;
    // The attempted flag is installed BEFORE the foreign call; no blind retry
    // after failure or native unwinding. Success+null still reaches this path.
    const HRESULT result = probe.lockedVertex ? probe.lockedVertex->Unlock() : probe.lockedIndex->Unlock();
    probe.lock.finishUnlock(SUCCEEDED(result));
    probe.mapped = nullptr;
    if (FAILED(result)) { fatal(); return false; }
    probe.lockedVertex = nullptr; probe.lockedIndex = nullptr;
    return true;
}
static void cleanup(bool aborted) noexcept {
    single_pass::setEyePixelConstants(0,0,nullptr,nullptr); // Never left set past this draw (or a foreign unwind).
    single_pass::setInstancingSuspended(false);
    const bool retired = probe.generation != graphicsResourceGeneration();
    if (aborted && probe.idleTrace) probe.idleTrace->reject(IdleWeaponTrace::Rejection::GpuAbort);
    if (aborted) { retireScopeGeometry(); if (!retired) scopeGpuFault(); }
    if (probe.imageChanged) restoreImageState(probe.device);
    if (probe.lock.phase == ScopeLockPhase::Acquired) unlock();
    if (probe.lock.outstanding()) {
        fatal();
        // Transfer exactly one existing reference into the uncertain owner.
        // No process/reset path releases or retries this acquisition.
        if (probe.lockedVertex == probe.bindings[0].vertex[0]) probe.bindings[0].vertex[0] = nullptr;
        if (probe.lockedIndex == probe.bindings[0].index) probe.bindings[0].index = nullptr;
    }
    for (auto &b : probe.bindings) releaseBindings(b);
    if (probe.hash) { const auto h=probe.hash; probe.hash=nullptr; BCryptDestroyHash(h); }
    if (probe.algorithm) { const auto a=probe.algorithm; probe.algorithm=nullptr; BCryptCloseAlgorithmProvider(a,0); }
    release(probe.image); release(probe.originalTexture); release(probe.imageShader);
    release(probe.restoredTexture); release(probe.restoredShader);
    const bool current = !probe.transaction || scopeGpuTransactionCurrent(probe.device);
    auto *device = probe.device;
    release(probe.device);
    bool afterReleaseCurrent=true;
    if (probe.transaction) {
        // Current performs identity comparisons only, never dereferences device.
        const bool afterRelease=scopeGpuTransactionCurrent(device);
        afterReleaseCurrent=afterRelease;
        if(probe.idleTrace && probe.idleTrace->streamProbe.selected) {
            auto &receipt=probe.idleTrace->streamProbe;
            receipt.invalidations=IdleWeaponTrace::checks({probe.streamReentered,aborted,
                probe.generation!=graphicsResourceGeneration(),!current,!afterRelease});
            if(!aborted && !retired && !probe.streamReentered && current && afterRelease &&
               probe.generation==graphicsResourceGeneration())receipt.flags|=IdleWeaponTrace::StreamProbe::CleanupCurrent;
            else receipt.flags&=~IdleWeaponTrace::StreamProbe::CleanupCurrent;
        }
        if(probe.idleTrace && (retired || !current || !afterRelease))probe.idleTrace->reject(IdleWeaponTrace::Rejection::GpuRetired,IdleWeaponTrace::checks({!retired,current,afterRelease}));
        if (!retired && probe.split && (!current || !afterRelease)) scopeGpuFault();
        scopeGpuTransactionEnd();
    }
    // Release callbacks can invalidate a previously matching post sample.
    // Numeric device comparison and generation checks perform no COM calls.
    if(probe.submissionOwner)probe.submissionOwner->submissions.finalize(probe.submissionSlot,aborted,
        current && afterReleaseCurrent && probe.generation==graphicsResourceGeneration() && scopeGpuRoutingCurrent(device),
        probe.split);
    probe.submissionOwner=nullptr;probe.submissionSlot=IdleSubmissionTrace::NoSlot;
    probe.paletteApiSlot=IdleWeaponTrace::MaxPaletteApiPayloads;
    probe.sourceView = {}; probe.imageConstants = {};
    probe.eyeConstants[0] = probe.eyeConstants[1] = {}; probe.stereoImage = false;
    // No transaction is required for this value-only mapping observation. Its
    // own tail checks run AFTER every Release and retain the sticky reentry bit.
    // `device` is only compared numerically after its invocation reference retires.
    const bool rideOwnerCurrent=probe.rideMatched && remote_render::rideMainDrawCurrent(probe.rideAfter);
    RideCameraCopy cameraAfterRelease;
    const bool rideCameraCurrent=!probe.rideCameraCaptured ||
        (copyExecutedRideCamera(probe.rideAfter.identity.player,probe.generation,probe.rideAfter.eye,cameraAfterRelease) &&
         cameraAfterRelease==probe.rideCamera);
    // Native owner admission precedes the FINAL reentry/generation checks, not
    // the reverse; even an unexpected query callback cannot lend its old result.
    probe.rideReady=rideOwnerCurrent && !aborted && !retired && !probe.rideReentered && !probe.split &&
        probe.rideOriginalReturned && probe.rideMatched &&
        probe.generation==graphicsResourceGeneration() && scopeGpuForwardingAllowed() &&
        scopeGpuMappingObservationCurrent(device);
    probe.rideGpu.copied=probe.rideReady && probe.rideGpuMatched && rideCameraCurrent;
    probe.transaction = probe.split = probe.imageChanged = probe.imageRestoreFailed = false;
    probe.cap = {}; probe.uvRows={}; probe.program={}; probe.colorProgram={};
    probe.idleTrace=nullptr;probe.idleNativeId=-1;
    probe.streamReentered=false;
    probe.busy = false;
}
void retireIdleSubmissionOwner(IdleWeaponTrace *owner) noexcept {
    // The outer weapon's native-finally owns this last-resort retirement, even
    // if a foreign unwind interrupted the inner COM cleanup. No callbacks.
    if(owner && probe.submissionOwner==owner) {
        owner->submissions.finalize(probe.submissionSlot,true,false,probe.split);
        probe.submissionOwner=nullptr;probe.submissionSlot=IdleSubmissionTrace::NoSlot;
        probe.paletteApiSlot=IdleWeaponTrace::MaxPaletteApiPayloads;
    }
}
static bool identity(IUnknown *object,IUnknown *&out,HRESULT *observed=nullptr) {
    if(!object){if(observed)*observed=E_POINTER;return false;}
    const HRESULT result=object->QueryInterface(IID_IUnknown,reinterpret_cast<void **>(&out));
    if(observed)*observed=result;
    return SUCCEEDED(result) && out;
}
enum class GeometryBufferPolicy { Scope,Idle,Ride };
static bool boundInputs(IDirect3DDevice9 *d,BoundInputs &b,const ScopeIndexedDraw &draw,GeometryBufferPolicy policy=GeometryBufferPolicy::Scope,
                        IdleWeaponTrace::InputFailure *diagnostic=nullptr,int idleNativeId=1,
                        const ScopeSurfaceLayout *copiedSurface=nullptr) {
    const bool idle=policy==GeometryBufferPolicy::Idle,ride=policy==GeometryBufferPolicy::Ride;
    b.values = {}; b.count = MAXD3DDECLLENGTH+1;
    HRESULT result=S_OK;
    auto fail=[&](uint32_t step,uint32_t index=0) {
        if(diagnostic){diagnostic->step=step;diagnostic->index=index;diagnostic->hresult=int32_t(result);}
        return false;
    };
    D3DCAPS9 caps{};
    result=d->GetDeviceCaps(&caps);if(FAILED(result))return fail(1);
    if(diagnostic){diagnostic->valid|=1;diagnostic->caps=caps.MaxVertexShaderConst;}
    if(!caps.MaxVertexShaderConst || caps.MaxVertexShaderConst>256)return fail(2);
    b.constantCount=caps.MaxVertexShaderConst;
    result=d->GetVertexShaderConstantF(0,b.constants[0].data(),b.constantCount);if(FAILED(result))return fail(3);
    D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH+1]{};
    result=d->GetVertexDeclaration(&b.declaration);if(FAILED(result))return fail(4);
    if(!b.declaration)return fail(5);
    result=b.declaration->GetDeclaration(elements,&b.count);if(FAILED(result))return fail(6);
    if(diagnostic){diagnostic->valid|=8;diagnostic->declarationCount=b.count;}
    if(!b.count || b.count>std::size(elements))return fail(7);
    bool weights=false;
    for(UINT i=0;i<b.count;++i) {
        const auto e=elements[i];b.elements[i]={e.Stream,e.Offset,e.Type,e.Method,e.Usage,e.UsageIndex};
        if(e.Stream==6 && e.Type!=D3DDECLTYPE_UNUSED)weights=true;
    }
    if(diagnostic){diagnostic->valid|=2;diagnostic->declaration=b.elements;}
    const bool observed78=(idle || ride) && observed78Declaration(std::span(b.elements).first(b.count));
    const bool noUV56=(idle || ride) && noUV56Declaration(std::span(b.elements).first(b.count));
    const bool multiUV78=(idle || ride) && observedMultiUV78Declaration(std::span(b.elements).first(b.count));
    const bool noUV78=(idle || ride) && noUV78Declaration(std::span(b.elements).first(b.count));
    const bool uses78=observed78 || multiUV78 || noUV78;
    if(diagnostic)diagnostic->layout=observed78?GeometryInputLayout::Observed78:
        noUV56?GeometryInputLayout::NoUV56:multiUV78?GeometryInputLayout::ObservedMultiUV78:
        noUV78?GeometryInputLayout::NoUV78:GeometryInputLayout::Legacy56;
    if(uses78 || noUV56)weights=true;
    b.values.surface=copiedSurface?*copiedSurface:(idle?probe.idle.raster.layout:probe.raster.pose.layout);b.values.draw=draw;
    ScopeStreamInput *streams[]{&b.values.positions,&b.values.localIndices,&b.values.uv,&b.values.weights};
    const UINT streamNumbers[]{0,uses78?7u:5u,noUV78?2u:3u,uses78?8u:6u};
    // NoUV78's declared FLOAT2 stream2 replaces stale undeclared stream3.
    // Auxiliary bytes still require all five hashes; v2 is not a known position input.
    // Other families retain auxiliary stream3 for exact asset matching.
    // It is never seeded into the position interpreter as a consumed input.
    for(unsigned i=0;i<(weights?4u:3u);++i) {
        auto &stream=*streams[i];
        result=d->GetStreamSource(streamNumbers[i],&b.vertex[i],&stream.offset,&stream.stride);if(FAILED(result))return fail(8,streamNumbers[i]);
        if(!b.vertex[i])return fail(9,streamNumbers[i]);
        result=d->GetStreamSourceFreq(streamNumbers[i],&stream.frequency);if(FAILED(result))return fail(10,streamNumbers[i]);
        if(!identity(b.vertex[i],b.identity[i],&result))return fail(11,streamNumbers[i]);
        stream.object=reinterpret_cast<uintptr_t>(b.identity[i]);
    }
    D3DVERTEXBUFFER_DESC vertex{};D3DINDEXBUFFER_DESC index{};
    result=b.vertex[0]->GetDesc(&vertex);if(FAILED(result))return fail(12);
    if(vertex.Type!=D3DRTYPE_VERTEXBUFFER)return fail(13);
    result=d->GetIndices(&b.index);if(FAILED(result))return fail(14);
    if(!b.index)return fail(15);
    result=b.index->GetDesc(&index);if(FAILED(result))return fail(16);
    if(index.Type!=D3DRTYPE_INDEXBUFFER)return fail(17);
    if(!identity(b.index,b.identity[4],&result))return fail(18);
    if(!identity(b.declaration,b.identity[5],&result))return fail(19);
    b.values.vertex={vertex.Size,vertex.Usage,uint32_t(vertex.Pool),uint32_t(vertex.Format),vertex.FVF};
    b.values.index={index.Size,index.Usage,uint32_t(index.Pool),uint32_t(index.Format),0};
    b.values.indexObject=reinterpret_cast<uintptr_t>(b.identity[4]);
    if(diagnostic){diagnostic->valid|=4;diagnostic->inputs=b.values;}
    ScopeCopyRanges ranges;
    if(!(ride?rideBufferRanges(b.values,std::span(b.elements).first(b.count),ranges):
         idle?idleBufferRanges(b.values,std::span(b.elements).first(b.count),ranges,diagnostic?&diagnostic->rangeChecks:nullptr,idleNativeId):
              scopeBufferRanges(b.values,std::span(b.elements).first(b.count),ranges)))return fail(20);
    result=d->GetVertexShader(&b.shader);if(FAILED(result))return fail(21);
    if(!b.shader)return fail(22);
    if(!identity(b.shader,b.identity[6],&result))return fail(23);
    if(ride)return true; // Raw float constants were copied already; no scope UV admission.
    result=d->GetVertexShaderConstantF(8,b.uvRows[0].data(),2);if(FAILED(result))return fail(24);
    for(const auto &row:b.uvRows)for(float value:row)if(!std::isfinite(value))return fail(25);
    return true;
}
// Passive actual stream observations for the exact selected family. These slots are deliberately not labelled
// weights/local indices and are never passed to geometry admission or replay.
// Failed/abnormal outputs remain owned by the existing ProbeOwner cleanup;
// only an entirely normal snapshot exposes copied scalar outputs.
static bool sampleIdleApi(IDirect3DDevice9 *d,BoundInputs &b,IdleWeaponTrace::StreamSnapshot &out,
                           std::span<uint32_t> program,uint32_t &words,
                           std::array<unsigned,3> numbers,bool actualDeclaration,bool copyProgram) {
    out={};out.status=IdleWeaponTrace::StreamSnapshot::Interrupted;
    b.values={};b.elements={};b.constants={};b.count=MAXD3DDECLLENGTH+1;
    HRESULT result=S_OK;
    auto at=[&](uint32_t step,uint32_t index=0) {out.step=step;out.index=index;};
    auto fail=[&] {
        const auto step=out.step,index=out.index;out={};
        out.status=IdleWeaponTrace::StreamSnapshot::Failed;out.step=step;out.index=index;out.hresult=int32_t(result);
        return false;
    };
    D3DCAPS9 caps{};
    at(1);result=d->GetDeviceCaps(&caps);if(FAILED(result))return fail();
    at(2);if(!caps.MaxVertexShaderConst || caps.MaxVertexShaderConst>256)return fail();
    b.constantCount=caps.MaxVertexShaderConst;
    at(3);result=d->GetVertexShaderConstantF(0,b.constants[0].data(),b.constantCount);if(FAILED(result))return fail();
    at(4);result=d->GetVertexDeclaration(&b.declaration);if(FAILED(result))return fail();
    at(5);if(!b.declaration)return fail();
    D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH+1]{};
    at(6);result=b.declaration->GetDeclaration(elements,&b.count);if(FAILED(result))return fail();
    at(7);if(!b.count || b.count>std::size(elements))return fail();
    at(8);if(!identity(b.declaration,b.identity[5],&result))return fail();
    for(UINT i=0;i<b.count;++i) {
        const auto e=elements[i];b.elements[i]={e.Stream,e.Offset,e.Type,e.Method,e.Usage,e.UsageIndex};
    }
    if(actualDeclaration) {
        GeometryInputLayout layout{};bool weights=false;
        at(22);
        if(!idleGeometryInputs(std::span(b.elements).first(b.count),layout,weights) || !weights)return fail();
        const bool uses78=layout==GeometryInputLayout::Observed78 || layout==GeometryInputLayout::ObservedMultiUV78 ||
                          layout==GeometryInputLayout::NoUV78;
        numbers=uses78?std::array<unsigned,3>{0,7,8}:std::array<unsigned,3>{0,5,6};
    }
    std::array<ScopeStreamInput,3> streams{};
    for(unsigned i=0;i<3;++i) {
        auto &v=streams[i];
        at(9,numbers[i]);result=d->GetStreamSource(numbers[i],&b.vertex[i],&v.offset,&v.stride);if(FAILED(result))return fail();
        at(10,numbers[i]);if(!b.vertex[i])return fail();
        at(11,numbers[i]);result=d->GetStreamSourceFreq(numbers[i],&v.frequency);if(FAILED(result))return fail();
        at(12,numbers[i]);if(!identity(b.vertex[i],b.identity[i],&result))return fail();
        v.object=reinterpret_cast<uintptr_t>(b.identity[i]);
    }
    at(13);result=d->GetIndices(&b.index);if(FAILED(result))return fail();
    at(14);if(!b.index)return fail();
    at(15);if(!identity(b.index,b.identity[4],&result))return fail();
    at(16);result=d->GetVertexShader(&b.shader);if(FAILED(result))return fail();
    at(17);if(!b.shader)return fail();
    at(18);if(!identity(b.shader,b.identity[6],&result))return fail();
    if(copyProgram) {
        UINT bytes=0;
        at(19);result=b.shader->GetFunction(nullptr,&bytes);if(FAILED(result))return fail();
        at(20);if(bytes<8 || bytes%4 || bytes>program.size_bytes())return fail();
        const UINT expected=bytes;
        at(21);result=b.shader->GetFunction(program.data(),&bytes);
        if(FAILED(result) || bytes!=expected)return fail();
        words=bytes/4;
    }
    out.status=IdleWeaponTrace::StreamSnapshot::Copied;out.step=out.index=0;out.hresult=0;
    out.caps=b.constantCount;out.declarationCount=b.count;out.declaration=b.elements;out.streams=streams;
    out.declarationObject=uint32_t(reinterpret_cast<uintptr_t>(b.identity[5]));
    out.indexObject=uint32_t(reinterpret_cast<uintptr_t>(b.identity[4]));
    out.shaderObject=uint32_t(reinterpret_cast<uintptr_t>(b.identity[6]));
    for(unsigned i=0;i<b.constantCount;++i)for(unsigned j=0;j<4;++j)
        out.constants[i][j]=std::bit_cast<uint32_t>(b.constants[i][j]);
    return true;
}
static bool sampleIdleStreams(IDirect3DDevice9 *d,BoundInputs &b,unsigned phase) {
    auto &receipt=probe.idleTrace->streamProbe;
    receipt.attempts=phase+1;
    return sampleIdleApi(d,b,receipt.snapshots[phase],receipt.program,receipt.words,
                        IdleWeaponTrace::passiveStreamNumbers(probe.idleTrace->inputFailure),false,!phase);
}
static bool paletteApiOwnerCurrent(IDirect3DDevice9 *d,IdleWeaponTrace *owner,unsigned slot,unsigned api) {
    if(!owner || probe.submissionOwner!=owner || probe.submissionSlot!=slot ||
       api>=IdleWeaponTrace::MaxPaletteApiPayloads ||
       probe.generation!=graphicsResourceGeneration() || !scopeGpuRoutingCurrent(d))return false;
    if(owner->nativeId!=2 || api>=owner->paletteApiCount || !owner->submissions.pending(slot))return false;
    const auto &p=owner->paletteApiPayloads[api];
    return p.submissionSlot==slot && p.ordinal==owner->submissions.rows[slot].ordinal;
}
static bool armPaletteBoundary(IDirect3DDevice9 *d,IdleWeaponTrace *owner,unsigned slot,unsigned api,unsigned phase) {
    if(!paletteApiOwnerCurrent(d,owner,slot,api))return false;
    auto &receipt=owner->paletteApiPayloads[api].boundary;
    if(!receipt.kind) {receipt={};receipt.phase=phase;}
    return true;
}
static void recordPaletteBoundaryFailure(IDirect3DDevice9 *d,IdleWeaponTrace *owner,unsigned slot,unsigned api,
                                        unsigned phase,const IdlePaletteBoundaryCopy &copy={}) {
    if(!paletteApiOwnerCurrent(d,owner,slot,api) || copy.wrapper==99)return;
    auto &receipt=owner->paletteApiPayloads[api].boundary;
    if(receipt.kind)return; // Freeze the first returned, current failure.
    receipt=copy;receipt.phase=phase;receipt.kind=1;
}
static bool copyPaletteBoundary(IDirect3DDevice9 *d,IdleWeaponTrace *owner,unsigned slot,unsigned api,
                                unsigned phase,IdlePaletteCopy &out) {
    if(!armPaletteBoundary(d,owner,slot,api,phase))return false;
    IdlePaletteBoundaryCopy copy; // Caller-owned values above native finally frames.
    const bool copied=copyIdleSubmissionPalette(owner,out,&copy);
    if(!copied)recordPaletteBoundaryFailure(d,owner,slot,api,phase,copy);
    return copied; // Original caller retains its own completion/current check.
}
static void recordPaletteApiFailure(IDirect3DDevice9 *d,IdleWeaponTrace *owner,unsigned slot,unsigned api,
                                    unsigned phase,const IdleWeaponTrace::StreamSnapshot &snapshot) {
    IdlePaletteBoundaryCopy copy;
    copy.valid=16;copy.apiStatus=snapshot.status;copy.apiStep=snapshot.step;
    copy.apiIndex=snapshot.index;copy.apiResult=snapshot.hresult;
    recordPaletteBoundaryFailure(d,owner,slot,api,phase,copy);
}
static bool serializePaletteGeometry(IdleWeaponTrace::PaletteApiPayload &p) {
    const auto &g=probe.idle;const auto &b=probe.bindings[0];
    if(!g.words || g.words>p.program.size() || !g.constantCount || g.constantCount>256 ||
       !g.declarationCount || g.declarationCount>65 || !b.identity[4] || !b.identity[5] || !b.identity[6])return false;
    auto &s=p.before;s={};s.status=IdleWeaponTrace::StreamSnapshot::Copied;
    s.caps=g.constantCount;s.declarationCount=g.declarationCount;s.declaration=g.declaration;
    s.declarationObject=uint32_t(reinterpret_cast<uintptr_t>(b.identity[5]));
    s.indexObject=uint32_t(reinterpret_cast<uintptr_t>(b.identity[4]));
    s.shaderObject=uint32_t(reinterpret_cast<uintptr_t>(b.identity[6]));
    s.streams={g.inputs.positions,g.inputs.localIndices,g.inputs.weights};
    for(unsigned i=0;i<g.constantCount;++i)for(unsigned j=0;j<4;++j)
        s.constants[i][j]=std::bit_cast<uint32_t>(g.constants[i][j]);
    p.words=g.words;std::copy_n(g.program.begin(),g.words,p.program.begin());
    return true; // Copied scalar state only; retain both original binding owners.
}
static bool collectPaletteContent(IDirect3DDevice9 *,IdleWeaponTrace *,unsigned,unsigned,const IdlePaletteCopy &);
static bool paletteContentMatchesApi(const BoundInputs &b,const IdleWeaponTrace::StreamSnapshot &s) noexcept {
    if(s.status!=IdleWeaponTrace::StreamSnapshot::Copied || b.count!=s.declarationCount ||
       b.elements!=s.declaration || b.constantCount!=s.caps ||
       uint32_t(reinterpret_cast<uintptr_t>(b.identity[5]))!=s.declarationObject ||
       uint32_t(reinterpret_cast<uintptr_t>(b.identity[4]))!=s.indexObject ||
       uint32_t(reinterpret_cast<uintptr_t>(b.identity[6]))!=s.shaderObject ||
       std::array<ScopeStreamInput,3>{b.values.positions,b.values.localIndices,b.values.weights}!=s.streams)return false;
    for(unsigned i=0;i<s.caps;++i)for(unsigned j=0;j<4;++j)
        if(std::bit_cast<uint32_t>(b.constants[i][j])!=s.constants[i][j])return false;
    return true;
}
static bool beginPaletteApi(IDirect3DDevice9 *d,IdleWeaponTrace *owner,unsigned slot,unsigned api,
                            bool geometryAdmitted,bool contentEligible) {
    if(!paletteApiOwnerCurrent(d,owner,slot,api))return false;
    // Capability getters are outside the sampled interval. An admitted geometry
    // already performed them; do not add callbacks after its certification.
    if(!geometryAdmitted) {
        if(!armPaletteBoundary(d,owner,slot,api,1))return false;
        if(!nativeUiDeviceCurrent(d)) {recordPaletteBoundaryFailure(d,owner,slot,api,1);return false;}
        if(!paletteApiOwnerCurrent(d,owner,slot,api))return false;
    }
    IdlePaletteCopy before,bookend;
    if(!copyPaletteBoundary(d,owner,slot,api,2,before) || !paletteApiOwnerCurrent(d,owner,slot,api))return false;
    auto &p=owner->paletteApiPayloads[api];
    bool copied=false;
    if(!armPaletteBoundary(d,owner,slot,api,3))return false;
    if(geometryAdmitted) {
        copied=serializePaletteGeometry(p);
        p.content=probe.idle.inputs;p.contentHashes=probe.idle.hashes;
        p.contentCopied=copied;
    }
    else {
        // Reuse the existing slots only when no geometry was certified.
        releaseBindings(probe.bindings[1]);releaseBindings(probe.bindings[2]);
        if(!paletteApiOwnerCurrent(d,owner,slot,api))return false;
        if(contentEligible && before.count>1 && !probe.algorithm && !probe.hash)
            p.contentCopied=collectPaletteContent(d,owner,slot,api,before);
        // A failed unlock must stop before further getters or native bookends.
        if(!scopeGpuForwardingAllowed() || !paletteApiOwnerCurrent(d,owner,slot,api))return false;
        copied=sampleIdleApi(d,probe.bindings[1],p.before,p.program,p.words,{},true,true);
    }
    if(!paletteApiOwnerCurrent(d,owner,slot,api))return false;
    p.beforeCopied=copied;
    p.contentMatched=p.contentCopied && copied && paletteContentMatchesApi(probe.bindings[0],p.before);
    if(!copied) {recordPaletteApiFailure(d,owner,slot,api,3,p.before);return false;}
    if(!copyPaletteBoundary(d,owner,slot,api,4,bookend) || !paletteApiOwnerCurrent(d,owner,slot,api))return false;
    if(before!=bookend) {recordPaletteBoundaryFailure(d,owner,slot,api,8);return false;}
    owner->submissions.before(slot,before.metadata,true);
    before.projectionBookend(bookend);
    owner->submissions.paletteBefore(slot,before,true);
    return true;
}
static bool finishPaletteApi(IDirect3DDevice9 *d,IdleWeaponTrace *owner,unsigned slot,unsigned api,
                             IdlePaletteCopy &after) {
    if(!paletteApiOwnerCurrent(d,owner,slot,api))return false;
    IdlePaletteCopy bookend;
    if(!copyPaletteBoundary(d,owner,slot,api,5,after) || !paletteApiOwnerCurrent(d,owner,slot,api))return false;
    IdleWeaponTrace::StreamSnapshot snapshot;
    uint32_t unusedWords=0;
    if(!armPaletteBoundary(d,owner,slot,api,6))return false;
    const bool copied=sampleIdleApi(d,probe.bindings[2],snapshot,{},unusedWords,{},true,false);
    if(!paletteApiOwnerCurrent(d,owner,slot,api))return false;
    auto &p=owner->paletteApiPayloads[api];p.afterCopied=copied;
    if(!copied) {recordPaletteApiFailure(d,owner,slot,api,6,snapshot);return false;}
    if(!copyPaletteBoundary(d,owner,slot,api,7,bookend) || !paletteApiOwnerCurrent(d,owner,slot,api))return false;
    if(after!=bookend) {recordPaletteBoundaryFailure(d,owner,slot,api,9);return false;}
    p.matched=p.beforeCopied && IdleWeaponTrace::sameStreamSnapshots(p.before,snapshot);
    after.projectionBookend(bookend);
    owner->submissions.paletteAfter(slot,after,true);
    return true;
}
static bool idleStreamOwnerCurrent(IDirect3DDevice9 *d) {
    return !probe.streamReentered && nativeUiDeviceCurrent(d) && probe.generation==graphicsResourceGeneration() &&
        scopeGpuTransactionCurrent(d) && idleRejectedRasterCurrent(probe.idle.raster,probe.idleTrace);
}
static void beginIdleStreamProbe(IDirect3DDevice9 *d) {
    auto &receipt=probe.idleTrace->streamProbe;receipt.selected=true;
    if(!idleStreamOwnerCurrent(d) || !sampleIdleStreams(d,probe.bindings[1],0))return;
    receipt.flags|=IdleWeaponTrace::StreamProbe::BeforeCopied;
    const auto &a=probe.bindings[0];const auto &b=probe.bindings[1];
    const auto &snapshot=receipt.snapshots[0];
    const auto numbers=IdleWeaponTrace::passiveStreamNumbers(probe.idleTrace->inputFailure);
    // The new 5/6 branch has original getters for both selected streams;
    // compare their already captured scalar bindings without extra COM calls.
    const bool originalStreams=numbers[1]!=5 ||
        (a.values.localIndices==snapshot.streams[1] && a.values.weights==snapshot.streams[2]);
    if(originalStreams && a.count==b.count && a.elements==b.elements && a.identity[5]==b.identity[5] &&
       a.identity[4]==b.identity[4] && a.values.positions==snapshot.streams[0] &&
       a.constantCount==b.constantCount && !std::memcmp(a.constants.data(),b.constants.data(),a.constantCount*4*sizeof(float)) &&
       idleStreamOwnerCurrent(d))receipt.flags|=IdleWeaponTrace::StreamProbe::BeforeCurrent;
}
static void finishIdleStreamProbe(IDirect3DDevice9 *d,HRESULT result) {
    auto &receipt=probe.idleTrace->streamProbe;
    receipt.flags|=IdleWeaponTrace::StreamProbe::ForwardReturned;receipt.forwardResult=int32_t(result);
    if(FAILED(result))return;
    receipt.flags|=IdleWeaponTrace::StreamProbe::ForwardSucceeded;
    if(!(receipt.flags&IdleWeaponTrace::StreamProbe::BeforeCurrent) || !idleStreamOwnerCurrent(d) ||
       !sampleIdleStreams(d,probe.bindings[2],1))return;
    receipt.flags|=IdleWeaponTrace::StreamProbe::AfterCopied;
    if(IdleWeaponTrace::sameStreamSnapshots(receipt.snapshots[0],receipt.snapshots[1]))
        receipt.flags|=IdleWeaponTrace::StreamProbe::SameInputs;
    if(idleStreamOwnerCurrent(d))receipt.flags|=IdleWeaponTrace::StreamProbe::AfterCurrent;
}
// Shader objects expose no bytecode mutator; same canonical identity in the
// second snapshot pins the program admitted here. All calls precede VB locks.
static bool boundProgram(bool idle=false,bool ride=false) {
    auto *shader=probe.bindings[0].shader;
    UINT size=0;
    if (!shader || FAILED(shader->GetFunction(nullptr,&size)) || size<8 ||
        size%sizeof(uint32_t) || size>sizeof(probe.program) || (idle && size>sizeof(probe.idle.program))) return false;
    const UINT expected=size;
    if (FAILED(shader->GetFunction(probe.program.data(),&size)) || size!=expected) return false;
    probe.programWords = size/sizeof(uint32_t);
    return idle || ride || scopeUvProgram(std::span(probe.program).first(probe.programWords));
}
// Optional color admission is independent of the existing geometry/UV path.
// All COM output owners live in ProbeOwner before calls, including partial
// getter results and the extra MRT probe. No getters execute while VB is locked.
// Diagnostics: the last failing boundColor step (0 = none), plus 90 cap position.
static unsigned lastColorFailure = 0;
static double lastCapError = -1;
double scopeCapError() noexcept { return lastCapError; }
static uint32_t lastColorStates[16]{};
unsigned scopeColorFailure() noexcept { return lastColorFailure; }
const uint32_t *scopeColorStates() noexcept { return lastColorStates; }
static bool boundColor(IDirect3DDevice9 *d,BoundColor &b,bool program) {
    if (!probe.raster.opaqueMode || !currentScopeQueryBoundary() ||
        !scopeGpuEyeOwner(d,b.identity[3],b.identity[4],b.width,b.height,b.surfaceWidth,b.surfaceHeight,b.stereo) ||
        FAILED(d->GetRenderTarget(0,&b.target)) || !b.target || !identity(b.target,b.identity[1]) ||
        FAILED(d->GetDepthStencilSurface(&b.depth)) || !b.depth || !identity(b.depth,b.identity[2]) ||
        b.identity[1]!=b.identity[3] || b.identity[2]!=b.identity[4] ||
        FAILED(d->GetViewport(&b.viewport))) { lastColorFailure=1; return false; }
    const auto &vp=b.viewport;
    if (vp.X || vp.Y || vp.Width!=b.width || vp.Height!=b.height ||
        !std::isfinite(vp.MinZ) || !std::isfinite(vp.MaxZ) ||
        vp.MinZ!=probe.raster.nearDepth || vp.MaxZ!=probe.raster.farDepth ||
        vp.MinZ<0 || vp.MaxZ>1 || vp.MaxZ<=vp.MinZ) { lastColorFailure=2; return false; }
    D3DSURFACE_DESC target{},depth{};
    if (FAILED(b.target->GetDesc(&target)) || FAILED(b.depth->GetDesc(&depth))) { lastColorFailure=3; return false; }
    for (const auto &surface:{target,depth})
        if (surface.Type!=D3DRTYPE_SURFACE || surface.Width!=b.surfaceWidth || surface.Height!=b.surfaceHeight ||
            surface.MultiSampleType!=D3DMULTISAMPLE_NONE || surface.MultiSampleQuality) { lastColorFailure=4; return false; }
    if (!(target.Usage&D3DUSAGE_RENDERTARGET) || !(depth.Usage&D3DUSAGE_DEPTHSTENCIL)) { lastColorFailure=5; return false; }
    D3DCAPS9 caps{};
    if (FAILED(d->GetDeviceCaps(&caps)) || !caps.NumSimultaneousRTs || caps.NumSimultaneousRTs>4 ||
        caps.MaxUserClipPlanes>6) { lastColorFailure=6; return false; }
    for (DWORD i=1;i<caps.NumSimultaneousRTs;++i) {
        const HRESULT result=d->GetRenderTarget(i,&b.extra);
        const bool extra=b.extra!=nullptr;
        release(b.extra);
        if (extra || (FAILED(result) && result!=D3DERR_NOTFOUND)) { lastColorFailure=7; return false; }
    }
    for (size_t i=0;i<colorStates.size();++i)
        if (FAILED(d->GetRenderState(colorStates[i],&b.states[i]))) { lastColorFailure=8; return false; }
    const auto &s=b.states;
    // Fog may be on (world-pass weapons draw inside the level's fog); the lens
    // draw keeps it, so the image fogs exactly like the native cap it replaces.
    if (s[0]!=D3DZB_TRUE || s[1]!=TRUE || s[2]!=D3DCMP_LESSEQUAL || s[3] || s[4] || s[5] || s[6]>1 ||
        (s[7]&7)!=7 || (s[7]&~15u) || s[8]!=D3DFILL_SOLID || s[9]<D3DCULL_NONE || s[9]>D3DCULL_CCW ||
        s[10]>1 || (s[11]&~((1u<<caps.MaxUserClipPlanes)-1)) || s[14]!=TRUE || s[15]>1 ||
        !std::isfinite(std::bit_cast<float>(s[12])) || !std::isfinite(std::bit_cast<float>(s[13]))) {
        lastColorFailure=9; std::copy(s.begin(),s.end(),lastColorStates); return false;
    }
    if (s[10] && (FAILED(d->GetScissorRect(&b.scissor)) || b.scissor.left<0 || b.scissor.top<0 ||
        b.scissor.right<=b.scissor.left || b.scissor.bottom<=b.scissor.top ||
        uint32_t(b.scissor.right)>b.width || uint32_t(b.scissor.bottom)>b.height)) { lastColorFailure=10; return false; }
    for (unsigned i=0;i<b.clipPlanes.size();++i) if (s[11]&(1u<<i)) {
        if (FAILED(d->GetClipPlane(i,b.clipPlanes[i].data()))) { lastColorFailure=11; return false; }
        for (float value:b.clipPlanes[i]) if (!std::isfinite(value)) { lastColorFailure=12; return false; }
    }
    if (FAILED(d->GetPixelShader(&b.shader)) || !b.shader || !identity(b.shader,b.identity[0])) { lastColorFailure=13; return false; }
    if (program) {
        UINT size=0;
        if (FAILED(b.shader->GetFunction(nullptr,&size)) || size<8 || size%4 || size>sizeof(probe.colorProgram)) { lastColorFailure=14; return false; }
        const UINT expected=size;
        if (FAILED(b.shader->GetFunction(probe.colorProgram.data(),&size)) || size!=expected ||
            !scopeColorProgram(std::span(probe.colorProgram).first(size/4))) { lastColorFailure=15; return false; }
    }
    b.valid=true;
    return true;
}
static bool sameColor(const BoundColor &a,const BoundColor &b) noexcept {
    return a.valid && b.valid && a.width==b.width && a.height==b.height &&
        a.surfaceWidth==b.surfaceWidth && a.surfaceHeight==b.surfaceHeight && a.stereo==b.stereo &&
        std::equal(std::begin(a.identity),std::end(a.identity),std::begin(b.identity)) &&
        !std::memcmp(&a.viewport,&b.viewport,sizeof(a.viewport)) &&
        !std::memcmp(&a.scissor,&b.scissor,sizeof(a.scissor)) && a.states==b.states &&
        !std::memcmp(a.clipPlanes.data(),b.clipPlanes.data(),sizeof(a.clipPlanes));
}
static bool copySlice(bool index,ScopeByteRange range,std::span<uint8_t> destination) {
    if (range.size != destination.size() || probe.lock.outstanding()) return false;
    probe.lockedVertex=index?nullptr:probe.bindings[0].vertex[0];
    probe.lockedIndex=index?probe.bindings[0].index:nullptr;
    probe.mapped=nullptr;
    if (!probe.lock.beginLock()) return false;
    const HRESULT result=index ? probe.lockedIndex->Lock(range.offset,range.size,&probe.mapped,D3DLOCK_READONLY) :
                                 probe.lockedVertex->Lock(range.offset,range.size,&probe.mapped,D3DLOCK_READONLY);
    probe.lock.finishLock(SUCCEEDED(result));
    if (FAILED(result)) {
        probe.lockedVertex=nullptr; probe.lockedIndex=nullptr;
        return false;
    }
    const bool copied=probe.mapped != nullptr;
    if (copied) std::memcpy(destination.data(),probe.mapped,destination.size());
    return unlock() && copied && !fatalDraw.load(std::memory_order_acquire);
}
static bool hashSlices() {
    DWORD size=0,reported=0;
    if (BCryptOpenAlgorithmProvider(&probe.algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0 ||
        BCryptGetProperty(probe.algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&reported,0)<0 ||
        reported!=sizeof(size) || !size || size>probe.hashObject.size()) return false;
    const ScopeSliceBytes slices{std::span(probe.positions).first(ScopeVertices*12),
        std::span(probe.indices).first(ScopeTriangles*6),std::span(probe.weights).first(ScopeVertices*4),
        std::span(probe.localIndices).first(ScopeVertices*4),std::span(probe.uv).first(ScopeVertices*8)};
    const std::array<std::span<const uint8_t>,5> bytes{slices.positions,slices.indices,slices.weights,slices.localIndices,slices.uv};
    constexpr const char *expected[]{
        "a83d4d52827f2f95418f46c2ef8f8eca3ac594110ff35173bef4a526188a99c2",
        "9e906c5c63a118742d857f0641ab39494ed1350374ea9e132fa5745d3637781b",
        "2852dade36b5c2b533147f17dc9018ae35eff637e68d7e85fee12c05271d30ad",
        "4bcd1d733a353b8de5512ea22b608ebc74c4a818fe00663ff32ea4f7a3dd808a",
        "199e21cfca88d708d605db3ee54f35bba76191bf698c725baf4a39034480b2fd"};
    constexpr char hex[]="0123456789abcdef";
    for (unsigned i=0;i<bytes.size();++i) {
        std::array<uint8_t,32> digest{};
        if (BCryptCreateHash(probe.algorithm,&probe.hash,probe.hashObject.data(),size,nullptr,0,0)<0 ||
            BCryptHashData(probe.hash,const_cast<PUCHAR>(bytes[i].data()),ULONG(bytes[i].size()),0)<0 ||
            BCryptFinishHash(probe.hash,digest.data(),ULONG(digest.size()),0)<0) return false;
        const auto h=probe.hash; probe.hash=nullptr;
        if (BCryptDestroyHash(h)<0) return false;
        for (unsigned j=0;j<digest.size();++j)
            if (expected[i][2*j]!=hex[digest[j]>>4] || expected[i][2*j+1]!=hex[digest[j]&15]) return false;
    }
    return copyScopeCap(slices,probe.cap);
}
static bool hashGeometrySlices(const ScopeCopyRanges &ranges,std::array<std::array<uint8_t,32>,5> &digests) {
    DWORD size=0,reported=0;
    if(BCryptOpenAlgorithmProvider(&probe.algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0 ||
       BCryptGetProperty(probe.algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&reported,0)<0 ||
       reported!=sizeof(size) || !size || size>probe.hashObject.size())return false;
    const std::array<std::span<uint8_t>,5> storage{probe.positions,probe.indices,probe.weights,probe.localIndices,probe.uv};
    for(unsigned i=0;i<5;++i) {
        if(ranges.slices[i].size>storage[i].size() ||
           BCryptCreateHash(probe.algorithm,&probe.hash,probe.hashObject.data(),size,nullptr,0,0)<0 ||
           BCryptHashData(probe.hash,storage[i].data(),ranges.slices[i].size,0)<0 ||
           BCryptFinishHash(probe.hash,digests[i].data(),32,0)<0)return false;
        const auto h=probe.hash;probe.hash=nullptr;if(BCryptDestroyHash(h)<0)return false;
    }
    return true;
}
static bool hashIdleSlices(const ScopeCopyRanges &ranges) { return hashGeometrySlices(ranges,probe.idle.hashes); }
// Existing buffer owners, five bounded copies and crypto owner; no affine admission.
static bool collectPaletteContent(IDirect3DDevice9 *d,IdleWeaponTrace *owner,unsigned slot,unsigned api,
                                  const IdlePaletteCopy &native) {
    releaseBindings(probe.bindings[0]);
    if(!scopeGpuForwardingAllowed() || !paletteApiOwnerCurrent(d,owner,slot,api))return false;
    const auto draw=owner->submissions.rows[slot].draw;
    auto &b=probe.bindings[0];ScopeCopyRanges ranges;
    if(!boundInputs(d,b,draw,GeometryBufferPolicy::Idle,nullptr,2,&native.metadata.layout) ||
       !paletteApiOwnerCurrent(d,owner,slot,api) ||
       !idleBufferRanges(b.values,std::span(b.elements).first(b.count),ranges,nullptr,2))return false;
    const std::array<std::span<uint8_t>,5> storage{probe.positions,probe.indices,probe.weights,probe.localIndices,probe.uv};
    for(unsigned i=0;i<storage.size();++i) {
        if(ranges.slices[i].size>storage[i].size() ||
           !copySlice(i==1,ranges.slices[i],storage[i].first(ranges.slices[i].size)))return false;
        if(!scopeGpuForwardingAllowed() || !paletteApiOwnerCurrent(d,owner,slot,api))return false;
    }
    // Hash only owned copies after every successful unlock; no retry of live crypto.
    if(!scopeGpuForwardingAllowed() || probe.algorithm || probe.hash || !hashIdleSlices(ranges) ||
       !paletteApiOwnerCurrent(d,owner,slot,api))return false;
    auto &p=owner->paletteApiPayloads[api];p.content=b.values;p.contentHashes=probe.idle.hashes;
    return true;
}
static bool sameInputs(const BoundInputs &a,const BoundInputs &b) {
    return a.values==b.values && a.count==b.count && a.elements==b.elements &&
        std::equal(std::begin(a.identity),std::end(a.identity),std::begin(b.identity)) &&
        a.constantCount==b.constantCount &&
        !std::memcmp(a.constants.data(),b.constants.data(),a.constantCount*4*sizeof(float));
}
// Optional vehicle observation uses the SAME references, scratch, locks and
// crypto owner. No getter, hashing or native query runs while a buffer is locked.
static bool rideGpuOwnerCurrent(IDirect3DDevice9 *d) {
    const bool owner=remote_render::rideMainDrawCurrent(probe.rideBefore);
    RideCameraCopy camera;
    const bool cameraCurrent=!probe.rideCameraCaptured ||
        (copyExecutedRideCamera(probe.rideBefore.identity.player,probe.generation,probe.rideBefore.eye,camera) &&
         camera==probe.rideCamera);
    return owner && cameraCurrent && !probe.rideReentered && probe.generation==graphicsResourceGeneration() &&
        scopeGpuForwardingAllowed() && scopeGpuMappingObservationCurrent(d);
}
static bool rideGpuBookend(IDirect3DDevice9 *d) {
    RideMainDrawCopy now;
    if(!remote_render::copyCurrentRideMainDraw(now))return false;
    now.api=probe.rideBefore.api;
    return now==probe.rideBefore && rideGpuOwnerCurrent(d);
}
static bool beginRideGpu(IDirect3DDevice9 *d) {
    auto &g=probe.rideGpu;g.copied=false;g.handles={};g.handleWorld={};g.handlePositionAgrees=false;
    probe.rideCameraCaptured=copyExecutedRideCamera(probe.rideBefore.identity.player,
        probe.generation,probe.rideBefore.eye,probe.rideCamera);
    const auto draw=probe.rideBefore.api;
    if(!rideGpuOwnerCurrent(d) || !boundInputs(d,probe.bindings[0],draw,GeometryBufferPolicy::Ride,
            nullptr,-1,&probe.rideBefore.layout) || !boundProgram(false,true) || !rideGpuBookend(d))return false;
    ScopeCopyRanges ranges;
    auto &b=probe.bindings[0];
    if(!rideBufferRanges(b.values,std::span(b.elements).first(b.count),ranges))return false;
    const std::array<std::span<uint8_t>,5> storage{probe.positions,probe.indices,probe.weights,probe.localIndices,probe.uv};
    for(unsigned i=0;i<storage.size();++i) {
        if(ranges.slices[i].size>storage[i].size() ||
           !copySlice(i==1,ranges.slices[i],storage[i].first(ranges.slices[i].size)))return false;
        if(!rideGpuOwnerCurrent(d))return false;
    }
    if(probe.algorithm || probe.hash || !hashGeometrySlices(ranges,g.hashes) || !rideGpuOwnerCurrent(d) ||
       !boundInputs(d,probe.bindings[1],draw,GeometryBufferPolicy::Ride,nullptr,-1,&probe.rideBefore.layout) ||
       !sameInputs(b,probe.bindings[1]) || !rideGpuBookend(d))return false;
    bool weights=false;GeometryInputLayout layout{};
    if(!idleGeometryInputs(std::span(b.elements).first(b.count),layout,weights) || !weights)return false;
    g.inputs=b.values;g.declaration=b.elements;g.declarationElements=b.count;
    g.programWords=uint32_t(probe.programWords);std::copy_n(probe.program.begin(),g.programWords,g.program.begin());
    g.constantRows=b.constantCount;g.inputLayout=uint32_t(layout);
    g.declarationObject=uint32_t(reinterpret_cast<uintptr_t>(b.identity[5]));
    g.shaderObject=uint32_t(reinterpret_cast<uintptr_t>(b.identity[6]));
    for(unsigned i=0;i<b.constantCount;++i)for(unsigned j=0;j<4;++j)
        g.constants[4*i+j]=std::bit_cast<uint32_t>(b.constants[i][j]);
    const auto profile=rideHandleProfile(g.hashes);
    const ScopeSliceBytes slices{storage[0].first(ranges.slices[0].size),storage[1].first(ranges.slices[1].size),
        storage[2].first(ranges.slices[2].size),storage[3].first(ranges.slices[3].size),storage[4].first(ranges.slices[4].size)};
    if(profile!=RideHandleProfile::Unknown && copyRideHandles(slices,profile,probe.rideBefore.paletteCount,g.handles) &&
       probe.rideCameraCaptured) {
        Matrix34 world{},main{};
        std::memcpy(world.m,probe.rideBefore.world.data(),sizeof(world.m));
        std::memcpy(main.m,probe.rideBefore.actualPalette.data(),sizeof(main.m));
        g.handlePositionAgrees=rideHandlesPosition(g.handles,probe.rideBefore.paletteCount,probe.rideBefore.localMainSlot,
            std::span(g.program).first(g.programWords),std::span(b.constants).first(b.constantCount),layout,
            world,main,probe.rideCamera.view,probe.rideCamera.projection,g.handleWorld) ||
            rideHandlesWorld(g.handles,probe.rideBefore.paletteCount,probe.rideBefore.localMainSlot,world,main,g.handleWorld);
    }
    return true;
}
// Lab (SS2VR_LAB_RIDE_DUMP=1): an unknown ride surface's positions and
// indices, once per layout, as Bin/ride-surface-<vertices>-<triangles>.bin
// (u32 vertices, u32 triangles, float3 positions, u16 indices), to find a
// vehicle's control handles offline. Read-only locks, nothing retained.
static void labDumpRideSurface(IDirect3DDevice9 *d,const RideMainDrawCopy &draw) {
    static const bool enabled=[] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_RIDE_DUMP",v,2)==1 && v[0]==L'1'; }();
    static std::array<std::pair<int32_t,int32_t>,16> done{};
    static unsigned doneCount=0;
    const auto &l=draw.layout;
    if(!enabled || l.vertices<=0 || l.triangles<=0 || l.vertices>200000 || l.triangles>400000 || doneCount>=done.size())return;
    for(unsigned i=0;i<doneCount;++i)if(done[i]==std::make_pair(l.vertices,l.triangles))return;
    done[doneCount++]={l.vertices,l.triangles};
    log("Lab ride surface layout vertices=%d triangles=%d positions=%u/%02x/%u indices=%u/%02x/%u weights=%u/%02x/%u local=%u/%02x/%u palette=%u",
        l.vertices,l.triangles,l.channels[0].offset,l.channels[0].format,l.channels[0].buffer,l.channels[1].offset,l.channels[1].format,
        l.channels[1].buffer,l.channels[2].offset,l.channels[2].format,l.channels[2].buffer,l.channels[3].offset,l.channels[3].format,
        l.channels[3].buffer,draw.paletteCount);
    if(l.channels[0].format!=0x85 || l.channels[1].format!=0x87)return;
    IDirect3DVertexBuffer9 *vb=nullptr;UINT offset=0,stride=0;IDirect3DIndexBuffer9 *ib=nullptr;
    if(FAILED(d->GetStreamSource(l.channels[0].buffer,&vb,&offset,&stride)) || !vb)return;
    if(FAILED(d->GetIndices(&ib)) || !ib) {vb->Release();return;}
    std::vector<uint8_t> positions(size_t(l.vertices)*12),indices(size_t(l.triangles)*6);
    void *mapped=nullptr;
    bool ok=SUCCEEDED(vb->Lock(l.channels[0].offset,UINT(positions.size()),&mapped,D3DLOCK_READONLY)) && mapped;
    if(ok){std::memcpy(positions.data(),mapped,positions.size());vb->Unlock();}
    mapped=nullptr;
    ok=ok && SUCCEEDED(ib->Lock(l.channels[1].offset,UINT(indices.size()),&mapped,D3DLOCK_READONLY)) && mapped;
    if(ok){std::memcpy(indices.data(),mapped,indices.size());ib->Unlock();}
    vb->Release();ib->Release();
    if(!ok)return;
    wchar_t path[MAX_PATH]{};HMODULE self=nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&labDumpRideSurface),&self);
    std::wstring file(path,GetModuleFileNameW(self,path,MAX_PATH));
    file=file.substr(0,file.find_last_of(L"\\/")+1)+L"ride-surface-"+std::to_wstring(l.vertices)+L"-"+std::to_wstring(l.triangles)+L".bin";
    if(FILE *f=_wfopen(file.c_str(),L"wb")) {
        const uint32_t header[2]{uint32_t(l.vertices),uint32_t(l.triangles)};
        std::fwrite(header,4,2,f);std::fwrite(positions.data(),1,positions.size(),f);std::fwrite(indices.data(),1,indices.size(),f);
        std::fclose(f);
        log("Lab ride surface written vertices=%d triangles=%d",l.vertices,l.triangles);
    }
}
static bool finishRideGpu(IDirect3DDevice9 *d) {
    return rideGpuOwnerCurrent(d) &&
        boundInputs(d,probe.bindings[2],probe.rideBefore.api,GeometryBufferPolicy::Ride,nullptr,-1,&probe.rideBefore.layout) &&
        sameInputs(probe.bindings[0],probe.bindings[2]);
}
static bool collectIdleGeometry(IDirect3DDevice9 *d,const ScopeIndexedDraw &draw) {
    auto reject=[](IdleWeaponTrace::Rejection reason) {if(probe.idleTrace)probe.idleTrace->reject(reason);return false;};
    if(probe.idleTrace)probe.idleTrace->callbacks|=IdleWeaponTrace::GeometrySeen;
    if(!nativeUiDeviceCurrent(d))return reject(IdleWeaponTrace::Rejection::CollectDevice);
    ScopeCopyRanges ranges;
    IdleWeaponTrace::InputFailure inputFailure{};
    if(!boundInputs(d,probe.bindings[0],draw,GeometryBufferPolicy::Idle,&inputFailure,probe.idleNativeId)) {
        const bool first=probe.idleTrace && probe.idleTrace->rejection==IdleWeaponTrace::Rejection::None;
        if(first)probe.idleTrace->inputFailure=inputFailure;
        reject(IdleWeaponTrace::Rejection::CollectInputs); // Original immediate rejection precedes all extra queries.
        if(first && probe.paletteApiSlot==IdleWeaponTrace::MaxPaletteApiPayloads &&
           IdleWeaponTrace::observedStreamFamily(inputFailure))beginIdleStreamProbe(d);
        return false;
    }
    if(!idleBufferRanges(probe.bindings[0].values,std::span(probe.bindings[0].elements).first(probe.bindings[0].count),ranges,nullptr,probe.idleNativeId))return reject(IdleWeaponTrace::Rejection::CollectRanges);
    if(!boundProgram(true))return reject(IdleWeaponTrace::Rejection::CollectProgram);
    const std::array<std::span<uint8_t>,5> storage{probe.positions,probe.indices,probe.weights,probe.localIndices,probe.uv};
    for(unsigned i=0;i<5;++i)
        if(ranges.slices[i].size>storage[i].size() || !copySlice(i==1,ranges.slices[i],storage[i].first(ranges.slices[i].size)))return reject(IdleWeaponTrace::Rejection::CollectSlice);
    if(!hashIdleSlices(ranges))return reject(IdleWeaponTrace::Rejection::CollectHash);
    if(!boundInputs(d,probe.bindings[1],draw,GeometryBufferPolicy::Idle,nullptr,probe.idleNativeId))return reject(IdleWeaponTrace::Rejection::CollectRebind);
    if(!sameInputs(probe.bindings[0],probe.bindings[1]))return reject(IdleWeaponTrace::Rejection::CollectChanged);
    IdleRasterCopy now;IdleWeaponTrace *trace=nullptr;
    if(!currentIdleRaster(now,trace) || trace!=probe.idleTrace || now!=probe.idle.raster)return reject(IdleWeaponTrace::Rejection::CollectRaster);
    probe.idle.factorBookend(now,false);
    if(!nativeUiDeviceCurrent(d) || probe.generation!=graphicsResourceGeneration() || !scopeGpuTransactionCurrent(d))return reject(IdleWeaponTrace::Rejection::CollectLifetime);
    probe.idle.inputs=probe.bindings[0].values;
    probe.idle.words=unsigned(probe.programWords);
    std::copy_n(probe.program.begin(),probe.programWords,probe.idle.program.begin());
    probe.idle.declarationCount=probe.bindings[0].count;
    probe.idle.declaration=probe.bindings[0].elements;
    probe.idle.constantCount=probe.bindings[0].constantCount;
    probe.idle.constants=probe.bindings[0].constants;
    return true;
}
static constexpr D3DSAMPLERSTATETYPE imageSamplerStates[]{D3DSAMP_ADDRESSU,D3DSAMP_ADDRESSV,D3DSAMP_ADDRESSW,
    D3DSAMP_MAGFILTER,D3DSAMP_MINFILTER,D3DSAMP_MIPFILTER,D3DSAMP_SRGBTEXTURE,D3DSAMP_MAXMIPLEVEL,
    D3DSAMP_MIPMAPLODBIAS,D3DSAMP_MAXANISOTROPY};
static constexpr DWORD imageSamplerValues[]{D3DTADDRESS_CLAMP,D3DTADDRESS_CLAMP,D3DTADDRESS_CLAMP,
    D3DTEXF_LINEAR,D3DTEXF_LINEAR,D3DTEXF_LINEAR,0,0,0,1}; // Trilinear over the lens image's mip chain.
static constexpr D3DRENDERSTATETYPE imageStates[]{D3DRS_ZWRITEENABLE,D3DRS_ZFUNC,D3DRS_COLORWRITEENABLE};
static bool restoreImageState(IDirect3DDevice9 *d) noexcept {
    if (!probe.imageChanged) return !probe.imageRestoreFailed;
    probe.imageChanged = false; // One bounded restoration attempt, including foreign unwind.
    if (probe.generation != graphicsResourceGeneration()) {
        probe.imageRestoreFailed = false;
        return true; // Retired invocation: release-only cleanup, no old shader/texture restoration.
    }
    probe.imageRestoreFailed = true;
    bool ok = SUCCEEDED(d->SetPixelShader(probe.bindings[0].color.shader));
    ok = SUCCEEDED(d->SetTexture(0,probe.originalTexture)) && ok;
    ok = SUCCEEDED(d->SetPixelShaderConstantF(0,probe.pixelConstants[0].data(),7)) && ok;
    for (unsigned i=0;i<std::size(imageSamplerStates);++i)
        ok = SUCCEEDED(d->SetSamplerState(0,imageSamplerStates[i],probe.sampler[i])) && ok;
    const DWORD values[]{probe.bindings[0].color.states[1],probe.bindings[0].color.states[2],probe.bindings[0].color.states[7]};
    for (unsigned i=0;i<std::size(imageStates);++i) ok = SUCCEEDED(d->SetRenderState(imageStates[i],values[i])) && ok;
    if (ok) {
        ok = SUCCEEDED(d->GetPixelShader(&probe.restoredShader)) &&
            probe.restoredShader == probe.bindings[0].color.shader &&
            SUCCEEDED(d->GetTexture(0,&probe.restoredTexture)) && probe.restoredTexture == probe.originalTexture;
        std::array<std::array<float,4>,7> constants{};
        ok = SUCCEEDED(d->GetPixelShaderConstantF(0,constants[0].data(),7)) &&
            !std::memcmp(constants.data(),probe.pixelConstants.data(),sizeof(constants)) && ok;
        for (unsigned i=0;i<std::size(imageSamplerStates);++i) {
            DWORD value=0;
            ok = SUCCEEDED(d->GetSamplerState(0,imageSamplerStates[i],&value)) && value == probe.sampler[i] && ok;
        }
        for (unsigned i=0;i<std::size(imageStates);++i) {
            DWORD value=0;
            ok = SUCCEEDED(d->GetRenderState(imageStates[i],&value)) && value == values[i] && ok;
        }
        release(probe.restoredTexture); release(probe.restoredShader);
    }
    probe.imageRestoreFailed = !ok;
    if (!ok) scopeGpuFault();
    return ok;
}
static bool imageCurrent(IDirect3DDevice9 *d) noexcept {
    ScopeRasterObservation current;
    return scopeGpuTransactionCurrent(d) && scopeGpuForwardingAllowed() && nativeUiDeviceCurrent(d) &&
        d->TestCooperativeLevel() == D3D_OK &&
        !probe.imageRestoreFailed && currentScopeRaster(current) && currentScopeQueryBoundary() &&
        current.hand == probe.raster.hand && scopeImagePoseMatches(probe.raster.pose,current.pose,probe.sourceView.alwaysLive);
}
static bool prepareImage(IDirect3DDevice9 *d) {
    auto &color = probe.bindings[0].color;
    if (!color.valid || color.states[15] != FALSE ||
        !scopeGpuImage(d,probe.raster.hand,probe.sourceView,probe.image,probe.imageShader) ||
        probe.sourceView.hand != probe.raster.hand ||
        !scopeImageSourceRecent(probe.sourceView.pose,probe.raster.pose,probe.sourceView.alwaysLive) ||
        !scopeOpticalFrameValid(probe.sourceView.pose.opticalFrame)) return false;
    ScopeOpticalFrame optic;
    ScopeImageCoordinates coordinates;
    if (!scopeOpticalFrame(probe.cap,probe.raster.pose.affine,optic) ||
        !scopeImageCoordinates(probe.cap,probe.raster.pose.affine,ScopeUvTransform{probe.uvRows},coordinates))
        return false;
    const auto &source = probe.sourceView.pose.opticalFrame;
    Vec3 target;
    const bool aimed = scopeImageTarget(probe.raster.hand,target);
    // One eye's lens constants (only the eye position, row 3, differs per eye).
    const auto build = [&](Vec3 eye, ScopeShaderConstants &out) {
        if (!scopeShaderConstants(optic,probe.sourceView.opticalProjection,coordinates,eye,1,0,0,out)) return false;
        // Forgiving eye box: the sniper's scope rides well above the controller, so
        // a strict exit pupil (about 8 degrees at 4x) blacks the lens unless the
        // head is exactly behind it. Only a quarter of the eye's sideways offset
        // drives parallax; the lens still shifts naturally as the head moves.
        constexpr float EyeBoxParallax = .25f;
        out.rows[3][0] *= EyeBoxParallax;
        out.rows[3][1] *= EyeBoxParallax;
        // The source image was rendered from the previous frame's scope pose:
        // look it up in that camera's frame (rotation and translation exact).
        if (!scopeReprojectConstants(optic,source,out)) return false;
        // The crosshair: thin lines across the whole lens through where the shot
        // lands (the laser's hit), as a rifle scope's reticle.
        // Without this frame's laser sample: the optical centre, where the shots
        // land too (lab V621: the laser's hit at uv 0.500,0.49).
        if (!aimed || !scopeReticleTarget(source,probe.sourceView.opticalProjection,target,.0015f,2.f,out))
            out.rows[6] = {.5f,.5f,.0015f,2.f};
        return true;
    };
    probe.stereoImage = false;
    if (probe.bindings[0].color.stereo) {
        Vec3 eyes[2];
        if (!scopeImageEyes(eyes) || !build(eyes[0],probe.eyeConstants[0]) || !build(eyes[1],probe.eyeConstants[1]) ||
            !(probe.eyeConstants[0].rows[3][2] > 1e-6f) || !(probe.eyeConstants[1].rows[3][2] > 1e-6f)) return false;
        probe.imageConstants = probe.eyeConstants[0];
        probe.stereoImage = true;
    } else {
        Vec3 eye;
        if (!scopeImageEye(eye) || !build(eye,probe.imageConstants)) return false;
    }
    if (FAILED(d->GetTexture(0,&probe.originalTexture)) ||
        FAILED(d->GetPixelShaderConstantF(0,probe.pixelConstants[0].data(),7))) return false;
    for (unsigned i=0;i<std::size(imageSamplerStates);++i)
        if (FAILED(d->GetSamplerState(0,imageSamplerStates[i],&probe.sampler[i]))) return false;
    return imageCurrent(d) && d->TestCooperativeLevel() == D3D_OK;
}
static unsigned imageDraws = 0;
static std::array<float,4> lastEyeInOptic{};
unsigned scopeImageDraws() noexcept { return imageDraws; }
const float *scopeEyeInOptic() noexcept { return lastEyeInOptic.data(); }
static bool drawImage(IDirect3DDevice9 *d,D3DPRIMITIVETYPE type,INT base,UINT minimum,UINT vertices,
                      UINT start,ScopeIndexedForward forward,ScopeDrawRange range) noexcept {
    ++imageDraws; lastEyeInOptic = probe.imageConstants.rows[3];
    probe.imageChanged = true;
    bool ok = SUCCEEDED(d->SetPixelShader(probe.imageShader)) && SUCCEEDED(d->SetTexture(0,probe.image)) &&
        SUCCEEDED(d->SetPixelShaderConstantF(0,probe.imageConstants.rows[0].data(),7));
    for (unsigned i=0;i<std::size(imageSamplerStates);++i)
        ok = SUCCEEDED(d->SetSamplerState(0,imageSamplerStates[i],imageSamplerValues[i])) && ok;
    const DWORD values[]{FALSE,D3DCMP_EQUAL,D3DCOLORWRITEENABLE_RED|D3DCOLORWRITEENABLE_GREEN|D3DCOLORWRITEENABLE_BLUE};
    for (unsigned i=0;i<std::size(imageStates);++i) ok = SUCCEEDED(d->SetRenderState(imageStates[i],values[i])) && ok;
    if (ok && imageCurrent(d)) {
        // Single pass: each eye's region gets its own eye position (parallax).
        if (probe.stereoImage)
            single_pass::setEyePixelConstants(0,7,probe.eyeConstants[0].rows[0].data(),probe.eyeConstants[1].rows[0].data());
        ok = SUCCEEDED(forward(d,type,base,minimum,vertices,start+range.firstIndex,range.triangles));
        single_pass::setEyePixelConstants(0,0,nullptr,nullptr);
    } else ok = false;
    ok = restoreImageState(d) && ok;
    return ok && imageCurrent(d);
}
static HRESULT probeScopeDraw(IDirect3DDevice9 *d,D3DPRIMITIVETYPE type,INT base,UINT minimum,UINT vertices,
                     UINT start,UINT primitives,uintptr_t caller,ScopeIndexedForward forward) noexcept {
    if(probe.busy)probe.rideReentered=true;
    if (!scopeGpuForwardingAllowed()) return D3DERR_INVALIDCALL;
    static const auto gfx=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"GfxD3D.dll")); // Loaded before any draw; never unloaded.
    if(probe.busy && probe.idleTrace)probe.idleTrace->reject(IdleWeaponTrace::Rejection::GpuReentry);
    if(probe.busy && probe.idleTrace && probe.idleTrace->streamProbe.selected)probe.streamReentered=true;
    if(probe.busy && probe.submissionOwner)probe.submissionOwner->submissions.reenter(probe.submissionSlot);
    if (probe.busy || !gfx || caller != gfx+0xa011) return forward(d,type,base,minimum,vertices,start,primitives);
    // Nearly every world draw is neither the tracked weapon's model nor a ride
    // main draw: forward it without resetting the observation state (lab
    // profile: the per-draw resets were ~15% of each eye's CPU time).
    static const bool labSlowPath = [] { wchar_t value[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_NO_FASTPATH", value, 2) == 1 && value[0] == L'1'; }();
    if (!labSlowPath && !weaponModelDrawActive() && !remote_render::rideDrawPossible())
        return forward(d,type,base,minimum,vertices,start,primitives);
    // All owning state exists above NativeFinally; no stack-native pointer is
    // kept through any COM call, and no allocation/native callback occurs locked.
    probe.busy=true;
    probe.cap={}; probe.raster={}; probe.idle={}; probe.idleTrace=nullptr;probe.idleNativeId=-1; probe.uvRows={}; probe.program={}; probe.colorProgram={};
    probe.generation=graphicsResourceGeneration();
    probe.programWords=0;
    probe.streamReentered=false;
    probe.rideBefore={};probe.rideAfter={};
    probe.rideGpu.copied=false;probe.rideGpuAttempt=false;probe.rideGpuMatched=false;
    probe.rideCameraCaptured=false;
    probe.rideReentered=false;probe.rideOriginalReturned=false;probe.rideMatched=false;probe.rideReady=false;
    probe.paletteApiSlot=IdleWeaponTrace::MaxPaletteApiPayloads;
    HRESULT result=D3DERR_INVALIDCALL;
    withNativeFinally([&] {
        // Arm an independent bounded value owner before the first AddRef.
        // The existing geometry rejection (including cap8) is untouched.
        probe.submissionOwner=idleSubmissionOwner();
        if(probe.submissionOwner && probe.submissionOwner->nativeId==2)
            probe.submissionOwner->projectionOpportunities.mark(probe.submissionOwner->projectionOpportunities.submission);
        if(probe.submissionOwner)probe.submissionSlot=probe.submissionOwner->submissions.reserve(
            ScopeIndexedDraw{uint32_t(type),base,minimum,vertices,start,primitives});
        if(probe.submissionOwner)probe.paletteApiSlot=probe.submissionOwner->reservePaletteApi(probe.submissionSlot);
        bool colorCandidate=false;
        bool admitted=currentScopeRaster(probe.raster) && nativeUiDeviceCurrent(d);
        if (probe.raster.pose.valid) lastColorFailure = admitted ? 100 : 101;
        // Establish the borrowed ID1 trace before the first retained COM call,
        // so reentry during AddRef also rejects the outer observation.
        const bool idleCandidate=!probe.raster.pose.valid && currentIdleRaster(probe.idle.raster,probe.idleTrace);
        probe.idleNativeId=idleCandidate?probe.idleTrace->nativeId:-1;
        // Exclude every prior scope/legacy collection attempt, even an early failure.
        const bool paletteContentEligible=!admitted && !probe.raster.pose.valid && !idleCandidate;
        if(paletteContentEligible && remote_render::copyCurrentRideMainDraw(probe.rideBefore))
            probe.rideBefore.api={uint32_t(type),base,minimum,vertices,start,primitives};
        probe.device = d; d->AddRef();
        if(probe.rideBefore.bank) {
            // Bounded trace: which ride surfaces are seen and whether they are admitted.
            static unsigned traced=0;
            static std::array<std::pair<int32_t,int32_t>,24> seen{};
            const auto key=std::make_pair(probe.rideBefore.layout.vertices,probe.rideBefore.layout.triangles);
            bool fresh=true;for(unsigned i=0;i<traced && i<seen.size();++i)fresh=fresh && seen[i]!=key;
            if(fresh && traced<seen.size()) {
                seen[traced++]=key;
                log("Ride surface seen vertices=%d triangles=%d supported=%d palette=%u",key.first,key.second,
                    int(rideSurfaceSupported(probe.rideBefore.layout)),probe.rideBefore.paletteCount);
            }
        }
        if(probe.rideBefore.bank && rideSurfaceSupported(probe.rideBefore.layout) && !probe.submissionOwner && !probe.idleTrace && !probe.raster.pose.valid &&
           remote_render::claimRideGpuAttempt(probe.rideBefore)) {
            probe.rideGpuAttempt=beginRideGpu(d);
            static unsigned traced=0;
            if(traced<40) {
                ++traced;
                const auto &g=probe.rideGpu;
                log("Ride gpu attempt=%d copied=%d profile=%u handles=%d agrees=%d camera=%d",int(probe.rideGpuAttempt),int(g.copied),
                    unsigned(g.handles.profile),int(g.handles.copied),int(g.handlePositionAgrees),int(probe.rideCameraCaptured));
            }
        } else if(probe.rideBefore.bank && !rideSurfaceSupported(probe.rideBefore.layout) && !probe.submissionOwner &&
                  !probe.idleTrace && !probe.raster.pose.valid) {
            labDumpRideSurface(d,probe.rideBefore);
        }
        if (admitted) probe.transaction = scopeGpuTransactionBegin(d);
        if (admitted) {
            const ScopeIndexedDraw draw{uint32_t(type),base,minimum,vertices,start,primitives};
            ScopeCopyRanges ranges;
            admitted=boundInputs(d,probe.bindings[0],draw) &&
                scopeBufferRanges(probe.bindings[0].values,std::span(probe.bindings[0].elements).first(probe.bindings[0].count),ranges) && boundProgram();
            if (!admitted) lastColorFailure = 102;
            if (admitted) colorCandidate=boundColor(d,probe.bindings[0].color,true);
            const std::array<std::span<uint8_t>,5> bytes{std::span(probe.positions).first(ScopeVertices*12),
                std::span(probe.indices).first(ScopeTriangles*6),std::span(probe.weights).first(ScopeVertices*4),
                std::span(probe.localIndices).first(ScopeVertices*4),std::span(probe.uv).first(ScopeVertices*8)};
            for (unsigned i=0;admitted && i<bytes.size();++i) admitted=copySlice(i==1,ranges.slices[i],bytes[i]);
            if (probe.lock.outstanding() || fatalDraw.load(std::memory_order_acquire)) return;
            if (admitted) admitted=boundInputs(d,probe.bindings[1],draw) &&
                probe.bindings[0].values==probe.bindings[1].values &&
                probe.bindings[0].constantCount == probe.bindings[1].constantCount &&
                !std::memcmp(probe.bindings[0].constants.data(),probe.bindings[1].constants.data(),
                             probe.bindings[0].constantCount*4*sizeof(float)) &&
                probe.bindings[0].identity[5]==probe.bindings[1].identity[5] &&
                probe.bindings[0].identity[6]==probe.bindings[1].identity[6] &&
                !std::memcmp(probe.bindings[0].uvRows.data(),probe.bindings[1].uvRows.data(),sizeof(probe.uvRows)) &&
                probe.bindings[0].count==probe.bindings[1].count &&
                std::equal(probe.bindings[0].elements.begin(),probe.bindings[0].elements.begin()+probe.bindings[0].count,
                           probe.bindings[1].elements.begin()) && nativeUiDeviceCurrent(d) &&
                probe.generation==graphicsResourceGeneration() && scopeGpuRoutingCurrent(d);
            if (admitted) {
                probe.uvRows=probe.bindings[0].uvRows;
                colorCandidate=colorCandidate && boundColor(d,probe.bindings[1].color,false) &&
                    sameColor(probe.bindings[0].color,probe.bindings[1].color);
            }
            if (admitted) admitted=hashSlices(); // Hash ONLY owned copies, after all unlocks.
            const bool colorBefore = colorCandidate && admitted;
            colorCandidate = colorCandidate && admitted && probe.raster.capClipValid &&
                scopeCapPosition(std::span(probe.program).first(probe.programWords),
                    std::span(probe.bindings[0].constants).first(probe.bindings[0].constantCount),
                    probe.cap,probe.bindings[0].values.weights.object != 0,probe.raster.capClip,&lastCapError);
            if (colorBefore && !colorCandidate) lastColorFailure = probe.raster.capClipValid ? 91 : 90;
            else if (colorCandidate) lastColorFailure = 0;
            // Keep the first immutable identities through the whole optional
            // transaction; resample after hashing, getters and source retention.
            if (admitted && colorCandidate && probe.transaction && prepareImage(d)) {
                releaseBindings(probe.bindings[1]);
                auto &a = probe.bindings[0]; auto &b = probe.bindings[1];
                const bool unchanged = boundInputs(d,b,draw) && boundColor(d,b.color,false) &&
                    a.values == b.values && a.count == b.count && a.elements == b.elements &&
                    std::equal(std::begin(a.identity),std::end(a.identity),std::begin(b.identity)) &&
                    a.constantCount == b.constantCount &&
                    !std::memcmp(a.constants.data(),b.constants.data(),a.constantCount*4*sizeof(float)) &&
                    sameColor(a.color,b.color) && imageCurrent(d) && start <= UINT32_MAX-ScopeTriangles*3;
                if (unchanged) {
                    probe.split = true;
                    // The native cap ranges and the lens over them (EQUAL depth) take
                    // the same per-region path in a single pass.
                    single_pass::setInstancingSuspended(true);
                    const bool rendered = executeScopeCapDraw(
                        [&](ScopeDrawRange range) noexcept {
                            return SUCCEEDED(forward(d,type,base,minimum,vertices,start+range.firstIndex,range.triangles));
                        },[&](ScopeDrawRange range) noexcept {
                            return drawImage(d,type,base,minimum,vertices,start,forward,range);
                        },[&]() noexcept { return imageCurrent(d); });
                    single_pass::setInstancingSuspended(false);
                    if (!rendered) { scopeGpuFault(); result = D3DERR_INVALIDCALL; }
                    else result = D3D_OK;
                }
            }
        }
        bool idleAdmitted=false;
        if(idleCandidate) {
            probe.transaction=scopeGpuTransactionBegin(d);
            idleAdmitted=probe.transaction && collectIdleGeometry(d,{uint32_t(type),base,minimum,vertices,start,primitives});
        }
        if(probe.idleTrace && !idleAdmitted)probe.idleTrace->reject(IdleWeaponTrace::Rejection::GpuAdmission,IdleWeaponTrace::checks({idleCandidate,probe.transaction!=0}));
        if (!scopeGpuForwardingAllowed()) return;
        if (!probe.split) {
            auto *const owner=probe.submissionOwner;
            const unsigned slot=probe.submissionSlot,api=probe.paletteApiSlot;
            const bool paletteBefore=beginPaletteApi(d,owner,slot,api,idleAdmitted,paletteContentEligible);
            // Before fallback metadata/native calls as well as the original draw.
            if(!scopeGpuForwardingAllowed())return;
            if(!paletteBefore && owner && probe.submissionOwner==owner && probe.submissionSlot==slot &&
               owner->submissions.pending(slot)) {
                IdleSubmissionMetadata before;
                const bool copied=scopeGpuRoutingCurrent(d) && probe.generation==graphicsResourceGeneration() &&
                    copyIdleSubmissionMetadata(owner,before);
                if(probe.submissionOwner==owner && probe.submissionSlot==slot)
                    owner->submissions.before(slot,before,
                        copied && scopeGpuRoutingCurrent(d) && probe.generation==graphicsResourceGeneration());
            }
            if(probe.idleTrace && probe.idleTrace->streamProbe.selected)
                probe.idleTrace->streamProbe.flags|=IdleWeaponTrace::StreamProbe::ForwardCalled;
            result=forward(d,type,base,minimum,vertices,start,primitives);
            probe.rideOriginalReturned=true;
            if(probe.rideGpuAttempt && SUCCEEDED(result))probe.rideGpuMatched=finishRideGpu(d);
            if(probe.rideBefore.bank && SUCCEEDED(result) &&
               remote_render::copyCurrentRideMainDraw(probe.rideAfter)) {
                probe.rideAfter.api={uint32_t(type),base,minimum,vertices,start,primitives};
                probe.rideMatched=probe.rideBefore==probe.rideAfter;
            }
            IdlePaletteCopy paletteAfter;
            const bool paletteCopied=paletteBefore && finishPaletteApi(d,owner,slot,api,paletteAfter);
            if(idleAdmitted && paletteBefore && !paletteApiOwnerCurrent(d,owner,slot,api)) {
                idleAdmitted=false;
                if(probe.idleTrace)probe.idleTrace->reject(IdleWeaponTrace::Rejection::CollectLifetime);
            } else if(idleAdmitted && paletteBefore && (!paletteCopied || !owner->paletteApiPayloads[api].matched)) {
                idleAdmitted=false;
                if(probe.idleTrace)probe.idleTrace->reject(IdleWeaponTrace::Rejection::CollectChanged);
            }
            if(owner && probe.submissionOwner==owner && probe.submissionSlot==slot && slot<IdleSubmissionTrace::MaxAttempts) {
                IdleSubmissionMetadata after;
                if(paletteCopied)after=paletteAfter.metadata;
                const bool copied=paletteCopied || (owner->submissions.pending(slot) &&
                    scopeGpuRoutingCurrent(d) && probe.generation==graphicsResourceGeneration() &&
                    copyIdleSubmissionMetadata(owner,after));
                if(probe.submissionOwner==owner && probe.submissionSlot==slot)
                    owner->submissions.after(slot,after,
                        copied && scopeGpuRoutingCurrent(d) && probe.generation==graphicsResourceGeneration(),int32_t(result));
            }
        }
        if(!probe.split && probe.idleTrace && probe.idleTrace->streamProbe.selected)finishIdleStreamProbe(d,result);
        if(probe.idleTrace && idleAdmitted) {
            IdleRasterCopy now;IdleWeaponTrace *trace=nullptr;
            const bool current=currentIdleRaster(now,trace) && trace==probe.idleTrace && now==probe.idle.raster &&
                nativeUiDeviceCurrent(d) && probe.generation==graphicsResourceGeneration() && scopeGpuTransactionCurrent(d);
            if(current)probe.idle.factorBookend(now,true);
            probe.idleTrace->draw(probe.idle,SUCCEEDED(result),current);
        }
        if (probe.raster.pose.valid) {
            if (!admitted || FAILED(result) || probe.generation!=graphicsResourceGeneration()) retireScopeGeometry();
            else recordScopeGeometry(probe.raster,probe.cap,ScopeUvTransform{probe.uvRows},colorCandidate);
        }
    },[](bool aborted) noexcept { cleanup(aborted); });
    // A foreign unwind never reaches this point. Local failure/reentry/Release
    // retirement denies ready; the existing bank rechecks the captured owner.
    const auto completedRide=probe.rideAfter;
    const bool rideReady=probe.rideReady;
    const std::optional<RideDrawGpuCopy> completedGpu=rideReady && probe.rideGpu.copied?
        std::optional<RideDrawGpuCopy>{probe.rideGpu}:std::nullopt;
    probe.rideReady=false;
    if(rideReady)remote_render::recordRideMainDraw(completedRide,completedGpu?&*completedGpu:nullptr);
    return result;
}
HRESULT scopeGpuDraw(IDirect3DDevice9 *d,D3DPRIMITIVETYPE type,INT base,UINT minimum,UINT vertices,
                     UINT start,UINT primitives,uintptr_t caller,ScopeIndexedForward forward) noexcept {
    if(probe.busy)probe.rideReentered=true;
    scopeGpuOutput(d);
    if (!scopeGpuForwardingAllowed()) return D3DERR_INVALIDCALL;
    return withScopeDrawCoverage(scopeGpuRoutingCurrent(d),
        [&] { return probeScopeDraw(d,type,base,minimum,vertices,start,primitives,caller,forward); },
        [&] { return forward(d,type,base,minimum,vertices,start,primitives); });
}
} // namespace ss2vr::game
