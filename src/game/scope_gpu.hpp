#pragma once
#include <d3d9.h>
#include <cstdint>
#include "scope_views.hpp"
namespace ss2vr::game {
using ScopeIndexedForward = HRESULT (*)(IDirect3DDevice9 *,D3DPRIMITIVETYPE,INT,UINT,UINT,UINT,UINT);
HRESULT scopeGpuDraw(IDirect3DDevice9 *,D3DPRIMITIVETYPE,INT,UINT,UINT,UINT,UINT,
                     uintptr_t caller,ScopeIndexedForward) noexcept;
bool scopeGpuForwardingAllowed() noexcept;
bool scopeGpuRoutingCurrent(IDirect3DDevice9 *) noexcept;
// Numeric-only observation check, including pair faults; safe after the
// invocation's device reference retires. Never calls the released device.
bool scopeGpuMappingObservationCurrent(IDirect3DDevice9 *) noexcept;
// Partial successful outputs are owned references; the caller retains them
// above NativeFinally and releases them on every path.
unsigned scopeColorFailure() noexcept;
const uint32_t *scopeColorStates() noexcept;
double scopeCapError() noexcept; // Diagnostic: last cap position error (clip-scaled).
unsigned scopeImageDraws() noexcept;
const float *scopeEyeInOptic() noexcept; // Diagnostic: last eye position in the optic frame. // Diagnostic: lens image draws so far.
bool scopeGpuEyeOwner(IDirect3DDevice9 *, IUnknown *&color, IUnknown *&depth, UINT &width, UINT &height,
                      UINT &surfaceWidth, UINT &surfaceHeight, bool &stereo) noexcept;
// Reuse the bridge's existing resource epoch and unconditional pair fault.
uint64_t graphicsResourceGeneration() noexcept;
void scopeGpuFault() noexcept;
// Borrowed frame outputs become owned references only when non-null. Caller
// holds them above NativeFinally and releases partial outputs on every path.
bool scopeGpuImage(IDirect3DDevice9 *, unsigned hand, ScopeSourceView &,
                   IDirect3DTexture9 *&, IDirect3DPixelShader9 *&) noexcept;
bool scopeGpuTransactionBegin(IDirect3DDevice9 *) noexcept;
bool scopeGpuTransactionCurrent(IDirect3DDevice9 *) noexcept;
void scopeGpuTransactionEnd() noexcept;
void scopeGpuOutput(IDirect3DDevice9 *) noexcept;
bool scopeImageEye(Vec3 &out) noexcept;
bool scopeImageEyes(Vec3 (&out)[2]) noexcept;
bool scopeImageTarget(unsigned hand,Vec3 &out) noexcept;
} // namespace ss2vr::game
