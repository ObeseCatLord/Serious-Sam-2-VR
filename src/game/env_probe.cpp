#include "env_probe.hpp"
#include "common/env_probe.hpp"
#include "common/water_reflection.hpp"
#include "game.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace ss2vr::game::env_probe {
namespace probe = ss2vr::env_probe;
namespace {
IDirect3DDevice9 *owner = nullptr;
IDirect3DCubeTexture9 *cubeTexture = nullptr;
IDirect3DTexture9 *source = nullptr, *faceSource = nullptr;
IDirect3DSurface9 *sourceSurface = nullptr, *faceSourceSurface = nullptr;
// Rendered faces: the face being rendered, its target, which faces hold a render
// of the current anchor, and when one last completed.
unsigned passFace = 0, faceNext = 0, faceFresh = 0, faceEver = 0;
IDirect3DSurface9 *passTarget = nullptr;
bool passActive = false, passCaptured = false;
uint64_t lastFaceTick = 0;
IDirect3DPixelShader9 *program = nullptr;
IDirect3DStateBlock9 *state = nullptr;
bool failed = false, filled = false;
uint64_t lastWater = 0;
float lastCamera[3]{}, anchorAt[3]{};
bool anchored = false;
// The cube's fixed world point moves to the viewer only once they are this far
// from it (metres): within that, reflections stay put as the head moves.
constexpr float AnchorMove = 4.f;
// The sphere's radius (metres): eased toward the measured surroundings, within
// these limits (too small and distant scenery swims; too large and nearby
// things reflect as if far away, gliding with the head).
constexpr float RadiusMin = 4.f, RadiusMax = 40.f;
float surroundings = ss2vr::water::ProxyRadius, sphereRadius = ss2vr::water::ProxyRadius;
uint64_t radiusTick = 0;
constexpr UINT SourceSize = 512;
constexpr uint64_t KeepMs = 4000; // After the last water draw.
template<class T> void drop(T *&p) { if (p) { p->Release(); p = nullptr; } }
// Sky-like start until views fill the cube (water seen before any capture).
constexpr D3DCOLOR Neutral = D3DCOLOR_XRGB(150, 175, 200);
bool clearFaces(IDirect3DDevice9 *d, const Device &device) {
    IDirect3DSurface9 *previous = nullptr;
    if (FAILED(d->GetRenderTarget(0, &previous))) return false;
    bool ok = true;
    for (UINT face = 0; face < 6 && ok; ++face) {
        IDirect3DSurface9 *surface = nullptr;
        ok = SUCCEEDED(cubeTexture->GetCubeMapSurface(D3DCUBEMAP_FACES(face), 0, &surface)) &&
             SUCCEEDED(device.setRT(d, 0, surface)) && SUCCEEDED(d->Clear(0, nullptr, D3DCLEAR_TARGET, Neutral, 1, 0));
        drop(surface);
    }
    device.setRT(d, 0, previous);
    drop(previous);
    return ok;
}
bool ensure(IDirect3DDevice9 *d, const Device &device, D3DFORMAT format) {
    if (owner != d) release();
    if (failed) return false;
    if (cubeTexture) return true;
    owner = d;
    const auto words = probe::captureProgram();
    if (FAILED(d->CreateCubeTexture(probe::FaceSize, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, &cubeTexture, nullptr)) ||
        FAILED(d->CreateTexture(SourceSize, SourceSize, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, &source, nullptr)) ||
        FAILED(source->GetSurfaceLevel(0, &sourceSurface)) ||
        FAILED(d->CreateTexture(probe::FaceSize, probe::FaceSize, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, &faceSource, nullptr)) ||
        FAILED(faceSource->GetSurfaceLevel(0, &faceSourceSurface)) ||
        FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD *>(words.data()), &program)) ||
        FAILED(d->CreateStateBlock(D3DSBT_ALL, &state)) || !clearFaces(d, device)) {
        log("Water environment cube unavailable");
        release();
        failed = true;
        return false;
    }
    log("Water environment cube %ux%u format=%d", probe::FaceSize, probe::FaceSize, int(format));
    return true;
}
} // namespace

