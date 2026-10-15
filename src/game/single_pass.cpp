#include "single_pass.hpp"
#include "common/shader_patch.hpp"
#include "common/water_reflection.hpp"
#include "game.hpp"
#include <MinHook.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
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
using SetClipPlaneF = HRESULT(WINAPI *)(IDirect3DDevice9 *, DWORD, const float *);
using GetClipPlaneF = HRESULT(WINAPI *)(IDirect3DDevice9 *, DWORD, float *);
using SetRenderStateF = HRESULT(WINAPI *)(IDirect3DDevice9 *, D3DRENDERSTATETYPE, DWORD);
using SetPixelShaderF = HRESULT(WINAPI *)(IDirect3DDevice9 *, IDirect3DPixelShader9 *);
SetVertexShaderF oSetVertexShader = nullptr;
SetDeclarationF oSetDeclaration = nullptr;
SetFvfF oSetFvf = nullptr;
SetViewportF oSetViewport = nullptr;
SetScissorF oSetScissor = nullptr;
SetClipPlaneF oSetClipPlane = nullptr;
GetClipPlaneF oGetClipPlane = nullptr;
SetRenderStateF oSetRenderState = nullptr;
SetPixelShaderF oSetPixelShader = nullptr;
Originals bridge;
bool hooked = false, hookFailed = false;
// Instanced draws (one two-instance draw with a texkill per half) match the
// per-half issue pixel for pixel in the lab (same-frame mirror, SS2VR_LAB_SP_MIRROR=1)
// but saved only ~0.1 ms of render-thread time, so they stay opt-in
// ([Render] SinglePassInstancing). Earlier artifacts (sky, smoke, waterfall,
// seam column) came from the engine reading back the wide viewport, the seam
// pixel centre and the eye test reaching texcoord sets pixel programs read.
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
// User clip planes: D3D9 applies them to the vertex program's clip-space output,
// and the engine sets them for the centre camera's clip space; each eye region
// gets them transformed by the inverse of its clip-space map.
float enginePlanes[6][4]{};
DWORD clipPlanes = 0;           // D3DRS_CLIPPLANEENABLE as set by the engine.
bool planesApplied = false;     // The device holds transformed planes.

// Pass state.
bool active = false;
IDirect3DSurface9 *wideTarget = nullptr;
UINT eyeWidth = 0, eyeHeight = 0;
Layout layout;      // Regions of the target the eyes are drawn into.
bool wideLayout = true; // The plain double-wide layout (instancing possible).
EyeChange changes[2];
Stats stats;

