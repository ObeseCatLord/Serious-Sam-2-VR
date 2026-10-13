#include "single_pass.hpp"
#include "common/shader_patch.hpp"
#include "game.hpp"
#include <MinHook.h>
#include <algorithm>
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
using SetPixelShaderF = HRESULT(WINAPI *)(IDirect3DDevice9 *, IDirect3DPixelShader9 *);
SetVertexShaderF oSetVertexShader = nullptr;
SetDeclarationF oSetDeclaration = nullptr;
SetFvfF oSetFvf = nullptr;
SetViewportF oSetViewport = nullptr;
SetScissorF oSetScissor = nullptr;
SetRenderStateF oSetRenderState = nullptr;
SetPixelShaderF oSetPixelShader = nullptr;
Originals bridge;
bool hooked = false, hookFailed = false;
// Instanced draws (one two-instance draw with a texkill per half) are off by
// default: in the lab, with 1.x pixel programs a waterfall showed through the
// cliff and smoke vanished, and with 2.0 programs a laser and sky streaks bled
// into the other half. Each draw is then issued once per eye half.
bool instancingEnabled = false;
const float *engineAdjusted = nullptr, *engineProjection = nullptr, *engineView = nullptr;

// Engine-side binding state, tracked on every call.
IDirect3DVertexShader9 *engineShader = nullptr;
IDirect3DVertexDeclaration9 *engineDeclaration = nullptr; // Null while an FVF is in use.
IDirect3DPixelShader9 *enginePixelShader = nullptr;
D3DVIEWPORT9 engineViewport{};
RECT engineScissor{};
bool scissorEnabled = false;
DWORD colorWrites = 0xf;

// Pass state.
bool active = false;
IDirect3DSurface9 *wideTarget = nullptr;
UINT eyeWidth = 0;
EyeChange changes[2];
Stats stats;

// Stereo variants, keyed by the engine's objects (one reference held on each key
// so a pointer cannot be reused for a different object while mapped).
std::unordered_map<IDirect3DVertexShader9 *, IDirect3DVertexShader9 *> shaderVariants;
std::unordered_map<IDirect3DVertexShader9 *, uint32_t> shaderTexcoords; // oT sets the original writes.
std::unordered_map<IDirect3DVertexDeclaration9 *, IDirect3DVertexDeclaration9 *> declarationVariants;