// Lab (SS2VR_LAB_CUBE_DUMP=1): the six faces every `every` updates, as one strip (Bin/env-cube-N.ppm).
static void labDump(IDirect3DDevice9 *d, D3DFORMAT format, unsigned every) noexcept {
    static const bool dump = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_CUBE_DUMP", v, 2) == 1 && v[0] == L'1'; }();
    static unsigned captures = 0;
    if (dump && ++captures % every == 0 && captures <= 5 * every) {
        const UINT n = probe::FaceSize;
        std::vector<uint8_t> strip(size_t(n) * 6 * n * 3);
        IDirect3DSurface9 *read = nullptr;
        if (SUCCEEDED(d->CreateOffscreenPlainSurface(n, n, format, D3DPOOL_SYSTEMMEM, &read, nullptr))) {
            for (UINT face = 0; face < 6; ++face) {
                IDirect3DSurface9 *surface = nullptr;
                D3DLOCKED_RECT locked{};
                if (SUCCEEDED(cubeTexture->GetCubeMapSurface(D3DCUBEMAP_FACES(face), 0, &surface)) &&
                    SUCCEEDED(d->GetRenderTargetData(surface, read)) && SUCCEEDED(read->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
                    for (UINT y = 0; y < n; ++y)
                        for (UINT x = 0; x < n; ++x) {
                            const auto *px = static_cast<const uint8_t *>(locked.pBits) + size_t(y) * locked.Pitch + x * 4;
                            uint8_t *o = &strip[(size_t(y) * 6 * n + face * n + x) * 3];
                            o[0] = px[2]; o[1] = px[1]; o[2] = px[0];
                        }
                    read->UnlockRect();
                }
                drop(surface);
            }
            drop(read);
            // Next to this module (Bin), where the lab collects its images.
            wchar_t name[MAX_PATH]{};
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&release), &self);
            const DWORD length = GetModuleFileNameW(self, name, MAX_PATH);
            wchar_t *slash = length ? wcsrchr(name, L'\\') : nullptr;
            if (slash) std::swprintf(slash + 1, MAX_PATH - size_t(slash + 1 - name), L"env-cube-%u.ppm", captures / every);
            if (FILE *f = slash ? _wfopen(name, L"wb") : nullptr) {
                std::fprintf(f, "P6\n%u %u\n255\n", n * 6, n);
                std::fwrite(strip.data(), 1, strip.size(), f);
                std::fclose(f);
                log("Lab environment cube dump %u", captures / every);
            }
        }
    }
}
void noteWater() noexcept { lastWater = GetTickCount64(); }
bool wanted() noexcept { return lastWater && GetTickCount64() - lastWater < KeepMs && !failed; }
bool rendered() noexcept { return lastFaceTick && GetTickCount64() - lastFaceTick < 1000; }

