#pragma once
#include <windows.h>
#include <d3d9.h>

// Environment cube for water reflections (common/env_probe.hpp): rendered from
// a fixed world point, sampled by the patched water programs. Until it is
// rendered the water gets a neutral reflection; never the eye's view.
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
// The cube once it holds a complete render from its anchor on this device, else null.
IDirect3DCubeTexture9 *cube(IDirect3DDevice9 *d) noexcept;
// Reflection off: one texel of sky colour, the same from every direction and
// for every head pose. As a cube (for the water programs' cube variants) and as
// an image (for the engine's own programs: the cube's colour times `gain`, the
// factor the cube variants apply). Made on first use; null when that failed.
IDirect3DCubeTexture9 *neutralCube(IDirect3DDevice9 *d) noexcept;
IDirect3DTexture9 *neutralImage(IDirect3DDevice9 *d, float gain) noexcept;
// The cube's fixed world point (its surroundings lie on a sphere around it).
bool anchor(float (&out)[3]) noexcept;
// The measured distance of the surroundings from the anchor (metres): the
// sphere's radius eases toward it.
void setSurroundings(float metres) noexcept;
float radius() noexcept; // The sphere's radius now (metres).
// The distance (metres, along each face's view) nearer than which the face being
// rendered leaves scenery out.
float nearCut() noexcept;
// Where the surroundings are measured from: the anchor being prepared, else the cube's.
bool probeAnchor(float (&out)[3]) noexcept;
void release() noexcept; // Device reset or loss: default-pool resources go.
} // namespace ss2vr::game::env_probe