IDirect3DVertexShader9 *variantOf(IDirect3DDevice9 *d, IDirect3DVertexShader9 *s) noexcept {
    if (!s) return nullptr;
    if (const auto it = shaderVariants.find(s); it != shaderVariants.end()) return it->second;
    IDirect3DVertexShader9 *variant = nullptr;
    sp::VertexInfo info;
    UINT bytes = 0;
    if (SUCCEEDED(s->GetFunction(nullptr, &bytes)) && bytes >= 8 && bytes % 4 == 0) {
        std::vector<uint32_t> words(bytes / 4), patched;
        UINT got = bytes;
        if (SUCCEEDED(s->GetFunction(words.data(), &got)) &&
            sp::patchVertexShader(words.data(), words.size(), patched, &info) &&
            FAILED(d->CreateVertexShader(reinterpret_cast<const DWORD *>(patched.data()), &variant)))
            variant = nullptr;
    }
    s->AddRef();
    shaderVariants.emplace(s, variant);
    shaderTexcoords.emplace(s, info.texcoordsWritten);
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
// Pixel programs with a texkill on texcoord set k, per (program, k).
struct PixelInfo { uint32_t sets = 0, hash = 0; bool known = false, sm2 = false; IDirect3DPixelShader9 *variants[8]{}; bool tried[8]{}; std::vector<uint32_t> words; };
std::unordered_map<IDirect3DPixelShader9 *, PixelInfo> pixelInfos;
PixelInfo &pixelInfo(IDirect3DPixelShader9 *ps) noexcept {
    auto [it, fresh] = pixelInfos.try_emplace(ps);
    if (fresh && ps) {
        ps->AddRef();
        UINT bytes = 0;
        if (SUCCEEDED(ps->GetFunction(nullptr, &bytes)) && bytes >= 8 && bytes % 4 == 0) {
            it->second.words.resize(bytes / 4);
            UINT got = bytes;
            if (SUCCEEDED(ps->GetFunction(it->second.words.data(), &got)))
                it->second.known = sp::pixelTexcoords(it->second.words.data(), it->second.words.size(), it->second.sets);
            it->second.sm2 = it->second.known && it->second.words[0] == sp::PsVersion20;
            uint32_t h = 2166136261u;
            for (uint32_t w : it->second.words) { h ^= w; h *= 16777619u; }
            it->second.hash = h;
        }
    }
    return it->second;
}
IDirect3DPixelShader9 *pixelVariant(IDirect3DDevice9 *d, PixelInfo &info, unsigned k) noexcept {
    if (k >= 8) return nullptr;
    if (!info.tried[k]) {
        info.tried[k] = true;
        std::vector<uint32_t> patched;
        if (info.known && sp::patchPixelShader(info.words.data(), info.words.size(), k, patched) &&
            FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD *>(patched.data()), &info.variants[k])))
            info.variants[k] = nullptr;
    }
    return info.variants[k];
}
// Per-eye records for the current engine matrices, written to a ring on stream 15.
IDirect3DVertexBuffer9 *ring = nullptr;
constexpr UINT RingRecords = 2048, RecordBytes = sizeof(sp::EyeRecord);
UINT ringNext = 0, eyeOffset[2]{}, instancedOffset = 0;
float cachedAdjusted[16]{}, cachedProjection[16]{}, cachedView[12]{};
bool recordsValid = false;
bool recordsOrtho = false; // The current matrices are not a perspective view (fills, 2D).
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
    // Records 0/1: each eye in its own viewport (per-half issue). Records 2/3:
    // the same, mapped into the eye's half of the wide viewport (x'' = x/2 -/+ w/2),
    // with the kill row -x'' (left) / +x'' (right) for the pixel program.
    sp::EyeRecord records[4];
    recordsOrtho = false;
    for (unsigned i = 0; i < 2; ++i) {
        const Rewrite r = rewrite(state, changes[i]);
        recordsOrtho = recordsOrtho || !r.valid || r.identity;
        for (unsigned row = 0; row < 4; ++row)
            for (unsigned c = 0; c < 4; ++c)
                records[i].r[row][c] = r.valid && !r.identity ? float(r.clip[4 * row + c]) : (row == c ? 1.f : 0.f);
        auto &wide = records[2 + i];
        std::memcpy(wide.r, records[i].r, sizeof(wide.r));
        const float side = i ? .5f : -.5f;
        for (unsigned c = 0; c < 4; ++c) {
            wide.r[0][c] = .5f * records[i].r[0][c] + side * records[i].r[3][c];
            wide.kill[c] = i ? wide.r[0][c] : -wide.r[0][c];
        }
    }
    if (!ring && FAILED(d->CreateVertexBuffer(RingRecords * RecordBytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0,
                                              D3DPOOL_DEFAULT, &ring, nullptr)))
        return ring = nullptr, false;
    DWORD flags = D3DLOCK_NOOVERWRITE;
    if (ringNext + 4 > RingRecords) { ringNext = 0; flags = D3DLOCK_DISCARD; }
    void *mapped = nullptr;
    if (FAILED(ring->Lock(ringNext * RecordBytes, 4 * RecordBytes, &mapped, flags)) || !mapped) return false;
    std::memcpy(mapped, records, sizeof(records));
    ring->Unlock();
    eyeOffset[0] = ringNext * RecordBytes; eyeOffset[1] = (ringNext + 1) * RecordBytes;
    instancedOffset = (ringNext + 2) * RecordBytes;
    ringNext += 4;
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
// Device state as applied during the pass (lazily switched between draw modes).
bool appliedWide = false, appliedInstances = false;
IDirect3DPixelShader9 *appliedPixelShader = nullptr;
HRESULT WINAPI hSetViewport(IDirect3DDevice9 *d, const D3DVIEWPORT9 *v) {
    if (v) engineViewport = *v;
    appliedWide = false;
    return oSetViewport(d, v);
}
HRESULT WINAPI hSetPixelShader(IDirect3DDevice9 *d, IDirect3DPixelShader9 *s) {
    enginePixelShader = s;
    appliedPixelShader = s;
    return oSetPixelShader(d, s);
}
HRESULT WINAPI hSetScissor(IDirect3DDevice9 *d, const RECT *r) {
    if (r) engineScissor = *r;
    if (appliedWide) { oSetViewport(d, &engineViewport); appliedWide = false; } // Wide state re-applied on demand.
    return oSetScissor(d, r);
}
HRESULT WINAPI hSetRenderState(IDirect3DDevice9 *d, D3DRENDERSTATETYPE s, DWORD v) {
    if (s == D3DRS_COLORWRITEENABLE) colorWrites = v;
    if (s == D3DRS_SCISSORTESTENABLE) {
        scissorEnabled = v != 0;
        if (appliedWide) { oSetViewport(d, &engineViewport); oSetScissor(d, &engineScissor); appliedWide = false; }
    }
    return oSetRenderState(d, s, v);
}
template<class T> bool hook(void *address, void *detour, T &original) {
    if (MH_CreateHook(address, detour, reinterpret_cast<void **>(&original)) != MH_OK) return false;
    if (MH_EnableHook(address) != MH_OK) { MH_RemoveHook(address); original = nullptr; return false; }
    return true;
}
IDirect3DIndexBuffer9 *identityIndices = nullptr;
bool labInstancePlain() noexcept {
    static const bool off = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SP_PLAIN", v, 2) == 1 && v[0] == L'0'; }();
    return !off;
}
unsigned eligibleIndex = 0, lastEligible = 0, labLo = 0, labHi = 0, labDetail = ~0u;
std::vector<uint32_t> labPixelSeen, labPixelAllowed;
bool labPixelRestrict = false;
bool labRangeSet = false;
void perHalfState(IDirect3DDevice9 *d) noexcept {
    if (appliedWide) {
        oSetViewport(d, &engineViewport);
        if (scissorEnabled) oSetScissor(d, &engineScissor);
        appliedWide = false;
    }
    if (appliedInstances) { d->SetStreamSourceFreq(0, 1); appliedInstances = false; }
    if (appliedPixelShader != enginePixelShader) { oSetPixelShader(d, enginePixelShader); appliedPixelShader = enginePixelShader; }
}
// One two-instance draw over the wide viewport, when the pixel program can
// discard the other half (a texcoord set free in both programs).
D3DPRIMITIVETYPE currentPrimitive = D3DPT_TRIANGLELIST;
template<class Call> bool instanced(IDirect3DDevice9 *d, Call call, HRESULT &hr) noexcept {
    static const bool linesOff = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SP_LINES", v, 2) == 1 && v[0] == L'0'; }();
    if (linesOff && (currentPrimitive == D3DPT_LINELIST || currentPrimitive == D3DPT_LINESTRIP)) return false;
    static const bool disabled = [] {
        wchar_t v[2]{};
        if (GetEnvironmentVariableW(L"SS2VR_LAB_SP_INSTANCING", v, 2) == 1) return v[0] == L'0';
        return !instancingEnabled;
    }();
    static const bool orthoOff = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SP_ORTHO", v, 2) == 1 && v[0] == L'0'; }();
    if (disabled || !enginePixelShader || engineViewport.X || engineViewport.Y || engineViewport.Width != eyeWidth ||
        (orthoOff && recordsOrtho))
        return false;
    static const bool backgroundOff = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SP_BACKGROUND", v, 2) == 1 && v[0] == L'0'; }();
    if (backgroundOff && engineView[3] == 0 && engineView[7] == 0 && engineView[11] == 0) return false;
    // Occlusion-query boxes (colour writes off) drawn instanced gave wrong query
    // results (the engine then culled visible objects, e.g. the moon); they stay per half.
    if (!colorWrites) return false;

    if (scissorEnabled && (engineScissor.left > 0 || engineScissor.top > 0 || engineScissor.right < LONG(eyeWidth) ||
                           engineScissor.bottom < LONG(engineViewport.Height)))
        return false;
    auto &pixel = pixelInfo(enginePixelShader);
    if (!pixel.known) return false;
    static const int onlyVersion = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SP_PSVER", v, 2) == 1 ? int(v[0] - L'0') : 0; }();
    if (onlyVersion == 1 && pixel.sm2) return false;
    if (onlyVersion == 2 && !pixel.sm2) return false;
    const uint32_t written = shaderTexcoords.count(engineShader) ? shaderTexcoords[engineShader] : 0xffu;
    unsigned k = 0;
    const unsigned sets = pixel.sm2 ? 8 : 4;
    while (k < sets && ((pixel.sets | written) & (1u << k))) ++k;
    IDirect3DPixelShader9 *variant = k < sets ? pixelVariant(d, pixel, k) : nullptr;
    if (!variant) return false;
    if (labPixelRestrict && std::find(labPixelAllowed.begin(), labPixelAllowed.end(), pixel.hash) == labPixelAllowed.end())
        return false;
    if (std::find(labPixelSeen.begin(), labPixelSeen.end(), pixel.hash) == labPixelSeen.end() && labPixelSeen.size() < 64)
        labPixelSeen.push_back(pixel.hash);
    {
        static const bool logStages = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SP_STAGES", v, 2) == 1 && v[0] == L'1'; }();
        static unsigned stageLogs = 0;
        if (logStages && stageLogs < 400) {
            ++stageLogs;
            DWORD tci = 0, ttf = 0, blend = 0, src = 0, dst = 0;
            d->GetTextureStageState(k, D3DTSS_TEXCOORDINDEX, &tci); d->GetTextureStageState(k, D3DTSS_TEXTURETRANSFORMFLAGS, &ttf);
            d->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend); d->GetRenderState(D3DRS_SRCBLEND, &src); d->GetRenderState(D3DRS_DESTBLEND, &dst);
            log("Single pass lab stage ps=%08x ver=%08x k=%u tci=%lx ttf=%lx blend=%lu %lu/%lu", pixel.hash, pixel.words.empty() ? 0u : pixel.words[0], k, tci, ttf, blend, src, dst);
        }
    }
    static const uint32_t logHash = [] { wchar_t v[16]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SP_LOGPS", v, 16) ? uint32_t(wcstoul(v, nullptr, 16)) : 0u; }();
    static unsigned logged = 0;
    if (logHash && pixel.hash == logHash && logged < 40) {
        ++logged;
        DWORD colorWrite = 0, zEnable = 0, zWrite = 0, blend = 0, zFunc = 0, stencil = 0, alphaTest = 0, cull = 0;
        d->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite); d->GetRenderState(D3DRS_ZENABLE, &zEnable);
        d->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite); d->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
        d->GetRenderState(D3DRS_ZFUNC, &zFunc); d->GetRenderState(D3DRS_STENCILENABLE, &stencil);
        d->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest); d->GetRenderState(D3DRS_CULLMODE, &cull);
        DWORD tci = 0, ttf = 0;
        d->GetTextureStageState(k, D3DTSS_TEXCOORDINDEX, &tci); d->GetTextureStageState(k, D3DTSS_TEXTURETRANSFORMFLAGS, &ttf);
        log("Single pass lab ps stage %u texcoordindex=%lx transformflags=%lx", k, tci, ttf);
        log("Single pass lab ps draw prim=%u written=%x", unsigned(currentPrimitive), written);
        log("Single pass lab ps draw: vs=%p k=%u colorWrite=%lx z=%lu zwrite=%lu zfunc=%lu blend=%lu stencil=%lu alphaTest=%lu cull=%lu depth=%.2f-%.2f view0=%u",
            static_cast<void *>(engineShader), k, colorWrite, zEnable, zWrite, zFunc, blend, stencil, alphaTest, cull,
            engineViewport.MinZ, engineViewport.MaxZ, unsigned(engineView[3] == 0 && engineView[7] == 0 && engineView[11] == 0));
    }
    const unsigned eligible = eligibleIndex++;
    if (eligible == labDetail)
        log("Single pass lab draw %u: vs=%p ps=%p k=%u ps version=%08x sets=%x written=%x", eligible, static_cast<void *>(engineShader),
            static_cast<void *>(enginePixelShader), k, pixel.words.empty() ? 0u : pixel.words[0], pixel.sets, written);
    if (labRangeSet && (eligible < labLo || eligible >= labHi)) return false;
    if (!appliedWide) {
        D3DVIEWPORT9 wide = engineViewport;
        wide.Width = eyeWidth * 2;
        oSetViewport(d, &wide);
        if (scissorEnabled) { RECT r{0, 0, LONG(eyeWidth * 2), LONG(engineViewport.Height)}; oSetScissor(d, &r); }
        appliedWide = true;
    }
    if (!appliedInstances) { d->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | 2u); appliedInstances = true; }
    if (appliedPixelShader != variant) { oSetPixelShader(d, variant); appliedPixelShader = variant; }
    d->SetStreamSource(15, ring, instancedOffset, RecordBytes);
    hr = call(true);
    ++stats.instanced;
    return true;
}
// Issues `call` for the left half (engine viewport) and the right half (shifted).
template<class Call> HRESULT bothHalves(IDirect3DDevice9 *d, Call call, bool instancing = true) noexcept {
    const bool stereo = stereoBound();
    if (stereo && !writeRecords(d)) return call(false);
    HRESULT result = D3D_OK;
    if (stereo && instancing && instanced(d, call, result)) return result;
    perHalfState(d);
    if (!stereo) ++stats.unpatched;
    if (stereo) d->SetStreamSource(15, ring, eyeOffset[0], RecordBytes);
    HRESULT hr = call(false);
    D3DVIEWPORT9 right = engineViewport;
    right.X += eyeWidth;
    oSetViewport(d, &right);
    if (scissorEnabled) {
        RECT r = engineScissor; r.left += LONG(eyeWidth); r.right += LONG(eyeWidth);
        oSetScissor(d, &r);
    }
    if (stereo) d->SetStreamSource(15, ring, eyeOffset[1], RecordBytes);
    const HRESULT second = call(false);
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
             hook(v[107], reinterpret_cast<void *>(hSetPixelShader), oSetPixelShader) &&
             bridge.draw && bridge.drawIndexed && bridge.clear;
    hookFailed = !hooked;
    // Current bindings, in case the engine bound them before the hooks existed.
    d->GetVertexShader(&engineShader); if (engineShader) engineShader->Release();
    d->GetVertexDeclaration(&engineDeclaration); if (engineDeclaration) engineDeclaration->Release();
    d->GetPixelShader(&enginePixelShader); if (enginePixelShader) enginePixelShader->Release();
    d->GetViewport(&engineViewport); d->GetScissorRect(&engineScissor);
    DWORD scissor = 0; d->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor); scissorEnabled = scissor != 0;
    d->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrites);
    log("Single pass hooks installed=%u", unsigned(hooked));
    return hooked;
}
bool installed() noexcept { return hooked; }
void setInstancing(bool enabled) noexcept { instancingEnabled = enabled; }
void setEngineMatrices(const float *adjusted, const float *projection, const float *view) noexcept {
    engineAdjusted = adjusted; engineProjection = projection; engineView = view;
}
bool begin(IDirect3DDevice9 *d, IDirect3DSurface9 *target, UINT width, const EyeChange (&change)[2]) noexcept {
    if (!hooked || !engineAdjusted || !engineProjection || !engineView || !target || !change[0].valid || !change[1].valid)
        return false;
    active = true; wideTarget = target; eyeWidth = width;
    changes[0] = change[0]; changes[1] = change[1];
    recordsValid = false;
    eligibleIndex = 0;
    appliedWide = appliedInstances = false;
    appliedPixelShader = enginePixelShader;
    if (!identityIndices && SUCCEEDED(d->CreateIndexBuffer(65536 * 2, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT,
                                                            &identityIndices, nullptr))) {
        void *mapped = nullptr;
        if (SUCCEEDED(identityIndices->Lock(0, 0, &mapped, 0)) && mapped) {
            auto *indices = static_cast<uint16_t *>(mapped);
            for (unsigned k = 0; k < 65536; ++k) indices[k] = uint16_t(k);
            identityIndices->Unlock();
        } else { identityIndices->Release(); identityIndices = nullptr; }
    }
    d->SetStreamSourceFreq(15, D3DSTREAMSOURCE_INSTANCEDATA | 1u);
    bindVariants(d);
    ++stats.passes;
    return true;
}
void end(IDirect3DDevice9 *d) noexcept {
    if (!active) return;
    perHalfState(d);
    lastEligible = eligibleIndex;
    labRangeSet = false; labDetail = ~0u;
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
unsigned vertexCount(D3DPRIMITIVETYPE type, UINT primitives) noexcept {
    switch (type) {
    case D3DPT_TRIANGLELIST: return primitives * 3;
    case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return primitives + 2;
    case D3DPT_LINELIST: return primitives * 2;
    case D3DPT_LINESTRIP: return primitives + 1;
    default: return primitives;
    }
}
HRESULT draw(IDirect3DDevice9 *d, D3DPRIMITIVETYPE type, UINT start, UINT count) noexcept {
    ++stats.draws;
    currentPrimitive = type;
    const unsigned vertices = vertexCount(type, count);
    return bothHalves(d, [&](bool instances) -> HRESULT {
        // DXVK instances indexed draws only: draw through the identity index buffer.
        if (!instances) return bridge.draw(d, type, start, count);
        IDirect3DIndexBuffer9 *previous = nullptr;
        d->GetIndices(&previous);
        d->SetIndices(identityIndices);
        const HRESULT hr = bridge.drawIndexed(d, type, INT(start), 0, vertices, 0, count);
        d->SetIndices(previous);
        if (previous) previous->Release();
        return hr;
    }, identityIndices && vertices <= 65536 && labInstancePlain());
}
HRESULT drawIndexed(IDirect3DDevice9 *d, D3DPRIMITIVETYPE type, INT base, UINT minimum, UINT vertices, UINT start, UINT count) noexcept {
    ++stats.draws;
    currentPrimitive = type;
    return bothHalves(d, [&](bool) { return bridge.drawIndexed(d, type, base, minimum, vertices, start, count); });
}
HRESULT clear(IDirect3DDevice9 *d, DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) noexcept {
    perHalfState(d);
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
void labInstanceRange(unsigned lo, unsigned hi, unsigned detail) noexcept { labLo = lo; labHi = hi; labDetail = detail; labRangeSet = true; }
unsigned labEligibleCount() noexcept { return lastEligible; }
std::vector<uint32_t> labInstancedPixelShaders() noexcept { return labPixelSeen; }
void labAllowPixelShaders(const std::vector<uint32_t> &hashes, bool restrict) noexcept { labPixelAllowed = hashes; labPixelRestrict = restrict; }
Stats takeStats() noexcept { const Stats out = stats; stats = {}; return out; }
} // namespace ss2vr::game::single_pass
