#include "eye_replay.hpp"
#include "game.hpp"
#include <MinHook.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace ss2vr::game::replay {
using namespace ss2vr::replay;
namespace {
enum class Op : uint8_t {
    RenderState, TextureStage, Sampler, Texture, VertexShader, PixelShader, VSConstF, VSConstI, VSConstB,
    PSConstF, PSConstI, PSConstB, Stream, StreamFreq, Indices, Declaration, Fvf, Viewport, Scissor, ClipPlane,
    Transform, Material, Light, LightEnable, RenderTarget, Depth, Clear, Stretch, Fill, Draw, DrawIndexed,
    UpdateTexture
};
struct Cmd {
    Op op;
    uint32_t a = 0, b = 0, c = 0, d = 0, e = 0, f = 0;
    void *p = nullptr, *q = nullptr;
    uint32_t data = 0;          // arena word offset
    uint32_t state = ~0u;       // tagged matrix upload: ViewState index
    uint8_t indexSource = 0;    // DrawIndexed: 0 bound buffer, 2/4 copied 16/32-bit indices (index arena)
};
// Device method types (IDirect3DDevice9 vtable order).
using SetTransformF = HRESULT(WINAPI *)(IDirect3DDevice9 *, D3DTRANSFORMSTATETYPE, const D3DMATRIX *);
using SetViewportF = HRESULT(WINAPI *)(IDirect3DDevice9 *, const D3DVIEWPORT9 *);
using SetMaterialF = HRESULT(WINAPI *)(IDirect3DDevice9 *, const D3DMATERIAL9 *);
using SetLightF = HRESULT(WINAPI *)(IDirect3DDevice9 *, DWORD, const D3DLIGHT9 *);
using LightEnableF = HRESULT(WINAPI *)(IDirect3DDevice9 *, DWORD, BOOL);
using SetClipPlaneF = HRESULT(WINAPI *)(IDirect3DDevice9 *, DWORD, const float *);
using SetRenderStateF = HRESULT(WINAPI *)(IDirect3DDevice9 *, D3DRENDERSTATETYPE, DWORD);
using SetTextureF = HRESULT(WINAPI *)(IDirect3DDevice9 *, DWORD, IDirect3DBaseTexture9 *);
using SetStageF = HRESULT(WINAPI *)(IDirect3DDevice9 *, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD);
using SetSamplerF = HRESULT(WINAPI *)(IDirect3DDevice9 *, DWORD, D3DSAMPLERSTATETYPE, DWORD);
using SetScissorF = HRESULT(WINAPI *)(IDirect3DDevice9 *, const RECT *);
using SetDeclarationF = HRESULT(WINAPI *)(IDirect3DDevice9 *, IDirect3DVertexDeclaration9 *);
using SetFvfF = HRESULT(WINAPI *)(IDirect3DDevice9 *, DWORD);
using SetVertexShaderF = HRESULT(WINAPI *)(IDirect3DDevice9 *, IDirect3DVertexShader9 *);
using SetConstFF = HRESULT(WINAPI *)(IDirect3DDevice9 *, UINT, const float *, UINT);
using SetConstIF = HRESULT(WINAPI *)(IDirect3DDevice9 *, UINT, const int *, UINT);
using SetConstBF = HRESULT(WINAPI *)(IDirect3DDevice9 *, UINT, const BOOL *, UINT);
using SetStreamF = HRESULT(WINAPI *)(IDirect3DDevice9 *, UINT, IDirect3DVertexBuffer9 *, UINT, UINT);
using SetStreamFreqF = HRESULT(WINAPI *)(IDirect3DDevice9 *, UINT, UINT);
using SetIndicesF = HRESULT(WINAPI *)(IDirect3DDevice9 *, IDirect3DIndexBuffer9 *);
using SetPixelShaderF = HRESULT(WINAPI *)(IDirect3DDevice9 *, IDirect3DPixelShader9 *);
using UpdateTextureF = HRESULT(WINAPI *)(IDirect3DDevice9 *, IDirect3DBaseTexture9 *, IDirect3DBaseTexture9 *);
using LockVertexF = HRESULT(WINAPI *)(IDirect3DVertexBuffer9 *, UINT, UINT, void **, DWORD);
using LockIndexF = HRESULT(WINAPI *)(IDirect3DIndexBuffer9 *, UINT, UINT, void **, DWORD);
using UnlockIndexF = HRESULT(WINAPI *)(IDirect3DIndexBuffer9 *);

SetTransformF oSetTransform = nullptr; SetViewportF oSetViewport = nullptr; SetMaterialF oSetMaterial = nullptr;
SetLightF oSetLight = nullptr; LightEnableF oLightEnable = nullptr; SetClipPlaneF oSetClipPlane = nullptr;
SetRenderStateF oSetRenderState = nullptr; SetTextureF oSetTexture = nullptr; SetStageF oSetStage = nullptr;
SetSamplerF oSetSampler = nullptr; SetScissorF oSetScissor = nullptr; SetDeclarationF oSetDeclaration = nullptr;
SetFvfF oSetFvf = nullptr; SetVertexShaderF oSetVertexShader = nullptr; SetConstFF oSetVSConstF = nullptr;
SetConstIF oSetVSConstI = nullptr; SetConstBF oSetVSConstB = nullptr; SetStreamF oSetStream = nullptr;
SetStreamFreqF oSetStreamFreq = nullptr; SetIndicesF oSetIndices = nullptr; SetPixelShaderF oSetPixelShader = nullptr;
SetConstFF oSetPSConstF = nullptr; SetConstIF oSetPSConstI = nullptr; SetConstBF oSetPSConstB = nullptr;
UpdateTextureF oUpdateTexture = nullptr; LockVertexF oLockVertex = nullptr; LockIndexF oLockIndex = nullptr;
UnlockIndexF oUnlockIndex = nullptr;
// Dynamic index buffers are written through a CPU shadow (copied to the real
// mapping on unlock): the engine's index ring wraps several times per frame,
// so recorded draws copy their indices out of the shadow for the replay.
struct IndexShadow {
    IDirect3DIndexBuffer9 *buffer = nullptr;
    std::vector<uint8_t> data;
    uint8_t *mapped = nullptr;
    UINT lockOffset = 0, lockBytes = 0;
    bool dynamic = false, wide = false, locked = false;
};
std::vector<IndexShadow> shadows;
IDirect3DIndexBuffer9 *currentIndices = nullptr;
std::vector<uint8_t> indexArena[2];             // recorded 16/32-bit indices
IDirect3DIndexBuffer9 *replayIndices[2]{};
UINT replayIndexBytes[2]{};
IndexShadow *shadowOf(IDirect3DIndexBuffer9 *b) noexcept {
    if (!b) return nullptr;
    D3DINDEXBUFFER_DESC desc{};
    if (FAILED(b->GetDesc(&desc))) return nullptr;
    for (auto &s : shadows)
        if (s.buffer == b) {
            if (s.data.size() == desc.Size && s.wide == (desc.Format == D3DFMT_INDEX32)) return &s;
            s = {}; s.buffer = b; // Recycled pointer: a different buffer now.
            s.dynamic = desc.Usage & D3DUSAGE_DYNAMIC; s.wide = desc.Format == D3DFMT_INDEX32;
            if (s.dynamic) s.data.resize(desc.Size);
            return &s;
        }
    IndexShadow s; s.buffer = b; s.dynamic = desc.Usage & D3DUSAGE_DYNAMIC; s.wide = desc.Format == D3DFMT_INDEX32;
    if (s.dynamic) s.data.resize(desc.Size);
    shadows.push_back(std::move(s));
    return &shadows.back();
}
unsigned indexCount(D3DPRIMITIVETYPE type, UINT primitives) noexcept {
    switch (type) {
    case D3DPT_TRIANGLELIST: return primitives * 3;
    case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return primitives + 2;
    case D3DPT_LINELIST: return primitives * 2;
    case D3DPT_LINESTRIP: return primitives + 1;
    default: return primitives;
    }
}
Originals bridge;
bool installed = false, installFailed = false;

const float *engineAdjusted = nullptr, *engineProjection = nullptr, *engineView = nullptr;

// Recording state (render thread only).
bool active = false;        // Recording calls (left eye world render).
bool window = false;        // Tracking buffer locks (from begin until reset).
bool valid = false;
const char *reason = "none";
std::vector<Cmd> cmds;
std::vector<uint32_t> arena;
std::vector<ViewState> states;
std::vector<Rewrite> rewrites;
IDirect3DStateBlock9 *initial = nullptr;
IDirect3DSurface9 *recordedTarget = nullptr, *recordedDepth = nullptr;
IDirect3DIndexBuffer9 *initialIndices = nullptr;
unsigned tagType = 0;
bool tagPending = false;
Stats stats;
LARGE_INTEGER frequency{};

// Objects referenced by the recording: one reference each until reset.
constexpr unsigned SetSize = 8192;
IUnknown *referenced[SetSize]{};
unsigned referencedCount = 0;
inline unsigned slotOf(const void *p) noexcept { return (uint32_t(reinterpret_cast<uintptr_t>(p)) >> 4) * 2654435761u & (SetSize - 1); }
bool isReferenced(const void *p) noexcept {
    if (!p) return false;
    for (unsigned i = slotOf(p), n = 0; n < SetSize; ++n, i = (i + 1) & (SetSize - 1)) {
        if (!referenced[i]) return false;
        if (referenced[i] == p) return true;
    }
    return false;
}
void reference(IUnknown *p) noexcept {
    if (!p) return;
    for (unsigned i = slotOf(p), n = 0; n < SetSize; ++n, i = (i + 1) & (SetSize - 1)) {
        if (referenced[i] == p) return;
        if (!referenced[i]) {
            if (referencedCount >= SetSize * 3 / 4) { valid = false; reason = "too many objects"; return; }
            referenced[i] = p; p->AddRef(); ++referencedCount;
            return;
        }
    }
}
void releaseReferences() noexcept {
    for (auto *&p : referenced) if (p) { p->Release(); p = nullptr; }
    referencedCount = 0;
}
uint32_t store(const void *data, size_t bytes) {
    const uint32_t at = uint32_t(arena.size());
    arena.resize(arena.size() + (bytes + 3) / 4);
    if (bytes) std::memcpy(arena.data() + at, data, bytes);
    return at;
}
inline bool live() noexcept { return active && valid; }
Cmd &push(Op op) {
    cmds.push_back(Cmd{op});
    return cmds.back();
}
template<class T> bool hook(void *address, void *detour, T &original) {
    if (MH_CreateHook(address, detour, reinterpret_cast<void **>(&original)) != MH_OK) return false;
    if (MH_EnableHook(address) != MH_OK) { MH_RemoveHook(address); original = nullptr; return false; }
    return true;
}
void snapshotState(Cmd &c) {
    ViewState s;
    std::memcpy(s.adjusted.data(), engineAdjusted, sizeof(s.adjusted));
    std::memcpy(s.projection.data(), engineProjection, sizeof(s.projection));
    std::memcpy(s.view.data(), engineView, sizeof(s.view));
    if (states.empty() || !(states.back() == s)) states.push_back(s);
    c.state = uint32_t(states.size() - 1);
}

HRESULT WINAPI hSetTransform(IDirect3DDevice9 *d, D3DTRANSFORMSTATETYPE t, const D3DMATRIX *m) {
    if (live() && m) { auto &c = push(Op::Transform); c.a = t; c.data = store(m, sizeof(D3DMATRIX)); }
    return oSetTransform(d, t, m);
}
HRESULT WINAPI hSetViewport(IDirect3DDevice9 *d, const D3DVIEWPORT9 *v) {
    if (live() && v) { auto &c = push(Op::Viewport); c.data = store(v, sizeof(*v)); }
    return oSetViewport(d, v);
}
HRESULT WINAPI hSetMaterial(IDirect3DDevice9 *d, const D3DMATERIAL9 *m) {
    if (live() && m) { auto &c = push(Op::Material); c.data = store(m, sizeof(*m)); }
    return oSetMaterial(d, m);
}
HRESULT WINAPI hSetLight(IDirect3DDevice9 *d, DWORD i, const D3DLIGHT9 *l) {
    if (live() && l) { auto &c = push(Op::Light); c.a = i; c.data = store(l, sizeof(*l)); }
    return oSetLight(d, i, l);
}
HRESULT WINAPI hLightEnable(IDirect3DDevice9 *d, DWORD i, BOOL e) {
    if (live()) { auto &c = push(Op::LightEnable); c.a = i; c.b = uint32_t(e); }
    return oLightEnable(d, i, e);
}
HRESULT WINAPI hSetClipPlane(IDirect3DDevice9 *d, DWORD i, const float *plane) {
    if (live() && plane) { auto &c = push(Op::ClipPlane); c.a = i; c.data = store(plane, 16); snapshotState(c); }
    return oSetClipPlane(d, i, plane);
}
HRESULT WINAPI hSetRenderState(IDirect3DDevice9 *d, D3DRENDERSTATETYPE s, DWORD v) {
    if (live()) { auto &c = push(Op::RenderState); c.a = s; c.b = v; }
    return oSetRenderState(d, s, v);
}
HRESULT WINAPI hSetTexture(IDirect3DDevice9 *d, DWORD stage, IDirect3DBaseTexture9 *t) {
    if (live()) { auto &c = push(Op::Texture); c.a = stage; c.p = t; reference(t); }
    return oSetTexture(d, stage, t);
}
HRESULT WINAPI hSetStage(IDirect3DDevice9 *d, DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD v) {
    if (live()) { auto &c = push(Op::TextureStage); c.a = stage; c.b = t; c.c = v; }
    return oSetStage(d, stage, t, v);
}
HRESULT WINAPI hSetSampler(IDirect3DDevice9 *d, DWORD s, D3DSAMPLERSTATETYPE t, DWORD v) {
    if (live()) { auto &c = push(Op::Sampler); c.a = s; c.b = t; c.c = v; }
    return oSetSampler(d, s, t, v);
}
HRESULT WINAPI hSetScissor(IDirect3DDevice9 *d, const RECT *r) {
    if (live() && r) { auto &c = push(Op::Scissor); c.data = store(r, sizeof(*r)); }
    return oSetScissor(d, r);
}
HRESULT WINAPI hSetDeclaration(IDirect3DDevice9 *d, IDirect3DVertexDeclaration9 *v) {
    if (live()) { auto &c = push(Op::Declaration); c.p = v; reference(v); }
    return oSetDeclaration(d, v);
}
HRESULT WINAPI hSetFvf(IDirect3DDevice9 *d, DWORD f) {
    if (live()) { auto &c = push(Op::Fvf); c.a = f; }
    return oSetFvf(d, f);
}
HRESULT WINAPI hSetVertexShader(IDirect3DDevice9 *d, IDirect3DVertexShader9 *s) {
    if (live()) { auto &c = push(Op::VertexShader); c.p = s; reference(s); }
    return oSetVertexShader(d, s);
}
HRESULT WINAPI hSetVSConstF(IDirect3DDevice9 *d, UINT r, const float *v, UINT n) {
    if (live() && v) {
        auto &c = push(Op::VSConstF); c.a = r; c.b = n; c.data = store(v, size_t(n) * 16);
        if (tagPending && (n == 3 || n == 4)) { c.e = tagType; snapshotState(c); ++stats.tagged; }
        else if (r <= 1 && r + n >= 5) { c.e = 0; c.f = 1 - r; snapshotState(c); } // Shader block with c1..c4.
    }
    return oSetVSConstF(d, r, v, n);
}
HRESULT WINAPI hSetVSConstI(IDirect3DDevice9 *d, UINT r, const int *v, UINT n) {
    if (live() && v) { auto &c = push(Op::VSConstI); c.a = r; c.b = n; c.data = store(v, size_t(n) * 16); }
    return oSetVSConstI(d, r, v, n);
}
HRESULT WINAPI hSetVSConstB(IDirect3DDevice9 *d, UINT r, const BOOL *v, UINT n) {
    if (live() && v) { auto &c = push(Op::VSConstB); c.a = r; c.b = n; c.data = store(v, size_t(n) * 4); }
    return oSetVSConstB(d, r, v, n);
}
HRESULT WINAPI hSetStream(IDirect3DDevice9 *d, UINT s, IDirect3DVertexBuffer9 *b, UINT offset, UINT stride) {
    if (live()) { auto &c = push(Op::Stream); c.a = s; c.p = b; c.b = offset; c.c = stride; reference(b); }
    return oSetStream(d, s, b, offset, stride);
}
HRESULT WINAPI hSetStreamFreq(IDirect3DDevice9 *d, UINT s, UINT f) {
    if (live()) { auto &c = push(Op::StreamFreq); c.a = s; c.b = f; }
    return oSetStreamFreq(d, s, f);
}
HRESULT WINAPI hSetIndices(IDirect3DDevice9 *d, IDirect3DIndexBuffer9 *b) {
    currentIndices = b;
    if (live()) { auto &c = push(Op::Indices); c.p = b; reference(b); }
    return oSetIndices(d, b);
}
HRESULT WINAPI hSetPixelShader(IDirect3DDevice9 *d, IDirect3DPixelShader9 *s) {
    if (live()) { auto &c = push(Op::PixelShader); c.p = s; reference(s); }
    return oSetPixelShader(d, s);
}
HRESULT WINAPI hSetPSConstF(IDirect3DDevice9 *d, UINT r, const float *v, UINT n) {
    if (live() && v) { auto &c = push(Op::PSConstF); c.a = r; c.b = n; c.data = store(v, size_t(n) * 16); }
    return oSetPSConstF(d, r, v, n);
}
HRESULT WINAPI hSetPSConstI(IDirect3DDevice9 *d, UINT r, const int *v, UINT n) {
    if (live() && v) { auto &c = push(Op::PSConstI); c.a = r; c.b = n; c.data = store(v, size_t(n) * 16); }
    return oSetPSConstI(d, r, v, n);
}
HRESULT WINAPI hSetPSConstB(IDirect3DDevice9 *d, UINT r, const BOOL *v, UINT n) {
    if (live() && v) { auto &c = push(Op::PSConstB); c.a = r; c.b = n; c.data = store(v, size_t(n) * 4); }
    return oSetPSConstB(d, r, v, n);
}
HRESULT WINAPI hUpdateTexture(IDirect3DDevice9 *d, IDirect3DBaseTexture9 *s, IDirect3DBaseTexture9 *t) {
    if (live()) { auto &c = push(Op::UpdateTexture); c.p = s; c.q = t; reference(s); reference(t); }
    return oUpdateTexture(d, s, t);
}
// A lock that may overwrite or rename data an already recorded draw reads
// (anything but NOOVERWRITE/READONLY on a referenced buffer) spoils the replay.
HRESULT WINAPI hLockIndex(IDirect3DIndexBuffer9 *, UINT, UINT, void **, DWORD);
void countLock(unsigned kind, DWORD flags, bool referencedBuffer) noexcept;
HRESULT WINAPI hLockVertex(IDirect3DVertexBuffer9 *b, UINT offset, UINT size, void **data, DWORD flags) {
    countLock(0, flags, isReferenced(b));
    if (window && valid && !(flags & (D3DLOCK_NOOVERWRITE | D3DLOCK_READONLY)) && isReferenced(b)) {
        valid = false; reason = "vertex data overwritten";
    }
    return oLockVertex(b, offset, size, data, flags);
}
unsigned lockStats[2][4]{}; // [vertex/index][noOverwrite, discard, plain, readonly] during the window
unsigned lockReferenced[2]{};
void countLock(unsigned kind, DWORD flags, bool referencedBuffer) noexcept {
    if (!window) return;
    const unsigned category = flags & D3DLOCK_READONLY ? 3 : flags & D3DLOCK_NOOVERWRITE ? 0 : flags & D3DLOCK_DISCARD ? 1 : 2;
    ++lockStats[kind][category];
    if (referencedBuffer && category != 0 && category != 3) ++lockReferenced[kind];
}
HRESULT WINAPI hLockIndex(IDirect3DIndexBuffer9 *b, UINT offset, UINT size, void **data, DWORD flags) {
    countLock(1, flags, isReferenced(b));
    IndexShadow *shadow = shadowOf(b);
    const bool shadowed = shadow && shadow->dynamic && !shadow->locked && !(flags & D3DLOCK_READONLY);
    if (!shadowed && window && valid && !(flags & (D3DLOCK_NOOVERWRITE | D3DLOCK_READONLY)) && isReferenced(b)) {
        valid = false; reason = "index data overwritten";
    }
    const HRESULT hr = oLockIndex(b, offset, size, data, flags);
    if (SUCCEEDED(hr) && shadowed && data && *data) {
        const UINT bytes = size ? size : UINT(shadow->data.size()) - std::min<UINT>(offset, UINT(shadow->data.size()));
        if (uint64_t(offset) + bytes <= shadow->data.size()) {
            shadow->mapped = static_cast<uint8_t *>(*data);
            shadow->lockOffset = offset; shadow->lockBytes = bytes; shadow->locked = true;
            *data = shadow->data.data() + offset;
        }
    }
    return hr;
}
HRESULT WINAPI hUnlockIndex(IDirect3DIndexBuffer9 *b) {
    for (auto &s : shadows)
        if (s.buffer == b && s.locked) {
            std::memcpy(s.mapped, s.data.data() + s.lockOffset, s.lockBytes);
            s.locked = false; s.mapped = nullptr;
            break;
        }
    return oUnlockIndex(b);
}
} // namespace

