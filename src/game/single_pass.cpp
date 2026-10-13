#include "single_pass.hpp"
#include "common/shader_patch.hpp"
#include "game.hpp"
#include <MinHook.h>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace ss2vr::game::single_pass {
using namespace ss2vr::replay;
namespace sp = ss2vr::shader_patch;
namespace {
using SetVertexShaderF = HRESULT(WINAPI *)(IDirect3DDevice9 *, IDirect3DVertexShader9 *);
using SetDeclarationF = HRESULT(WINAPI *)(IDirect3DDevice9 *, IDirect3DVertexDeclaration9 *);
using SetFvfF = HRESULT(WINAPI *)(IDirect3DDevice9 *, DWORD);
using SetViewportF = HRESULT(WINAPI *)(IDirect3DDevice9 *, const D3DVIEWPORT9 *);
using SetScissorF = HRESULT(WINAPI *)(IDirect3DDevice9 *, const RECT *);
using SetRenderStateF = HRESULT(WINAPI *)(IDirect3DDevice9 *, D3DRENDERSTATETYPE, DWORD);
SetVertexShaderF oSetVertexShader = nullptr;
SetDeclarationF oSetDeclaration = nullptr;
SetFvfF oSetFvf = nullptr;
SetViewportF oSetViewport = nullptr;
SetScissorF oSetScissor = nullptr;
SetRenderStateF oSetRenderState = nullptr;
Originals bridge;
bool hooked = false, hookFailed = false;
const float *engineAdjusted = nullptr, *engineProjection = nullptr, *engineView = nullptr;

// Engine-side binding state, tracked on every call.
IDirect3DVertexShader9 *engineShader = nullptr;
IDirect3DVertexDeclaration9 *engineDeclaration = nullptr; // Null while an FVF is in use.
D3DVIEWPORT9 engineViewport{};
RECT engineScissor{};
bool scissorEnabled = false;

// Pass state.
bool active = false;
IDirect3DSurface9 *wideTarget = nullptr;
UINT eyeWidth = 0;
EyeChange changes[2];
Stats stats;

// Stereo variants, keyed by the engine's objects (one reference held on each key
// so a pointer cannot be reused for a different object while mapped).
std::unordered_map<IDirect3DVertexShader9 *, IDirect3DVertexShader9 *> shaderVariants;
std::unordered_map<IDirect3DVertexDeclaration9 *, IDirect3DVertexDeclaration9 *> declarationVariants;

IDirect3DVertexShader9 *variantOf(IDirect3DDevice9 *d, IDirect3DVertexShader9 *s) noexcept {
    if (!s) return nullptr;
    if (const auto it = shaderVariants.find(s); it != shaderVariants.end()) return it->second;
    IDirect3DVertexShader9 *variant = nullptr;
    UINT bytes = 0;
    if (SUCCEEDED(s->GetFunction(nullptr, &bytes)) && bytes >= 8 && bytes % 4 == 0) {
        std::vector<uint32_t> words(bytes / 4), patched;
        UINT got = bytes;
        if (SUCCEEDED(s->GetFunction(words.data(), &got)) &&
            sp::patchVertexShader(words.data(), words.size(), patched) &&
            FAILED(d->CreateVertexShader(reinterpret_cast<const DWORD *>(patched.data()), &variant)))
            variant = nullptr;
    }
    s->AddRef();
    shaderVariants.emplace(s, variant);
    if (variant) ++stats.variants; else ++stats.failedVariants;
    static unsigned logged = 0;
    if (!variant && logged < 16) { ++logged; log("Single pass: vertex program %p not patched (%u bytes)", static_cast<void *>(s), bytes); }
    return variant;
}
IDirect3DVertexDeclaration9 *variantOf(IDirect3DDevice9 *d, IDirect3DVertexDeclaration9 *v) noexcept {
    if (!v) return nullptr;
    if (const auto it = declarationVariants.find(v); it != declarationVariants.end()) return it->second;
    IDirect3DVertexDeclaration9 *variant = nullptr;
    D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH + 1]{};
    UINT count = MAXD3DDECLLENGTH + 1;
    if (SUCCEEDED(v->GetDeclaration(elements, &count)) && count >= 1) {
        std::vector<D3DVERTEXELEMENT9> out;
        bool conflict = false;
        for (UINT i = 0; i < count && elements[i].Stream != 0xff; ++i) {
            const auto &e = elements[i];
            conflict = conflict || e.Stream == 15 ||
                (e.Usage == D3DDECLUSAGE_TEXCOORD && e.UsageIndex >= sp::FirstStereoInput);
            out.push_back(e);
        }
        if (!conflict && out.size() + sp::StereoInputCount < MAXD3DDECLLENGTH) {
            for (unsigned k = 0; k < sp::StereoInputCount; ++k)
                out.push_back({15, WORD(k * 16), D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD,
                               BYTE(sp::FirstStereoInput + k)});
            out.push_back(D3DDECL_END());
            if (FAILED(d->CreateVertexDeclaration(out.data(), &variant))) variant = nullptr;
        }
    }
    v->AddRef();
    declarationVariants.emplace(v, variant);
    return variant;
}
// Per-eye records for the current engine matrices, written to a ring on stream 15.
IDirect3DVertexBuffer9 *ring = nullptr;
constexpr UINT RingRecords = 2048, RecordBytes = sizeof(sp::EyeRecord);
UINT ringNext = 0, eyeOffset[2]{};
float cachedAdjusted[16]{}, cachedProjection[16]{}, cachedView[12]{};
bool recordsValid = false;
bool writeRecords(IDirect3DDevice9 *d) noexcept {
    if (recordsValid && !std::memcmp(cachedAdjusted, engineAdjusted, sizeof(cachedAdjusted)) &&
        !std::memcmp(cachedProjection, engineProjection, sizeof(cachedProjection)) &&
        !std::memcmp(cachedView, engineView, sizeof(cachedView)))
        return true;
    std::memcpy(cachedAdjusted, engineAdjusted, sizeof(cachedAdjusted));
    std::memcpy(cachedProjection, engineProjection, sizeof(cachedProjection));
    std::memcpy(cachedView, engineView, sizeof(cachedView));
    ViewState state;
    std::memcpy(state.adjusted.data(), engineAdjusted, sizeof(cachedAdjusted));
    std::memcpy(state.projection.data(), engineProjection, sizeof(cachedProjection));
    std::memcpy(state.view.data(), engineView, sizeof(cachedView));
    sp::EyeRecord records[2];
    for (unsigned i = 0; i < 2; ++i) {
        const Rewrite r = rewrite(state, changes[i]);
        for (unsigned row = 0; row < 4; ++row)
            for (unsigned c = 0; c < 4; ++c)
                records[i].r[row][c] = r.valid && !r.identity ? float(r.clip[4 * row + c]) : (row == c ? 1.f : 0.f);
    }
    if (!ring && FAILED(d->CreateVertexBuffer(RingRecords * RecordBytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0,
                                              D3DPOOL_DEFAULT, &ring, nullptr)))
        return ring = nullptr, false;
    DWORD flags = D3DLOCK_NOOVERWRITE;
    if (ringNext + 2 > RingRecords) { ringNext = 0; flags = D3DLOCK_DISCARD; }
    void *mapped = nullptr;
    if (FAILED(ring->Lock(ringNext * RecordBytes, 2 * RecordBytes, &mapped, flags)) || !mapped) return false;
    std::memcpy(mapped, records, sizeof(records));
    ring->Unlock();
    eyeOffset[0] = ringNext * RecordBytes; eyeOffset[1] = (ringNext + 1) * RecordBytes;
    ringNext += 2;
    recordsValid = true;
    ++stats.stateChanges;
    return true;
}
void bindVariants(IDirect3DDevice9 *d) noexcept {
    IDirect3DVertexShader9 *vs = engineShader ? variantOf(d, engineShader) : nullptr;
    IDirect3DVertexDeclaration9 *decl = engineDeclaration ? variantOf(d, engineDeclaration) : nullptr;
    // A patched program needs the stereo declaration; otherwise both stay original.
    if (!vs || !decl) { vs = engineShader; decl = engineDeclaration; }
    if (decl) oSetDeclaration(d, decl);
    oSetVertexShader(d, vs);
}
bool stereoBound() noexcept {
    return engineShader && engineDeclaration && shaderVariants.count(engineShader) && shaderVariants[engineShader] &&
           declarationVariants.count(engineDeclaration) && declarationVariants[engineDeclaration];
}

