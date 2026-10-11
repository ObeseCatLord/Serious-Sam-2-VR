#include "scope_source_gpu.hpp"
#include "scope_gpu.hpp"
#include "game.hpp"
#include "native_finally.hpp"
#include "common/scope_source_layout.hpp"

namespace ss2vr::game {
namespace {
static_assert(D3DRTYPE_SURFACE == 1 && D3DPOOL_DEFAULT == 0 && D3DUSAGE_RENDERTARGET == 1 &&
              D3DMULTISAMPLE_NONE == 0 && D3DFMT_A8R8G8B8 == 21 && D3DFMT_X8R8G8B8 == 22);
template<class T> void release(T *&value) noexcept {
    T *owned = value; value = nullptr;
    if (owned) owned->Release();
}
struct CaptureOwner {
    IDirect3DSurface9 *source = nullptr, *target = nullptr, *after = nullptr, *extra = nullptr;
    IDirect3DDevice9 *sourceDevice = nullptr, *targetDevice = nullptr;
    IUnknown *sourceId = nullptr, *expectedId = nullptr, *targetId = nullptr, *afterId = nullptr;
    IUnknown *deviceId = nullptr, *sourceDeviceId = nullptr, *targetDeviceId = nullptr;
    D3DVIEWPORT9 viewport{};
    bool copied = false;
    void cleanup() noexcept {
        release(targetDeviceId); release(sourceDeviceId); release(deviceId);
        release(targetDevice); release(sourceDevice);
        release(afterId); release(targetId); release(expectedId); release(sourceId);
        release(extra); release(after); release(target); release(source);
    }
};
bool identity(IUnknown *object, IUnknown *&out) {
    return object && SUCCEEDED(object->QueryInterface(IID_IUnknown, reinterpret_cast<void **>(&out))) && out;
}
bool viewportValid(const D3DVIEWPORT9 &v, UINT width, UINT height) noexcept {
    return scopeSourceViewport({v.X,v.Y,v.Width,v.Height,v.MinZ,v.MaxZ},width,height);
}
bool sameViewport(const D3DVIEWPORT9 &a, const D3DVIEWPORT9 &b) noexcept {
    return a.X == b.X && a.Y == b.Y && a.Width == b.Width && a.Height == b.Height &&
           a.MinZ == b.MinZ && a.MaxZ == b.MaxZ;
}
ScopeSourceSurface surface(const D3DSURFACE_DESC &s) noexcept {
    return {uint32_t(s.Type),uint32_t(s.Pool),s.Width,s.Height,s.Usage,
            uint32_t(s.MultiSampleType),s.MultiSampleQuality,uint32_t(s.Format)};
}
} // namespace

bool captureScopeSource(IDirect3DDevice9 *device, IDirect3DSurface9 *expectedSource,
                        IDirect3DTexture9 *destination, UINT width, UINT height,
                        ScopeSourceCopy copy, ScopeSourceCurrent current, void *context) noexcept {
    if (!device || !expectedSource || !destination || !width || !height || !copy || !current ||
        !current(context) || !scopeGpuRoutingCurrent(device) || !scopeGpuForwardingAllowed()) return false;
    // All partial COM results remain owned above the foreign-finally boundary.
    CaptureOwner owner;
    const bool completed = withNativeFinally([&] {
        // D3D9 may silently return S_OK while discarding work on a lost device.
        // Successful getters/copy alone therefore do not admit source pixels.
        if (!nativeUiDeviceCurrent(device) || device->TestCooperativeLevel() != D3D_OK ||
            destination->GetType() != D3DRTYPE_TEXTURE || destination->GetLevelCount() < 1 ||
            FAILED(destination->GetSurfaceLevel(0, &owner.target)) || !owner.target ||
            FAILED(device->GetRenderTarget(0, &owner.source)) || !owner.source ||
            !identity(expectedSource, owner.expectedId) || !identity(owner.source, owner.sourceId) ||
            !identity(owner.target, owner.targetId) || owner.sourceId != owner.expectedId ||
            owner.targetId == owner.sourceId || FAILED(device->GetViewport(&owner.viewport)) ||
            !viewportValid(owner.viewport, width, height)) return;
        D3DSURFACE_DESC source{}, target{};
        if (FAILED(owner.source->GetDesc(&source)) || FAILED(owner.target->GetDesc(&target)) ||
            !scopeSourcePair(surface(source),surface(target),width,height)) return;
        if (FAILED(owner.source->GetDevice(&owner.sourceDevice)) || !owner.sourceDevice ||
            FAILED(owner.target->GetDevice(&owner.targetDevice)) || !owner.targetDevice ||
            !identity(device, owner.deviceId) || !identity(owner.sourceDevice, owner.sourceDeviceId) ||
            !identity(owner.targetDevice, owner.targetDeviceId) ||
            owner.sourceDeviceId != owner.deviceId || owner.targetDeviceId != owner.deviceId) return;
        D3DCAPS9 caps{};
        if (FAILED(device->GetDeviceCaps(&caps)) || !caps.NumSimultaneousRTs || caps.NumSimultaneousRTs > 4)
            return;
        for (DWORD i = 1; i < caps.NumSimultaneousRTs; ++i) {
            const HRESULT result = device->GetRenderTarget(i, &owner.extra);
            const bool bound = owner.extra != nullptr;
            release(owner.extra);
            if (bound || (FAILED(result) && result != D3DERR_NOTFOUND)) return;
        }
        if (!current(context) || !scopeGpuRoutingCurrent(device) || !scopeGpuForwardingAllowed() ||
            device->TestCooperativeLevel() != D3D_OK) return;
        if (FAILED(copy(device, owner.source, owner.target))) return;
        // A successful copy is not enough if its source/view owner changed.
        // No fallback or retry can relabel a partial/newer image as this one.
        D3DVIEWPORT9 after{};
        if (FAILED(device->GetRenderTarget(0, &owner.after)) || !owner.after ||
            !identity(owner.after, owner.afterId) || owner.afterId != owner.sourceId ||
            FAILED(device->GetViewport(&after)) || !sameViewport(after, owner.viewport)) return;
        owner.copied = device->TestCooperativeLevel() == D3D_OK && current(context) &&
                       scopeGpuRoutingCurrent(device) && scopeGpuForwardingAllowed();
    }, [&](bool aborted) noexcept {
        if (aborted) owner.copied = false;
        owner.cleanup();
    });
    return completed && owner.copied && current(context) && scopeGpuRoutingCurrent(device) &&
           scopeGpuForwardingAllowed();
}
} // namespace ss2vr::game
