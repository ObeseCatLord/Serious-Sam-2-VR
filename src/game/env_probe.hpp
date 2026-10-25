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
// Rendered cube: once a frame, before the eyes, one face is rendered from the
// cube's anchor (bridge, a native source render) and copied into the cube.
// `viewer` (world) moves the anchor when far from it; the face to render next
// and the anchor are returned. False when no face is due.
bool nextFace(IDirect3DDevice9 *d, const Device &device, D3DFORMAT format, const float (&viewer)[3], unsigned &face,
              float (&anchor)[3]) noexcept;
// Around that face's render (its draws go to `target`): water is left out.
void beginFace(IDirect3DSurface9 *target, unsigned face) noexcept;
bool facePass(IDirect3DDevice9 *d) noexcept; // A draw of the face's render.
// From the render's pre-bloom capture: `target` into the face (mirrored to D3D's layout).
bool captureFace(IDirect3DDevice9 *d, const Device &device, IDirect3DSurface9 *target) noexcept;
void endFace(bool complete) noexcept;
// Faces are being rendered (the eye-view capture below is then not used).
bool rendered() noexcept;
// Fallback when faces cannot be rendered: projects `eye` (the left eye's world
// image, `width` x `height`), seen with `view` (world -> view, 3x4) and
// `projection` (4x4), into the cube.
void capture(IDirect3DDevice9 *d, const Device &device, IDirect3DSurface9 *eye, UINT width, UINT height, const float *view,
             const float *projection) noexcept;
// The cube once anything was captured into it on this device, else null.
IDirect3DCubeTexture9 *cube(IDirect3DDevice9 *d) noexcept;
// The cube's fixed world point (its surroundings lie on a sphere around it).
bool anchor(float (&out)[3]) noexcept;
// The measured distance of the surroundings from the anchor (metres): the
// sphere's radius eases toward it.
void setSurroundings(float metres) noexcept;
float radius() noexcept; // The sphere's radius now (metres).
void release() noexcept; // Device reset or loss: default-pool resources go.
} // namespace ss2vr::game::env_probe