HRESULT WINAPI hSetVertexShader(IDirect3DDevice9 *d, IDirect3DVertexShader9 *s) {
    engineShader = s;
    if (!active) return oSetVertexShader(d, s);
    IDirect3DVertexShader9 *variant = variantOf(d, s);
    IDirect3DVertexDeclaration9 *decl = engineDeclaration ? variantOf(d, engineDeclaration) : nullptr;
    if (variant && decl) { oSetDeclaration(d, decl); return oSetVertexShader(d, variant); }
    if (engineDeclaration) oSetDeclaration(d, engineDeclaration);
    return oSetVertexShader(d, s);
}
HRESULT WINAPI hSetDeclaration(IDirect3DDevice9 *d, IDirect3DVertexDeclaration9 *v) {
    engineDeclaration = v;
    if (!active) return oSetDeclaration(d, v);
    const HRESULT hr = oSetDeclaration(d, v);
    bindVariants(d);
    return hr;
}
HRESULT WINAPI hSetFvf(IDirect3DDevice9 *d, DWORD f) {
    if (f) engineDeclaration = nullptr;
    const HRESULT hr = oSetFvf(d, f);
    if (active && f) oSetVertexShader(d, engineShader); // FVF draws keep the original program.
    return hr;
}
HRESULT WINAPI hSetViewport(IDirect3DDevice9 *d, const D3DVIEWPORT9 *v) {
    if (v) engineViewport = *v;
    return oSetViewport(d, v);
}
HRESULT WINAPI hSetScissor(IDirect3DDevice9 *d, const RECT *r) {
    if (r) engineScissor = *r;
    return oSetScissor(d, r);
}
HRESULT WINAPI hSetRenderState(IDirect3DDevice9 *d, D3DRENDERSTATETYPE s, DWORD v) {
    if (s == D3DRS_SCISSORTESTENABLE) scissorEnabled = v != 0;
    return oSetRenderState(d, s, v);
}
template<class T> bool hook(void *address, void *detour, T &original) {
    if (MH_CreateHook(address, detour, reinterpret_cast<void **>(&original)) != MH_OK) return false;
    if (MH_EnableHook(address) != MH_OK) { MH_RemoveHook(address); original = nullptr; return false; }
    return true;
}
// Issues `call` for the left half (engine viewport) and the right half (shifted).
template<class Call> HRESULT bothHalves(IDirect3DDevice9 *d, Call call) noexcept {
    const bool stereo = stereoBound();
    if (stereo && !writeRecords(d)) return call();
    if (!stereo) ++stats.unpatched;
    if (stereo) d->SetStreamSource(15, ring, eyeOffset[0], RecordBytes);
    HRESULT hr = call();
    D3DVIEWPORT9 right = engineViewport;
    right.X += eyeWidth;
    oSetViewport(d, &right);
    if (scissorEnabled) {
        RECT r = engineScissor; r.left += LONG(eyeWidth); r.right += LONG(eyeWidth);
        oSetScissor(d, &r);
    }
    if (stereo) d->SetStreamSource(15, ring, eyeOffset[1], RecordBytes);
    const HRESULT second = call();
    oSetViewport(d, &engineViewport);
    if (scissorEnabled) oSetScissor(d, &engineScissor);
    return FAILED(hr) ? hr : second;
}
} // namespace

