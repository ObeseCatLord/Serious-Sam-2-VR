#include "env_probe.hpp"
#include "common/env_probe.hpp"
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
IDirect3DTexture9 *source = nullptr;
IDirect3DSurface9 *sourceSurface = nullptr;
IDirect3DPixelShader9 *program = nullptr;
IDirect3DStateBlock9 *state = nullptr;
bool failed = false, filled = false;
uint64_t lastWater = 0;
float lastCamera[3]{};
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

void noteWater() noexcept { lastWater = GetTickCount64(); }
bool wanted() noexcept { return lastWater && GetTickCount64() - lastWater < KeepMs && !failed; }

void capture(IDirect3DDevice9 *d, const Device &device, IDirect3DSurface9 *eye, UINT width, UINT height, const float *view,
             const float *projection) noexcept {
    if (!wanted() || !d || !eye || !view || !projection || !device.setRT || !device.drawUp || !device.stretch) return;
    D3DSURFACE_DESC desc{};
    if (FAILED(eye->GetDesc(&desc)) || !ensure(d, device, desc.Format)) return;
    // The cube is centred on the viewer: a jump (respawn, load) starts it again.
    const float camera[3]{-(view[0] * view[3] + view[4] * view[7] + view[8] * view[11]),
                          -(view[1] * view[3] + view[5] * view[7] + view[9] * view[11]),
                          -(view[2] * view[3] + view[6] * view[7] + view[10] * view[11])};
    const float dx = camera[0] - lastCamera[0], dy = camera[1] - lastCamera[1], dz = camera[2] - lastCamera[2];
    if (filled && dx * dx + dy * dy + dz * dz > 40.f * 40.f) clearFaces(d, device);
    std::copy(std::begin(camera), std::end(camera), lastCamera);
    const RECT whole{0, 0, LONG(width), LONG(height)};
    if (FAILED(device.stretch(d, eye, &whole, sourceSurface, nullptr, D3DTEXF_LINEAR))) return;
    float rows[16];
    probe::captureRows(view, projection, rows);
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
        {D3DRS_ZENABLE, FALSE}, {D3DRS_ZWRITEENABLE, FALSE}, {D3DRS_ALPHATESTENABLE, FALSE}, {D3DRS_ALPHABLENDENABLE, FALSE},
        {D3DRS_CULLMODE, D3DCULL_NONE}, {D3DRS_FOGENABLE, FALSE}, {D3DRS_STENCILENABLE, FALSE}, {D3DRS_SCISSORTESTENABLE, FALSE},
        {D3DRS_COLORWRITEENABLE, 0xf}, {D3DRS_SRGBWRITEENABLE, FALSE}, {D3DRS_CLIPPLANEENABLE, 0}, {D3DRS_FILLMODE, D3DFILL_SOLID}};
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
    // Lab (SS2VR_LAB_CUBE_DUMP=1): the six faces after 600 captures, as one strip (Bin/env-cube.ppm).
    static const bool dump = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_CUBE_DUMP", v, 2) == 1 && v[0] == L'1'; }();
    static unsigned captures = 0;
    if (dump && ok && ++captures % 600 == 0 && captures <= 3000) {
        const UINT n = probe::FaceSize;
        std::vector<uint8_t> strip(size_t(n) * 6 * n * 3);
        IDirect3DSurface9 *read = nullptr;
        if (SUCCEEDED(d->CreateOffscreenPlainSurface(n, n, desc.Format, D3DPOOL_SYSTEMMEM, &read, nullptr))) {
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
            if (slash) std::swprintf(slash + 1, MAX_PATH - size_t(slash + 1 - name), L"env-cube-%u.ppm", captures / 600);
            if (FILE *f = slash ? _wfopen(name, L"wb") : nullptr) {
                std::fprintf(f, "P6\n%u %u\n255\n", n * 6, n);
                std::fwrite(strip.data(), 1, strip.size(), f);
                std::fclose(f);
                log("Lab environment cube dump %u", captures / 600);
            }
        }
    }
}

IDirect3DCubeTexture9 *cube(IDirect3DDevice9 *d) noexcept { return d == owner && filled ? cubeTexture : nullptr; }

void release() noexcept {
    drop(state); drop(program); drop(sourceSurface); drop(source); drop(cubeTexture);
    owner = nullptr;
    filled = false;
}
} // namespace ss2vr::game::env_probe