bool nextFace(IDirect3DDevice9 *d, const Device &device, D3DFORMAT format, const float (&viewer)[3], unsigned &face,
              float (&anchorOut)[3]) noexcept {
    if (!wanted() || !d || !std::isfinite(viewer[0] + viewer[1] + viewer[2]) || !ensure(d, device, format)) return false;
    // A jump (respawn, load) starts the cube again; walking away from the anchor
    // moves it, and every face is rendered again from there.
    const float dx = viewer[0] - anchorAt[0], dy = viewer[1] - anchorAt[1], dz = viewer[2] - anchorAt[2];
    const float moved = dx * dx + dy * dy + dz * dz;
    if (anchored && moved > 40.f * 40.f) { clearFaces(d, device); faceEver = 0; anchored = false; }
    if (!anchored || moved > AnchorMove * AnchorMove) {
        std::copy(std::begin(viewer), std::end(viewer), anchorAt);
        anchored = true;
        faceFresh = 0;
    }
    std::copy(std::begin(viewer), std::end(viewer), lastCamera);
    // Stale faces first (after a move), then round and round.
    unsigned k = faceNext;
    for (unsigned n = 0; n < probe::RenderedFaces; ++n) {
        const unsigned candidate = (faceNext + n) % probe::RenderedFaces;
        if (!(faceFresh & (1u << probe::renderedFace(candidate)))) { k = candidate; break; }
    }
    faceNext = (k + 1) % probe::RenderedFaces;
    face = probe::renderedFace(k);
    std::copy(std::begin(anchorAt), std::end(anchorAt), anchorOut);
    return true;
}
void beginFace(IDirect3DSurface9 *target, unsigned face) noexcept {
    passTarget = target;
    passActive = target && cubeTexture && face < 6;
    passCaptured = false;
    passFace = face;
}
bool facePass(IDirect3DDevice9 *d) noexcept {
    if (!passActive || !d) return false;
    IDirect3DSurface9 *current = nullptr;
    if (FAILED(d->GetRenderTarget(0, &current)) || !current) return false;
    const bool ours = current == passTarget;
    current->Release();
    return ours;
}
bool captureFace(IDirect3DDevice9 *d, const Device &device, IDirect3DSurface9 *target) noexcept {
    if (!passActive || passCaptured || d != owner || !target || !device.setRT || !device.drawUp || !device.stretch) return false;
    // Lab (SS2VR_LAB_CUBE_COLORS=1): each face a solid colour (+X red, -X green,
    // +Y blue, -Y black, +Z yellow, -Z magenta), to see where the water looks.
    static const bool colours = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_CUBE_COLORS", v, 2) == 1 && v[0] == L'1'; }();
    if (colours) {
        static const D3DCOLOR face[6]{D3DCOLOR_XRGB(220, 40, 40), D3DCOLOR_XRGB(40, 200, 40), D3DCOLOR_XRGB(40, 60, 230),
                                      D3DCOLOR_XRGB(0, 0, 0), D3DCOLOR_XRGB(230, 220, 40), D3DCOLOR_XRGB(220, 40, 220)};
        IDirect3DSurface9 *previous = nullptr, *surface = nullptr;
        bool ok = SUCCEEDED(d->GetRenderTarget(0, &previous)) && SUCCEEDED(cubeTexture->GetCubeMapSurface(D3DCUBEMAP_FACES(passFace), 0, &surface)) &&
                  SUCCEEDED(device.setRT(d, 0, surface)) && SUCCEEDED(d->Clear(0, nullptr, D3DCLEAR_TARGET, face[passFace], 1, 0));
        // A white cross through the face centre (s = 0 and t = 0) for orientation.
        const D3DRECT bars[2]{{LONG(probe::FaceSize / 2 - 2), 0, LONG(probe::FaceSize / 2 + 2), LONG(probe::FaceSize)},
                              {0, LONG(probe::FaceSize / 2 - 2), LONG(probe::FaceSize), LONG(probe::FaceSize / 2 + 2)}};
        ok = ok && SUCCEEDED(d->Clear(2, bars, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 255, 255), 1, 0));
        drop(surface);
        if (previous) device.setRT(d, 0, previous);
        drop(previous);
        passCaptured = ok;
        return ok;
    }
    // The square view (rendered into the whole eye target) down to the face size,
    // then into the face mirrored left to right (D3D's faces are left-handed).
    if (FAILED(device.stretch(d, target, nullptr, faceSourceSurface, nullptr, D3DTEXF_LINEAR))) return false;
    IDirect3DSurface9 *previous = nullptr, *surface = nullptr;
    if (FAILED(state->Capture()) || FAILED(d->GetRenderTarget(0, &previous))) { drop(previous); return false; }
    d->SetVertexShader(nullptr);
    d->SetPixelShader(nullptr);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    d->SetTexture(0, faceSource);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0); d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE); d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT); d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    const std::pair<D3DRENDERSTATETYPE, DWORD> states[]{
        {D3DRS_ZENABLE, FALSE}, {D3DRS_ZWRITEENABLE, FALSE}, {D3DRS_ALPHATESTENABLE, FALSE}, {D3DRS_ALPHABLENDENABLE, FALSE},
        {D3DRS_CULLMODE, D3DCULL_NONE}, {D3DRS_FOGENABLE, FALSE}, {D3DRS_STENCILENABLE, FALSE}, {D3DRS_SCISSORTESTENABLE, FALSE},
        {D3DRS_COLORWRITEENABLE, 0xf}, {D3DRS_SRGBWRITEENABLE, FALSE}, {D3DRS_CLIPPLANEENABLE, 0}, {D3DRS_FILLMODE, D3DFILL_SOLID},
        {D3DRS_LIGHTING, FALSE}};
    for (const auto &[which, value] : states) d->SetRenderState(which, value);
    struct Vertex { float x, y, z, rhw, u, v; };
    const float n = float(probe::FaceSize) - .5f;
    const Vertex quad[4]{{-.5f, -.5f, 0, 1, 1, 0}, {n, -.5f, 0, 1, 0, 0}, {-.5f, n, 0, 1, 1, 1}, {n, n, 0, 1, 0, 1}};
    const D3DVIEWPORT9 viewport{0, 0, probe::FaceSize, probe::FaceSize, 0, 1};
    const bool ok = SUCCEEDED(cubeTexture->GetCubeMapSurface(D3DCUBEMAP_FACES(passFace), 0, &surface)) &&
                    SUCCEEDED(device.setRT(d, 0, surface)) && SUCCEEDED(d->SetViewport(&viewport)) &&
                    SUCCEEDED(device.drawUp(d, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex)));
    drop(surface);
    device.setRT(d, 0, previous);
    drop(previous);
    state->Apply();
    passCaptured = ok;
    return ok;
}
void endFace(bool complete) noexcept {
    if (passActive && complete && passCaptured) {
        faceFresh |= 1u << passFace;
        faceEver |= 1u << passFace;
        lastFaceTick = GetTickCount64();
        if (!filled) log("Water environment cube rendered from its anchor");
        filled = true;
        if (owner) {
            D3DSURFACE_DESC desc{};
            if (SUCCEEDED(cubeTexture->GetLevelDesc(0, &desc))) labDump(owner, desc.Format, 300);
        }
    }
    passActive = false;
    passTarget = nullptr;
}

