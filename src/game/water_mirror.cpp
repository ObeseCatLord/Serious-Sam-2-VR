// Planar water reflections (water_mirror.hpp).
#include "water_mirror.hpp"
#include "common/water_mirror.hpp"
#include "game.hpp"
#include "single_pass.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace ss2vr::game::water_mirror {
namespace {
using ::ss2vr::replay::M4;
struct Seen { double plane[4]{}; Vec3 camera{}; uint64_t tick = 0; bool used = false; };
Seen seen[8]{};
IDirect3DDevice9 *device = nullptr;
IDirect3DTexture9 *texture = nullptr;
UINT textureWidth = 0, textureHeight = 0;
D3DFORMAT textureFormat = D3DFMT_UNKNOWN;
IDirect3DSurface9 *passTarget = nullptr;
bool passActive = false, cameraRecorded = false, captured = false, ready = false;
double passPlane[4]{}, readyPlane[4]{};
Vec3 passCamera{};
M4 passWorldToClip{}, readyWorldToClip{};
Stats stats;
} // namespace

void noteWater(const double (&plane)[4], const Vec3 &camera) noexcept {
    const uint64_t now = GetTickCount64();
    Seen *slot = nullptr;
    for (auto &s : seen)
        if (s.tick && ss2vr::water_mirror::samePlane(s.plane, plane)) { slot = &s; break; }
    if (!slot) {
        slot = &seen[0];
        for (auto &s : seen) if (s.tick < slot->tick) slot = &s; // The oldest goes.
        std::memcpy(slot->plane, plane, sizeof(slot->plane));
    }
    slot->camera = camera;
    slot->tick = now;
}
bool framePlane(double (&plane)[4]) noexcept {
    const uint64_t now = GetTickCount64();
    const Seen *best = nullptr;
    double bestDistance = 1e30;
    for (const auto &s : seen) {
        if (!s.tick || now - s.tick > 1000) continue;
        const double above = ss2vr::water_mirror::signedDistance(s.plane, s.camera);
        if (!(above > .02) || above > 200) continue; // Under the water or far above it: no mirror.
        if (above < bestDistance) { bestDistance = above; best = &s; }
    }
    if (!best) return false;
    std::memcpy(plane, best->plane, sizeof(plane));
    return true;
}
void frameStart() noexcept { ready = false; }
bool prepare(IDirect3DDevice9 *d, UINT width, UINT height, D3DFORMAT format) noexcept {
    if (!d || !width || !height) return false;
    if (texture && (device != d || textureWidth != width || textureHeight != height || textureFormat != format)) release();
    if (!texture && FAILED(d->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, &texture, nullptr))) {
        texture = nullptr;
        return false;
    }
    device = d; textureWidth = width; textureHeight = height; textureFormat = format;
    return true;
}
void beginPass(IDirect3DDevice9 *d, IDirect3DSurface9 *target, const double (&plane)[4], const Vec3 &camera) noexcept {
    passTarget = target;
    passCamera = camera;
    passActive = d && target && single_pass::installed();
    cameraRecorded = captured = false;
    std::memcpy(passPlane, plane, sizeof(passPlane));
    if (passActive) ++stats.passes;
}
void endPass() noexcept { passActive = false; passTarget = nullptr; }
bool drawing(IDirect3DDevice9 *d) noexcept {
    if (!passActive || !d) return false;
    IDirect3DSurface9 *current = nullptr;
    if (FAILED(d->GetRenderTarget(0, &current)) || !current) return false;
    const bool ours = current == passTarget;
    current->Release();
    return ours;
}
bool clipBegin(IDirect3DDevice9 *d, bool &skip) noexcept {
    skip = false;
    ++stats.draws;
    if (single_pass::waterBound()) { skip = true; ++stats.skippedWater; return false; }
    const float *adjusted = nullptr, *view = nullptr;
    if (!single_pass::engineCamera(adjusted, view)) { ++stats.noCamera; return false; }
    const M4 ap = ::ss2vr::replay::m4(adjusted, 4);
    // Perspective world draws only: fills, light-space images and the sky's own
    // camera (at the origin) keep everything.
    if (!::ss2vr::replay::perspective(::ss2vr::replay::m4(single_pass::engineProjectionMatrix(), 4))) { ++stats.flat; return false; }
    // The sky's own camera sits far from the mirror camera (kilometres below the world).
    const Vec3 c{-(view[0] * view[3] + view[4] * view[7] + view[8] * view[11]), -(view[1] * view[3] + view[5] * view[7] + view[9] * view[11]),
                 -(view[2] * view[3] + view[6] * view[7] + view[10] * view[11])};
    const Vec3 offset = c - passCamera;
    if (dot(offset, offset) > 1.f) { ++stats.sky; return false; }
    const M4 worldToClip = ::ss2vr::replay::multiply(ap, ::ss2vr::replay::m4(view, 3));
    if (!cameraRecorded) { passWorldToClip = worldToClip; cameraRecorded = true; }
    ++stats.clipped; // The oblique near plane (engine.cpp) clips it at the water.
    return false;
}
void clipEnd(IDirect3DDevice9 *) noexcept {}
bool capture(IDirect3DDevice9 *d, IDirect3DSurface9 *target, Copy copy) noexcept {
    if (!texture || !target || !copy || d != device) return false;
    IDirect3DSurface9 *level = nullptr;
    if (FAILED(texture->GetSurfaceLevel(0, &level)) || !level) return false;
    captured = SUCCEEDED(copy(d, target, nullptr, level, nullptr, D3DTEXF_LINEAR));
    stats.captured += captured;
    level->Release();
    return captured;
}
void publish(bool complete) noexcept {
    ready = complete && captured && cameraRecorded && texture;
    if (!ready) return;
    std::memcpy(readyPlane, passPlane, sizeof(readyPlane));
    readyWorldToClip = passWorldToClip;
    ++stats.published;
    // Lab (SS2VR_LAB_MIRROR_DUMP=1): the 300th mirror image, once, as Bin/SS2VR/water-mirror.ppm.
    static const bool dump = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_MIRROR_DUMP", v, 2) == 1 && v[0] == L'1'; }();
    static unsigned published = 0;
    if (dump && ++published == 300) {
        IDirect3DSurface9 *level = nullptr, *read = nullptr;
        D3DLOCKED_RECT locked{};
        if (SUCCEEDED(texture->GetSurfaceLevel(0, &level)) &&
            SUCCEEDED(device->CreateOffscreenPlainSurface(textureWidth, textureHeight, textureFormat, D3DPOOL_SYSTEMMEM, &read, nullptr)) &&
            SUCCEEDED(device->GetRenderTargetData(level, read)) && SUCCEEDED(read->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
            wchar_t path[MAX_PATH]{};
            DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
            while (n && path[n - 1] != L'\\' && path[n - 1] != L'/') --n;
            path[n] = 0;
            if (FILE *file = _wfopen((std::wstring(path) + L"SS2VR\\water-mirror.ppm").c_str(), L"wb")) {
                std::fprintf(file, "P6\n%u %u\n255\n", textureWidth, textureHeight);
                for (UINT y = 0; y < textureHeight; ++y) {
                    const auto *row = static_cast<const uint8_t *>(locked.pBits) + size_t(y) * locked.Pitch;
                    for (UINT x = 0; x < textureWidth; ++x) { const uint8_t rgb[3]{row[4 * x + 2], row[4 * x + 1], row[4 * x]}; std::fwrite(rgb, 1, 3, file); }
                }
                std::fclose(file);
                log("Water mirror image written (%ux%u)", textureWidth, textureHeight);
            }
            read->UnlockRect();
        }
        if (read) read->Release();
        if (level) level->Release();
    }
}
bool image(const double (&plane)[4], IDirect3DTexture9 *&out, M4 &worldToClip) noexcept {
    if (!ready || !texture || !ss2vr::water_mirror::samePlane(readyPlane, plane)) return false;
    out = texture;
    worldToClip = readyWorldToClip;
    ++stats.used;
    return true;
}
void release() noexcept {
    if (texture) texture->Release();
    texture = nullptr; device = nullptr; ready = false; passActive = false; passTarget = nullptr;
}
Stats takeStats() noexcept { const Stats out = stats; stats = {}; return out; }
} // namespace ss2vr::game::water_mirror