bool install(IDirect3DDevice9 *d, const Originals &o) noexcept {
    bridge = o;
    if (hooked || hookFailed) return hooked;
    auto v = *reinterpret_cast<void ***>(d);
    hooked = hook(v[92], reinterpret_cast<void *>(hSetVertexShader), oSetVertexShader) &&
             hook(v[87], reinterpret_cast<void *>(hSetDeclaration), oSetDeclaration) &&
             hook(v[89], reinterpret_cast<void *>(hSetFvf), oSetFvf) &&
             hook(v[47], reinterpret_cast<void *>(hSetViewport), oSetViewport) &&
             hook(v[75], reinterpret_cast<void *>(hSetScissor), oSetScissor) &&
             hook(v[57], reinterpret_cast<void *>(hSetRenderState), oSetRenderState) &&
             bridge.draw && bridge.drawIndexed && bridge.clear;
    hookFailed = !hooked;
    // Current bindings, in case the engine bound them before the hooks existed.
    d->GetVertexShader(&engineShader); if (engineShader) engineShader->Release();
    d->GetVertexDeclaration(&engineDeclaration); if (engineDeclaration) engineDeclaration->Release();
    d->GetViewport(&engineViewport); d->GetScissorRect(&engineScissor);
    DWORD scissor = 0; d->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor); scissorEnabled = scissor != 0;
    log("Single pass hooks installed=%u", unsigned(hooked));
    return hooked;
}
bool installed() noexcept { return hooked; }
void setEngineMatrices(const float *adjusted, const float *projection, const float *view) noexcept {
    engineAdjusted = adjusted; engineProjection = projection; engineView = view;
}
bool begin(IDirect3DDevice9 *d, IDirect3DSurface9 *target, UINT width, const EyeChange (&change)[2]) noexcept {
    if (!hooked || !engineAdjusted || !engineProjection || !engineView || !target || !change[0].valid || !change[1].valid)
        return false;
    active = true; wideTarget = target; eyeWidth = width;
    changes[0] = change[0]; changes[1] = change[1];
    recordsValid = false;
    d->SetStreamSourceFreq(15, D3DSTREAMSOURCE_INSTANCEDATA | 1u);
    bindVariants(d);
    ++stats.passes;
    return true;
}
void end(IDirect3DDevice9 *d) noexcept {
    if (!active) return;
    active = false; wideTarget = nullptr;
    d->SetStreamSource(15, nullptr, 0, 0);
    d->SetStreamSourceFreq(15, 1);
    if (engineDeclaration) oSetDeclaration(d, engineDeclaration);
    oSetVertexShader(d, engineShader);
    oSetViewport(d, &engineViewport);
}
bool drawing(IDirect3DDevice9 *d) noexcept {
    if (!active || !wideTarget) return false;
    IDirect3DSurface9 *rt = nullptr;
    if (FAILED(d->GetRenderTarget(0, &rt)) || !rt) return false;
    const bool wide = rt == wideTarget;
    rt->Release();
    return wide;
}
HRESULT draw(IDirect3DDevice9 *d, D3DPRIMITIVETYPE type, UINT start, UINT count) noexcept {
    ++stats.draws;
    return bothHalves(d, [&] { return bridge.draw(d, type, start, count); });
}
HRESULT drawIndexed(IDirect3DDevice9 *d, D3DPRIMITIVETYPE type, INT base, UINT minimum, UINT vertices, UINT start, UINT count) noexcept {
    ++stats.draws;
    return bothHalves(d, [&] { return bridge.drawIndexed(d, type, base, minimum, vertices, start, count); });
}
HRESULT clear(IDirect3DDevice9 *d, DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) noexcept {
    HRESULT hr = bridge.clear(d, count, rects, flags, color, z, stencil);
    D3DVIEWPORT9 right = engineViewport;
    right.X += eyeWidth;
    oSetViewport(d, &right);
    std::vector<D3DRECT> shifted(rects ? count : 0);
    for (DWORD i = 0; rects && i < count; ++i) { shifted[i] = rects[i]; shifted[i].x1 += LONG(eyeWidth); shifted[i].x2 += LONG(eyeWidth); }
    const HRESULT second = bridge.clear(d, count, rects ? shifted.data() : nullptr, flags, color, z, stencil);
    oSetViewport(d, &engineViewport);
    return FAILED(hr) ? hr : second;
}
Stats takeStats() noexcept { const Stats out = stats; stats = {}; return out; }
} // namespace ss2vr::game::single_pass
