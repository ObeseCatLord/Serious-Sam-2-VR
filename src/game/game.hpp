#pragma once
#include "common/ipc.hpp"
#include "common/math.hpp"
#include "common/ride_render_observation.hpp"
#include "common/ride_grip_frame.hpp"
#include <atomic>
#include <d3d9.h>
namespace ss2vr::game {
extern Channel channel;
extern std::atomic<bool> hooksReady;
void log(const char *fmt, ...);
bool attach(bool headless = false);
// Metadata only; reentered input boundaries contain GNU errors before crossing
// native frames. The enclosing simulation finally owns interval retirement.
void nativeInputFailed() noexcept;
bool nativeInputHealthy() noexcept;
bool supported(bool headless = false);
bool nativeModuleFingerprint(HMODULE,const char *);
// Explicit private-lab opt-in only. Install before the native online initializer;
// normal game/Steam behavior is untouched when the opt-in is absent.
void installLabOnlineIsolation();
void validateLabOnlineIsolation(bool gameplay);
bool labOnlineIsolationInstalled() noexcept;
bool foregroundGame();
bool vrInputLive() noexcept;
// Native eye canvas at the runtime's recommended size (installed before the
// engine creates its window canvas; applied on the main thread's input poll).
void installEyeCanvas() noexcept;
void applyEyeCanvas() noexcept;
void deviceReady(IDirect3DDevice9 *device);
void deviceCreated(IDirect3DDevice9 *device);
void deviceResetSucceeded(IDirect3DDevice9 *device);
bool nativeUiDeviceCurrent(IDirect3DDevice9 *device);
// Read-only presentation extent gate for a separately proven main-thread body owner.
bool nativePresentationIdleForBodyMove() noexcept;
void deviceLost();
void invalidateRenderer();
void present(IDirect3DDevice9 *device);
bool nativePresentationThreadCurrent() noexcept;
bool nativeRenderExtentCurrent() noexcept;
void presentChain(IDirect3DSwapChain9 *, uintptr_t caller, const RECT *, const RECT *, HWND,
                  const RGNDATA *, DWORD flags) noexcept;
void presentDevice(IDirect3DDevice9 *, uintptr_t caller, const RECT *, const RECT *, HWND,
                   const RGNDATA *) noexcept;
bool nativePresentationCaller(uintptr_t caller, bool chain) noexcept;
void nestedNativePresentation() noexcept;
void nativePresentationFailed(IDirect3DDevice9 *, IDirect3DSwapChain9 *) noexcept;
// Bounded read-only startup probe. Does not admit a canvas or start capture.
bool startupDeviceOwner(IDirect3DDevice9 *) noexcept;
bool presentationDeviceOwner(IDirect3DDevice9 *) noexcept;
uint32_t traceChainPresent(IDirect3DSwapChain9 *, uintptr_t caller, HWND overrideWindow) noexcept;
bool nativeUiProgramsCurrent(IDirect3DVertexShader9 *, IDirect3DPixelShader9 *);
bool copyExecutedUiProjection(void *player, const Request &, int index, Matrix44 &out);
// Value-only current root camera for a borrowed stereo vehicle draw. Mono,
// nested views, weapon passes and retired graphics owners decline.
bool copyExecutedRideCamera(uint32_t player,uint64_t generation,int eye,RideCameraCopy &out);
// Caller holds the native snapshot/bank owner. Value-copy only; no native getter.
void recordRideGripFrame(const RideGripFrame &);
bool nativeUiFrameCurrent(void *player, const Request &);
bool nativeUiOwnerCurrent(void *player);
// Fresh reciprocal ownership within the current original Render3D extent.
// No lifetime is retained by the copied result; only two pinned hover families.
bool copyNativeRideRenderIdentity(uint32_t expectedPlayer,RideRenderIdentity &out);
int nativeWorldEye() noexcept;
void nativeUiFault(const char *reason = "unspecified") noexcept;
bool nativeUiBeginOverlay(void *player, bool admitted);
void nativeUiEndOverlay(bool completed) noexcept;
bool nativeUiBeginFade();
void nativeUiEndFade(bool completed) noexcept;
void nativeUiFinishOwner();
void nativeUiEndOwner(bool aborted) noexcept;
using EyePostRender = bool (*)(void *, const Request &, int);
void stereo(void *puppet, void(__thiscall *original)(void *), EyePostRender postRender = nullptr);
void shutdown();
} // namespace ss2vr::game