bool install(IDirect3DDevice9 *d, const Originals &o) noexcept {
    bridge = o;
    if (installed || installFailed) return installed;
    QueryPerformanceFrequency(&frequency);
    auto v = *reinterpret_cast<void ***>(d);
    bool ok = hook(v[44], reinterpret_cast<void *>(hSetTransform), oSetTransform) &&
        hook(v[47], reinterpret_cast<void *>(hSetViewport), oSetViewport) &&
        hook(v[49], reinterpret_cast<void *>(hSetMaterial), oSetMaterial) &&
        hook(v[51], reinterpret_cast<void *>(hSetLight), oSetLight) &&
        hook(v[53], reinterpret_cast<void *>(hLightEnable), oLightEnable) &&
        hook(v[55], reinterpret_cast<void *>(hSetClipPlane), oSetClipPlane) &&
        hook(v[57], reinterpret_cast<void *>(hSetRenderState), oSetRenderState) &&
        hook(v[65], reinterpret_cast<void *>(hSetTexture), oSetTexture) &&
        hook(v[67], reinterpret_cast<void *>(hSetStage), oSetStage) &&
        hook(v[69], reinterpret_cast<void *>(hSetSampler), oSetSampler) &&
        hook(v[75], reinterpret_cast<void *>(hSetScissor), oSetScissor) &&
        hook(v[87], reinterpret_cast<void *>(hSetDeclaration), oSetDeclaration) &&
        hook(v[89], reinterpret_cast<void *>(hSetFvf), oSetFvf) &&
        hook(v[92], reinterpret_cast<void *>(hSetVertexShader), oSetVertexShader) &&
        hook(v[94], reinterpret_cast<void *>(hSetVSConstF), oSetVSConstF) &&
        hook(v[96], reinterpret_cast<void *>(hSetVSConstI), oSetVSConstI) &&
        hook(v[98], reinterpret_cast<void *>(hSetVSConstB), oSetVSConstB) &&
        hook(v[100], reinterpret_cast<void *>(hSetStream), oSetStream) &&
        hook(v[102], reinterpret_cast<void *>(hSetStreamFreq), oSetStreamFreq) &&
        hook(v[104], reinterpret_cast<void *>(hSetIndices), oSetIndices) &&
        hook(v[107], reinterpret_cast<void *>(hSetPixelShader), oSetPixelShader) &&
        hook(v[109], reinterpret_cast<void *>(hSetPSConstF), oSetPSConstF) &&
        hook(v[111], reinterpret_cast<void *>(hSetPSConstI), oSetPSConstI) &&
        hook(v[113], reinterpret_cast<void *>(hSetPSConstB), oSetPSConstB) &&
        hook(v[31], reinterpret_cast<void *>(hUpdateTexture), oUpdateTexture);
    IDirect3DVertexBuffer9 *vb = nullptr;
    IDirect3DIndexBuffer9 *ib = nullptr;
    ok = ok && SUCCEEDED(d->CreateVertexBuffer(64, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb, nullptr)) && vb &&
        SUCCEEDED(d->CreateIndexBuffer(64, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib, nullptr)) && ib &&
        hook((*reinterpret_cast<void ***>(vb))[11], reinterpret_cast<void *>(hLockVertex), oLockVertex) &&
        hook((*reinterpret_cast<void ***>(ib))[11], reinterpret_cast<void *>(hLockIndex), oLockIndex) &&
        hook((*reinterpret_cast<void ***>(ib))[12], reinterpret_cast<void *>(hUnlockIndex), oUnlockIndex);
    if (vb) vb->Release();
    if (ib) ib->Release();
    installed = ok && bridge.setRT && bridge.setDepth && bridge.clear && bridge.stretch && bridge.fill &&
                bridge.draw && bridge.drawIndexed;
    installFailed = !installed;
    log("Eye replay hooks installed=%u", unsigned(installed));
    return installed;
}
void setEngineMatrices(const float *adjusted, const float *projection, const float *view) noexcept {
    engineAdjusted = adjusted; engineProjection = projection; engineView = view;
}
bool recording() noexcept { return live(); }
bool begin(IDirect3DDevice9 *d, IDirect3DSurface9 *target, IDirect3DSurface9 *depth) noexcept {
    reset();
    if (!installed || !engineAdjusted || !engineProjection || !engineView || !target || !depth) return false;
    if (!initial && (FAILED(d->CreateStateBlock(D3DSBT_ALL, &initial)) || !initial)) { initial = nullptr; return false; }
    if (FAILED(initial->Capture())) return false;
    cmds.clear(); arena.clear(); states.clear(); rewrites.clear();
    indexArena[0].clear(); indexArena[1].clear();
    initialIndices = currentIndices; reference(initialIndices);
    recordedTarget = target; recordedDepth = depth;
    valid = true; reason = "none"; active = true; window = true; tagPending = false;
    return true;
}
void end() noexcept { active = false; tagPending = false; if (valid) ++stats.recorded; }
bool usable() noexcept { return valid && !active && window && !cmds.empty(); }
void invalidate(const char *why) noexcept { if (active && valid) { valid = false; reason = why; } }
void reset() noexcept {
    if (window && !valid) { ++stats.invalidated; stats.lastReason = reason; }
    active = window = false; valid = false; tagPending = false;
    releaseReferences();
    recordedTarget = recordedDepth = nullptr;
    initialIndices = nullptr;
}
void matrixBegin(unsigned type) noexcept { if (live()) { tagType = type; tagPending = true; } }
void matrixEnd() noexcept { tagPending = false; }
void recordRenderTarget(DWORD index, IDirect3DSurface9 *s) noexcept {
    if (!live()) return;
    auto &c = push(Op::RenderTarget); c.a = index; c.p = s; reference(s);
}
void recordDepth(IDirect3DSurface9 *s) noexcept {
    if (!live()) return;
    auto &c = push(Op::Depth); c.p = s; reference(s);
}
void recordClear(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) noexcept {
    if (!live()) return;
    auto &c = push(Op::Clear); c.a = rects ? count : 0; c.b = flags; c.c = color; std::memcpy(&c.d, &z, 4); c.e = stencil;
    c.data = rects ? store(rects, size_t(count) * sizeof(D3DRECT)) : ~0u;
}
void recordStretch(IDirect3DSurface9 *s, const RECT *sr, IDirect3DSurface9 *t, const RECT *tr, D3DTEXTUREFILTERTYPE f) noexcept {
    if (!live()) return;
    auto &c = push(Op::Stretch); c.p = s; c.q = t; c.a = f; reference(s); reference(t);
    c.b = sr ? store(sr, sizeof(RECT)) : ~0u; c.c = tr ? store(tr, sizeof(RECT)) : ~0u;
}
void recordFill(IDirect3DSurface9 *s, const RECT *r, D3DCOLOR color) noexcept {
    if (!live()) return;
    auto &c = push(Op::Fill); c.p = s; c.a = color; reference(s); c.b = r ? store(r, sizeof(RECT)) : ~0u;
}
void recordDraw(D3DPRIMITIVETYPE type, UINT start, UINT count) noexcept {
    if (!live()) return;
    auto &c = push(Op::Draw); c.a = type; c.b = start; c.c = count; ++stats.draws;
}
void recordDrawIndexed(D3DPRIMITIVETYPE type, INT base, UINT minimum, UINT vertices, UINT start, UINT count) noexcept {
    if (!live()) return;
    auto &c = push(Op::DrawIndexed); c.a = type; c.b = uint32_t(base); c.c = minimum; c.d = vertices; c.e = start; c.f = count;
    ++stats.draws;
    IndexShadow *shadow = nullptr;
    for (auto &s : shadows) if (s.buffer == currentIndices) { shadow = &s; break; }
    if (shadow && shadow->dynamic) {
        const unsigned width = shadow->wide ? 4 : 2, n = indexCount(type, count);
        if (uint64_t(start + n) * width > shadow->data.size()) { valid = false; reason = "index range outside buffer"; return; }
        auto &out = indexArena[shadow->wide ? 1 : 0];
        c.data = uint32_t(out.size() / width);
        out.insert(out.end(), shadow->data.begin() + size_t(start) * width, shadow->data.begin() + size_t(start + n) * width);
        c.indexSource = uint8_t(width);
    }
}
bool play(IDirect3DDevice9 *d, IDirect3DSurface9 *target, IDirect3DSurface9 *depth, const EyeChange &change,
          const ViewState &root) noexcept {
    if (!usable() || !initial || !change.valid) return false;
    LARGE_INTEGER start{}; QueryPerformanceCounter(&start);
    rewrites.resize(states.size());
    std::vector<bool> computed(states.size());
    const auto rewriteOf = [&](uint32_t index) -> const Rewrite & {
        if (!computed[index]) { rewrites[index] = rewrite(states[index], change); computed[index] = true; }
        return rewrites[index];
    };
    const auto map = [&](void *s) -> IDirect3DSurface9 * {
        if (s == recordedTarget) return target;
        if (s == recordedDepth) return depth;
        return static_cast<IDirect3DSurface9 *>(s);
    };
    const auto rect = [&](uint32_t at) -> const RECT * { return at == ~0u ? nullptr : reinterpret_cast<const RECT *>(arena.data() + at); };
    bool ok = SUCCEEDED(initial->Apply()) && SUCCEEDED(bridge.setRT(d, 0, target)) && SUCCEEDED(bridge.setDepth(d, depth));
    for (unsigned k = 0; k < 2 && ok; ++k) {
        const auto &source = indexArena[k];
        if (source.empty()) continue;
        if (replayIndexBytes[k] < source.size()) {
            if (replayIndices[k]) replayIndices[k]->Release();
            replayIndices[k] = nullptr;
            const UINT bytes = UINT(source.size() * 2);
            ok = SUCCEEDED(d->CreateIndexBuffer(bytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, k ? D3DFMT_INDEX32 : D3DFMT_INDEX16,
                                                D3DPOOL_DEFAULT, &replayIndices[k], nullptr)) && replayIndices[k];
            replayIndexBytes[k] = ok ? bytes : 0;
        }
        void *mapped = nullptr;
        ok = ok && SUCCEEDED(oLockIndex(replayIndices[k], 0, UINT(source.size()), &mapped, D3DLOCK_DISCARD)) && mapped;
        if (ok) { std::memcpy(mapped, source.data(), source.size()); ok = SUCCEEDED(oUnlockIndex(replayIndices[k])); }
    }
    IDirect3DIndexBuffer9 *logicalIndices = initialIndices, *boundIndices = initialIndices;
    // Fallback view states for blocks drawn after the root view returned (the
    // engine globals then no longer hold the eye view): the root view and its
    // rotation-only (background) variant.
    ViewState background = root;
    background.view[3] = background.view[7] = background.view[11] = 0;
    const Rewrite rootRewrite = rewrite(root, change), backgroundRewrite = rewrite(background, change);
    float rows[16];
    std::vector<float> block;
    unsigned blockRewrites = 0, blockMisses = 0, pendingMiss = 0, fallbacks[2]{}, pendingFallback = 0, drawKinds[8]{};
    unsigned frustumRewrites = 0;
    // A sizeable draw whose view matrix is not recognized would sit at the left
    // eye's position in the right eye (terrain showing objects through it): the
    // caller renders that frame's right eye natively instead.
    constexpr unsigned MissedPrimitiveLimit = 16;
    bool unrecognized = false;
    struct Recent { uint32_t prims, block, path; const void *vs; };
    Recent recent[48]{}; unsigned recentCount = 0;
    uint32_t lastBlock = 0, lastPath = 0; const void *lastVs = nullptr;
    char fell[512] = ""; int fellUsed = 0;
    char missed[512] = ""; int missedUsed = 0;
    for (const auto &c : cmds) {
        if (!ok) break;
        const uint32_t *w = arena.data() + (c.data == ~0u ? 0 : c.data);
        HRESULT hr = D3D_OK;
        switch (c.op) {
        case Op::RenderState: hr = oSetRenderState(d, D3DRENDERSTATETYPE(c.a), c.b); break;
        case Op::TextureStage: hr = oSetStage(d, c.a, D3DTEXTURESTAGESTATETYPE(c.b), c.c); break;
        case Op::Sampler: hr = oSetSampler(d, c.a, D3DSAMPLERSTATETYPE(c.b), c.c); break;
        case Op::Texture: hr = oSetTexture(d, c.a, static_cast<IDirect3DBaseTexture9 *>(c.p)); break;
        case Op::VertexShader: hr = oSetVertexShader(d, static_cast<IDirect3DVertexShader9 *>(c.p)); lastVs = c.p; break;
        case Op::PixelShader: hr = oSetPixelShader(d, static_cast<IDirect3DPixelShader9 *>(c.p)); break;
        case Op::VSConstF:
            if (c.state != ~0u && c.e) {
                rewriteUpload(c.e, rewriteOf(c.state), reinterpret_cast<const float *>(w), c.b, rows);
                hr = oSetVSConstF(d, c.a, rows, c.b);
            } else if (c.state != ~0u) {
                // Block upload: rewrite its c1..c4 rows when they are this view's MVP.
                block.assign(reinterpret_cast<const float *>(w), reinterpret_cast<const float *>(w) + size_t(c.b) * 4);
                float *mvp = block.data() + size_t(c.f) * 4;
                const auto &own = rewriteOf(c.state);
                unsigned path = 0;
                lastBlock = c.a * 256 + c.b;
                if (rewriteIfMvp(own, mvp, rows)) path = 1;
                else if (rewriteIfMvp(rootRewrite, mvp, rows)) path = 2;
                else if (rewriteIfMvp(backgroundRewrite, mvp, rows)) path = 3;
                else if (rewriteFrustumMvp(own, mvp, rows)) path = 4;
                if (path) {
                    std::memcpy(mvp, rows, sizeof(rows)); ++blockRewrites;
                    if (path == 4) ++frustumRewrites;
                    else if (path > 1) { ++fallbacks[path - 2]; pendingFallback = path * 65536 + c.a * 256 + c.b; }
                } else if (!own.identity) {
                    ++blockMisses; pendingMiss = c.a * 256 + c.b;
                    static unsigned missDumps = 0;
                    if (missDumps < 12 && own.modelValid) {
                        ++missDumps;
                        const M4 m = multiply(own.toModel, m4(mvp, 4));
                        log("Eye replay miss c%u:%u at %u rows %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f "
                            "model %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.5f %.5f %.5f %.5f",
                            c.a, c.b, c.f, mvp[0],mvp[1],mvp[2],mvp[3],mvp[4],mvp[5],mvp[6],mvp[7],mvp[8],mvp[9],mvp[10],mvp[11],
                            mvp[12],mvp[13],mvp[14],mvp[15], m[0],m[1],m[2],m[3],m[4],m[5],m[6],m[7],m[8],m[9],m[10],m[11],m[12],m[13],m[14],m[15]);
                        const auto &q = states[c.state].adjusted;
                        log("Eye replay miss state AP %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f",
                            q[0],q[1],q[2],q[3],q[4],q[5],q[6],q[7],q[8],q[9],q[10],q[11],q[12],q[13],q[14],q[15]);
                    }
                }
                lastPath = path ? path : (own.identity ? 8 : 9);
                hr = oSetVSConstF(d, c.a, block.data(), c.b);
            } else hr = oSetVSConstF(d, c.a, reinterpret_cast<const float *>(w), c.b);
            break;
        case Op::VSConstI: hr = oSetVSConstI(d, c.a, reinterpret_cast<const int *>(w), c.b); break;
        case Op::VSConstB: hr = oSetVSConstB(d, c.a, reinterpret_cast<const BOOL *>(w), c.b); break;
        case Op::PSConstF: hr = oSetPSConstF(d, c.a, reinterpret_cast<const float *>(w), c.b); break;
        case Op::PSConstI: hr = oSetPSConstI(d, c.a, reinterpret_cast<const int *>(w), c.b); break;
        case Op::PSConstB: hr = oSetPSConstB(d, c.a, reinterpret_cast<const BOOL *>(w), c.b); break;
        case Op::Stream: hr = oSetStream(d, c.a, static_cast<IDirect3DVertexBuffer9 *>(c.p), c.b, c.c); break;
        case Op::StreamFreq: hr = oSetStreamFreq(d, c.a, c.b); break;
        case Op::Indices:
            logicalIndices = static_cast<IDirect3DIndexBuffer9 *>(c.p);
            hr = oSetIndices(d, logicalIndices); boundIndices = logicalIndices;
            break;
        case Op::Declaration: hr = oSetDeclaration(d, static_cast<IDirect3DVertexDeclaration9 *>(c.p)); break;
        case Op::Fvf: hr = oSetFvf(d, c.a); break;
        case Op::Viewport: hr = oSetViewport(d, reinterpret_cast<const D3DVIEWPORT9 *>(w)); break;
        case Op::Scissor: hr = oSetScissor(d, reinterpret_cast<const RECT *>(w)); break;
        case Op::ClipPlane: {
            float plane[4];
            rewritePlane(rewriteOf(c.state), reinterpret_cast<const float *>(w), plane);
            hr = oSetClipPlane(d, c.a, plane);
            break;
        }
        case Op::Transform: hr = oSetTransform(d, D3DTRANSFORMSTATETYPE(c.a), reinterpret_cast<const D3DMATRIX *>(w)); break;
        case Op::Material: hr = oSetMaterial(d, reinterpret_cast<const D3DMATERIAL9 *>(w)); break;
        case Op::Light: hr = oSetLight(d, c.a, reinterpret_cast<const D3DLIGHT9 *>(w)); break;
        case Op::LightEnable: hr = oLightEnable(d, c.a, BOOL(c.b)); break;
        case Op::RenderTarget: hr = bridge.setRT(d, c.a, c.p ? map(c.p) : nullptr); break;
        case Op::Depth: hr = bridge.setDepth(d, c.p ? map(c.p) : nullptr); break;
        case Op::Clear: {
            float z; std::memcpy(&z, &c.d, 4);
            hr = bridge.clear(d, c.a, c.a ? reinterpret_cast<const D3DRECT *>(w) : nullptr, c.b, D3DCOLOR(c.c), z, c.e);
            break;
        }
        case Op::Stretch: hr = bridge.stretch(d, map(c.p), rect(c.b), map(c.q), rect(c.c), D3DTEXTUREFILTERTYPE(c.a)); break;
        case Op::Fill: hr = bridge.fill(d, map(c.p), rect(c.b), D3DCOLOR(c.a)); break;
        case Op::Draw: {
            // Lab bisection: SS2VR_LAB_REPLAY_SKIP=1 skips non-indexed draws, =2 line draws only.
            static const int labSkip = [] { wchar_t value[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_REPLAY_SKIP", value, 2) == 1 ? int(value[0] - L'0') : 0; }();
            const bool line = c.a == D3DPT_LINELIST || c.a == D3DPT_LINESTRIP;
            if (!(labSkip == 1 || (labSkip == 2 && line))) hr = bridge.draw(d, D3DPRIMITIVETYPE(c.a), c.b, c.c);
            ++drawKinds[std::min<uint32_t>(c.a, 7)];
        }
            if (pendingMiss && missedUsed < 440)
                missedUsed += snprintf(missed + missedUsed, sizeof(missed) - missedUsed, " c%u:%u->T%u/%u", pendingMiss >> 8, pendingMiss & 255, c.a, c.c);
            if (pendingMiss && c.c > MissedPrimitiveLimit) unrecognized = true;
            pendingMiss = 0;
            break;
        case Op::DrawIndexed: {
            IDirect3DIndexBuffer9 *wanted = c.indexSource ? replayIndices[c.indexSource == 4 ? 1 : 0] : logicalIndices;
            if (wanted != boundIndices) { hr = oSetIndices(d, wanted); boundIndices = wanted; }
            static const bool labSkipMisses = [] { wchar_t value[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_REPLAY_SKIP_MISSES", value, 2) == 1 && value[0] == L'1'; }();
            if (SUCCEEDED(hr) && !(labSkipMisses && pendingMiss))
                hr = bridge.drawIndexed(d, D3DPRIMITIVETYPE(c.a), INT(c.b), c.c, c.d, c.indexSource ? c.data : c.e, c.f);
            recent[recentCount++ % 48] = {c.f, lastBlock, lastPath, lastVs};
            lastPath = 0;
            if (pendingMiss && missedUsed < 440)
                missedUsed += snprintf(missed + missedUsed, sizeof(missed) - missedUsed, " c%u:%u->t%u/%u", pendingMiss >> 8, pendingMiss & 255, c.a, c.f);
            if (pendingMiss && c.f > MissedPrimitiveLimit) unrecognized = true;
            if (pendingFallback && fellUsed < 440)
                fellUsed += snprintf(fell + fellUsed, sizeof(fell) - fellUsed, " %s:c%u:%u->t%u/%u", pendingFallback >> 16 == 2 ? "root" : "sky",
                                     (pendingFallback >> 8) & 255, pendingFallback & 255, c.a, c.f);
            pendingMiss = 0; pendingFallback = 0;
            break;
        }
        case Op::UpdateTexture: hr = oUpdateTexture(d, static_cast<IDirect3DBaseTexture9 *>(c.p), static_cast<IDirect3DBaseTexture9 *>(c.q)); break;
        }
        ok = SUCCEEDED(hr) && !unrecognized;
    }
    if (boundIndices != logicalIndices) ok = SUCCEEDED(oSetIndices(d, logicalIndices)) && ok; // Engine's binding as it left it.
    static unsigned diagnostics = 0, replays = 0, loggedDraws = 0, changeLogs = 0;
    ++replays;
    const bool drawsChanged = (recentCount > loggedDraws + 8 || recentCount + 8 < loggedDraws) && changeLogs < 12;
    if (drawsChanged) { ++changeLogs; loggedDraws = recentCount; }
    if ((diagnostics < 3 || replays % 250 == 0 || drawsChanged) && !states.empty()) {
        ++diagnostics;
        unsigned identity = 0, rewritten = 0, invalid = 0;
        for (size_t i = 0; i < states.size(); ++i) {
            const auto &r = rewriteOf(uint32_t(i));
            if (!r.valid) ++invalid; else if (r.identity) ++identity; else ++rewritten;
        }
        unsigned byType[8]{}; // 0x46..0x4a, other
        for (const auto &c : cmds) if (c.op == Op::VSConstF && c.state != ~0u) ++byType[c.e >= 0x46 && c.e <= 0x4a ? c.e - 0x46 : 5];
        log("Eye replay tagged uploads MV=%u MVP=%u M=%u V=%u AP=%u other=%u", byType[0], byType[1], byType[2], byType[3], byType[4], byType[5]);
        unsigned keys[64]{}, counts[64]{}, tagged[64]{}, n = 0;
        for (const auto &c : cmds) {
            if (c.op != Op::VSConstF) continue;
            const unsigned key = (c.a << 8) | std::min<uint32_t>(c.b, 255);
            unsigned k = 0; while (k < n && keys[k] != key) ++k;
            if (k == n) { if (n == 64) continue; keys[n++] = key; }
            ++counts[k]; tagged[k] += c.state != ~0u;
        }
        char line[1024]; int used = snprintf(line, sizeof(line), "Eye replay VS constant uploads (reg:rows count tagged):");
        for (unsigned k = 0; k < n && used < 960; ++k)
            used += snprintf(line + used, sizeof(line) - used, " c%u:%u %u/%u", keys[k] >> 8, keys[k] & 255, counts[k], tagged[k]);
        log("%s", line);
        for (size_t k = 0; k < states.size() && k < 8; ++k) {
            const auto &q = states[k].projection; const auto &vv = states[k].view; const auto &rr = rewriteOf(uint32_t(k));
            log("Eye replay state %zu identity=%u P %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f Vt %.2f %.2f %.2f",
                k, unsigned(rr.identity), q[0],q[1],q[2],q[3],q[4],q[5],q[6],q[7],q[8],q[9],q[10],q[11],q[12],q[13],q[14],q[15], vv[3],vv[7],vv[11]);
        }
        const auto &p = states[0].projection, &a = states[0].adjusted;
        const auto &v = states[0].view;
        log("Eye replay block MVP rewrites=%u (frustum %u) misses=%u:%s", blockRewrites, frustumRewrites, blockMisses, missed);
        log("Eye replay fallback rewrites root=%u sky=%u:%s", fallbacks[0], fallbacks[1], fell);
        {
            char tail[1500]; int used = snprintf(tail, sizeof(tail), "Eye replay last indexed draws (prims block path vs):");
            for (unsigned k = 0; k < 48 && k < recentCount && used < 1450; ++k) {
                const auto &r = recent[(recentCount - std::min(recentCount, 48u) + k) % 48];
                used += snprintf(tail + used, sizeof(tail) - used, " %u/c%u:%u/%u/%04x", r.prims, r.block >> 8, r.block & 255, r.path,
                                 unsigned(reinterpret_cast<uintptr_t>(r.vs) & 0xffff));
            }
            log("%s", tail);
        }
        log("Eye replay plain draws points=%u lines=%u strips=%u triangles=%u tristrips=%u fans=%u", drawKinds[1], drawKinds[2], drawKinds[3], drawKinds[4], drawKinds[5], drawKinds[6]);
        log("Eye replay states=%zu rewritten=%u identity=%u invalid=%u change=%u P0 rows %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f",
            states.size(), rewritten, identity, invalid, unsigned(change.valid), p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],p[8],p[9],p[10],p[11],p[12],p[13],p[14],p[15]);
        log("Eye replay AP0 %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f V0 t=%.3f,%.3f,%.3f E t=%.4f,%.4f,%.4f",
            a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8],a[9],a[10],a[11],a[12],a[13],a[14],a[15], v[3],v[7],v[11],
            change.view[3],change.view[7],change.view[11]);
    }
    LARGE_INTEGER end{}; QueryPerformanceCounter(&end);
    stats.replayMs += 1000.0 * double(end.QuadPart - start.QuadPart) / double(frequency.QuadPart);
    stats.commands += unsigned(cmds.size());
    if (ok) ++stats.replayed; else { valid = false; reason = unrecognized ? "unrecognized view matrix" : "replay call failed"; }
    if (unrecognized) {
        static unsigned logged = 0;
        if (logged < 8) { ++logged; log("Eye replay abandoned: unrecognized view matrix:%s", missed); }
    }
    return ok;
}
Stats takeStats() noexcept {
    const Stats out = stats; stats = {};
    log("Eye replay locks vertex noOverwrite=%u discard=%u plain=%u read=%u referencedOverwrite=%u | index noOverwrite=%u discard=%u plain=%u read=%u referencedOverwrite=%u",
        lockStats[0][0], lockStats[0][1], lockStats[0][2], lockStats[0][3], lockReferenced[0],
        lockStats[1][0], lockStats[1][1], lockStats[1][2], lockStats[1][3], lockReferenced[1]);
    std::memset(lockStats, 0, sizeof(lockStats)); std::memset(lockReferenced, 0, sizeof(lockReferenced));
    return out;
}
} // namespace ss2vr::game::replay