// Stereo variants, keyed by the engine's objects (one reference held on each key
// so a pointer cannot be reused for a different object while mapped).
std::unordered_map<IDirect3DVertexShader9 *, IDirect3DVertexShader9 *> shaderVariants;
std::unordered_map<IDirect3DVertexShader9 *, uint32_t> shaderTexcoords; // oT sets the original writes.
// Water programs whose variant looks the reflection up by each eye's reflected
// view direction (water_reflection.hpp): eye position in c30, dome scale in c31,
// set for each half around their draws.
constexpr unsigned WaterConstant = 30;
std::unordered_map<IDirect3DVertexShader9 *, bool> shaderEyeClip;
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
            sp::patchVertexShader(words.data(), words.size(), patched, &info, WaterConstant) &&
            FAILED(d->CreateVertexShader(reinterpret_cast<const DWORD *>(patched.data()), &variant)))
            variant = nullptr;
    }
    s->AddRef();
    shaderVariants.emplace(s, variant);
    shaderTexcoords.emplace(s, info.texcoordsWritten);
    shaderEyeClip.emplace(s, variant && info.eyeClip);
    if (variant && info.eyeClip) log("Single pass: water vertex program %p looks its reflection up per eye", static_cast<void *>(s));
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
UINT ringNext = 0, eyeOffset[2]{}, instancedOffset = 0, regionOffset = 0;
float cachedAdjusted[16]{}, cachedProjection[16]{}, cachedView[12]{};
D3DVIEWPORT9 cachedViewport{};
// Eye pixel x/y to the region's target pixels.
float regionX(const Region &g, float x) noexcept {
    const float ndc = g.sx * (2.f * x / float(eyeWidth) - 1.f) + g.ox;
    return float(g.rect.left) + (ndc + 1.f) * .5f * float(g.rect.right - g.rect.left);
}
float regionY(const Region &g, float y) noexcept {
    const float ndc = g.sy * (1.f - 2.f * y / float(eyeHeight)) + g.oy;
    return float(g.rect.top) + (1.f - ndc) * .5f * float(g.rect.bottom - g.rect.top);
}
// An eye pixel rectangle in the region, clipped to it.
RECT regionRect(const Region &g, LONG left, LONG top, LONG right, LONG bottom) noexcept {
    RECT r{LONG(std::lround(regionX(g, float(left)))), LONG(std::lround(regionY(g, float(top)))),
           LONG(std::lround(regionX(g, float(right)))), LONG(std::lround(regionY(g, float(bottom))))};
    r.left = std::clamp(r.left, g.rect.left, g.rect.right); r.right = std::clamp(r.right, g.rect.left, g.rect.right);
    r.top = std::clamp(r.top, g.rect.top, g.rect.bottom); r.bottom = std::clamp(r.bottom, g.rect.top, g.rect.bottom);
    return r;
}
double inverseMap[2 + MaxRegions][16]{};
bool inverseValid[2 + MaxRegions]{};
bool invert4(const double *m, double *out) noexcept {
    double inv[16];
    inv[0] = m[5]*m[10]*m[15]-m[5]*m[11]*m[14]-m[9]*m[6]*m[15]+m[9]*m[7]*m[14]+m[13]*m[6]*m[11]-m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15]+m[4]*m[11]*m[14]+m[8]*m[6]*m[15]-m[8]*m[7]*m[14]-m[12]*m[6]*m[11]+m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15]-m[4]*m[11]*m[13]-m[8]*m[5]*m[15]+m[8]*m[7]*m[13]+m[12]*m[5]*m[11]-m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]+m[4]*m[10]*m[13]+m[8]*m[5]*m[14]-m[8]*m[6]*m[13]-m[12]*m[5]*m[10]+m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15]+m[1]*m[11]*m[14]+m[9]*m[2]*m[15]-m[9]*m[3]*m[14]-m[13]*m[2]*m[11]+m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15]-m[0]*m[11]*m[14]-m[8]*m[2]*m[15]+m[8]*m[3]*m[14]+m[12]*m[2]*m[11]-m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15]+m[0]*m[11]*m[13]+m[8]*m[1]*m[15]-m[8]*m[3]*m[13]-m[12]*m[1]*m[11]+m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14]-m[0]*m[10]*m[13]-m[8]*m[1]*m[14]+m[8]*m[2]*m[13]+m[12]*m[1]*m[10]-m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15]-m[1]*m[7]*m[14]-m[5]*m[2]*m[15]+m[5]*m[3]*m[14]+m[13]*m[2]*m[7]-m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15]+m[0]*m[7]*m[14]+m[4]*m[2]*m[15]-m[4]*m[3]*m[14]-m[12]*m[2]*m[7]+m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15]-m[0]*m[7]*m[13]-m[4]*m[1]*m[15]+m[4]*m[3]*m[13]+m[12]*m[1]*m[7]-m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]+m[0]*m[6]*m[13]+m[4]*m[1]*m[14]-m[4]*m[2]*m[13]-m[12]*m[1]*m[6]+m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11]+m[1]*m[7]*m[10]+m[5]*m[2]*m[11]-m[5]*m[3]*m[10]-m[9]*m[2]*m[7]+m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11]-m[0]*m[7]*m[10]-m[4]*m[2]*m[11]+m[4]*m[3]*m[10]+m[8]*m[2]*m[7]-m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]+m[0]*m[7]*m[9]+m[4]*m[1]*m[11]-m[4]*m[3]*m[9]-m[8]*m[1]*m[7]+m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10]-m[0]*m[6]*m[9]-m[4]*m[1]*m[10]+m[4]*m[2]*m[9]+m[8]*m[1]*m[6]-m[8]*m[2]*m[5];
    const double det = m[0]*inv[0]+m[1]*inv[4]+m[2]*inv[8]+m[3]*inv[12];
    if (!(std::abs(det) > 1e-30) || !std::isfinite(det)) return false;
    for (unsigned i = 0; i < 16; ++i) out[i] = inv[i] / det;
    return true;
}
bool fullEye(const D3DVIEWPORT9 &v) noexcept { return !v.X && !v.Y && v.Width == eyeWidth && v.Height == eyeHeight; }
bool recordsValid = false;
bool recordsOrtho = false; // The current matrices are not a perspective view (fills, 2D).
bool screenLocked = false; // Records built for the centre view in every eye.
bool engineScreenLock = false; // setScreenLocked: inside a camera-attached effect.
float mainCamera[3]{};
bool mainCameraValid = false;
// A perspective view whose camera is more than 1 m from the root view's: the
// sky's background viewer (V490: its camera sat 4.5 km below the world, so it
// was treated as a world view and the sky got 1-5 px of parallax, 10-40 m).
bool backgroundViewer(const float *v, const float *p) noexcept {
    if (!mainCameraValid || !v || !p || p[15] != 0) return false;
    const float c[3]{-(v[0] * v[3] + v[4] * v[7] + v[8] * v[11]), -(v[1] * v[3] + v[5] * v[7] + v[9] * v[11]),
                     -(v[2] * v[3] + v[6] * v[7] + v[10] * v[11])};
    const float d[3]{c[0] - mainCamera[0], c[1] - mainCamera[1], c[2] - mainCamera[2]};
    return d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 1.f;
}
bool writeRecords(IDirect3DDevice9 *d) noexcept {
    static bool lockedRecords = false;
    if (lockedRecords != screenLocked) { recordsValid = false; lockedRecords = screenLocked; }
    if (recordsValid && !std::memcmp(cachedAdjusted, engineAdjusted, sizeof(cachedAdjusted)) &&
        !std::memcmp(cachedProjection, engineProjection, sizeof(cachedProjection)) &&
        !std::memcmp(cachedView, engineView, sizeof(cachedView)) &&
        cachedViewport.X == engineViewport.X && cachedViewport.Y == engineViewport.Y &&
        cachedViewport.Width == engineViewport.Width && cachedViewport.Height == engineViewport.Height)
        return true;
    cachedViewport = engineViewport;
    std::memcpy(cachedAdjusted, engineAdjusted, sizeof(cachedAdjusted));
    std::memcpy(cachedProjection, engineProjection, sizeof(cachedProjection));
    std::memcpy(cachedView, engineView, sizeof(cachedView));
    {
        // Lab (SS2VR_LAB_SP_VIEWS=1): every distinct view state of the first passes.
        static const bool views = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SP_VIEWS", v, 2) == 1 && v[0] == L'1'; }();
        static unsigned logged = 0;
        if (views && logged < 120 && stats.passes > 200 && stats.passes < 204) {
            ++logged;
            const float *v = engineView, *p = engineProjection;
            // Camera position: -R^T t.
            const float cx = -(v[0] * v[3] + v[4] * v[7] + v[8] * v[11]), cy = -(v[1] * v[3] + v[5] * v[7] + v[9] * v[11]),
                        cz = -(v[2] * v[3] + v[6] * v[7] + v[10] * v[11]);
            log("Lab SP view pass=%u t=%.3f,%.3f,%.3f camera=%.2f,%.2f,%.2f fwd=%.3f,%.3f,%.3f p14=%.4f p15=%.4f p0=%.3f vp=%lux%lu",
                unsigned(stats.passes), v[3], v[7], v[11], cx, cy, cz, -v[8], -v[9], -v[10], p[14], p[15], p[0],
                engineViewport.Width, engineViewport.Height);
        }
    }
    ViewState state;
    std::memcpy(state.adjusted.data(), engineAdjusted, sizeof(cachedAdjusted));
    std::memcpy(state.projection.data(), engineProjection, sizeof(cachedProjection));
    std::memcpy(state.view.data(), engineView, sizeof(cachedView));
    // Records 0/1: each eye in its own viewport (per-half issue). Records 2/3:
    // the same, mapped into the eye's half of the wide viewport (x'' = x/2 -/+ w/2),
    // with the kill row -x'' (left) / +x'' (right), less half a pixel, for the pixel program.
    sp::EyeRecord records[4 + MaxRegions];
    recordsOrtho = false;
    const bool background = backgroundViewer(engineView, engineProjection);
    for (unsigned i = 0; i < 2; ++i) {
        EyeChange change = changes[i];
        if (background) change.view = change.rotation; // Rotation only, like the sky's own child view.
        Rewrite r = rewrite(state, change);
        if (screenLocked) r.identity = true;
        recordsOrtho = recordsOrtho || !r.valid || r.identity;
        for (unsigned row = 0; row < 4; ++row)
            for (unsigned c = 0; c < 4; ++c)
                records[i].r[row][c] = r.valid && !r.identity ? float(r.clip[4 * row + c]) : (row == c ? 1.f : 0.f);
        auto &wide = records[2 + i];
        std::memcpy(wide.r, records[i].r, sizeof(wide.r));
        const float side = i ? .5f : -.5f;
        for (unsigned c = 0; c < 4; ++c) {
            wide.r[0][c] = .5f * records[i].r[0][c] + side * records[i].r[3][c];
            // D3D9 pixel centres lie on integer coordinates, so the seam column's
            // centre is exactly x'' = 0 (which texkill keeps): split half a pixel
            // (1/W of wide NDC per pixel) left of it.
            const float half = .5f / float(eyeWidth ? eyeWidth : 1) * records[i].r[3][c];
            wide.kill[c] = i ? wide.r[0][c] + half : -wide.r[0][c] - half;
        }
    }
    // Records 4..: per region, the eye's clip space for the engine viewport mapped
    // into the region of the whole target (the region path's issue; x_target =
    // Ax x + Bx w, y_target = Ay y + By w). The plain wide layout with a full-eye
    // viewport reuses the instanced rows, so both issue modes agree bit for bit.
    const D3DVIEWPORT9 &v = engineViewport;
    const float W = float(eyeWidth), H = float(eyeHeight), AW = float(layout.width), AH = float(layout.height);
    const float av = float(v.Width) / W, bv = (2.f * float(v.X) + float(v.Width)) / W - 1.f;
    const float cv = float(v.Height) / H, dv = 1.f - (2.f * float(v.Y) + float(v.Height)) / H;
    for (unsigned j = 0; j < layout.count; ++j) {
        const Region &g = layout.region[j];
        auto &out = records[4 + j];
        const auto &eye = records[g.eye];
        std::memcpy(out.r, eye.r, sizeof(out.r));
        if (wideLayout && fullEye(v)) { std::memcpy(out.r[0], records[2 + g.eye].r[0], sizeof(out.r[0])); continue; }
        const float rw = float(g.rect.right - g.rect.left) / AW, rh = float(g.rect.bottom - g.rect.top) / AH;
        const float ax = rw * g.sx * av, bx = rw * (g.sx * bv + g.ox) + (2.f * float(g.rect.left)) / AW + rw - 1.f;
        const float ay = rh * g.sy * cv, by = rh * (g.sy * dv + g.oy) + 1.f - (2.f * float(g.rect.top)) / AH - rh;
        for (unsigned c = 0; c < 4; ++c) {
            out.r[0][c] = ax * eye.r[0][c] + bx * eye.r[3][c];
            out.r[1][c] = ay * eye.r[1][c] + by * eye.r[3][c];
        }
    }
    // Inverse clip maps for transforming clip planes (eye records, then regions).
    for (unsigned k = 0; k < 2 + layout.count; ++k) {
        const auto &rec = records[k < 2 ? k : 4 + (k - 2)];
        double m[16];
        for (unsigned row = 0; row < 4; ++row)
            for (unsigned c = 0; c < 4; ++c) m[4 * row + c] = rec.r[row][c];
        inverseValid[k] = invert4(m, inverseMap[k]);
    }
    const UINT count = 4 + layout.count;
    if (!ring && FAILED(d->CreateVertexBuffer(RingRecords * RecordBytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0,
                                              D3DPOOL_DEFAULT, &ring, nullptr)))
        return ring = nullptr, false;
    DWORD flags = D3DLOCK_NOOVERWRITE;
    if (ringNext + count > RingRecords) { ringNext = 0; flags = D3DLOCK_DISCARD; }
    void *mapped = nullptr;
    if (FAILED(ring->Lock(ringNext * RecordBytes, count * RecordBytes, &mapped, flags)) || !mapped) return false;
    std::memcpy(mapped, records, count * RecordBytes);
    ring->Unlock();
    eyeOffset[0] = ringNext * RecordBytes; eyeOffset[1] = (ringNext + 1) * RecordBytes;
    instancedOffset = (ringNext + 2) * RecordBytes;
    regionOffset = (ringNext + 4) * RecordBytes;
    ringNext += count;
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

bool suspended = false; // The engine is rendering to another target during the pass.
HRESULT WINAPI hSetVertexShader(IDirect3DDevice9 *d, IDirect3DVertexShader9 *s) {
    engineShader = s;
    if (!active || suspended) return oSetVertexShader(d, s);
    IDirect3DVertexShader9 *variant = variantOf(d, s);
    IDirect3DVertexDeclaration9 *decl = engineDeclaration ? variantOf(d, engineDeclaration) : nullptr;
    if (variant && decl) { oSetDeclaration(d, decl); return oSetVertexShader(d, variant); }
    if (engineDeclaration) oSetDeclaration(d, engineDeclaration);
    return oSetVertexShader(d, s);
}
HRESULT WINAPI hSetDeclaration(IDirect3DDevice9 *d, IDirect3DVertexDeclaration9 *v) {
    engineDeclaration = v;
    if (!active || suspended) return oSetDeclaration(d, v);
    const HRESULT hr = oSetDeclaration(d, v);
    bindVariants(d);
    return hr;
}
HRESULT WINAPI hSetFvf(IDirect3DDevice9 *d, DWORD f) {
    if (f) engineDeclaration = nullptr;
    const HRESULT hr = oSetFvf(d, f);
    if (active && !suspended && f) oSetVertexShader(d, engineShader); // FVF draws keep the original program.
    return hr;
}
// Device state as applied during the pass (lazily switched between draw modes):
// the whole-target viewport, a scissor rectangle other than the engine's, and
// the scissor test enabled by us while the engine has it off.
bool appliedWide = false, appliedInstances = false;
bool forcedScissor = false;
bool scissorApplied = false; // The device scissor is `appliedScissor`, not the engine's.
RECT appliedScissor{};
IDirect3DPixelShader9 *appliedPixelShader = nullptr;
void applyScissor(IDirect3DDevice9 *d, const RECT &r) noexcept {
    if (scissorApplied && !std::memcmp(&appliedScissor, &r, sizeof(r))) return;
    oSetScissor(d, &r);
    appliedScissor = r; scissorApplied = true;
}
void engineScissorBack(IDirect3DDevice9 *d) noexcept {
    if (!scissorApplied) return;
    oSetScissor(d, &engineScissor);
    scissorApplied = false;
}
void forceScissor(IDirect3DDevice9 *d, bool on) noexcept {
    if (forcedScissor == on) return;
    if (!scissorEnabled) oSetRenderState(d, D3DRS_SCISSORTESTENABLE, on ? TRUE : FALSE);
    forcedScissor = on;
}
// The whole target as the viewport (the engine's depth range).
void applyWide(IDirect3DDevice9 *d) noexcept {
    if (appliedWide) return;
    D3DVIEWPORT9 wide{0, 0, layout.width, layout.height, engineViewport.MinZ, engineViewport.MaxZ};
    oSetViewport(d, &wide);
    appliedWide = true;
}
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
HRESULT WINAPI hSetClipPlane(IDirect3DDevice9 *d, DWORD index, const float *plane) {
    if (index < 6 && plane) std::memcpy(enginePlanes[index], plane, sizeof(enginePlanes[index]));
    planesApplied = false;
    return oSetClipPlane(d, index, plane);
}
// Transformed planes for the map with inverse slot `k` (P_k = inverse^T P), or the engine's.
void applyPlanes(IDirect3DDevice9 *d, int k) noexcept {
    if (!clipPlanes) return;
    for (DWORD i = 0; i < 6; ++i) {
        if (!(clipPlanes & (1u << i))) continue;
        if (k < 0 || !inverseValid[k]) { oSetClipPlane(d, i, enginePlanes[i]); continue; }
        const double *n = inverseMap[k];
        float plane[4];
        for (unsigned c = 0; c < 4; ++c)
            plane[c] = float(n[c] * enginePlanes[i][0] + n[4 + c] * enginePlanes[i][1] + n[8 + c] * enginePlanes[i][2] +
                             n[12 + c] * enginePlanes[i][3]);
        oSetClipPlane(d, i, plane);
    }
    planesApplied = k >= 0;
}
HRESULT WINAPI hSetScissor(IDirect3DDevice9 *d, const RECT *r) {
    if (r) engineScissor = *r;
    scissorApplied = false;
    return oSetScissor(d, r);
}
HRESULT WINAPI hSetRenderState(IDirect3DDevice9 *d, D3DRENDERSTATETYPE s, DWORD v) {
    if (s == D3DRS_COLORWRITEENABLE) colorWrites = v;
    if (s == D3DRS_CLIPPLANEENABLE) clipPlanes = v & 0x3f;
    if (s == D3DRS_SCISSORTESTENABLE) {
        scissorEnabled = v != 0;
        forcedScissor = false;
    }
    return oSetRenderState(d, s, v);
}
// The engine reads state back mid-pass (the viewport, at least): it sees its own
// values, not the wide or per-eye state applied lazily for the draws.
using GetViewportF = HRESULT(WINAPI *)(IDirect3DDevice9 *, D3DVIEWPORT9 *);
using GetScissorF = HRESULT(WINAPI *)(IDirect3DDevice9 *, RECT *);
using GetRenderStateF = HRESULT(WINAPI *)(IDirect3DDevice9 *, D3DRENDERSTATETYPE, DWORD *);
using GetVertexShaderF = HRESULT(WINAPI *)(IDirect3DDevice9 *, IDirect3DVertexShader9 **);
using GetDeclarationF = HRESULT(WINAPI *)(IDirect3DDevice9 *, IDirect3DVertexDeclaration9 **);
using GetPixelShaderF = HRESULT(WINAPI *)(IDirect3DDevice9 *, IDirect3DPixelShader9 **);
GetViewportF oGetViewport = nullptr; GetScissorF oGetScissor = nullptr; GetRenderStateF oGetRenderState = nullptr;
GetVertexShaderF oGetVertexShader = nullptr; GetDeclarationF oGetDeclaration = nullptr; GetPixelShaderF oGetPixelShader = nullptr;
HRESULT WINAPI hGetViewport(IDirect3DDevice9 *d, D3DVIEWPORT9 *v) {
    if (!active || !v) return oGetViewport(d, v);
    *v = engineViewport;
    return D3D_OK;
}
HRESULT WINAPI hGetClipPlane(IDirect3DDevice9 *d, DWORD index, float *plane) {
    if (!active || !plane || index >= 6) return oGetClipPlane(d, index, plane);
    std::memcpy(plane, enginePlanes[index], sizeof(enginePlanes[index]));
    return D3D_OK;
}
HRESULT WINAPI hGetScissor(IDirect3DDevice9 *d, RECT *r) {
    if (!active || !r) return oGetScissor(d, r);
    *r = engineScissor;
    return D3D_OK;
}
HRESULT WINAPI hGetRenderState(IDirect3DDevice9 *d, D3DRENDERSTATETYPE s, DWORD *v) {
    if (active && v && s == D3DRS_SCISSORTESTENABLE) { *v = scissorEnabled; return D3D_OK; }
    return oGetRenderState(d, s, v);
}
template<class T> HRESULT engineBinding(T *engine, T **out) {
    *out = engine;
    if (engine) engine->AddRef();
    return D3D_OK;
}
HRESULT WINAPI hGetVertexShader(IDirect3DDevice9 *d, IDirect3DVertexShader9 **s) {
    return active && s ? engineBinding(engineShader, s) : oGetVertexShader(d, s);
}
HRESULT WINAPI hGetDeclaration(IDirect3DDevice9 *d, IDirect3DVertexDeclaration9 **v) {
    return active && v && engineDeclaration ? engineBinding(engineDeclaration, v) : oGetDeclaration(d, v);
}
HRESULT WINAPI hGetPixelShader(IDirect3DDevice9 *d, IDirect3DPixelShader9 **s) {
    return active && s ? engineBinding(enginePixelShader, s) : oGetPixelShader(d, s);
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
// Engine viewport and scissor state (per-half issue by shifting the viewport, clears).
void perHalfState(IDirect3DDevice9 *d) noexcept {
    if (appliedWide) { oSetViewport(d, &engineViewport); appliedWide = false; }
    engineScissorBack(d);
    forceScissor(d, false);
    if (planesApplied) applyPlanes(d, -1);
    if (appliedInstances) { d->SetStreamSourceFreq(0, 1); appliedInstances = false; }
    if (appliedPixelShader != enginePixelShader) { oSetPixelShader(d, enginePixelShader); appliedPixelShader = enginePixelShader; }
}
// One two-instance draw over the wide viewport, when the pixel program can
// discard the other half (a texcoord set free in both programs).
D3DPRIMITIVETYPE currentPrimitive = D3DPT_TRIANGLELIST;
// 1: mirror instanced vs per half; 2: mirror per half on the wide viewport vs shifted viewports.
unsigned labMirrorMode() noexcept { static const unsigned m = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SP_MIRROR", v, 2) == 1 ? unsigned(v[0] - L'0') : 0u; }(); return m; }
bool labMirror() noexcept { return labMirrorMode() != 0; }
template<class Call> bool instanced(IDirect3DDevice9 *d, Call call, HRESULT &hr) noexcept {
    static const bool linesOff = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SP_LINES", v, 2) == 1 && v[0] == L'0'; }();
    if (linesOff && (currentPrimitive == D3DPT_LINELIST || currentPrimitive == D3DPT_LINESTRIP)) return false;
    static const int labSetting = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SP_INSTANCING", v, 2) == 1 ? int(v[0] == L'1') : -1; }();
    const bool disabled = !wideLayout || (labSetting >= 0 ? !labSetting : !instancingEnabled && labMirrorMode() != 1);
    static const bool orthoOff = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SP_ORTHO", v, 2) == 1 && v[0] == L'0'; }();
    if (disabled || !enginePixelShader || !fullEye(engineViewport) ||
        (orthoOff && recordsOrtho))
        return false;
    static const bool backgroundOff = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SP_BACKGROUND", v, 2) == 1 && v[0] == L'0'; }();
    if (backgroundOff && engineView[3] == 0 && engineView[7] == 0 && engineView[11] == 0) return false;
    // Occlusion-query boxes (colour writes off) drawn instanced gave wrong query
    // results (the engine then culled visible objects, e.g. the moon); they stay per half.
    if (!colorWrites) return false;
    if (clipPlanes) return false; // One plane set cannot serve both instances.

    if (scissorEnabled && (engineScissor.left > 0 || engineScissor.top > 0 || engineScissor.right < LONG(eyeWidth) ||
                           engineScissor.bottom < LONG(engineViewport.Height)))
        return false;
    auto &pixel = pixelInfo(enginePixelShader);
    if (!pixel.known) return false;
    static const int onlyVersion = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SP_PSVER", v, 2) == 1 ? int(v[0] - L'0') : 0; }();
    if (onlyVersion == 1 && pixel.sm2) return false;
    if (onlyVersion == 2 && !pixel.sm2) return false;
    const uint32_t written = shaderTexcoords.count(engineShader) ? shaderTexcoords[engineShader] : 0xffu;
    // The vertex program carries the eye test in its lowest unwritten set; the
    // pixel program must not read that set (for 1.x, through any stage's index).
    unsigned k = 0;
    while (k < 8 && (written & (1u << k))) ++k;
    const unsigned sets = pixel.sm2 ? 8 : 4;
    if (k >= sets || (pixel.sets & (1u << k))) return false;
    if (!pixel.sm2)
        for (unsigned n = 0; n < 4; ++n) {
            DWORD index = n;
            if ((pixel.sets & (1u << n)) && SUCCEEDED(d->GetTextureStageState(n, D3DTSS_TEXCOORDINDEX, &index)) && (index & 0xffff) == k)
                return false;
        }
    IDirect3DPixelShader9 *variant = pixelVariant(d, pixel, k);
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
    applyWide(d);
    if (scissorEnabled || forcedScissor) applyScissor(d, RECT{0, 0, LONG(layout.width), LONG(layout.height)});
    if (!appliedInstances) { d->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | 2u); appliedInstances = true; }
    if (appliedPixelShader != variant) { oSetPixelShader(d, variant); appliedPixelShader = variant; }
    d->SetStreamSource(15, ring, instancedOffset, RecordBytes);
    hr = call(true);
    ++stats.instanced;
    return true;
}
// Issues `call` for the left half (engine viewport) and the right half (shifted).
// Lab (SS2VR_LAB_SP_MIRROR=1): every draw also goes to a mirror target pair,
// instanced where allowed, while the real target gets the per-half draws: the
// two images of one frame differ only by instancing.
IDirect3DSurface9 *mirrorColor = nullptr, *mirrorDepth = nullptr;
bool mirroring = false; // Inside the mirror half of a draw.
template<class Call> void onMirror(IDirect3DDevice9 *d, Call body) noexcept {
    IDirect3DSurface9 *color = nullptr, *depth = nullptr;
    if (!mirrorColor || !mirrorDepth || FAILED(d->GetRenderTarget(0, &color)) || FAILED(d->GetDepthStencilSurface(&depth))) {
        if (color) color->Release();
        if (depth) depth->Release();
        return;
    }
    mirroring = true;
    // SetRenderTarget resets the viewport and scissor rectangle to the target.
    d->SetRenderTarget(0, mirrorColor); d->SetDepthStencilSurface(mirrorDepth);
    oSetViewport(d, &engineViewport); oSetScissor(d, &engineScissor); appliedWide = false; scissorApplied = false;
    body();
    d->SetRenderTarget(0, color); d->SetDepthStencilSurface(depth);
    oSetViewport(d, &engineViewport); oSetScissor(d, &engineScissor); appliedWide = false; scissorApplied = false;
    color->Release(); depth->Release();
    mirroring = false;
}
bool waterDraw() noexcept {
    const auto it = engineShader ? shaderEyeClip.find(engineShader) : shaderEyeClip.end();
    return it != shaderEyeClip.end() && it->second;
}
// Saves c30..c31 around a water draw (later programs, e.g. skinned bones,
// expect the engine's values there) and sets each eye's position: the water
// program's view position (c4, in its model space) plus the eye's offset.
struct WaterConstantsScope {
    IDirect3DDevice9 *d;
    bool active = false, eyes = false;
    float saved[8]{}, view[4]{}, offset[2][3]{};
    WaterConstantsScope(IDirect3DDevice9 *device, bool stereo) noexcept : d(device) {
        if (!stereo || !waterDraw()) return;
        active = SUCCEEDED(d->GetVertexShaderConstantF(WaterConstant, saved, 2)) &&
                 SUCCEEDED(d->GetVertexShaderConstantF(4, view, 1));
        eyes = active && water::eyeOffset(engineView, changes[0], offset[0]) &&
               water::eyeOffset(engineView, changes[1], offset[1]);
    }
    void eye(unsigned i) noexcept {
        if (!active) return;
        const unsigned e = i & 1;
        const float c[8]{view[0] + (eyes ? offset[e][0] : 0.f), view[1] + (eyes ? offset[e][1] : 0.f),
                         view[2] + (eyes ? offset[e][2] : 0.f), 0, water::DomeScale, 1, 0, 0};
        d->SetVertexShaderConstantF(WaterConstant, c, 2);
    }
    ~WaterConstantsScope() { if (active) d->SetVertexShaderConstantF(WaterConstant, saved, 2); }
};
template<class Call> HRESULT perHalf(IDirect3DDevice9 *d, Call call, bool stereo, bool shifted = false) noexcept;
template<class Call> HRESULT issueHalves(IDirect3DDevice9 *d, Call call, bool instancing) noexcept;
// Issues a draw for both halves, then puts the engine's viewport, scissor and
// bindings back: nothing outside the pass's own draws (the engine's
// render-to-texture work, the bridge's draws, state read back) sees the state
// applied for the halves.
template<class Call> HRESULT bothHalves(IDirect3DDevice9 *d, Call call, bool instancing = true) noexcept {
    const HRESULT hr = issueHalves(d, call, instancing);
    perHalfState(d);
    return hr;
}
template<class Call> HRESULT issueHalves(IDirect3DDevice9 *d, Call call, bool instancing) noexcept {
    static const uint32_t lockHash = [] { wchar_t v[16]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SP_SCREENLOCK", v, 16) ? uint32_t(wcstoul(v, nullptr, 16)) : 0u; }();
    screenLocked = engineScreenLock || (lockHash && enginePixelShader && pixelInfo(enginePixelShader).hash == lockHash);
    if (screenLocked) ++stats.screenLocked;
    // Lab (SS2VR_LAB_SP_SKIP=1): the allowed pixel programs (0 = fixed function)
    // are not drawn at all; with the region sweep this finds a program by its effect.
    static const bool labSkip = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SP_SKIP", v, 2) == 1 && v[0] == L'1'; }();
    if (labSkip) {
        const uint32_t hash = enginePixelShader ? pixelInfo(enginePixelShader).hash : 0u;
        if (std::find(labPixelSeen.begin(), labPixelSeen.end(), hash) == labPixelSeen.end() && labPixelSeen.size() < 64)
            labPixelSeen.push_back(hash);
        if (labPixelRestrict && std::find(labPixelAllowed.begin(), labPixelAllowed.end(), hash) != labPixelAllowed.end())
            return D3D_OK;
    }
    const bool stereo = stereoBound();
    if (stereo && !writeRecords(d)) return call(false);
    HRESULT result = D3D_OK;
    if (labMirrorMode() == 2) {
        result = perHalf(d, call, stereo, true);
        onMirror(d, [&] {
            bool wide = stereo;
            if (wide && enginePixelShader) {
                const uint32_t hash = pixelInfo(enginePixelShader).hash;
                if (std::find(labPixelSeen.begin(), labPixelSeen.end(), hash) == labPixelSeen.end() && labPixelSeen.size() < 64)
                    labPixelSeen.push_back(hash);
                wide = !labPixelRestrict || std::find(labPixelAllowed.begin(), labPixelAllowed.end(), hash) != labPixelAllowed.end();
            }
            perHalf(d, call, stereo, !wide);
        });
        return result;
    }
    if (labMirror()) {
        result = perHalf(d, call, stereo);
        onMirror(d, [&] {
            HRESULT ignored = D3D_OK;
            if (!(stereo && instancing && instanced(d, call, ignored))) perHalf(d, call, stereo);
        });
        return result;
    }
    if (stereo && instancing && !waterDraw() && instanced(d, call, result)) return result;
    return perHalf(d, call, stereo);
}
template<class Call> HRESULT perHalf(IDirect3DDevice9 *d, Call call, bool stereo, bool shifted) noexcept {
    static const bool wideOff = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SP_WIDEHALF", v, 2) == 1 && v[0] == L'0'; }();
    static const bool wideBisect = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SP_WIDEBISECT", v, 2) == 1 && v[0] == L'1'; }();
    if (wideBisect && stereo && !shifted && enginePixelShader) {
        const uint32_t hash = pixelInfo(enginePixelShader).hash;
        if (std::find(labPixelSeen.begin(), labPixelSeen.end(), hash) == labPixelSeen.end() && labPixelSeen.size() < 64) labPixelSeen.push_back(hash);
        if (labPixelRestrict && std::find(labPixelAllowed.begin(), labPixelAllowed.end(), hash) == labPixelAllowed.end()) shifted = true;
    }
    // Region path: the whole target as the viewport, each region's mapping in its
    // record and the region (with the engine viewport and scissor mapped into it)
    // as the scissor. In the plain wide layout full-eye draws then match
    // instanced draws bit for bit (multi-pass surfaces mix both).
    if (stereo && ((!wideOff && !shifted) || !wideLayout)) {
        if (appliedInstances) { d->SetStreamSourceFreq(0, 1); appliedInstances = false; }
        if (appliedPixelShader != enginePixelShader) { oSetPixelShader(d, enginePixelShader); appliedPixelShader = enginePixelShader; }
        applyWide(d);
        forceScissor(d, true);
        const D3DVIEWPORT9 &v = engineViewport;
        RECT clip{LONG(v.X), LONG(v.Y), LONG(v.X + v.Width), LONG(v.Y + v.Height)};
        if (scissorEnabled) {
            clip.left = std::max(clip.left, engineScissor.left); clip.top = std::max(clip.top, engineScissor.top);
            clip.right = std::min(clip.right, engineScissor.right); clip.bottom = std::min(clip.bottom, engineScissor.bottom);
        }
        {
            static const uint32_t logHash = [] { wchar_t v[16]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_SP_LOGDRAW", v, 16) ? uint32_t(wcstoul(v, nullptr, 16)) : 0u; }();
            static unsigned logged = 0;
            if (logHash && enginePixelShader && pixelInfo(enginePixelShader).hash == logHash && logged < 3) {
                ++logged;
                DWORD blend = 0, src = 0, dst = 0, z = 0, cull = 0, fog = 0, alphaRef = 0, alphaTest = 0;
                d->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend); d->GetRenderState(D3DRS_SRCBLEND, &src); d->GetRenderState(D3DRS_DESTBLEND, &dst);
                d->GetRenderState(D3DRS_ZENABLE, &z); d->GetRenderState(D3DRS_CULLMODE, &cull); d->GetRenderState(D3DRS_FOGENABLE, &fog);
                d->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest); d->GetRenderState(D3DRS_ALPHAREF, &alphaRef);
                float c[16]{}; d->GetVertexShaderConstantF(0, c, 4);
                log("Single pass lab draw ps=%08x prim=%u planes=%lx scissor=%u vp=%lu,%lu %lux%lu z=%.2f-%.2f ortho=%u blend=%lu %lu/%lu z=%lu cull=%lu fog=%lu atest=%lu/%lu view0=%u c0=%.3f,%.3f,%.3f,%.3f c1=%.3f,%.3f,%.3f,%.3f",
                    logHash, unsigned(currentPrimitive), clipPlanes, unsigned(scissorEnabled), v.X, v.Y, v.Width, v.Height, v.MinZ, v.MaxZ,
                    unsigned(recordsOrtho), blend, src, dst, z, cull, fog, alphaTest, alphaRef,
                    unsigned(engineView[3] == 0 && engineView[7] == 0 && engineView[11] == 0), c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
                // Heuristic stack scan (no frame pointers): dwords pointing into engine modules.
                char line[1400]; size_t at = 0; unsigned found = 0;
                const auto *sp = reinterpret_cast<const uintptr_t *>(&found);
                const auto *tib = reinterpret_cast<const NT_TIB *>(NtCurrentTeb());
                const auto *top = static_cast<const uintptr_t *>(tib->StackBase);
                for (const uintptr_t *w = sp; w + 1 <= top && found < 28 && at < sizeof(line) - 64; ++w) {
                    const uintptr_t value = *w;
                    HMODULE m = nullptr;
                    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                            reinterpret_cast<LPCWSTR>(value), &m) || !m) continue;
                    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(m);
                    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(reinterpret_cast<const uint8_t *>(m) + dos->e_lfanew);
                    const uintptr_t start = reinterpret_cast<uintptr_t>(m);
                    if (value < start + 0x1000 || value >= start + nt->OptionalHeader.SizeOfImage) continue;
                    wchar_t name[MAX_PATH]{}; GetModuleFileNameW(m, name, MAX_PATH);
                    const wchar_t *base = wcsrchr(name, L'\\'); base = base ? base + 1 : name;
                    if (!_wcsicmp(base, L"d3d9.dll") || !wcsstr(name, L"Bin")) continue;
                    ++found;
                    at += size_t(std::snprintf(line + at, sizeof(line) - at, " %.4ls+%lx", base,
                        static_cast<unsigned long>(value - reinterpret_cast<uintptr_t>(m))));
                }
                log("Single pass lab draw stack:%s", line);
                float k[96]{}; d->GetVertexShaderConstantF(0, k, 24);
                for (unsigned r = 0; r < 24; r += 4)
                    log("Single pass lab draw c%u..: %.4f,%.4f,%.4f,%.4f | %.4f,%.4f,%.4f,%.4f | %.4f,%.4f,%.4f,%.4f | %.4f,%.4f,%.4f,%.4f", r,
                        k[4*r],k[4*r+1],k[4*r+2],k[4*r+3],k[4*r+4],k[4*r+5],k[4*r+6],k[4*r+7],k[4*r+8],k[4*r+9],k[4*r+10],k[4*r+11],k[4*r+12],k[4*r+13],k[4*r+14],k[4*r+15]);
                log("Single pass lab draw vs=%p adjusted=%.4f,%.4f,%.4f,%.4f / %.4f,%.4f,%.4f,%.4f", static_cast<void *>(engineShader),
                    engineAdjusted[0], engineAdjusted[1], engineAdjusted[2], engineAdjusted[3], engineAdjusted[4], engineAdjusted[5], engineAdjusted[6], engineAdjusted[7]);
                for (DWORD i = 0; i < 6; ++i)
                    if (clipPlanes & (1u << i))
                        log("Single pass lab draw plane %lu = %.4f,%.4f,%.4f,%.4f", i, enginePlanes[i][0], enginePlanes[i][1], enginePlanes[i][2], enginePlanes[i][3]);
            }
        }
        HRESULT hr = D3D_OK;
        WaterConstantsScope waterScope(d, stereo);
        for (unsigned j = 0; j < layout.count; ++j) {
            const RECT r = regionRect(layout.region[j], clip.left, clip.top, clip.right, clip.bottom);
            if (r.right <= r.left || r.bottom <= r.top) continue;
            applyScissor(d, r);
            applyPlanes(d, int(2 + j));
            waterScope.eye(layout.region[j].eye);
            d->SetStreamSource(15, ring, regionOffset + j * RecordBytes, RecordBytes);
            const HRESULT one = call(false);
            if (SUCCEEDED(hr)) hr = one;
        }
        return hr;
    }
    // Shifted path (programs without a stereo variant, lab): the engine viewport
    // placed in each eye's full view region; inset regions are skipped.
    perHalfState(d);
    if (!stereo) ++stats.unpatched;
    HRESULT hr = D3D_OK;
    const D3DVIEWPORT9 &v = engineViewport;
    WaterConstantsScope waterScope(d, stereo);
    for (unsigned j = 0; j < layout.count; ++j) {
        const Region &g = layout.region[j];
        if (!g.scaled) continue;
        waterScope.eye(g.eye);
        const RECT r = regionRect(g, LONG(v.X), LONG(v.Y), LONG(v.X + v.Width), LONG(v.Y + v.Height));
        if (r.right <= r.left || r.bottom <= r.top) continue;
        D3DVIEWPORT9 placed{DWORD(r.left), DWORD(r.top), DWORD(r.right - r.left), DWORD(r.bottom - r.top), v.MinZ, v.MaxZ};
        oSetViewport(d, &placed);
        if (scissorEnabled) {
            const RECT s = regionRect(g, engineScissor.left, engineScissor.top, engineScissor.right, engineScissor.bottom);
            oSetScissor(d, &s);
        }
        if (stereo) { d->SetStreamSource(15, ring, eyeOffset[g.eye], RecordBytes); applyPlanes(d, int(g.eye)); }
        const HRESULT one = call(false);
        if (SUCCEEDED(hr)) hr = one;
    }
    oSetViewport(d, &engineViewport);
    if (scissorEnabled) oSetScissor(d, &engineScissor);
    scissorApplied = false;
    return hr;
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
             hook(v[48], reinterpret_cast<void *>(hGetViewport), oGetViewport) &&
             hook(v[50], reinterpret_cast<void *>(hSetClipPlane), oSetClipPlane) &&
             hook(v[51], reinterpret_cast<void *>(hGetClipPlane), oGetClipPlane) &&
             hook(v[76], reinterpret_cast<void *>(hGetScissor), oGetScissor) &&
             hook(v[58], reinterpret_cast<void *>(hGetRenderState), oGetRenderState) &&
             hook(v[93], reinterpret_cast<void *>(hGetVertexShader), oGetVertexShader) &&
             hook(v[88], reinterpret_cast<void *>(hGetDeclaration), oGetDeclaration) &&
             hook(v[108], reinterpret_cast<void *>(hGetPixelShader), oGetPixelShader) &&
             hook(v[57], reinterpret_cast<void *>(hSetRenderState), oSetRenderState) &&
             hook(v[107], reinterpret_cast<void *>(hSetPixelShader), oSetPixelShader) &&
             bridge.draw && bridge.drawIndexed && bridge.drawUp && bridge.drawIndexedUp && bridge.clear;
    hookFailed = !hooked;
    // Current bindings, in case the engine bound them before the hooks existed.
    d->GetVertexShader(&engineShader); if (engineShader) engineShader->Release();
    d->GetVertexDeclaration(&engineDeclaration); if (engineDeclaration) engineDeclaration->Release();
    d->GetPixelShader(&enginePixelShader); if (enginePixelShader) enginePixelShader->Release();
    d->GetViewport(&engineViewport); d->GetScissorRect(&engineScissor);
    DWORD scissor = 0; d->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor); scissorEnabled = scissor != 0;
    d->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrites);
    DWORD planes = 0; d->GetRenderState(D3DRS_CLIPPLANEENABLE, &planes); clipPlanes = planes & 0x3f;
    for (DWORD i = 0; i < 6; ++i) d->GetClipPlane(i, enginePlanes[i]);
    log("Single pass hooks installed=%u", unsigned(hooked));
    return hooked;
}
bool installed() noexcept { return hooked; }
void setInstancing(bool enabled) noexcept { instancingEnabled = enabled; }
void setScreenLocked(bool locked) noexcept { engineScreenLock = locked; }
void setMainView(const float *v) noexcept {
    if (!v) return;
    mainCamera[0] = -(v[0] * v[3] + v[4] * v[7] + v[8] * v[11]);
    mainCamera[1] = -(v[1] * v[3] + v[5] * v[7] + v[9] * v[11]);
    mainCamera[2] = -(v[2] * v[3] + v[6] * v[7] + v[10] * v[11]);
    mainCameraValid = std::isfinite(mainCamera[0] + mainCamera[1] + mainCamera[2]);
}
void releaseDeviceResources() noexcept {
    // D3DPOOL_DEFAULT resources must be gone before IDirect3DDevice9::Reset; all
    // are recreated on first use.
    for (IUnknown **r : {reinterpret_cast<IUnknown **>(&ring), reinterpret_cast<IUnknown **>(&identityIndices),
                         reinterpret_cast<IUnknown **>(&mirrorColor), reinterpret_cast<IUnknown **>(&mirrorDepth)})
        if (*r) { (*r)->Release(); *r = nullptr; }
    ringNext = 0;
}
void setEngineMatrices(const float *adjusted, const float *projection, const float *view) noexcept {
    engineAdjusted = adjusted; engineProjection = projection; engineView = view;
}
Layout wideLayoutOf(UINT width, UINT height) noexcept {
    Layout l;
    l.width = width * 2; l.height = height; l.count = 2;
    for (unsigned i = 0; i < 2; ++i) l.region[i] = {RECT{LONG(i * width), 0, LONG((i + 1) * width), LONG(height)}, i, 1, 0, 1, 0, true};
    return l;
}
bool begin(IDirect3DDevice9 *d, IDirect3DSurface9 *target, UINT width, UINT height, const EyeChange (&change)[2],
           const Layout *regions) noexcept {
    if (!hooked || !engineAdjusted || !engineProjection || !engineView || !target || !change[0].valid || !change[1].valid ||
        !width || !height || (regions && (!regions->count || regions->count > MaxRegions)))
        return false;
    engineScreenLock = false; // Never inherited from an earlier, interrupted pass.
    active = true; wideTarget = target; eyeWidth = width; eyeHeight = height;
    layout = regions ? *regions : wideLayoutOf(width, height);
    wideLayout = !regions;
    changes[0] = change[0]; changes[1] = change[1];
    recordsValid = false;
    eligibleIndex = 0;
    appliedWide = appliedInstances = forcedScissor = suspended = scissorApplied = false;
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
    if (labMirror() && wideLayout) {
        D3DSURFACE_DESC cd{}, zd{};
        IDirect3DSurface9 *depth = nullptr;
        if (!mirrorColor && SUCCEEDED(target->GetDesc(&cd)))
            d->CreateRenderTarget(cd.Width, cd.Height, cd.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &mirrorColor, nullptr);
        if (!mirrorDepth && SUCCEEDED(d->GetDepthStencilSurface(&depth)) && depth && SUCCEEDED(depth->GetDesc(&zd)))
            d->CreateDepthStencilSurface(zd.Width, zd.Height, zd.Format, D3DMULTISAMPLE_NONE, 0, TRUE, &mirrorDepth, nullptr);
        if (depth) depth->Release();
        onMirror(d, [&] {
            D3DVIEWPORT9 full{0, 0, layout.width, layout.height, 0, 1};
            oSetViewport(d, &full);
            bridge.clear(d, 0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xff000000, 1, 0);
        });
    }
    ++stats.passes;
    return true;
}
void end(IDirect3DDevice9 *d) noexcept {
    if (!active) return;
    perHalfState(d);
    lastEligible = eligibleIndex;
    labRangeSet = false; labDetail = ~0u;
    active = false; wideTarget = nullptr; suspended = false;
    d->SetStreamSource(15, nullptr, 0, 0);
    d->SetStreamSourceFreq(15, 1);
    if (engineDeclaration) oSetDeclaration(d, engineDeclaration);
    oSetVertexShader(d, engineShader);
    oSetViewport(d, &engineViewport);
}
void beforeRenderTarget(IDirect3DDevice9 *d, DWORD index) noexcept {
    if (active && !index && !mirroring) perHalfState(d);
}
void afterRenderTarget(IDirect3DDevice9 *d, DWORD index, IDirect3DSurface9 *target) noexcept {
    if (!active || index || !target || mirroring) return;
    // SetRenderTarget resets the viewport and scissor rectangle to the target;
    // the wide target stands in for an eye-sized one.
    const bool away = target != wideTarget;
    D3DSURFACE_DESC desc{};
    if (SUCCEEDED(target->GetDesc(&desc))) {
        if (!away) desc.Width = eyeWidth, desc.Height = eyeHeight;
        engineViewport = {0, 0, desc.Width, desc.Height, 0.f, 1.f};
        engineScissor = {0, 0, LONG(desc.Width), LONG(desc.Height)};
        if (!away) { oSetViewport(d, &engineViewport); oSetScissor(d, &engineScissor); }
    }
    appliedWide = false; scissorApplied = false;
    if (away == suspended) return;
    suspended = away;
    if (away) {
        ++stats.suspensions;
        if (engineDeclaration) oSetDeclaration(d, engineDeclaration);
        oSetVertexShader(d, engineShader);
    } else bindVariants(d);
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
HRESULT drawUp(IDirect3DDevice9 *d, D3DPRIMITIVETYPE type, UINT count, const void *data, UINT stride) noexcept {
    ++stats.draws; ++stats.userPointer;
    currentPrimitive = type;
    return bothHalves(d, [&](bool) { return bridge.drawUp(d, type, count, data, stride); }, false);
}
HRESULT drawIndexedUp(IDirect3DDevice9 *d, D3DPRIMITIVETYPE type, UINT minimum, UINT vertices, UINT count, const void *indices,
                      D3DFORMAT format, const void *data, UINT stride) noexcept {
    ++stats.draws; ++stats.userPointer;
    currentPrimitive = type;
    return bothHalves(d, [&](bool) { return bridge.drawIndexedUp(d, type, minimum, vertices, count, indices, format, data, stride); }, false);
}
HRESULT clearHalves(IDirect3DDevice9 *d, DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) noexcept;
HRESULT clear(IDirect3DDevice9 *d, DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) noexcept {
    const HRESULT hr = clearHalves(d, count, rects, flags, color, z, stencil);
    if (labMirror()) onMirror(d, [&] { clearHalves(d, count, rects, flags, color, z, stencil); });
    return hr;
}
HRESULT clearHalves(IDirect3DDevice9 *d, DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) noexcept {
    // Each region: the engine viewport (and rectangles, and scissor) mapped into it.
    perHalfState(d);
    HRESULT hr = D3D_OK;
    const D3DVIEWPORT9 &v = engineViewport;
    std::vector<D3DRECT> mapped(rects ? count : 0);
    for (unsigned j = 0; j < layout.count; ++j) {
        const Region &g = layout.region[j];
        const RECT r = regionRect(g, LONG(v.X), LONG(v.Y), LONG(v.X + v.Width), LONG(v.Y + v.Height));
        if (r.right <= r.left || r.bottom <= r.top) continue;
        D3DVIEWPORT9 placed{DWORD(r.left), DWORD(r.top), DWORD(r.right - r.left), DWORD(r.bottom - r.top), v.MinZ, v.MaxZ};
        oSetViewport(d, &placed);
        if (scissorEnabled) {
            const RECT s = regionRect(g, engineScissor.left, engineScissor.top, engineScissor.right, engineScissor.bottom);
            oSetScissor(d, &s);
        }
        for (DWORD k = 0; rects && k < count; ++k) {
            const RECT m = regionRect(g, rects[k].x1, rects[k].y1, rects[k].x2, rects[k].y2);
            mapped[k] = {m.left, m.top, m.right, m.bottom};
        }
        const HRESULT one = bridge.clear(d, count, rects ? mapped.data() : nullptr, flags, color, z, stencil);
        if (SUCCEEDED(hr)) hr = one;
    }
    oSetViewport(d, &engineViewport);
    if (scissorEnabled) oSetScissor(d, &engineScissor);
    scissorApplied = false;
    return hr;
}
void labInstanceRange(unsigned lo, unsigned hi, unsigned detail) noexcept { labLo = lo; labHi = hi; labDetail = detail; labRangeSet = true; }
unsigned labEligibleCount() noexcept { return lastEligible; }
std::vector<uint32_t> labInstancedPixelShaders() noexcept { return labPixelSeen; }
IDirect3DSurface9 *labMirrorTarget() noexcept { return labMirror() ? mirrorColor : nullptr; }
void labAllowPixelShaders(const std::vector<uint32_t> &hashes, bool restrict) noexcept { labPixelAllowed = hashes; labPixelRestrict = restrict; }
Stats takeStats() noexcept { const Stats out = stats; stats = {}; return out; }
} // namespace ss2vr::game::single_pass
