#pragma once
#include <windows.h>
#include <d3d9.h>
#include "common/replay_math.hpp"

// Second-eye replay: the left eye's native world render is recorded at the
// D3D9 call level and replayed into the right eye's targets with the engine's
// view-dependent matrix uploads rewritten (common/replay_math.hpp). Any hazard
// (vertex/index data overwritten, an unknown failure) invalidates the
// recording and the caller renders the right eye natively instead.
namespace ss2vr::game::replay {
using SetRT = HRESULT(WINAPI *)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *);
using SetDepth = HRESULT(WINAPI *)(IDirect3DDevice9 *, IDirect3DSurface9 *);
using Clear = HRESULT(WINAPI *)(IDirect3DDevice9 *, DWORD, const D3DRECT *, DWORD, D3DCOLOR, float, DWORD);
using Stretch = HRESULT(WINAPI *)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *,
                                  const RECT *, D3DTEXTUREFILTERTYPE);
using Fill = HRESULT(WINAPI *)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, D3DCOLOR);
using Draw = HRESULT(WINAPI *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexed = HRESULT(WINAPI *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
struct Originals {
    SetRT setRT = nullptr; SetDepth setDepth = nullptr; Clear clear = nullptr; Stretch stretch = nullptr;
    Fill fill = nullptr; Draw draw = nullptr; DrawIndexed drawIndexed = nullptr;
};
// Hooks the remaining state setters and buffer locks (once per device class).
bool install(IDirect3DDevice9 *device, const Originals &originals) noexcept;
// Engine globals read when a matrix upload is tagged (adjusted projection,
// projection, view); set once at engine attach.
void setEngineMatrices(const float *adjusted, const float *projection, const float *view) noexcept;
bool begin(IDirect3DDevice9 *device, IDirect3DSurface9 *target, IDirect3DSurface9 *depth) noexcept;
void end() noexcept;
bool usable() noexcept;
bool play(IDirect3DDevice9 *device, IDirect3DSurface9 *target, IDirect3DSurface9 *depth,
          const ss2vr::replay::EyeChange &change, const ss2vr::replay::ViewState &root) noexcept;
void reset() noexcept;
bool recording() noexcept;
// Recording entry points for calls the bridge already hooks (post-mapping values).
void recordRenderTarget(DWORD index, IDirect3DSurface9 *surface) noexcept;
void recordDepth(IDirect3DSurface9 *surface) noexcept;
void recordClear(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) noexcept;
void recordStretch(IDirect3DSurface9 *source, const RECT *sourceRect, IDirect3DSurface9 *target,
                   const RECT *targetRect, D3DTEXTUREFILTERTYPE filter) noexcept;
void recordFill(IDirect3DSurface9 *surface, const RECT *rect, D3DCOLOR color) noexcept;
void recordDraw(D3DPRIMITIVETYPE type, UINT start, UINT count) noexcept;
void recordDrawIndexed(D3DPRIMITIVETYPE type, INT base, UINT minimum, UINT vertices, UINT start, UINT count) noexcept;
void invalidate(const char *reason) noexcept; // A call that cannot be replayed (UP draws, readbacks, ...).
// Engine gfxBindMatrix bracket: the vertex constant upload inside is tagged.
void matrixBegin(unsigned type) noexcept;
void matrixEnd() noexcept;
struct Stats {
    unsigned recorded = 0, replayed = 0, invalidated = 0, commands = 0, draws = 0, tagged = 0;
    double recordMs = 0, replayMs = 0;
    const char *lastReason = "none";
};
Stats takeStats() noexcept;
// Lab bisection: the next play() skips draws [lo, hi) (counting Draw and
// DrawIndexed) and logs the constants of draw `detail` (~0u for none).
void labSkipDraws(unsigned lo, unsigned hi, unsigned detail) noexcept;
unsigned labDrawCount() noexcept;
} // namespace ss2vr::game::replay