void capture(IDirect3DDevice9 *d, const Device &device, IDirect3DSurface9 *eye, UINT width, UINT height, const float *view,
             const float *projection) noexcept {
    if (!wanted() || !d || !eye || !view || !projection || !device.setRT || !device.drawUp || !device.stretch) return;
    D3DSURFACE_DESC desc{};
    if (FAILED(eye->GetDesc(&desc)) || !ensure(d, device, desc.Format)) return;
    // A jump (respawn, load) starts the cube again; walking away from its anchor
    // moves the anchor (the old views fade out as new ones blend in).
    const float camera[3]{-(view[0] * view[3] + view[4] * view[7] + view[8] * view[11]),
                          -(view[1] * view[3] + view[5] * view[7] + view[9] * view[11]),
                          -(view[2] * view[3] + view[6] * view[7] + view[10] * view[11])};
    const float dx = camera[0] - lastCamera[0], dy = camera[1] - lastCamera[1], dz = camera[2] - lastCamera[2];
    if (filled && dx * dx + dy * dy + dz * dz > 40.f * 40.f) { clearFaces(d, device); anchored = false; }
    std::copy(std::begin(camera), std::end(camera), lastCamera);
    const float ax = camera[0] - anchorAt[0], ay = camera[1] - anchorAt[1], az = camera[2] - anchorAt[2];
    if (!anchored || ax * ax + ay * ay + az * az > AnchorMove * AnchorMove) {
        std::copy(std::begin(camera), std::end(camera), anchorAt);
        anchored = true;
    }
    const RECT whole{0, 0, LONG(width), LONG(height)};
    if (FAILED(device.stretch(d, eye, &whole, sourceSurface, nullptr, D3DTEXF_LINEAR))) return;
    float rows[16];
    {
        // About a second to settle on a new measurement (no jumps in the reflections).
        const uint64_t now = GetTickCount64();
        const float dt = radiusTick ? std::min(float(now - radiusTick) * .001f, .1f) : 0.f;
        radiusTick = now;
        sphereRadius += (surroundings - sphereRadius) * std::min(1.f, dt * 2.5f);
    }
    probe::captureRows(view, projection, anchorAt, sphereRadius, rows);
    IDirect3DSurface9 *previous = nullptr;
    if (FAILED(state->Capture()) || FAILED(d->GetRenderTarget(0, &previous))) { drop(previous); return; }
    d->SetVertexShader(nullptr);
    d->SetPixelShader(program);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0));
    d->SetPixelShaderConstantF(0, rows, 4);
    d->SetTexture(0, source);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    const std::pair<D3DRENDERSTATETYPE, DWORD> states[]{
        {D3DRS_ZENABLE, FALSE}, {D3DRS_ZWRITEENABLE, FALSE}, {D3DRS_ALPHATESTENABLE, FALSE}, {D3DRS_ALPHABLENDENABLE, TRUE},
        {D3DRS_SRCBLEND, D3DBLEND_SRCALPHA}, {D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA}, {D3DRS_BLENDOP, D3DBLENDOP_ADD},
        {D3DRS_SEPARATEALPHABLENDENABLE, FALSE},
        {D3DRS_CULLMODE, D3DCULL_NONE}, {D3DRS_FOGENABLE, FALSE}, {D3DRS_STENCILENABLE, FALSE}, {D3DRS_SCISSORTESTENABLE, FALSE},
        {D3DRS_COLORWRITEENABLE, 0x7}, {D3DRS_SRGBWRITEENABLE, FALSE}, {D3DRS_CLIPPLANEENABLE, 0}, {D3DRS_FILLMODE, D3DFILL_SOLID}};
    for (const auto &[which, value] : states) d->SetRenderState(which, value);
    struct Vertex { float x, y, z, rhw, u, v, w; };
    const float size = float(probe::FaceSize);
    bool ok = true;
    for (UINT face = 0; face < 6 && ok; ++face) {
        IDirect3DSurface9 *surface = nullptr;
        Vertex quad[4];
        const float corners[4][2]{{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
        for (unsigned k = 0; k < 4; ++k) {
            float dir[3];
            probe::faceDirection(face, corners[k][0], corners[k][1], dir);
            quad[k] = {(corners[k][0] + 1) * .5f * size - .5f, (corners[k][1] + 1) * .5f * size - .5f, 0, 1, dir[0], dir[1], dir[2]};
        }
        const D3DVIEWPORT9 viewport{0, 0, probe::FaceSize, probe::FaceSize, 0, 1};
        ok = SUCCEEDED(cubeTexture->GetCubeMapSurface(D3DCUBEMAP_FACES(face), 0, &surface)) &&
             SUCCEEDED(device.setRT(d, 0, surface)) && SUCCEEDED(d->SetViewport(&viewport)) &&
             SUCCEEDED(device.drawUp(d, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex)));
        drop(surface);
    }
    device.setRT(d, 0, previous);
    drop(previous);
    state->Apply();
    if (ok && !filled) log("Water environment cube filled from the eye view");
    filled = filled || ok;
    labDump(d, desc.Format, 600);
}

IDirect3DCubeTexture9 *cube(IDirect3DDevice9 *d) noexcept {
    // Whenever the cube exists (its unrendered faces hold a neutral sky): without
    // it the water fell back to the engine's reflection, a flipped copy of the
    // screen, which in single pass is both eyes side by side (a band split at
    // each eye's centre, fixed to the head; lab V799-V830).
    return d == owner ? cubeTexture : nullptr;
}
void setSurroundings(float metres) noexcept {
    if (std::isfinite(metres)) surroundings = std::clamp(metres, RadiusMin, RadiusMax);
}
float radius() noexcept { return sphereRadius; }
bool anchor(float (&out)[3]) noexcept {
    if (!anchored) return false;
    std::copy(std::begin(anchorAt), std::end(anchorAt), out);
    return true;
}

void release() noexcept {
    drop(state); drop(program); drop(sourceSurface); drop(source); drop(faceSourceSurface); drop(faceSource); drop(cubeTexture);
    owner = nullptr;
    filled = false;
    anchored = false;
    faceFresh = faceEver = 0;
    passActive = false;
    passTarget = nullptr;
}
} // namespace ss2vr::game::env_probe
