#pragma once
#include <windows.h>
#include <d3d9.h>

// Environment cube for water reflections (common/env_probe.hpp): filled from
// each frame's left-eye world image, sampled by the patched water programs.
namespace ss2vr::game::env_probe {
struct Device { // The device's original (unhooked) entry points.
    HRESULT(WINAPI *setRT)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *) = nullptr;
    HRESULT(WINAPI *drawUp)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *, UINT) = nullptr;
    HRESULT(WINAPI *stretch)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const RECT *,
                             D3DTEXTUREFILTERTYPE) = nullptr;
};
// Water was drawn: the cube is kept up to date for a while.
void noteWater() noexcept;
bool wanted() noexcept;
// Projects `eye` (the left eye's world image, `width` x `height`), seen with
// `view` (world -> view, 3x4) and `projection` (4x4), into the cube.
void capture(IDirect3DDevice9 *d, const Device &device, IDirect3DSurface9 *eye, UINT width, UINT height, const float *view,
             const float *projection) noexcept;
// The cube once anything was captured into it on this device, else null.
IDirect3DCubeTexture9 *cube(IDirect3DDevice9 *d) noexcept;
void release() noexcept; // Device reset or loss: default-pool resources go.
} // namespace ss2vr::game::env_probe
