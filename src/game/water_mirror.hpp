#pragma once
// Planar water reflections ([Render] WaterMirror, common/water_mirror.hpp).
// Each frame the eye pass notes the planes its water was drawn on; before the
// next frame's eyes the world is rendered once more from the centre camera
// mirrored about the nearest of them (a scope-style source render into the eye
// scratch target, everything under the water clipped away, the water itself
// left out), copied into the mirror image, and that frame's water on that
// plane looks its reflection up there. Main thread only.
#include "common/replay_math.hpp"
#include <windows.h>
#include <d3d9.h>

namespace ss2vr::game::water_mirror {
using Copy = HRESULT(WINAPI *)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const RECT *,
                               D3DTEXTUREFILTERTYPE);
// Eye pass: a water draw on `plane`, seen from `camera` (world).
void noteWater(const double (&plane)[4], const Vec3 &camera) noexcept;
// The plane to mirror: of the water drawn within the last second, the nearest
// one below the camera.
bool framePlane(double (&plane)[4]) noexcept;
// Each frame before the mirror pass (or instead of it): last frame's image is spent.
void frameStart() noexcept;
// The mirror image (`width` x `height`, the eye target's format); false without one.
bool prepare(IDirect3DDevice9 *d, UINT width, UINT height, D3DFORMAT format) noexcept;
// Around the mirror pass's native render into `target`.
void beginPass(IDirect3DDevice9 *d, IDirect3DSurface9 *target, const double (&plane)[4], const Vec3 &camera) noexcept;
void endPass() noexcept;
// A draw belongs to the pass (active, render target 0 is its target).
bool drawing(IDirect3DDevice9 *d) noexcept;
// Issues one of the pass's draws: water is left out, perspective world draws
// get the user clip plane at the water (lowered a little), the first one
// records the mirror camera's world -> clip.
template<class Call> HRESULT around(IDirect3DDevice9 *d, Call call) noexcept;
bool clipBegin(IDirect3DDevice9 *d, bool &skip) noexcept; // Internal to around().
void clipEnd(IDirect3DDevice9 *d) noexcept;
// From the native pre-bloom capture: the target into the mirror image.
bool capture(IDirect3DDevice9 *d, IDirect3DSurface9 *target, Copy copy) noexcept;
// After the pass: the image is this frame's when the pass completed and captured.
void publish(bool complete) noexcept;
// Eye pass: this frame's image for water on `plane`, and the mirror camera's world -> clip.
bool image(const double (&plane)[4], IDirect3DTexture9 *&texture, ::ss2vr::replay::M4 &worldToClip) noexcept;
void release() noexcept; // Device reset or loss.
struct Stats { unsigned passes = 0, published = 0, clipped = 0, skippedWater = 0, used = 0, draws = 0, noCamera = 0, flat = 0, sky = 0, busy = 0, captured = 0; };
Stats takeStats() noexcept;

template<class Call> HRESULT around(IDirect3DDevice9 *d, Call call) noexcept {
    bool skip = false;
    const bool clipped = clipBegin(d, skip);
    if (skip) return D3D_OK;
    const HRESULT hr = call();
    if (clipped) clipEnd(d);
    return hr;
}
} // namespace ss2vr::game::water_mirror
