#pragma once
#include <windows.h>
#include <d3d9.h>
#include "common/replay_math.hpp"
#include <vector>

// Single-pass stereo: the world is traversed once from a centre camera into a
// double-wide target. Every draw made while the pass is active is issued once
// per eye half with that eye's clip-space reprojection supplied to a patched
// vertex program (common/shader_patch.hpp) on stream 15.
namespace ss2vr::game::single_pass {
using Draw = HRESULT(WINAPI *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexed = HRESULT(WINAPI *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawUp = HRESULT(WINAPI *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *, UINT);
using DrawIndexedUp = HRESULT(WINAPI *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *, D3DFORMAT, const void *, UINT);
using Clear = HRESULT(WINAPI *)(IDirect3DDevice9 *, DWORD, const D3DRECT *, DWORD, D3DCOLOR, float, DWORD);
struct Originals {
    Draw draw = nullptr; DrawIndexed drawIndexed = nullptr; DrawUp drawUp = nullptr; DrawIndexedUp drawIndexedUp = nullptr;
    Clear clear = nullptr;
};
// Hooks shader/declaration binding, viewport, scissor and render states.
bool install(IDirect3DDevice9 *device, const Originals &originals) noexcept;
bool installed() noexcept;
void setInstancing(bool enabled) noexcept; // [Render] SinglePassInstancing
// Draws made while set show the centre view in every eye (no per-eye
// reprojection): camera-attached effects, which two-pass renders draw in front
// of each eye's own camera.
void setScreenLocked(bool locked) noexcept;
// The root view (world -> view, row-major 3x4) the pass renders the world with.
// Views with their camera elsewhere (the sky's background viewer) get eye
// rotation only, as each eye's own camera would render them.
void setMainView(const float *view) noexcept;
// Releases the pass's default-pool resources (before a device Reset).
void releaseDeviceResources() noexcept;
// Engine globals: adjusted projection, projection, view (read at draw time).
void setEngineMatrices(const float *adjusted, const float *projection, const float *view) noexcept;
// Where the eyes go in the pass's target. Each region is a pixel rectangle of
// the target showing part of one eye's view: eye NDC (x, y) maps to region NDC
// (sx x + ox, sy y + oy). The default layout is the double-wide target (2W x H,
// each eye's full view in one half); a foveated layout adds a full-resolution
// inset per eye beside low-resolution full views.
constexpr unsigned MaxRegions = 4;
struct Region {
    RECT rect{};
    unsigned eye = 0;
    float sx = 1, ox = 0, sy = 1, oy = 0;
    bool scaled = true; // The eye's full view (only scaled): programs without a stereo variant draw here.
};
struct Layout {
    UINT width = 0, height = 0;
    unsigned count = 0;
    Region region[MaxRegions];
};
// Starts the pass on `target`: `change[i]` maps the centre camera's view/frustum
// to eye i's (W x H eyes); `layout` null for the double-wide target.
bool begin(IDirect3DDevice9 *device, IDirect3DSurface9 *target, UINT eyeWidth, UINT eyeHeight,
           const ss2vr::replay::EyeChange (&change)[2], const Layout *layout = nullptr) noexcept;
void end(IDirect3DDevice9 *device) noexcept;
// Whether a draw on this device belongs to the pass (active and RT0 is the wide target).
bool drawing(IDirect3DDevice9 *device) noexcept;
// The active pass's target size and each eye's (logical) size.
bool passExtent(UINT &targetWidth, UINT &targetHeight, UINT &eyeWidth, UINT &eyeHeight) noexcept;
// Pixel constants that differ per eye (the scope lens's eye position): set in
// each eye's region before its draw, for draws issued while set. Null clears.
// Also keeps those draws off the instanced path (one draw cannot carry both).
void setEyePixelConstants(UINT start, UINT count, const float *left, const float *right) noexcept;
// Keeps draws on the per-region path (the native scope cap the lens is drawn over
// with an EQUAL depth test must take the same path as the lens).
void setInstancingSuspended(bool suspended) noexcept;
// Around the engine's SetRenderTarget: the pass restores the engine's state
// first, and suspends its stereo bindings while another target is bound.
void beforeRenderTarget(IDirect3DDevice9 *device, DWORD index) noexcept;
void afterRenderTarget(IDirect3DDevice9 *device, DWORD index, IDirect3DSurface9 *target) noexcept;
// The engine state the pass tracks, for the water mirror pass (water_mirror.hpp):
// the bound vertex program is water, the live engine matrices, the sky's own
// camera, a program (not fixed function) bound, the engine's user plane mask,
// and the device's clip plane/render state set past the pass's own tracking.
bool waterBound() noexcept;
bool engineCamera(const float *&adjusted, const float *&view) noexcept;
const float *engineProjectionMatrix() noexcept;
bool backgroundBound() noexcept;
bool programBound() noexcept;
DWORD engineClipPlanes() noexcept;
void rawClipPlane(IDirect3DDevice9 *d, DWORD index, const float *plane) noexcept;
void rawRenderState(IDirect3DDevice9 *d, D3DRENDERSTATETYPE state, DWORD value) noexcept;
HRESULT draw(IDirect3DDevice9 *d, D3DPRIMITIVETYPE type, UINT start, UINT count) noexcept;
HRESULT drawIndexed(IDirect3DDevice9 *d, D3DPRIMITIVETYPE type, INT base, UINT minimum, UINT vertices, UINT start, UINT count) noexcept;
HRESULT drawUp(IDirect3DDevice9 *d, D3DPRIMITIVETYPE type, UINT count, const void *data, UINT stride) noexcept;
HRESULT drawIndexedUp(IDirect3DDevice9 *d, D3DPRIMITIVETYPE type, UINT minimum, UINT vertices, UINT count, const void *indices,
                      D3DFORMAT format, const void *data, UINT stride) noexcept;
HRESULT clear(IDirect3DDevice9 *d, DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) noexcept;
struct Stats {
    unsigned passes = 0, draws = 0, instanced = 0, unpatched = 0, stateChanges = 0, variants = 0, failedVariants = 0, suspensions = 0, userPointer = 0, screenLocked = 0, scissors = 0, farExtended = 0;
};
Stats takeStats() noexcept;
// Lab: instance only eligible draws with index in [lo, hi) (counted per pass);
// logs the programs of eligible draw `detail`.
void labInstanceRange(unsigned lo, unsigned hi, unsigned detail) noexcept;
unsigned labEligibleCount() noexcept;
// Lab: pixel programs (bytecode hashes) seen in instanced draws, and an allow list
// (empty vector = all) restricting which may instance.
std::vector<uint32_t> labInstancedPixelShaders() noexcept;
void labAllowPixelShaders(const std::vector<uint32_t> &hashes, bool restrict) noexcept;
IDirect3DSurface9 *labMirrorTarget() noexcept; // Mirror image of the last pass (lab).
} // namespace ss2vr::game::single_pass
