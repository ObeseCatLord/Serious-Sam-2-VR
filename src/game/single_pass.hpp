#pragma once
#include <windows.h>
#include <d3d9.h>
#include "common/replay_math.hpp"

// Single-pass stereo: the world is traversed once from a centre camera into a
// double-wide target. Every draw made while the pass is active is issued once
// per eye half with that eye's clip-space reprojection supplied to a patched
// vertex program (common/shader_patch.hpp) on stream 15.
namespace ss2vr::game::single_pass {
using Draw = HRESULT(WINAPI *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexed = HRESULT(WINAPI *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using Clear = HRESULT(WINAPI *)(IDirect3DDevice9 *, DWORD, const D3DRECT *, DWORD, D3DCOLOR, float, DWORD);
struct Originals { Draw draw = nullptr; DrawIndexed drawIndexed = nullptr; Clear clear = nullptr; };
// Hooks shader/declaration binding, viewport, scissor and render states.
bool install(IDirect3DDevice9 *device, const Originals &originals) noexcept;
bool installed() noexcept;
// Engine globals: adjusted projection, projection, view (read at draw time).
void setEngineMatrices(const float *adjusted, const float *projection, const float *view) noexcept;
// Starts the pass on `target` (2W x H): `change[i]` maps the centre camera's
// view/frustum to eye i's.
bool begin(IDirect3DDevice9 *device, IDirect3DSurface9 *target, UINT eyeWidth, const ss2vr::replay::EyeChange (&change)[2]) noexcept;
void end(IDirect3DDevice9 *device) noexcept;
// Whether a draw on this device belongs to the pass (active and RT0 is the wide target).
bool drawing(IDirect3DDevice9 *device) noexcept;
HRESULT draw(IDirect3DDevice9 *d, D3DPRIMITIVETYPE type, UINT start, UINT count) noexcept;
HRESULT drawIndexed(IDirect3DDevice9 *d, D3DPRIMITIVETYPE type, INT base, UINT minimum, UINT vertices, UINT start, UINT count) noexcept;
HRESULT clear(IDirect3DDevice9 *d, DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) noexcept;
struct Stats {
    unsigned passes = 0, draws = 0, unpatched = 0, stateChanges = 0, variants = 0, failedVariants = 0;
};
Stats takeStats() noexcept;
} // namespace ss2vr::game::single_pass
