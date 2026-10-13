#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_NO_PROTOTYPES
#include "common/color.hpp"
#include "common/controls.hpp"
#include "common/frame_policy.hpp"
#include "common/head_clearance.hpp"
#include "common/native_ui_finish.hpp"
#include "common/ipc.hpp"
#include "common/math.hpp"
#include "common/rider.hpp"
#include "common/ui.hpp"
#include "common/weapon_names.hpp"
#include "common/win_settings.hpp"
#include "common/world_submission.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <d3d11.h>
#include <dxgi.h>
#include <exception>
#include <limits>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>

// No engine pointers or gameplay policy live here. The game owns slot Rendering,
// controls, inventory, and wheel selection; this process owns OpenXR presentation.
namespace {
using namespace ss2vr;
static_assert(sizeof(void *) == 8, "The OpenXR host must be built for x64");
constexpr XrDuration ImageWaitNs = 2'000'000;
constexpr ULONGLONG UiFreshMs = 250;
constexpr uint64_t CachedPairHoldMs = 1000;

template <class T> struct Com {
    T *p = nullptr;
    ~Com() {
        reset();
    }
    Com() = default;
    Com(const Com &) = delete;
    Com &operator=(const Com &) = delete;
    T **put() {
        reset();
        return &p;
    }
    T *operator->() const {
        return p;
    }
    void reset() {
        if (p)
            p->Release();
        p = nullptr;
    }
    void abandon() {
        p = nullptr;
    } // Only for a hung GPU at process teardown.
};
struct Handle {
    HANDLE h = nullptr;
    ~Handle() {
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
};
struct Library {
    HMODULE h = nullptr;
    ~Library() {
        if (h)
            FreeLibrary(h);
    }
};

std::wstring executableDirectory() {
    std::vector<wchar_t> path(32768);
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    if (!n || n >= path.size())
        throw std::runtime_error("Cannot locate host directory");
    std::wstring value(path.data(), n);
    const auto slash = value.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        throw std::runtime_error("Host path has no directory");
    return value.substr(0, slash + 1);
}

struct Log {
    Handle file;
    DWORD written = 0;
    explicit Log(const std::wstring &directory) {
        file.h = CreateFileW((directory + L"ss2vr_host.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    void write(const char *message) noexcept {
        // Bounded, concise diagnostics only: no channel token, paths or pose logs.
        if (file.h == INVALID_HANDLE_VALUE || !file.h || written >= 256 * 1024)
            return;
        DWORD n = 0;
        WriteFile(file.h, message, DWORD(std::strlen(message)), &n, nullptr);
        written += n;
        WriteFile(file.h, "\r\n", 2, &n, nullptr);
        written += n;
    }
    void result(const char *operation, long long result) noexcept {
        char line[256];
        std::snprintf(line, sizeof(line), "%s: %lld", operation, result);
        write(line);
    }
};

// Channel::lock does not handle WAIT_ABANDONED. Use this narrow guard without
// changing the frozen IPC header; an abandoned writer invalidates the channel.
// Long IPC mutex holds stall the game's pair publication (diagnostics).
struct MutexStats {
    uint64_t longHolds = 0;
    double maxHoldMs = 0;
    uintptr_t site = 0;
};
inline MutexStats mutexStats;
struct Mutex {
    Channel &channel;
    bool held = false;
    LARGE_INTEGER acquired{};
    uintptr_t site = 0;
    __attribute__((noinline)) Mutex(Channel &c, DWORD wait) : channel(c) {
        site = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
        const DWORD result = WaitForSingleObject(c.mutex, wait);
        QueryPerformanceCounter(&acquired);
        if (result == WAIT_ABANDONED) {
            ReleaseMutex(c.mutex);
            throw std::runtime_error("IPC mutex abandoned; refusing possibly partial payload");
        }
        if (result == WAIT_FAILED)
            throw std::runtime_error("IPC mutex wait failed");
        held = result == WAIT_OBJECT_0;
    }
    ~Mutex() {
        if (!held)
            return;
        LARGE_INTEGER now{}, frequency{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        const double ms = 1000. * double(now.QuadPart - acquired.QuadPart) / double(frequency.QuadPart);
        if (ms > 1) {
            ++mutexStats.longHolds;
            if (ms > mutexStats.maxHoldMs) { mutexStats.maxHoldMs = ms; mutexStats.site = site; }
        }
        ReleaseMutex(channel.mutex);
    }
    explicit operator bool() const {
        return held;
    }
};

#define XR_INSTANCE_FUNCTIONS(X)                                                                             \
    X(DestroyInstance)                                                                                       \
    X(GetInstanceProperties) X(PathToString)                                                                 \
    X(GetSystem)                                                                                             \
    X(GetSystemProperties)                                                                                   \
    X(EnumerateViewConfigurations)                                                                           \
    X(EnumerateViewConfigurationViews)                                                                       \
    X(EnumerateEnvironmentBlendModes)                                                                        \
    X(GetD3D11GraphicsRequirementsKHR)                                                                       \
    X(CreateSession)                                                                                         \
    X(DestroySession)                                                                                        \
    X(BeginSession)                                                                                          \
    X(EndSession)                                                                                            \
    X(PollEvent)                                                                                             \
    X(CreateReferenceSpace) X(DestroySpace) X(LocateSpace) X(EnumerateSwapchainFormats) X(CreateSwapchain)   \
        X(DestroySwapchain) X(EnumerateSwapchainImages) X(AcquireSwapchainImage) X(WaitSwapchainImage)       \
            X(ReleaseSwapchainImage) X(WaitFrame) X(BeginFrame) X(EndFrame) X(LocateViews) X(StringToPath)   \
                X(CreateActionSet) X(DestroyActionSet) X(CreateAction) X(SuggestInteractionProfileBindings)  \
                    X(AttachSessionActionSets) X(CreateActionSpace) X(ApplyHapticFeedback)                   \
                        X(StopHapticFeedback) X(SyncActions) X(GetActionStatePose) X(GetActionStateBoolean)  \
                            X(GetActionStateFloat) X(GetActionStateVector2f) X(GetCurrentInteractionProfile)

struct Api {
    Library loader;
    Log &log;
    bool lossPending = false;
    DXGI_FORMAT colorFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    PFN_xrGetInstanceProcAddr GetInstanceProcAddr = nullptr;
    PFN_xrEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties = nullptr;
    PFN_xrCreateInstance CreateInstance = nullptr;
#define DECLARE(name) PFN_xr##name name = nullptr;
    XR_INSTANCE_FUNCTIONS(DECLARE)
#undef DECLARE
    explicit Api(Log &l) : log(l) {}
    void check(XrResult result, const char *operation) {
        if (result == XR_SESSION_LOSS_PENDING)
            lossPending = true;
        if (XR_FAILED(result)) {
            log.result(operation, result);
            throw std::runtime_error(operation);
        }
    }
    void cleanup(XrResult result, const char *operation) noexcept {
        if (XR_FAILED(result))
            log.result(operation, result);
    }
    template <class T> void load(T &destination, XrInstance instance, const char *name) {
        PFN_xrVoidFunction fn = nullptr;
        check(GetInstanceProcAddr(instance, name, &fn), name);
        if (!fn)
            throw std::runtime_error("Loader returned a null OpenXR function");
        destination = reinterpret_cast<T>(fn);
    }
    void open(const std::wstring &directory) {
        // Absolute filenames restrict the loader search to the packaged host directory.
        for (const auto *filename : {L"libopenxr_loader.dll", L"openxr_loader.dll"}) {
            loader.h = LoadLibraryExW((directory + filename).c_str(), nullptr,
                                      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (loader.h)
                break;
        }
        if (!loader.h)
            throw std::runtime_error("Official OpenXR loader missing or unloadable beside host");
        const FARPROC entry = GetProcAddress(loader.h, "xrGetInstanceProcAddr");
        static_assert(sizeof(entry) == sizeof(GetInstanceProcAddr));
        std::memcpy(&GetInstanceProcAddr, &entry, sizeof(entry));
        if (!GetInstanceProcAddr)
            throw std::runtime_error("Loader has no xrGetInstanceProcAddr");
        load(EnumerateInstanceExtensionProperties, XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties");
        load(CreateInstance, XR_NULL_HANDLE, "xrCreateInstance");
    }
    void instanceFunctions(XrInstance instance) {
        // Load destruction first, so partial initialization can be unwound.
#define LOAD(name) load(name, instance, "xr" #name);
        XR_INSTANCE_FUNCTIONS(LOAD)
#undef LOAD
    }
};
#undef XR_INSTANCE_FUNCTIONS

void hresult(HRESULT result, const char *operation, Log &log) {
    if (FAILED(result)) {
        log.result(operation, result);
        throw std::runtime_error(operation);
    }
}

Pose pose(const XrPosef &value) {
    return {{value.orientation.x, value.orientation.y, value.orientation.z, value.orientation.w},
            {value.position.x, value.position.y, value.position.z}};
}
XrPosef xrPose(Pose value) {
    // Runtimes validate layer orientations strictly (Monado rejects frames with
    // XR_ERROR_POSE_INVALID); composed float quaternions drift, so renormalize.
    const double x = value.q.x, y = value.q.y, z = value.q.z, w = value.q.w;
    const double norm = std::sqrt(x * x + y * y + z * z + w * w);
    if (!std::isfinite(norm) || norm < 1e-6 || !std::isfinite(value.p.x) || !std::isfinite(value.p.y) ||
        !std::isfinite(value.p.z))
        return {{0, 0, 0, 1}, {0, 0, 0}};
    return {{float(x / norm), float(y / norm), float(z / norm), float(w / norm)},
            {value.p.x, value.p.y, value.p.z}};
}
bool validPose(Pose p) {
    const float norm = p.q.x * p.q.x + p.q.y * p.q.y + p.q.z * p.q.z + p.q.w * p.q.w;
    return finite(p) && norm > .99f && norm < 1.01f;
}
bool located(const XrSpaceLocation &location) {
    constexpr auto needed = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
                            XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT |
                            XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    return (location.locationFlags & needed) == needed && validPose(pose(location.pose));
}

struct Swapchain {
    Api *api = nullptr;
    XrSwapchain handle = XR_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    ImageOwnership ownership;
    std::vector<XrSwapchainImageD3D11KHR> images;
    std::vector<uint8_t> converted;
    ~Swapchain() {
        destroy();
    }
    void destroy() noexcept {
        if (handle) {
            // Owner drains our submitted GPU work before destruction. Retire any
            // available acquisition first; a timed-out unwritten image may only
            // be disposed of by destroying the chain, never released unwaited.
            if (ownership.acquired && !api->lossPending) {
                try {
                    if (!releaseAcquired())
                        api->log.write("Unwritten image wait timed out during swapchain destruction");
                } catch (const std::exception &) {
                    api->log.write("Acquisition retirement failed during swapchain destruction");
                }
            }
            api->cleanup(api->DestroySwapchain(handle), "xrDestroySwapchain");
        }
        handle = XR_NULL_HANDLE;
        ownership = {};
        images.clear();
        width = height = 0;
    }
    void abandon() noexcept {
        handle = XR_NULL_HANDLE;
        images.clear();
    }
    void create(Api &functions, XrSession session, uint32_t w, uint32_t h) {
        api = &functions;
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                          XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        info.format = api->colorFormat;
        info.sampleCount = 1;
        info.width = w;
        info.height = h;
        info.faceCount = info.arraySize = info.mipCount = 1;
        api->check(api->CreateSwapchain(session, &info, &handle), "xrCreateSwapchain BGRA8 UNORM");
        width = w;
        height = h;
        uint32_t count = 0;
        api->check(api->EnumerateSwapchainImages(handle, 0, &count, nullptr),
                   "xrEnumerateSwapchainImages count");
        if (!count)
            throw std::runtime_error("OpenXR swapchain contains no images");
        images.assign(count, XrSwapchainImageD3D11KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        api->check(api->EnumerateSwapchainImages(
                       handle, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data())),
                   "xrEnumerateSwapchainImages");
        images.resize(count);
        char description[192];
        std::snprintf(description, sizeof(description), "OpenXR swapchain: %ux%u format=%lld images=%u",
                      width, height, static_cast<long long>(info.format), count);
        api->log.write(description);
        for (auto &image : images) {
            if (!image.texture)
                throw std::runtime_error("OpenXR returned null D3D11 texture");
            D3D11_TEXTURE2D_DESC description{};
            image.texture->GetDesc(&description);
            if (description.Width != width || description.Height != height || description.ArraySize != 1 ||
                description.SampleDesc.Count != 1 || description.MipLevels != 1 ||
                (description.Format != api->colorFormat &&
                 description.Format != (api->colorFormat == DXGI_FORMAT_B8G8R8A8_UNORM
                                            ? DXGI_FORMAT_B8G8R8A8_TYPELESS
                                            : DXGI_FORMAT_R8G8B8A8_TYPELESS)))
                throw std::runtime_error("OpenXR image layout does not match packed BGRA transport");
        }
    }
    bool waitImage() {
        if (api->lossPending)
            return false;
        if (!ownership.acquired) {
            XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            api->check(api->AcquireSwapchainImage(handle, &acquire, &ownership.index),
                       "xrAcquireSwapchainImage");
            ownership.acquired = true;
            if (ownership.index >= images.size())
                throw std::runtime_error("OpenXR swapchain index out of bounds");
        }
        if (!ownership.waited) {
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout = ImageWaitNs;
            const XrResult result = api->WaitSwapchainImage(handle, &wait);
            if (result == XR_TIMEOUT_EXPIRED)
                return false;
            api->check(result, "xrWaitSwapchainImage");
            ownership.waited = true;
        }
        return !api->lossPending;
    }
    void uploadWaited(ID3D11DeviceContext *context, const uint8_t *bytes) {
        if (!ownership.acquired || !ownership.waited)
            throw std::runtime_error("Writing an unowned OpenXR image");
        if (api->colorFormat == DXGI_FORMAT_R8G8B8A8_UNORM) {
            converted.resize(size_t(width) * height * 4);
            bgraToRgba(converted.data(), bytes, size_t(width) * height);
            bytes = converted.data();
        }
        context->UpdateSubresource(images[ownership.index].texture, 0, nullptr, bytes, width * 4, 0);
        context->Flush();
        releaseWaited();
    }
    // Zero-copy: the game's shared eye image goes straight into the waited image.
    void copyWaited(ID3D11DeviceContext *context, ID3D11Texture2D *source) {
        if (!ownership.acquired || !ownership.waited)
            throw std::runtime_error("Writing an unowned OpenXR image");
        context->CopySubresourceRegion(images[ownership.index].texture, 0, 0, 0, 0, source, 0, nullptr);
    }
    void releaseWaited() {
        if (!ownership.acquired || !ownership.waited)
            throw std::runtime_error("Releasing an unwaited OpenXR image");
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        api->check(api->ReleaseSwapchainImage(handle, &release), "xrReleaseSwapchainImage");
        ownership.didRelease();
    }
    bool releaseAcquired() {
        if (!ownership.acquired)
            return true;
        if (!waitImage())
            return false;
        releaseWaited();
        return true;
    }
    bool upload(ID3D11DeviceContext *context, const uint8_t *bytes) {
        if (!waitImage())
            return false;
        uploadWaited(context, bytes);
        return true;
    }
    XrSwapchainSubImage subimage() const {
        return {handle, {{0, 0}, {int32_t(width), int32_t(height)}}, 0};
    }
};

struct Frame {
    Api &api;
    XrSession session;
    XrTime time;
    bool active = true;
    Frame(Api &a, XrSession s, XrTime t) : api(a), session(s), time(t) {}
    void rejected(XrResult result, const std::vector<const XrCompositionLayerBaseHeader *> &layers) {
        static unsigned reports = 0;
        if (reports >= 8) return;
        ++reports;
        char line[256];
        std::snprintf(line, sizeof line, "xrEndFrame rejected %d with %u layers; frame dropped", int(result),
                      unsigned(layers.size()));
        api.log.write(line);
        for (const auto *layer : layers) {
            if (!layer) continue;
            XrPosef pose{};
            if (layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD)
                pose = reinterpret_cast<const XrCompositionLayerQuad *>(layer)->pose;
            else if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                const auto *projection = reinterpret_cast<const XrCompositionLayerProjection *>(layer);
                if (projection->viewCount && projection->views) pose = projection->views[0].pose;
            }
            const auto &q = pose.orientation;
            std::snprintf(line, sizeof line, "  layer type=%d q=%.6g,%.6g,%.6g,%.6g p=%.6g,%.6g,%.6g", int(layer->type),
                          q.x, q.y, q.z, q.w, pose.position.x, pose.position.y, pose.position.z);
            api.log.write(line);
        }
    }
    ~Frame() {
        if (active) {
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
            end.displayTime = time;
            end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            api.cleanup(api.EndFrame(session, &end), "xrEndFrame zero layers during unwind");
        }
    }
    XrResult submit(const std::vector<const XrCompositionLayerBaseHeader *> &layers) {
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = time;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount = uint32_t(layers.size());
        end.layers = layers.empty() ? nullptr : layers.data();
        // Khronos recommends discarding failed submissions rather than retrying
        // xrEndFrame with different parameters. Close our frame guard on attempt.
        active = false;
        const XrResult result = api.EndFrame(session, &end);
        // A rejected layer set loses one frame, not the VR session.
        if (result == XR_ERROR_POSE_INVALID || result == XR_ERROR_LAYER_INVALID ||
            result == XR_ERROR_SWAPCHAIN_RECT_INVALID) {
            rejected(result, layers);
            return result;
        }
        api.check(result, "xrEndFrame");
        return result;
    }
};

struct Actions {
    XrActionSet set = XR_NULL_HANDLE;
    XrPath hand[2]{};
    XrAction aim{}, grip{}, haptic{}, trigger{}, stick{}, wheel{}, wheelValue{}, use{}, jump{}, menu{}, reload{}, netricsa{},
        recenter{}, sprint{}, wheelOpen{}, wheelOpenValue{};
    XrSpace aimSpace[2]{}, gripSpace[2]{};
    // XR_EXT_eye_gaze_interaction: one gaze pose without subaction paths.
    XrAction gaze{};
    XrSpace gazeSpace{};
    bool gazeEnabled = false;
    XrPath viveProfile = XR_NULL_PATH, indexProfile = XR_NULL_PATH, currentProfile[2]{};
    bool wheelForceDown[2]{};
    ActionStream primaryStream[2], zoomStream[2], gripPoseStream[2];
    SqueezeAdmission squeeze[2];
    WheelOpenGate wheelGate[2];
    void invalidateStreams() {
        for (unsigned h = 0; h != 2; ++h) {
            wheelGate[h].sample(false, false, false);
            primaryStream[h].invalidate();
            zoomStream[h].invalidate();
            gripPoseStream[h].invalidate();
            squeeze[h].invalidate();
        }
    }
    XrPath path(Api &api, XrInstance instance, const char *name) {
        XrPath result = XR_NULL_PATH;
        api.check(api.StringToPath(instance, name, &result), "xrStringToPath");
        return result;
    }
    XrAction action(Api &api, const char *name, const char *label, XrActionType type) {
        XrActionCreateInfo create{XR_TYPE_ACTION_CREATE_INFO};
        std::snprintf(create.actionName, sizeof(create.actionName), "%s", name);
        std::snprintf(create.localizedActionName, sizeof(create.localizedActionName), "%s", label);
        create.actionType = type;
        create.countSubactionPaths = 2;
        create.subactionPaths = hand;
        XrAction result = XR_NULL_HANDLE;
        api.check(api.CreateAction(set, &create, &result), "xrCreateAction");
        return result;
    }
    void createGaze(Api &api, XrInstance instance) {
        XrActionCreateInfo create{XR_TYPE_ACTION_CREATE_INFO};
        std::snprintf(create.actionName, sizeof(create.actionName), "eye_gaze");
        std::snprintf(create.localizedActionName, sizeof(create.localizedActionName), "Eye gaze (foveation)");
        create.actionType = XR_ACTION_TYPE_POSE_INPUT;
        api.check(api.CreateAction(set, &create, &gaze), "xrCreateAction eye gaze");
        XrActionSuggestedBinding binding{gaze, path(api, instance, "/user/eyes_ext/input/gaze_ext/pose")};
        XrInteractionProfileSuggestedBinding suggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggestion.interactionProfile = path(api, instance, "/interaction_profiles/ext/eye_gaze_interaction");
        suggestion.countSuggestedBindings = 1;
        suggestion.suggestedBindings = &binding;
        const XrResult result = api.SuggestInteractionProfileBindings(instance, &suggestion);
        gazeEnabled = XR_SUCCEEDED(result);
        if (!gazeEnabled) api.log.result("Eye gaze binding", result);
    }
    // Gaze direction in `space` at `time`; false while eye tracking is unavailable.
    bool gazeDirection(Api &api, XrSession session, XrSpace space, XrTime time, Vec3 &direction) {
        if (!gazeEnabled || !gazeSpace) return false;
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = gaze;
        XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};
        if (XR_FAILED(api.GetActionStatePose(session, &info, &state)) || !state.isActive) return false;
        XrEyeGazeSampleTimeEXT sampleTime{XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT};
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        location.next = &sampleTime;
        if (XR_FAILED(api.LocateSpace(gazeSpace, space, time, &location))) return false;
        constexpr XrSpaceLocationFlags needed = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
        if ((location.locationFlags & needed) != needed) return false;
        const auto &q = location.pose.orientation;
        direction = rotate(normalize(Quat{q.x, q.y, q.z, q.w}), {0, 0, -1});
        return std::isfinite(direction.x) && std::isfinite(direction.y) && std::isfinite(direction.z);
    }
    void create(Api &api, XrInstance instance, XrSession session, bool frameController, bool eyeGaze = false) {
        hand[0] = path(api, instance, "/user/hand/left");
        hand[1] = path(api, instance, "/user/hand/right");
        viveProfile = path(api, instance, "/interaction_profiles/htc/vive_controller");
        indexProfile = path(api, instance, "/interaction_profiles/valve/index_controller");
        XrActionSetCreateInfo create{XR_TYPE_ACTION_SET_CREATE_INFO};
        std::strcpy(create.actionSetName, "ss2vr");
        std::strcpy(create.localizedActionSetName, "Serious Sam 2 VR");
        api.check(api.CreateActionSet(instance, &create, &set), "xrCreateActionSet");
        aim = action(api, "aim", "Aim pose", XR_ACTION_TYPE_POSE_INPUT);
        grip = action(api, "grip", "Weapon attachment pose", XR_ACTION_TYPE_POSE_INPUT);
        haptic = action(api, "feedback", "Weapon and damage feedback", XR_ACTION_TYPE_VIBRATION_OUTPUT);
        trigger = action(api, "trigger", "Fire trigger", XR_ACTION_TYPE_FLOAT_INPUT);
        stick = action(api, "stick", "Move turn and wheel axis", XR_ACTION_TYPE_VECTOR2F_INPUT);
        wheel = action(api, "wheel_click", "Grip: jump, carry and vehicle grasp (click)", XR_ACTION_TYPE_BOOLEAN_INPUT);
        wheelValue = action(api, "wheel_value", "Grip: jump, carry and vehicle grasp (value)", XR_ACTION_TYPE_FLOAT_INPUT);
        wheelOpen = action(api, "wheel_open", "Hold for the weapon wheel", XR_ACTION_TYPE_BOOLEAN_INPUT);
        wheelOpenValue = action(api, "wheel_open_force", "Press for the weapon wheel (force)", XR_ACTION_TYPE_FLOAT_INPUT);
        use = action(api, "use", "Use", XR_ACTION_TYPE_BOOLEAN_INPUT);
        jump = action(api, "jump", "Jump (the grip also jumps on foot)", XR_ACTION_TYPE_BOOLEAN_INPUT);
        menu = action(api, "menu", "Pause and menu", XR_ACTION_TYPE_BOOLEAN_INPUT);
        recenter = action(api, "recenter_chord", "Recenter with both grips", XR_ACTION_TYPE_BOOLEAN_INPUT);
        sprint = action(api, "sprint", "Sprint / sniper zoom", XR_ACTION_TYPE_BOOLEAN_INPUT);
        reload = action(api, "reload", "Reload weapon", XR_ACTION_TYPE_BOOLEAN_INPUT);
        netricsa = action(api, "netricsa", "Netricsa / objectives", XR_ACTION_TYPE_BOOLEAN_INPUT);
        suggest(api, instance, frameController);
        if (eyeGaze) createGaze(api, instance);
        XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attach.countActionSets = 1;
        attach.actionSets = &set;
        api.check(api.AttachSessionActionSets(session, &attach), "xrAttachSessionActionSets");
        for (unsigned h = 0; h != 2; ++h) {
            XrActionSpaceCreateInfo space{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            space.action = aim;
            space.subactionPath = hand[h];
            space.poseInActionSpace.orientation.w = 1;
            api.check(api.CreateActionSpace(session, &space, &aimSpace[h]), "xrCreateActionSpace aim");
            space.action = grip;
            api.check(api.CreateActionSpace(session, &space, &gripSpace[h]), "xrCreateActionSpace grip");
        }
        if (gazeEnabled) {
            XrActionSpaceCreateInfo space{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            space.action = gaze;
            space.poseInActionSpace.orientation.w = 1;
            gazeEnabled = XR_SUCCEEDED(api.CreateActionSpace(session, &space, &gazeSpace));
        }
    }
    void suggest(Api &api, XrInstance instance, bool frameController) {
        struct Profile {
            const char *name;
            const char *axis;
            bool analogGrip;
            unsigned kind;
        };
        const Profile profiles[] = {
            {"/interaction_profiles/oculus/touch_controller", "thumbstick", true, 0},
            {"/interaction_profiles/valve/index_controller", "thumbstick", true, 1},
            {"/interaction_profiles/htc/vive_controller", "trackpad", false, 2},
            {"/interaction_profiles/microsoft/motion_controller", "thumbstick", false, 3},
            {"/interaction_profiles/valve/frame_controller_valve", "thumbstick", true, 4}};
        unsigned accepted = 0;
        for (const auto &profile : profiles) {
            if (profile.kind==4 && !frameController) continue;
            std::vector<XrActionSuggestedBinding> bindings;
            auto bind = [&](XrAction a, unsigned h, const std::string &component) {
                const std::string name =
                    std::string(h ? "/user/hand/right/input/" : "/user/hand/left/input/") + component;
                bindings.push_back({a, path(api, instance, name.c_str())});
            };
            for (unsigned h = 0; h != 2; ++h) {
                bind(aim, h, "aim/pose");
                bind(grip, h, "grip/pose");
                bindings.push_back(
                    {haptic, path(api, instance,
                                  h ? "/user/hand/right/output/haptic" : "/user/hand/left/output/haptic")});
                bind(trigger, h, "trigger/value");
                bind(stick, h, profile.axis);
                // Index squeeze/value is finger curl (high whenever the controller
                // is held); its force sensor is the deliberate grab.
                bind(profile.analogGrip ? wheelValue : wheel, h,
                     !profile.analogGrip ? "squeeze/click" : profile.kind == 1 ? "squeeze/force" : "squeeze/value");
            }
            const std::string click = std::string(profile.axis) + "/click";
            bind(sprint, 0, click);
            bind(recenter, 0, click); // Same button, activated only with both grips held.
            // The grip jumps on foot (game side); Netricsa is a weapon-wheel bubble.
            switch (profile.kind) {
            case 0:
                bind(sprint, 1, click);
                bind(reload, 0, "x/click");
                bind(use, 1, "a/click");
                bind(wheelOpen, 0, "y/click");
                bind(wheelOpen, 1, "b/click");
                bind(menu, 0, "menu/click"); // Touch has no right menu component.
                break;
            case 1:
                bind(sprint, 1, click);
                bind(reload, 0, "a/click");
                bind(use, 1, "a/click");
                bind(jump, 1, "b/click");
                bind(menu, 0, "b/click");
                // A float, thresholded here: the runtime's boolean conversion of
                // trackpad force needs a hard press.
                bind(wheelOpenValue, 0, "trackpad/force");
                bind(wheelOpenValue, 1, "trackpad/force");
                break;
            case 2:
                bind(menu, 0, "menu/click");
                bind(use, 1, "menu/click");
                bind(wheelOpen, 0, "trackpad/click");
                bind(wheelOpen, 1, "trackpad/click");
                break;
            case 3:
                // Both trackpads open the wheels, so Use moves to the left menu
                // button and the pause menu stays on the right one.
                bind(sprint, 1, click);
                bind(use, 0, "menu/click");
                bind(wheelOpen, 0, "trackpad/click");
                bind(wheelOpen, 1, "trackpad/click");
                bind(menu, 1, "menu/click");
                break;
            case 4:
                // Valve Frame has a left D-pad/view and right ABXY/menu.
                // Keep the established action semantics; no invented left XY.
                bind(sprint, 1, click);
                bind(reload, 0, "dpad_down/click");
                bind(use, 1, "a/click");
                bind(wheelOpen, 0, "dpad_up/click");
                bind(wheelOpen, 1, "b/click");
                bind(menu, 0, "view/click");
                bind(menu, 1, "menu/click");
                break;
            }
            XrInteractionProfileSuggestedBinding suggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggestion.interactionProfile = path(api, instance, profile.name);
            suggestion.countSuggestedBindings = uint32_t(bindings.size());
            suggestion.suggestedBindings = bindings.data();
            const XrResult result = api.SuggestInteractionProfileBindings(instance, &suggestion);
            if (result != XR_SUCCESS) {
                api.log.result(profile.name,
                               result); // One profile's unsupported paths do not spoil the others.
                if (result != XR_ERROR_PATH_UNSUPPORTED)
                    api.check(result, "xrSuggestInteractionProfileBindings");
            } else
                ++accepted;
        }
        if (!accepted)
            throw std::runtime_error("Runtime rejected all four controller binding profiles");
    }
    XrActionStateGetInfo get(XrAction a, unsigned h) const {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = a;
        info.subactionPath = hand[h];
        return info;
    }
    bool boolean(Api &api, XrSession session, XrAction a, unsigned h) {
        const auto state = booleanState(api, session, a, h);
        return state.isActive && state.currentState;
    }
    XrActionStateBoolean booleanState(Api &api, XrSession session, XrAction a, unsigned h) {
        auto info = get(a, h);
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        api.check(api.GetActionStateBoolean(session, &info, &state), "xrGetActionStateBoolean");
        return state;
    }
    XrActionStateFloat floatState(Api &api, XrSession session, XrAction a, unsigned h) {
        auto info = get(a, h);
        XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
        api.check(api.GetActionStateFloat(session, &info, &state), "xrGetActionStateFloat");
        return state;
    }
    void sample(Api &api, XrInstance instance, XrSession session, XrSpace local, XrTime time, Input &input, bool validViews) {
        XrActiveActionSet active{set, XR_NULL_PATH};
        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &active;
        const XrResult result = api.SyncActions(session, &sync);
        if (result == XR_SESSION_NOT_FOCUSED) {
            input.focused = 0;
            for (unsigned h = 0; h != 2; ++h) {
                squeeze[h].sample(0, false, false, false);
                if (wheelGate[h].sample(false, false, false)) input.blockedWheels |= 1u << h;
                gripPoseStream[h].sample(false);
                input.wheelInputEpoch[h] = squeeze[h].epoch;
            }
            return;
        }
        api.check(result, "xrSyncActions");
        uint32_t squeezeSources[2]{};
        bool squeezeValuesValid[2]{}, wheelOpenActive[2]{}, wheelOpenDown[2]{};
        for (unsigned h = 0; h != 2; ++h) {
            XrInteractionProfileState profile{XR_TYPE_INTERACTION_PROFILE_STATE};
            api.check(api.GetCurrentInteractionProfile(session, hand[h], &profile), "xrGetCurrentInteractionProfile");
            if (profile.interactionProfile != currentProfile[h]) {
                currentProfile[h] = profile.interactionProfile;
                char pathName[XR_MAX_PATH_LENGTH] = "<none>";
                uint32_t length = 0;
                if (profile.interactionProfile) {
                    const auto result = api.PathToString(instance, profile.interactionProfile,
                        sizeof(pathName), &length, pathName);
                    if (XR_FAILED(result)) {
                        std::strcpy(pathName, "<unavailable>");
                        api.log.result("Interaction profile diagnostic", result);
                    }
                }
                char description[XR_MAX_PATH_LENGTH + 48];
                std::snprintf(description, sizeof(description), "OpenXR %s profile: %s",
                              h ? "right" : "left", pathName);
                api.log.write(description);
                primaryStream[h].invalidate();
                zoomStream[h].invalidate();
                gripPoseStream[h].invalidate();
                squeeze[h].invalidate();
                wheelGate[h].sample(false, false, false); // A profile change is a loss, not a release.
                wheelForceDown[h] = false;
            }
            auto info = get(aim, h);
            XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};
            api.check(api.GetActionStatePose(session, &info, &state), "xrGetActionStatePose");
            if (state.isActive) {
                XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
                api.check(api.LocateSpace(aimSpace[h], local, time, &location), "xrLocateSpace aim");
                if (located(location)) {
                    input.hand[h] = pose(location.pose);
                    input.handValid[h] = 1;
                }
            }
            auto gripInfo = get(grip, h);
            XrActionStatePose gripState{XR_TYPE_ACTION_STATE_POSE};
            api.check(api.GetActionStatePose(session, &gripInfo, &gripState), "xrGetActionStatePose grip");
            if (gripState.isActive) {
                XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
                api.check(api.LocateSpace(gripSpace[h], local, time, &location), "xrLocateSpace grip");
                if (located(location)) {
                    input.grip[h] = pose(location.pose);
                    input.gripValid[h] = 1;
                }
            }
            gripPoseStream[h].sample(rideGripPoseEligible(input,h));
            if (!input.focused)
                continue;
            auto triggerInfo = get(trigger, h);
            XrActionStateFloat triggerState{XR_TYPE_ACTION_STATE_FLOAT};
            api.check(api.GetActionStateFloat(session, &triggerInfo, &triggerState), "xrGetActionStateFloat trigger");
            const auto primary = primaryActionSample(primaryStream[h], triggerState.isActive != XR_FALSE,
                                                     triggerState.currentState);
            input.primaryInputGeneration[h] = primary.generation;
            if (primary.active) {
                input.primaryActiveMask |= 1u << h;
                input.trigger[h] = primary.value;
            }
            auto axisInfo = get(stick, h);
            XrActionStateVector2f axis{XR_TYPE_ACTION_STATE_VECTOR2F};
            api.check(api.GetActionStateVector2f(session, &axisInfo, &axis), "xrGetActionStateVector2f");
            if (axis.isActive && std::isfinite(axis.currentState.x) && std::isfinite(axis.currentState.y)) {
                input.axis[h][0] = std::clamp(axis.currentState.x, -1.f, 1.f);
                input.axis[h][1] = std::clamp(axis.currentState.y, -1.f, 1.f);
            }
            // Query both sources; an active malformed analog value is unavailable,
            // even when the boolean source is pressed. Preserve raw chord input.
            const auto wheelClick = booleanState(api, session, wheel, h);
            const auto wheelAnalog = floatState(api, session, wheelValue, h);
            const bool indexHand = currentProfile[h] == indexProfile;
            const auto rawSqueeze = squeezeSample(wheelClick.isActive != XR_FALSE, wheelClick.currentState,
                wheelAnalog.isActive != XR_FALSE, wheelAnalog.currentState, indexHand ? .4f : .65f);
            squeezeSources[h] = rawSqueeze.sources;
            squeezeValuesValid[h] = rawSqueeze.valid;
            if (rawSqueeze.down) input.buttons[h] |= Wheel;
            const auto openState = booleanState(api, session, wheelOpen, h);
            const auto openForce = floatState(api, session, wheelOpenValue, h);
            const bool forceActive = openForce.isActive != XR_FALSE && std::isfinite(openForce.currentState);
            wheelForceDown[h] = forceActive && openForce.currentState > (wheelForceDown[h] ? .12f : .25f);
            wheelOpenActive[h] = openState.isActive != XR_FALSE || forceActive;
            wheelOpenDown[h] = (openState.isActive && openState.currentState) || wheelForceDown[h];
            if (boolean(api, session, use, h))
                input.buttons[h] |= Use;
            const auto jumpState = booleanState(api, session, jump, h);
            const auto sprintState = booleanState(api, session, sprint, h);
            if (jumpState.isActive && jumpState.currentState)
                input.buttons[h] |= Jump;
            if (jumpState.isActive)
                input.buttons[h] |= JumpBound;
            if (boolean(api, session, menu, h))
                input.buttons[h] |= Menu;
            if (boolean(api, session, reload, h))
                input.buttons[h] |= Reload;
            if (boolean(api, session, netricsa, h))
                input.buttons[h] |= Netricsa;
            if (sprintState.isActive && sprintState.currentState)
                input.buttons[h] |= Sprint;
            const bool viveRight = h == 1 && currentProfile[h] == viveProfile;
            const auto &zoomState = viveRight ? jumpState : sprintState;
            input.zoomSourceButton[h] = viveRight ? Jump : Sprint;
            const bool zoomActive = zoomStream[h].sample(zoomState.isActive != XR_FALSE);
            input.zoomInputGeneration[h] = zoomStream[h].generation;
            if (zoomActive) {
                input.zoomActiveMask |= 1u << h;
                if (zoomState.currentState)
                    input.zoomDownMask |= 1u << h;
            }
        }
        applyRecenterChord(input, boolean(api, session, recenter, 0));
        for (unsigned h = 0; h != 2; ++h) {
            const bool admitted = squeeze[h].sample(squeezeSources[h], squeezeValuesValid[h],
                (input.buttons[h] & Wheel) != 0,
                input.focused && input.headValid && input.handValid[h] && validViews && !recenterHeld(input));
            input.wheelInputEpoch[h] = squeeze[h].epoch;
            if (admitted) input.wheelAdmissionMask |= 1u << h;
            if (wheelGate[h].sample(wheelOpenActive[h], wheelOpenDown[h],
                    input.focused && input.headValid && input.handValid[h] && validViews && !recenterHeld(input)))
                input.blockedWheels |= 1u << h;
            if (wheelGate[h].down) input.buttons[h] |= WheelOpen;
        }
    }
    bool pulse(Api &api, XrSession session, unsigned handIndex, float amplitude, XrDuration duration) {
        XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
        info.action = haptic;
        info.subactionPath = hand[handIndex];
        XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
        vibration.duration = duration;
        vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
        vibration.amplitude = amplitude;
        const XrResult result =
            api.ApplyHapticFeedback(session, &info, reinterpret_cast<const XrHapticBaseHeader *>(&vibration));
        if (XR_FAILED(result)) {
            api.log.result("Optional controller haptics disabled", result);
            return false;
        }
        api.check(result, "xrApplyHapticFeedback");
        return !api.lossPending;
    }
    void stopFeedback(Api &api, XrSession session) noexcept {
        if (!haptic || !session)
            return;
        for (unsigned h = 0; h != 2; ++h) {
            XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
            info.action = haptic;
            info.subactionPath = hand[h];
            api.cleanup(api.StopHapticFeedback(session, &info), "xrStopHapticFeedback");
        }
    }
    void destroy(Api &api) noexcept {
        for (auto &space : aimSpace) {
            if (space)
                api.cleanup(api.DestroySpace(space), "xrDestroySpace aim");
            space = XR_NULL_HANDLE;
        }
        for (auto &space : gripSpace) {
            if (space)
                api.cleanup(api.DestroySpace(space), "xrDestroySpace grip");
            space = XR_NULL_HANDLE;
        }
        if (set)
            api.cleanup(api.DestroyActionSet(set), "xrDestroyActionSet");
        set = XR_NULL_HANDLE; // Child actions are destroyed with their action set.
    }
};

// GDI renders labels into a top-down DIB as straight-alpha BGRA cards.
struct WheelBitmap {
    HDC dc = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ oldBitmap = nullptr;
    HFONT label = nullptr, ammo = nullptr, title = nullptr, detail = nullptr;
    uint8_t *bits = nullptr;
    uint32_t size = 0, height = 0;
    ~WheelBitmap() {
        if (oldBitmap)
            SelectObject(dc, oldBitmap);
        if (dib)
            DeleteObject(dib);
        if (label)
            DeleteObject(label);
        if (ammo)
            DeleteObject(ammo);
        if (title)
            DeleteObject(title);
        if (detail)
            DeleteObject(detail);
        if (dc)
            DeleteDC(dc);
    }
    void create(uint32_t pixels, uint32_t rows = 0) {
        size = pixels;
        height = rows ? rows : pixels;
        dc = CreateCompatibleDC(nullptr);
        if (!dc)
            throw std::runtime_error("GDI wheel memory DC unavailable");
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = LONG(size);
        info.bmiHeader.biHeight = -LONG(height);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void *data = nullptr;
        dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &data, nullptr, 0);
        if (!dib || !data)
            throw std::runtime_error("GDI wheel DIB unavailable");
        bits = static_cast<uint8_t *>(data);
        oldBitmap = SelectObject(dc, dib);
        if (!oldBitmap || oldBitmap == HGDI_ERROR)
            throw std::runtime_error("GDI wheel DIB selection failed");
        label = CreateFontW(-int(size * .032f), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                            DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        ammo = CreateFontW(-int(size * .030f), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                           DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        title = CreateFontW(-int(height * .34f), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                            DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        detail = CreateFontW(-int(height * .22f), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        if (!label || !ammo || !title || !detail)
            throw std::runtime_error("GDI wheel fonts unavailable");
        SetBkMode(dc, TRANSPARENT);
    }
    void text(const wchar_t *value, float x, float y, HFONT font, COLORREF color, float width, float line = 0) {
        const HGDIOBJ old = SelectObject(dc, font);
        SetTextColor(dc, color);
        RECT rectangle{LONG(x - width / 2), LONG(y), LONG(x + width / 2), LONG(y + (line > 0 ? line : size * .052f))};
        const int result =
            DrawTextW(dc, value, -1, &rectangle, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, old);
        if (!result)
            throw std::runtime_error("GDI wheel label drawing failed");
    }
    // Compact card at the centre of the in-world ring (drawn by the game): the
    // hovered item's name and ammunition, or how to choose and cancel.
    void draw(const WheelUi &ui, unsigned hand) {
        if (!GdiFlush())
            throw std::runtime_error("GDI flush failed");
        const uint32_t count = std::min(ui.count, WeaponCount);
        const WheelBubble bubble = wheelHoveredBubble(ui.hover);
        const bool weapon = ui.hover >= 0 && uint32_t(ui.hover) < count;
        const bool hovered = weapon || bubble != WheelBubble::None;
        // Accent edge in the laser colour of the opening hand while an item is hovered.
        const uint8_t accent[3] = {uint8_t(hand ? 255 : 64), uint8_t(hand ? 223 : 255), uint8_t(hand ? 64 : 96)};
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < size; ++x) {
                auto *p = bits + (size_t(y) * size + x) * 4;
                const bool edge = hovered && x < size / 64;
                p[0] = edge ? accent[0] : 24;
                p[1] = edge ? accent[1] : 18;
                p[2] = edge ? accent[2] : 15;
                p[3] = 215;
            }
        wchar_t heading[64], line[64];
        if (weapon) {
            // Stock names are verified against the native parameter-path switch.
            std::swprintf(heading, 64, L"%ls", weaponName(ui.weapon[ui.hover]));
            if (ui.ammo[ui.hover] > 0)
                std::swprintf(line, 64, L"Ammo %d", ui.ammo[ui.hover]);
            else
                std::swprintf(line, 64, ui.ammo[ui.hover] == 0 ? L"Out of ammo" : L"Release to equip");
        } else if (bubble != WheelBubble::None) {
            std::swprintf(heading, 64, L"%ls", wheelBubbleName(bubble));
            std::swprintf(line, 64, bubble == WheelBubble::QuickSave   ? L"Release to save"
                                    : bubble == WheelBubble::QuickLoad ? L"Release to load"
                                                                       : L"Release to open");
        } else {
            std::swprintf(heading, 64, hand ? L"Right hand" : L"Left hand");
            std::swprintf(line, 64, L"Reach to an item, release to choose");
        }
        text(heading, size * .5f, height * .08f, title, RGB(255, 255, 255), size * .92f, height * .44f);
        text(line, size * .5f, height * .58f, detail, hovered ? RGB(235, 240, 250) : RGB(190, 200, 210),
             size * .92f, height * .32f);
        if (!GdiFlush())
            throw std::runtime_error("GDI wheel draw flush failed");
        // GDI clears text alpha; keep the whole card's straight alpha predictable.
        for (size_t i = 0; i < size_t(size) * height; i++)
            bits[i * 4 + 3] = 215;
    }
};

bool sameWheel(const WheelUi &a, const WheelUi &b) {
    return a.open == b.open && a.count == b.count && a.hover == b.hover &&
           !std::memcmp(a.weapon, b.weapon, sizeof(a.weapon)) && !std::memcmp(a.ammo, b.ammo, sizeof(a.ammo));
}
// The game draws the ring in the world; the host only labels it. The card sits
// at the ring centre, slightly toward the user, captured from this host's own
// sample of the hand when the game first reports that wheel open.
constexpr float WheelCardWidth = .17f, WheelCardLift = .04f;
struct WheelLayer {
    Swapchain chain;
    WheelBitmap bitmap;
    bool anchored = false, uploaded = false;
    Pose anchor{};
    WheelUi drawn{};
    void reset() {
        anchored = uploaded = false;
    }
};

// VR options: a panel opened from the weapon wheel's options bubble, drawn by
// GDI like the wheel cards. A stick moves between rows and changes the value
// (a trigger or A steps it too); B, the menu button or the Close row close it.
// Game input is held neutral while it is open. Values apply at once and are
// written to SS2VR.user.ini; the game re-reads its settings when it changes.
struct OptionsRow {
    const wchar_t *label;
    std::wstring value;
    const wchar_t *note = L"";
};
struct OptionsBitmap {
    HDC dc = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ oldBitmap = nullptr;
    HFONT title = nullptr, row = nullptr, small = nullptr;
    uint8_t *bits = nullptr;
    uint32_t width = 0, height = 0;
    ~OptionsBitmap() {
        if (oldBitmap) SelectObject(dc, oldBitmap);
        for (HGDIOBJ o : {HGDIOBJ(dib), HGDIOBJ(title), HGDIOBJ(row), HGDIOBJ(small)})
            if (o) DeleteObject(o);
        if (dc) DeleteDC(dc);
    }
    void create(uint32_t w, uint32_t h) {
        width = w; height = h;
        dc = CreateCompatibleDC(nullptr);
        if (!dc) throw std::runtime_error("GDI options memory DC unavailable");
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = LONG(w);
        info.bmiHeader.biHeight = -LONG(h);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void *data = nullptr;
        dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &data, nullptr, 0);
        if (!dib || !data) throw std::runtime_error("GDI options DIB unavailable");
        bits = static_cast<uint8_t *>(data);
        oldBitmap = SelectObject(dc, dib);
        const auto font = [&](float size, int weight) {
            return CreateFontW(-int(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        };
        title = font(h * .065f, FW_BOLD); row = font(h * .042f, FW_SEMIBOLD); small = font(h * .030f, FW_NORMAL);
        if (!oldBitmap || oldBitmap == HGDI_ERROR || !title || !row || !small)
            throw std::runtime_error("GDI options resources unavailable");
        SetBkMode(dc, TRANSPARENT);
    }
    void text(const std::wstring &value, RECT r, HFONT font, COLORREF color, UINT align) {
        const HGDIOBJ old = SelectObject(dc, font);
        SetTextColor(dc, color);
        DrawTextW(dc, value.c_str(), -1, &r, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, old);
    }
    void fill(RECT r, uint8_t b, uint8_t g, uint8_t red) {
        for (LONG y = std::max(0L, r.top); y < std::min(LONG(height), r.bottom); ++y)
            for (LONG x = std::max(0L, r.left); x < std::min(LONG(width), r.right); ++x) {
                auto *p = bits + (size_t(y) * width + x) * 4;
                p[0] = b; p[1] = g; p[2] = red;
            }
    }
    void draw(const std::vector<OptionsRow> &rows, unsigned selected) {
        GdiFlush();
        fill({0, 0, LONG(width), LONG(height)}, 26, 20, 16);
        const LONG margin = LONG(width * .05f), top = LONG(height * .12f);
        const LONG line = LONG((height * .80f) / float(std::max<size_t>(rows.size(), 1)));
        fill({0, 0, LONG(width), LONG(height * .1f)}, 60, 40, 24);
        text(L"VR options", {margin, 0, LONG(width) - margin, LONG(height * .1f)}, title, RGB(255, 255, 255), DT_LEFT);
        for (size_t i = 0; i < rows.size(); ++i) {
            const RECT r{margin / 2, top + LONG(i) * line, LONG(width) - margin / 2, top + LONG(i + 1) * line - 4};
            const bool on = i == selected;
            if (on) fill(r, 40, 120, 190);
            const COLORREF ink = on ? RGB(255, 255, 255) : RGB(210, 218, 228);
            text(rows[i].label, {r.left + margin / 2, r.top, LONG(width * .55f), r.bottom}, row, ink, DT_LEFT);
            const std::wstring value = rows[i].value + (on && *rows[i].note ? std::wstring(L"  ") + rows[i].note : L"");
            text(value, {LONG(width * .5f), r.top, r.right - margin / 2, r.bottom}, on ? row : small,
                 on ? RGB(255, 236, 160) : RGB(170, 200, 230), DT_RIGHT);
        }
        text(L"Stick: choose and change    Trigger / A: change    B / Menu: close",
             {margin, LONG(height * .93f), LONG(width) - margin, LONG(height)}, small, RGB(170, 180, 190), DT_CENTER);
        GdiFlush();
        for (size_t i = 0; i < size_t(width) * height; ++i) bits[i * 4 + 3] = 235;
    }
};
struct OptionsPanel {
    Swapchain chain;
    OptionsBitmap bitmap;
    bool open = false, uploaded = false;
    Pose anchor{};
    unsigned selected = 0;
    int stepY = 0, stepX = 0;
    ULONGLONG repeatY = 0, repeatX = 0;
    bool triggerHeld[2]{}, buttonHeld = false, closeHeld = false;
    std::vector<OptionsRow> drawn;
    unsigned drawnSelected = ~0u;
    int32_t wheelHover[2]{-1, -1};
    bool wheelOpen[2]{};
};
constexpr float OptionsWidth = .9f, OptionsAspect = .75f, OptionsDistance = .95f;

struct Host {
    Log &log;
    Api api;
    Channel channel;
    Handle gameProcess;
    bool ownsChannel = false, running = false, quit = false, failed = false;
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace local = XR_NULL_HANDLE, viewSpace = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
    std::array<XrViewConfigurationView, 2> viewConfiguration{};
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    Com<ID3D11Query> drainQuery;
    // Zero-copy eye images opened from the game's shared handles (slot x eye).
    Com<ID3D11Texture2D> sharedEye[2][2];
    uint32_t sharedOpened = 0, sharedTried = 0, sharedWidth = 0, sharedHeight = 0;
    uint64_t zeroCopyPairs = 0, latched = 0;
    Actions actions;
    WorldSubmissionStream worldSubmission;
    Swapchain eye[2];
    WheelLayer wheels[2];
    OptionsPanel options;
    std::wstring settingsFile;
    ComfortAnchor menuAnchor, hudAnchor;
    VrSettings settings;
    uint32_t lastFire[2]{}, lastDamage = 0;
    int32_t lastHover[2]{-1, -1};
    bool feedbackSeeded = false, hapticsAvailable = true;
    Fov currentFov[2];
    bool recenterWasHeld = false, recenterUi = false;
    Swapchain menuChain, cursorChain;
    // Foveated readback: eye[] carries the downsampled full view, inset[] the
    // full-resolution crop submitted as a second projection layer.
    Swapchain inset[2];
    bool eyeGazeExtension = false, eyeGazeSupported = false, foveationActive = false, cachedFoveated = false;
    Fov cachedInset[2];
    Vec3 lastGaze{};
    ULONGLONG lastGazeTick = 0;
    bool cursorUploaded = false;
    std::vector<uint8_t> menuSourcePixels;
    uint64_t menuSequence = 0;
    MenuPointer presentedPointer;
    int cursorX = -1, cursorY = -1;
    PendingRequest outstanding[2];
    Request cachedRequest;
    HeadClearance cachedClearance;
    uint64_t cachedDeadline = 0;
    uint32_t cachedPresentation = 0;
    bool cachedValid = false, viewsTracked = false;
    PixelStore pixels; // The game's current pixel sections, reopened per generation.
    std::vector<XrTime> referenceChanges;
    uint32_t sessionGeneration = 0, referenceGeneration = 1;
    uint64_t inputSequence = 0, requestSequence = 0;
    uint64_t xrEndSuccess = 0, xrLayerSuccess = 0, xrMenuSuccess = 0, xrWorldSuccess = 0;
    uint64_t uiPairs = 0, clearedPairs = 0; // Pairs with the native HUD composited; head-clearance opaque pairs.
    uint64_t completed = 0, timeouts = 0, fullSlots = 0, discarded = 0, imageTimeouts = 0, invalidViews = 0;
    uint64_t retired = 0, submissions = 0, reused = 0, lastSubmittedSequence = 0;
    ULONGLONG lastDiagnostics = 0, lastMutexDiagnostic = 0;
    // Host cost accounting for the 5 s diagnostics line (milliseconds sums).
    double perfWait = 0, perfUpload = 0, perfLoop = 0, perfSubmit = 0, perfPairAge = 0;
    uint64_t perfLoops = 0, perfUploads = 0;
    LARGE_INTEGER perfLoopStart{};
    static double perfMs(LARGE_INTEGER a, LARGE_INTEGER b) {
        static LARGE_INTEGER frequency{};
        if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
        return 1000. * double(b.QuadPart - a.QuadPart) / double(frequency.QuadPart);
    }
    static LARGE_INTEGER perfNow() { LARGE_INTEGER t{}; QueryPerformanceCounter(&t); return t; }

    explicit Host(Log &l) : log(l), api(l) {}
    ~Host() {
        disconnect();
        // Never destroy a texture while our immediate-context work still uses it.
        // A GPU that ignores the bounded drain cannot be torn down safely here;
        // retain the graphics/runtime objects until OS process teardown instead.
        if (context.p && !drainGpu()) {
            log.write("GPU drain timed out; graphics/runtime retained until process exit");
            for (auto &chain : eye)
                chain.abandon();
            for (auto &wheel : wheels)
                wheel.chain.abandon();
            options.chain.abandon();
            menuChain.abandon();
            cursorChain.abandon();
            for (auto &chain : inset)
                chain.abandon();
            drainQuery.abandon();
            for (auto &row : sharedEye)
                for (auto &image : row)
                    image.abandon();
            context.abandon();
            device.abandon();
            api.loader.h = nullptr;
            return;
        }
        for (auto &chain : eye)
            chain.destroy();
        for (auto &wheel : wheels)
            wheel.chain.destroy();
        options.chain.destroy();
        menuChain.destroy();
        cursorChain.destroy();
        for (auto &chain : inset)
            chain.destroy();
        actions.destroy(api);
        if (viewSpace)
            api.cleanup(api.DestroySpace(viewSpace), "xrDestroySpace VIEW");
        if (local)
            api.cleanup(api.DestroySpace(local), "xrDestroySpace LOCAL");
        if (session)
            api.cleanup(api.DestroySession(session), "xrDestroySession");
        for (auto &row : sharedEye)
            for (auto &image : row)
                image.reset();
        context.reset();
        drainQuery.reset();
        device.reset();
        if (instance && api.DestroyInstance)
            api.cleanup(api.DestroyInstance(instance), "xrDestroyInstance");
    }
    bool drainGpu() noexcept {
        if (!context.p || !drainQuery.p)
            return true; // No upload can precede creation of this query.
        context->End(drainQuery.p);
        context->Flush();
        const ULONGLONG deadline = GetTickCount64() + 500;
        for (;;) {
            const HRESULT result = context->GetData(drainQuery.p, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (result == S_OK || FAILED(device->GetDeviceRemovedReason()))
                return true;
            if (FAILED(result) || GetTickCount64() >= deadline)
                return false;
            Sleep(1);
        }
    }
    void connect(const std::wstring &token) {
        if (!channel.open(token, false))
            throw std::runtime_error("Existing IPC channel missing or incompatible");
        Mutex lock(channel, 100);
        if (!lock)
            throw std::runtime_error("IPC channel busy during initialization");
        auto &s = *channel.shared;
        if (s.magic != Magic || s.abi != Abi || s.bytes != sizeof(Shared) || !s.gamePid || s.hostPid ||
            s.shutdown || s.slot[0].state != SlotState::Empty || s.slot[1].state != SlotState::Empty)
            throw std::runtime_error("IPC channel stale, occupied or incompatible");
        gameProcess.h = OpenProcess(SYNCHRONIZE, FALSE, s.gamePid);
        if (!gameProcess.h || WaitForSingleObject(gameProcess.h, 0) != WAIT_TIMEOUT)
            throw std::runtime_error("IPC game process unavailable");
        s.hostPid = GetCurrentProcessId();
        ownsChannel = true;
    }
    void disconnect() noexcept {
        if (!ownsChannel)
            return;
        try {
            Mutex lock(channel, 20);
            if (lock && channel.shared->hostPid == GetCurrentProcessId()) {
                Input input{};
                input.sequence = ++inputSequence;
                input.tickMs = GetTickCount64();
                input.session = sessionGeneration;
                input.reference = referenceGeneration;
                channel.shared->latest = input;
                channel.shared->gripPoseGeneration[0]=channel.shared->gripPoseGeneration[1]=0;
                worldSubmission.invalidate();
                channel.shared->worldSubmission=worldSubmission.value;
                channel.shared->pointer = {};
                if (failed && !channel.shared->error)
                    channel.shared->error = 1;
                channel.shared->hostPid = 0;
                // Invalidate input even if a damaged slot cannot be reaped. Only
                // touch requests we own, and never release a Rendering slot.
                for (unsigned i = 0; i != 2; ++i) {
                    const auto &tracked = outstanding[i];
                    auto &slot = channel.shared->slot[i];
                    if (!tracked.active || !sameRequest(slot.request, tracked.request))
                        continue;
                    if (slot.state == SlotState::Rendering)
                        slot.cancelled = 1;
                    else if (slot.state == SlotState::Requested || slot.state == SlotState::Ready) {
                        slot.state = SlotState::Empty;
                        slot.cancelled = 0;
                    }
                }
            } else if (!lock)
                log.write("IPC mutex busy at shutdown; game stale-input guard must expire tracking");
        } catch (...) {
            log.write("Could not invalidate channel during shutdown");
        }
        SetEvent(channel.ready);
        ownsChannel = false;
    }
    void initialize(const std::wstring &directory) {
        settingsFile = directory + L"SS2VR.ini";
        settings = loadSettings(settingsFile);
        const bool wine=GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"wine_get_version")!=nullptr;
        log.write(wine ? "Wine environment: host inherits the game's prefix; Proton OpenXR uses its native runtime bridge" :
                         "Windows environment: using the packaged loader and selected Windows OpenXR runtime");
        api.open(directory);
        uint32_t count = 0;
        api.check(api.EnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr),
                  "xrEnumerateInstanceExtensionProperties count");
        std::vector<XrExtensionProperties> extensions(count,
                                                      XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
        api.check(api.EnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data()),
                  "xrEnumerateInstanceExtensionProperties");
        const bool supported = std::any_of(extensions.begin(), extensions.end(), [](const auto &e) {
            return !std::strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
        });
        if (!supported) {
            if (wine) log.write("Proton: check wineopenxr registration, DXVK and native OpenXR runtime visibility in Steam's container");
            throw std::runtime_error("Runtime does not offer XR_KHR_D3D11_enable");
        }
        constexpr const char *frameExtension="XR_VALVE_frame_controller_interaction";
        const bool frameController=std::any_of(extensions.begin(),extensions.end(),[](const auto &e) {
            return !std::strcmp(e.extensionName,"XR_VALVE_frame_controller_interaction");
        });
        // Eye gaze drives foveated readback when the runtime reports a tracker.
        eyeGazeExtension = settings.foveation.mode != 0 && std::any_of(extensions.begin(), extensions.end(),
            [](const auto &e) { return !std::strcmp(e.extensionName, "XR_EXT_eye_gaze_interaction"); });
        std::vector<const char *> enabled{XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
        if (frameController) enabled.push_back(frameExtension);
        if (eyeGazeExtension) enabled.push_back("XR_EXT_eye_gaze_interaction");
        XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
        std::strcpy(create.applicationInfo.applicationName, "Serious Sam 2 VR");
        std::strcpy(create.applicationInfo.engineName, "SS2VR host");
        create.applicationInfo.applicationVersion = create.applicationInfo.engineVersion = 1;
        // All used core calls are 1.0; this admits 1.0 runtimes with the required extension.
        create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        create.enabledExtensionCount = uint32_t(enabled.size());
        create.enabledExtensionNames = enabled.data();
        api.check(api.CreateInstance(&create, &instance), "xrCreateInstance");
        log.write(frameController ? "Frame controller extension enabled; native profile bindings available" :
                                   "Frame controller extension unavailable; existing runtime profile fallbacks retained");
        api.instanceFunctions(instance);
        XrInstanceProperties runtime{XR_TYPE_INSTANCE_PROPERTIES};
        const auto propertiesResult = api.GetInstanceProperties(instance, &runtime);
        if (XR_SUCCEEDED(propertiesResult)) {
            char description[XR_MAX_RUNTIME_NAME_SIZE + 96];
            std::snprintf(description, sizeof(description), "OpenXR runtime: %s version=%u.%u.%u",
                runtime.runtimeName, XR_VERSION_MAJOR(runtime.runtimeVersion),
                XR_VERSION_MINOR(runtime.runtimeVersion), XR_VERSION_PATCH(runtime.runtimeVersion));
            log.write(description);
        } else log.result("Runtime properties diagnostic", propertiesResult);
        log.write("OpenXR enabled extension: XR_KHR_D3D11_enable");
        XrSystemGetInfo get{XR_TYPE_SYSTEM_GET_INFO};
        get.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        api.check(api.GetSystem(instance, &get, &system), "xrGetSystem HMD");
        XrSystemEyeGazeInteractionPropertiesEXT gazeProperties{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
        if (eyeGazeExtension) systemProperties.next = &gazeProperties;
        api.check(api.GetSystemProperties(instance, system, &systemProperties), "xrGetSystemProperties");
        systemProperties.next = nullptr;
        eyeGazeSupported = eyeGazeExtension && gazeProperties.supportsEyeGazeInteraction;
        if (systemProperties.graphicsProperties.maxLayerCount < 4)
            throw std::runtime_error("Runtime cannot present stereo, HUD and two wheel layers");
        api.check(api.EnumerateViewConfigurations(instance, system, 0, &count, nullptr),
                  "xrEnumerateViewConfigurations count");
        std::vector<XrViewConfigurationType> configurations(count);
        api.check(api.EnumerateViewConfigurations(instance, system, count, &count, configurations.data()),
                  "xrEnumerateViewConfigurations");
        if (std::find(configurations.begin(), configurations.end(),
                      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) == configurations.end())
            throw std::runtime_error("HMD does not offer PRIMARY_STEREO");
        api.check(api.EnumerateViewConfigurationViews(
                      instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr),
                  "xrEnumerateViewConfigurationViews count");
        if (count != 2)
            throw std::runtime_error("PRIMARY_STEREO must contain exactly two views");
        for (auto &v : viewConfiguration)
            v = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
        api.check(api.EnumerateViewConfigurationViews(instance, system,
                                                      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count,
                                                      viewConfiguration.data()),
                  "xrEnumerateViewConfigurationViews");
        if (count != 2)
            throw std::runtime_error("Runtime changed stereo view count");
        api.check(api.EnumerateEnvironmentBlendModes(
                      instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr),
                  "xrEnumerateEnvironmentBlendModes count");
        std::vector<XrEnvironmentBlendMode> blends(count);
        api.check(api.EnumerateEnvironmentBlendModes(instance, system,
                                                     XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count,
                                                     blends.data()),
                  "xrEnumerateEnvironmentBlendModes");
        if (std::find(blends.begin(), blends.end(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE) == blends.end())
            throw std::runtime_error("HMD does not offer opaque environment blending");
        graphics();
        XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        binding.device = device.p;
        XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
        sessionInfo.next = &binding;
        sessionInfo.systemId = system;
        const XrResult sessionResult=api.CreateSession(instance,&sessionInfo,&session);
        if (XR_FAILED(sessionResult) && wine)
            log.write("Proton D3D11 OpenXR requires DXVK interop; forcing WineD3D/OpenGL is not this bridge's supported path");
        api.check(sessionResult,"xrCreateSession D3D11");
        XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space.poseInReferenceSpace.orientation.w = 1;
        space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        api.check(api.CreateReferenceSpace(session, &space, &local), "xrCreateReferenceSpace LOCAL");
        space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        api.check(api.CreateReferenceSpace(session, &space, &viewSpace), "xrCreateReferenceSpace VIEW");
        actions.create(api, instance, session, frameController, eyeGazeSupported);
        {
            wchar_t lab[2]{}; // Lab: SS2VR_LAB_FOVEATION=0/1/2 overrides [Render] Foveation.
            if (GetEnvironmentVariableW(L"SS2VR_LAB_FOVEATION", lab, 2) == 1 && lab[0] >= L'0' && lab[0] <= L'2')
                settings.foveation.mode = int(lab[0] - L'0');
        }
        const auto &f = settings.foveation;
        foveationActive = systemProperties.graphicsProperties.maxLayerCount >= 5 &&
            (f.mode == 2 || (f.mode == 1 && actions.gazeEnabled));
        {
            char line[192];
            std::snprintf(line, sizeof line, "Foveated readback %s, rendering %s (mode=%d eyeGaze=%s inset=%.2f periphery=%.2f)",
                          foveationActive ? "enabled" : "disabled", renderFoveation() ? "enabled" : "disabled", f.mode,
                          actions.gazeEnabled ? "tracked" : eyeGazeExtension ? "unsupported" : "unavailable",
                          f.inset, f.periphery);
            log.write(line);
        }
        api.check(api.EnumerateSwapchainFormats(session, 0, &count, nullptr),
                  "xrEnumerateSwapchainFormats count");
        std::vector<int64_t> formats(count);
        api.check(api.EnumerateSwapchainFormats(session, count, &count, formats.data()),
                  "xrEnumerateSwapchainFormats");
        if (std::find(formats.begin(), formats.end(), int64_t(DXGI_FORMAT_B8G8R8A8_UNORM)) != formats.end())
            api.colorFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        else if (std::find(formats.begin(), formats.end(), int64_t(DXGI_FORMAT_R8G8B8A8_UNORM)) !=
                 formats.end())
            api.colorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        else
            throw std::runtime_error(
                "Runtime offers neither BGRA8 nor RGBA8 UNORM; sRGB encoding is not assumed");
        const auto &g = systemProperties.graphicsProperties;
        const uint32_t wheelSize = std::min({1024u, g.maxSwapchainImageWidth, g.maxSwapchainImageHeight});
        if (wheelSize < 512)
            throw std::runtime_error("Runtime wheel texture limit too small for legible labels");
        for (auto &wheel : wheels) {
            wheel.chain.create(api, session, wheelSize, wheelSize / 4);
            wheel.bitmap.create(wheelSize, wheelSize / 4);
        }
        options.chain.create(api, session, wheelSize, uint32_t(wheelSize * OptionsAspect));
        options.bitmap.create(wheelSize, uint32_t(wheelSize * OptionsAspect));
        cursorChain.create(api, session, CursorSize, CursorSize);
        cursorUploaded = false;
        log.write("OpenXR D3D11 HMD session initialized; awaiting runtime READY and game dimensions");
    }
    void graphics() {
        XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        api.check(api.GetD3D11GraphicsRequirementsKHR(instance, system, &requirements),
                  "xrGetD3D11GraphicsRequirementsKHR");
        Com<IDXGIFactory1> factory;
        hresult(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(factory.put())),
                "CreateDXGIFactory1", log);
        Com<IDXGIAdapter1> adapter;
        for (UINT i = 0;; ++i) {
            const HRESULT result = factory->EnumAdapters1(i, adapter.put());
            if (result == DXGI_ERROR_NOT_FOUND)
                throw std::runtime_error("Required OpenXR adapter LUID not found");
            hresult(result, "EnumAdapters1", log);
            DXGI_ADAPTER_DESC1 desc{};
            hresult(adapter->GetDesc1(&desc), "GetDesc1", log);
            if (desc.AdapterLuid.LowPart == requirements.adapterLuid.LowPart &&
                desc.AdapterLuid.HighPart == requirements.adapterLuid.HighPart)
                break;
        }
        const D3D_FEATURE_LEVEL available[] = {
            D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
            D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,  D3D_FEATURE_LEVEL_9_1};
        std::vector<D3D_FEATURE_LEVEL> levels;
        for (auto level : available)
            if (level >= requirements.minFeatureLevel)
                levels.push_back(level);
        if (levels.empty())
            throw std::runtime_error("OpenXR minimum D3D feature level unsupported by this host");
        D3D_FEATURE_LEVEL selected{};
        HRESULT result = D3D11CreateDevice(
            adapter.p, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels.data(),
            UINT(levels.size()), D3D11_SDK_VERSION, device.put(), &selected, context.put());
        // Older D3D11 libraries reject newer feature-level enum values. Retry
        // without those values, never dropping below the runtime's minimum.
        while (result == E_INVALIDARG && levels.size() > 1) {
            levels.erase(levels.begin());
            result = D3D11CreateDevice(adapter.p, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels.data(), UINT(levels.size()),
                                       D3D11_SDK_VERSION, device.put(), &selected, context.put());
        }
        hresult(result, "D3D11CreateDevice on required adapter", log);
        DXGI_ADAPTER_DESC1 selectedAdapter{};
        if (SUCCEEDED(adapter->GetDesc1(&selectedAdapter))) {
            char name[160]{}, description[256];
            WideCharToMultiByte(CP_UTF8, 0, selectedAdapter.Description, -1, name, sizeof(name), nullptr, nullptr);
            std::snprintf(description, sizeof(description), "D3D11 adapter: %s vendor=%04x device=%04x feature=%04x",
                          name, selectedAdapter.VendorId, selectedAdapter.DeviceId, static_cast<unsigned>(selected));
            log.write(description);
        }
        if (selected < requirements.minFeatureLevel)
            throw std::runtime_error("D3D device below runtime minimum");
        D3D11_QUERY_DESC query{};
        query.Query = D3D11_QUERY_EVENT;
        hresult(device->CreateQuery(&query, drainQuery.put()), "CreateQuery GPU drain", log);
    }
    void cancelRequests() {
        for (auto &entry : outstanding)
            if (entry.active) {
                entry.expired = true;
                entry.expiry = Expiry::Invalidation;
            }
    }
    void invalidateCachedPair() {
        cachedValid = false;
        cachedPresentation = 0;
        cachedClearance = {};
    }
    void invalidate() {
        actions.invalidateStreams();
        worldSubmission.invalidate();
        presentedPointer = {};
        feedbackSeeded = false;
        menuAnchor.reset();
        hudAnchor.reset();
        invalidateCachedPair();
        cancelRequests();
        for (auto &wheel : wheels)
            wheel.reset();
    }
    Input emptyInput() {
        Input input{};
        input.sequence = ++inputSequence;
        input.tickMs = GetTickCount64();
        input.session = sessionGeneration;
        input.reference = referenceGeneration;
        return input;
    }
    void events() {
        for (;;) {
            XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
            const XrResult result = api.PollEvent(instance, &event);
            if (result == XR_EVENT_UNAVAILABLE)
                break;
            api.check(result, "xrPollEvent");
            switch (event.type) {
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                log.write("OpenXR instance loss pending");
                failed = quit = true;
                invalidate();
                break;
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                const auto &change = *reinterpret_cast<const XrEventDataSessionStateChanged *>(&event);
                if (change.session != session)
                    break;
                state = change.state;
                log.result("OpenXR session state", state);
                if (state != XR_SESSION_STATE_FOCUSED) {
                    if (running && !api.lossPending)
                        actions.stopFeedback(api, session);
                    invalidate();
                }
                if (state == XR_SESSION_STATE_READY && !running) {
                    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                    begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    api.check(api.BeginSession(session, &begin), "xrBeginSession");
                    running = true;
                    if (++sessionGeneration == 0)
                        throw std::runtime_error("Session generation overflow");
                    invalidate();
                } else if (state == XR_SESSION_STATE_STOPPING && running) {
                    finishSessionAcquisitions();
                    api.check(api.EndSession(session), "xrEndSession");
                    running = false;
                } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
                    quit = true;
                    if (state == XR_SESSION_STATE_LOSS_PENDING)
                        failed = true;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
                const auto &change =
                    *reinterpret_cast<const XrEventDataReferenceSpaceChangePending *>(&event);
                if (change.session == session && change.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                    if (referenceChanges.size() >= 32)
                        throw std::runtime_error("Too many pending LOCAL space changes");
                    referenceChanges.push_back(change.changeTime);
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_EVENTS_LOST:
                // A missed reference/session event invalidates our coordinate-generation contract.
                throw std::runtime_error("OpenXR events lost; refusing unknown tracking generation");
            case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED: {
                const auto &change = *reinterpret_cast<const XrEventDataInteractionProfileChanged *>(&event);
                if (change.session == session)
                    actions.invalidateStreams(); // Same-profile rebinding has no per-hand event identifier.
                break;
            }
            default:
                break;
            }
        }
    }
    void applyReferenceChanges(XrTime time) {
        for (auto i = referenceChanges.begin(); i != referenceChanges.end();) {
            if (*i <= time) {
                if (++referenceGeneration == 0)
                    throw std::runtime_error("Reference generation overflow");
                invalidate();
                i = referenceChanges.erase(i);
                log.write("LOCAL reference generation changed");
            } else
                ++i;
        }
    }
    FrameContext frameContext() const {
        const auto &shared = *channel.shared;
        return {GetTickCount64(),
                shared.ui.tickMs,
                sessionGeneration,
                referenceGeneration,
                trackingEpoch(shared),
                shared.ui.trackingGeneration,
                shared.width,
                shared.height,
                shared.rendererReady != 0,
                shared.ui.gameplay != 0 && !shared.menu.visible,
                shared.latest.focused != 0,
                shared.latest.headValid != 0 && viewsTracked};
    }
    void reapLocked() {
        auto current = frameContext();
        for (unsigned i = 0; i != 2; ++i) {
            switch (pollSlot(channel.shared->slot[i], outstanding[i], current)) {
            case PollResult::IdentityError:
                throw std::runtime_error("IPC slot identity/state changed");
            case PollResult::AgeExpired:
                ++timeouts;
                break;
            case PollResult::Invalidated:
                ++discarded;
                break;
            case PollResult::Retired:
                ++retired;
                break;
            default:
                break;
            }
        }
    }
    // Loading blocks the game's presentation for seconds at a time: keep the
    // last flat frame (the loading screen) up instead of showing nothing.
    static constexpr ULONGLONG MenuHoldMs = 60000;
    struct Snapshot {
        uint32_t width = 0, height = 0;
        bool renderer = false;
        Ui ui{};
        bool menuVisible = false;
    };
    bool prepareHudAnchor(const Snapshot &snapshot, const Input &input) {
        const ULONGLONG now = GetTickCount64();
        if (!snapshot.ui.gameplay || snapshot.menuVisible || !input.focused || !input.headValid ||
            !snapshot.ui.tickMs || now < snapshot.ui.tickMs || now - snapshot.ui.tickMs > UiFreshMs) {
            hudAnchor.reset();
            return false;
        }
        return hudAnchor.update(input.head, true, now, recenterUi, settings.comfort);
    }
    bool publish(const Input &input, Snapshot &snapshot) {
        Mutex lock(channel, 1);
        if (!lock) {
            if (GetTickCount64() - lastMutexDiagnostic > 5000) {
                log.write("IPC mutex busy; input/render publication deferred");
                lastMutexDiagnostic = GetTickCount64();
            }
            return false;
        }
        auto &shared = *channel.shared;
        if (shared.magic != Magic || shared.abi != Abi || shared.bytes != sizeof(Shared) ||
            shared.hostPid != GetCurrentProcessId())
            throw std::runtime_error("IPC header or host ownership changed");
        if (shared.shutdown) {
            quit = true;
            invalidate();
        }
        shared.latest =
            quit ? emptyInput() : input; // Independent of render slot availability and renderer readiness.
        for(unsigned h=0;h<2;++h)shared.gripPoseGeneration[h]=quit?0:actions.gripPoseStream[h].generation;
        shared.worldSubmission=worldSubmission.value;
        const auto eyeSize = recommendedEye();
        shared.recommendedWidth = eyeSize.first;
        shared.recommendedHeight = eyeSize.second;
        openSharedImages(shared.images);
        reapLocked();
        snapshot = {shared.width, shared.height, shared.rendererReady != 0, shared.ui,
                    shared.menu.visible && GetTickCount64() - shared.menu.tickMs < MenuHoldMs};
        return true;
    }
    void publishWorldSubmission() {
        Mutex lock(channel,1);
        if(!lock)return; // The durable epoch survives until the next publication.
        auto &shared=*channel.shared;
        if(shared.magic!=Magic || shared.abi!=Abi || shared.bytes!=sizeof(Shared) ||
           shared.hostPid!=GetCurrentProcessId())
            throw std::runtime_error("World feedback IPC ownership changed");
        shared.worldSubmission=worldSubmission.value;
    }
    // OpenXR recommendedImageRect for the stereo views (the larger of the two),
    // scaled only by the user's [Render] Scale and bounded by the runtime maxima.
    std::pair<uint32_t, uint32_t> recommendedEye() const {
        uint32_t width = 0, height = 0, maxWidth = MaxDimension, maxHeight = MaxDimension;
        for (const auto &v : viewConfiguration) {
            width = std::max(width, v.recommendedImageRectWidth);
            height = std::max(height, v.recommendedImageRectHeight);
            maxWidth = std::min(maxWidth, v.maxImageRectWidth);
            maxHeight = std::min(maxHeight, v.maxImageRectHeight);
        }
        const auto &g = systemProperties.graphicsProperties;
        maxWidth = std::min(maxWidth, g.maxSwapchainImageWidth);
        maxHeight = std::min(maxHeight, g.maxSwapchainImageHeight);
        if (!width || !height || !maxWidth || !maxHeight) return {0, 0};
        const double scale = settings.renderScale;
        return {std::clamp(uint32_t(std::lround(width * scale)), 1u, maxWidth),
                std::clamp(uint32_t(std::lround(height * scale)), 1u, maxHeight)};
    }
    // Reopen the game's pixel sections when it publishes a new generation.
    // Private lab only: one zero-copy left eye written as a PPM beside the log,
    // proving what the headset receives (lab captures otherwise use pixel pairs).
    void probeZeroCopy(ID3D11Texture2D *source, const wchar_t *name = L"zero-copy-probe-left.ppm") {
        wchar_t lab[2]{};
        if (GetEnvironmentVariableW(L"SS2VR_LAB_PRIVATE_DISPLAY", lab, 2) != 1 || lab[0] != L'1' || !source)
            return;
        D3D11_TEXTURE2D_DESC description{};
        source->GetDesc(&description);
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        description.MiscFlags = 0;
        Com<ID3D11Texture2D> staging;
        if (FAILED(device->CreateTexture2D(&description, nullptr, staging.put())))
            return;
        context->CopyResource(staging.p, source);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.p, 0, D3D11_MAP_READ, 0, &mapped)))
            return;
        if (auto *file = _wfopen((executableDirectory() + name).c_str(), L"wb")) {
            std::fprintf(file, "P6\n%u %u\n255\n", description.Width, description.Height);
            std::vector<uint8_t> row(size_t(description.Width) * 3);
            for (uint32_t y = 0; y < description.Height; ++y) {
                const auto *bgra = static_cast<const uint8_t *>(mapped.pData) + size_t(y) * mapped.RowPitch;
                for (uint32_t x = 0; x < description.Width; ++x) {
                    row[x * 3] = bgra[x * 4 + 2];
                    row[x * 3 + 1] = bgra[x * 4 + 1];
                    row[x * 3 + 2] = bgra[x * 4];
                }
                std::fwrite(row.data(), 1, row.size(), file);
            }
            std::fclose(file);
            log.write("Zero-copy probe written: zero-copy-probe-left.ppm");
        }
        context->Unmap(staging.p, 0);
    }
    // Opens the game's shared eye images once per generation and acknowledges
    // them; until then (or if this fails) pairs arrive as pixel sections.
    void openSharedImages(SharedImages &images) {
        if (!device.p || images.generation == sharedTried) {
            images.opened = images.generation && images.generation == sharedOpened ? sharedOpened : 0;
            return;
        }
        sharedTried = images.generation;
        for (auto &row : sharedEye)
            for (auto &image : row)
                image.reset();
        sharedOpened = sharedWidth = sharedHeight = 0;
        images.opened = 0;
        if (!images.generation)
            return;
        // Copies need the swapchain's byte layout (BGRA); RGBA swapchains keep pixels.
        bool ok = api.colorFormat == DXGI_FORMAT_B8G8R8A8_UNORM && images.width && images.height &&
            images.width <= MaxDimension && images.height <= MaxDimension;
        HRESULT failure = S_OK;
        for (unsigned slot = 0; slot != 2 && ok; ++slot)
            for (unsigned h = 0; h != 2 && ok; ++h) {
                const HANDLE handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(images.handle[slot][h]));
                failure = device->OpenSharedResource(handle, __uuidof(ID3D11Texture2D),
                                                     reinterpret_cast<void **>(sharedEye[slot][h].put()));
                ok = SUCCEEDED(failure) && sharedEye[slot][h].p;
                if (ok) {
                    D3D11_TEXTURE2D_DESC description{};
                    sharedEye[slot][h]->GetDesc(&description);
                    ok = description.Width == images.width && description.Height == images.height &&
                        description.MipLevels == 1 && description.ArraySize == 1 && description.SampleDesc.Count == 1 &&
                        (description.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                         description.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS);
                }
            }
        char message[160];
        if (!ok) {
            for (auto &row : sharedEye)
                for (auto &image : row)
                    image.reset();
            std::snprintf(message, sizeof(message), "Shared eye images generation=%u unavailable hr=%08lx; pixel pairs",
                          images.generation, static_cast<unsigned long>(failure));
            log.write(message);
            return;
        }
        sharedOpened = images.generation;
        sharedWidth = images.width;
        sharedHeight = images.height;
        images.opened = sharedOpened;
        std::snprintf(message, sizeof(message), "Shared eye images generation=%u %ux%u opened: zero-copy pairs",
                      images.generation, images.width, images.height);
        log.write(message);
    }
    bool currentPixels(const PixelSections &published) {
        if (!published.generation) return false;
        if (pixels.matches(published.generation, published.width, published.height)) return true;
        if (!pixels.open(channel.token, published.generation, published.width, published.height, false)) {
            log.write("Pixel sections unavailable; frame skipped");
            return false;
        }
        char message[128];
        std::snprintf(message, sizeof(message), "Pixel sections generation=%u %ux%u", published.generation,
                      published.width, published.height);
        log.write(message);
        return true;
    }
    void eyeChains(uint32_t width, uint32_t height) {
        if (!width || !height || width > MaxDimension || height > MaxDimension)
            throw std::runtime_error("Game dimensions outside IPC bounds");
        for (const auto &v : viewConfiguration) {
            if (width > v.maxImageRectWidth || height > v.maxImageRectHeight || v.maxSwapchainSampleCount < 1)
                throw std::runtime_error("Game dimensions exceed stereo runtime view limits");
        }
        const auto &g = systemProperties.graphicsProperties;
        if (width > g.maxSwapchainImageWidth || height > g.maxSwapchainImageHeight)
            throw std::runtime_error("Game dimensions exceed runtime swapchain limits");
        if (eye[0].width == width && eye[0].height == height && eye[1].handle)
            return;
        invalidateCachedPair();
        cancelRequests();
        if (!drainGpu())
            throw std::runtime_error("GPU did not drain before eye swapchain resize");
        for (auto &chain : eye)
            chain.destroy();
        for (unsigned h = 0; h != 2; ++h)
            eye[h].create(api, session, width, height);
        char message[128];
        std::snprintf(message, sizeof(message), "Stereo BGRA eye swapchains: %u x %u", width, height);
        log.write(message);
    }
    bool viewsAt(XrTime time, std::array<XrView, 2> &views) {
        for (auto &view : views)
            view = {XR_TYPE_VIEW};
        XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
        locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate.displayTime = time;
        locate.space = local;
        XrViewState validity{XR_TYPE_VIEW_STATE};
        uint32_t count = 0;
        api.check(api.LocateViews(session, &locate, &validity, 2, &count, views.data()), "xrLocateViews");
        constexpr auto needed = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
                                XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
        if (count != 2)
            throw std::runtime_error("Runtime returned a non-stereo view count");
        if ((validity.viewStateFlags & needed) != needed) {
            ++invalidViews;
            if (!settings.labFixedPose)
                return false;
            // Measurement only ([Lab] FixedPose): an untracked headset (e.g. base
            // stations off) gets a fixed head at the origin with the runtime's FOV.
            for (unsigned h = 0; h != 2; ++h) {
                auto &view = views[h];
                view.pose = {{0, 0, 0, 1}, {h ? .032f : -.032f, 0, 0}};
                auto &f = view.fov;
                if (!std::isfinite(f.angleLeft) || f.angleLeft >= f.angleRight || f.angleDown >= f.angleUp)
                    f = {-.8f, .8f, .8f, -.8f};
            }
        }
        for (const auto &view : views) {
            const auto &f = view.fov;
            if (!validPose(pose(view.pose)) || !std::isfinite(f.angleLeft) || !std::isfinite(f.angleRight) ||
                !std::isfinite(f.angleUp) || !std::isfinite(f.angleDown) || f.angleLeft >= f.angleRight ||
                f.angleDown >= f.angleUp || std::abs(f.angleLeft) >= Pi * .5f ||
                std::abs(f.angleRight) >= Pi * .5f || std::abs(f.angleUp) >= Pi * .5f ||
                std::abs(f.angleDown) >= Pi * .5f)
                throw std::runtime_error(
                    "Runtime returned invalid pose or FOV unsupported by perspective IPC renderer");
        }
        return true;
    }
    Input sample(XrTime time, const std::array<XrView, 2> &views, bool validViews) {
        Input input = emptyInput();
        input.focused = state == XR_SESSION_STATE_FOCUSED ? 1 : 0;
        XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
        api.check(api.LocateSpace(viewSpace, local, time, &head), "xrLocateSpace head VIEW");
        if (located(head)) {
            input.head = pose(head.pose);
            input.headValid = 1;
        } else if (validViews) {
            const Pose a = pose(views[0].pose), b = pose(views[1].pose);
            const float sign = a.q.x * b.q.x + a.q.y * b.q.y + a.q.z * b.q.z + a.q.w * b.q.w < 0 ? -1.f : 1.f;
            input.head = {normalize({a.q.x + b.q.x * sign, a.q.y + b.q.y * sign, a.q.z + b.q.z * sign,
                                     a.q.w + b.q.w * sign}),
                          (a.p + b.p) * .5f};
            input.headValid = 1;
        }
        actions.sample(api, instance, session, local, time, input, validViews);
        return input;
    }
    void discardEyeAcquisitions() {
        for (auto *chains : {eye, inset})
            for (unsigned h = 0; h != 2; ++h) {
                auto &chain = chains[h];
                if (chain.ownership.acquired && chain.waitImage()) {
                    // Release changes the runtime's most-recent image even without a write.
                    invalidateCachedPair();
                    chain.releaseWaited();
                }
            }
    }
    // Lab (SS2VR_LAB_GAZE=yaw,pitch in degrees, head-relative): a simulated eye
    // tracker for the eye-tracked foveation path (the lab runtime has none).
    static bool labGazeAngles(float &yawDegrees, float &pitchDegrees) {
        static const int set = [] {
            wchar_t v[32]{};
            return GetEnvironmentVariableW(L"SS2VR_LAB_GAZE", v, 32) ? 1 : 0;
        }();
        static float y = 0, p = 0;
        static const bool parsed = [] {
            wchar_t v[32]{};
            return GetEnvironmentVariableW(L"SS2VR_LAB_GAZE", v, 32) && swscanf(v, L"%f,%f", &y, &p) == 2;
        }();
        yawDegrees = y; pitchDegrees = p;
        return set && parsed;
    }
    static bool labGaze(const Input &input, Vec3 &gaze) {
        float y = 0, p = 0;
        if (!labGazeAngles(y, p) || !input.headValid) return false;
        const float a = y * Pi / 180, b = p * Pi / 180;
        gaze = rotate(input.head.q, Vec3{-std::sin(a) * std::cos(b), std::sin(b), -std::cos(a) * std::cos(b)});
        return true;
    }
    // Foveated rendering in the game: [Render] Foveation 2 always, 1 with eye gaze.
    bool renderFoveation() const {
        const int mode = settings.foveation.mode;
        float y = 0, p = 0;
        return mode == 2 || (mode == 1 && (actions.gazeEnabled || labGazeAngles(y, p)));
    }
    void insetChains(uint32_t width, uint32_t height) {
        if (inset[0].width == width && inset[0].height == height && inset[1].handle)
            return;
        invalidateCachedPair();
        cancelRequests();
        if (!drainGpu())
            throw std::runtime_error("GPU did not drain before inset swapchain resize");
        for (auto &chain : inset)
            chain.destroy();
        for (auto &chain : inset)
            chain.create(api, session, width, height);
        char message[128];
        std::snprintf(message, sizeof(message), "Foveated inset swapchains: %u x %u", width, height);
        log.write(message);
    }
    void finishSessionAcquisitions() {
        invalidate();
        menuSequence = 0;
        const uint64_t deadline = GetTickCount64() + 500;
        for (;;) {
            bool retiredAll = true;
            for (auto &chain : eye)
                retiredAll = chain.releaseAcquired() && retiredAll;
            for (auto &wheel : wheels)
                retiredAll = wheel.chain.releaseAcquired() && retiredAll;
            retiredAll = options.chain.releaseAcquired() && retiredAll;
            retiredAll = menuChain.releaseAcquired() && retiredAll;
            retiredAll = cursorChain.releaseAcquired() && retiredAll;
            for (auto &chain : inset)
                retiredAll = chain.releaseAcquired() && retiredAll;
            if (retiredAll)
                return;
            if (api.lossPending || GetTickCount64() >= deadline)
                throw std::runtime_error("Image retirement before session stop timed out");
            Sleep(1);
        }
    }
    void pollEyes() {
        int candidate = -1;
        {
            Mutex lock(channel, 1);
            if (!lock)
                return;
            reapLocked();
            // With two requests in flight both may be Ready: show the newest and
            // retire the older one so time never runs backwards.
            for (int i = 0; i < 2; i++)
                if (outstanding[i].active && !outstanding[i].expired &&
                    channel.shared->slot[i].state == SlotState::Ready && channel.shared->slot[i].gpuReady &&
                    (candidate < 0 || channel.shared->slot[i].request.sequence >
                                          channel.shared->slot[candidate].request.sequence))
                    candidate = i;
            if (candidate >= 0) {
                auto &older = channel.shared->slot[1 - candidate];
                if (outstanding[1 - candidate].active && older.state == SlotState::Ready && older.gpuReady &&
                    older.request.sequence < channel.shared->slot[candidate].request.sequence) {
                    older.state = SlotState::Empty;
                    older.cancelled = 0;
                    older.presentation = 0;
                    older.presentationReserved = 0;
                    older.headClearance = {};
                    outstanding[1 - candidate] = {};
                    ++discarded;
                }
            }
        }
        if (candidate < 0) {
            discardEyeAcquisitions();
            return;
        }
        // No IPC lock across XR waits. Ready remains owned until BOTH images are writable.
        const bool left = eye[0].waitImage(), right = eye[1].waitImage();
        // Inset images only for foveated readback (zero-copy pairs are never foveated
        // readback: with foveated rendering the game returns full-size eyes).
        const bool insets = foveationActive && !sharedOpened;
        const bool insetLeft = !insets || inset[0].waitImage(), insetRight = !insets || inset[1].waitImage();
        if (!left || !right || !insetLeft || !insetRight) {
            ++imageTimeouts;
            return;
        }
        if (!bothImagesReady(eye[0].ownership, eye[1].ownership) ||
            (insets && !bothImagesReady(inset[0].ownership, inset[1].ownership)))
            throw std::runtime_error("Stereo wait barrier failed");
        Request reply;
        HeadClearance clearance;
        uint64_t deadline = 0;
        uint32_t presentation = 0;
        bool accepted = false;
        uint8_t *source[2]{}, *insetSource[2]{};
        bool zeroCopy = false;
        Fov insetFov[2];
        {
            Mutex lock(channel, 0);
            if (lock) {
                reapLocked();
                auto &slot = channel.shared->slot[candidate];
                auto &tracked = outstanding[candidate];
                if (tracked.active && !tracked.expired && slot.state == SlotState::Ready &&
                    eligibleFrame(tracked.request, tracked.deadline, frameContext())) {
                    reply = slot.request;
                    if (slot.presentationReserved || (slot.presentation & ~NativeUiComplete) ||
                        (slot.presentation == NativeUiComplete && reply.uiRequested != 1))
                        throw std::runtime_error("IPC response presentation metadata invalid");
                    if(!validHeadClearance(slot.headClearance))
                        throw std::runtime_error("IPC head clearance metadata invalid");
                    clearance=slot.headClearance;
                    deadline = tracked.deadline;
                    presentation = slot.presentation;
                    // The pair is uploaded straight from its pixel sections after the
                    // lock: only this thread can request the slot again afterwards.
                    zeroCopy = slot.sharedImage != 0;
                    accepted = zeroCopy ?
                        slot.gpuReady && slot.sharedImage == sharedOpened && reply.foveated != 1 &&
                            reply.width == sharedWidth && reply.height == sharedHeight &&
                            reply.width == eye[0].width && reply.height == eye[0].height &&
                            sharedEye[candidate][0].p && sharedEye[candidate][1].p :
                        currentPixels(channel.shared->pixels) &&
                        pixels.matches(slot.pixelGeneration, reply.width, reply.height) &&
                        (reply.foveated != 1 || insets);
                    for (unsigned h = 0; h < 2; h++)
                        source[h] = accepted && !zeroCopy ? pixels.buffer(slotPixelBuffer(candidate, h)) : nullptr;
                    if (accepted && reply.foveated == 1) {
                        accepted = foveationFits(reply.width, reply.height, reply.lowWidth, reply.lowHeight,
                                                 reply.insetWidth, reply.insetHeight);
                        for (unsigned h = 0; accepted && h < 2; h++) {
                            const PixelRect rect{slot.insetRect[h][0], slot.insetRect[h][1], slot.insetRect[h][2],
                                                 slot.insetRect[h][3]};
                            accepted = rect.w == int32_t(reply.insetWidth) && rect.h == int32_t(reply.insetHeight) &&
                                rect.x >= 0 && rect.y >= 0 && rect.x <= int32_t(reply.width) - rect.w &&
                                rect.y <= int32_t(reply.height) - rect.h;
                            insetSource[h] = source[h] + size_t(reply.lowWidth) * reply.lowHeight * 4;
                            insetFov[h] = rectFov(reply.fov[h], reply.width, reply.height, rect);
                        }
                    }
                    slot.state = SlotState::Empty;
                    slot.cancelled = 0;
                    slot.presentation = 0;
                    slot.presentationReserved = 0;
                    slot.headClearance = {};
                    slot.sharedImage = slot.gpuReady = 0;
                    tracked = {};
                    if (accepted) {
                        ++completed;
                        if (presentation == NativeUiComplete) ++uiPairs;
                        if (clearance.mode == HeadClearanceMode::Opaque) ++clearedPairs;
                    }
                }
            }
        }
        if (!accepted) {
            discardEyeAcquisitions();
            return;
        }
        invalidateCachedPair(); // A failure between releases must never expose a mixed pair.
        const LARGE_INTEGER uploadStart = perfNow();
        struct UploadTimer {
            Host &host; LARGE_INTEGER start;
            ~UploadTimer() { host.perfUpload += perfMs(start, perfNow()); ++host.perfUploads; }
        } uploadTimer{*this, uploadStart};
        if (zeroCopy) {
            // The slot is requested again only after this returns, so the copy
            // must have executed before the game can render into the image again.
            eye[0].copyWaited(context.p, sharedEye[candidate][0].p);
            eye[1].copyWaited(context.p, sharedEye[candidate][1].p);
            if (!drainGpu())
                throw std::runtime_error("Zero-copy eye copy did not complete");
            eye[0].releaseWaited();
            eye[1].releaseWaited();
            perfPairAge += double(GetTickCount64() - (deadline - FrameAgeMs));
            if (++zeroCopyPairs == 600)
                probeZeroCopy(sharedEye[candidate][0].p);
            if (zeroCopyPairs == 600 || zeroCopyPairs == 1500) // Lab: both eyes, early and later.
                probeZeroCopy(sharedEye[candidate][1].p, zeroCopyPairs == 600 ? L"zero-copy-probe-right.ppm" : L"zero-copy-probe-right-late.ppm");
            if (zeroCopyPairs == 1500)
                probeZeroCopy(sharedEye[candidate][0].p, L"zero-copy-probe-left-late.ppm");
        } else {
            eye[0].uploadWaited(context.p, source[0]);
            eye[1].uploadWaited(context.p, source[1]);
        }
        if (reply.foveated == 1)
            for (unsigned h = 0; h != 2; ++h) {
                // The inset fades over its border so the seam to the base view is soft.
                featherAlpha(insetSource[h], reply.insetWidth, reply.insetHeight, std::max(8u, reply.insetWidth / 16));
                inset[h].uploadWaited(context.p, insetSource[h]);
                cachedInset[h] = insetFov[h];
            }
        else if (insets)
            for (auto &chain : inset)
                chain.releaseWaited();
        hresult(device->GetDeviceRemovedReason(), "D3D11 device removed", log);
        if (!api.lossPending) {
            cachedRequest = reply;
            cachedClearance = clearance;
            // A dropped pair or two must not blank the world: the last pair stays
            // up (the runtime reprojects it to the current head pose) for a second.
            cachedDeadline = deadline - FrameAgeMs + CachedPairHoldMs;
            cachedPresentation = presentation;
            cachedFoveated = reply.foveated == 1;
            cachedValid = true;
        }
    }
    void requestEyes(Request request, bool allowSecond = true) {
        Mutex lock(channel, 2);
        if (!lock)
            return;
        reapLocked();
        auto &shared = *channel.shared;
        request.trackingGeneration = trackingEpoch(shared);
        uint64_t deadline = GetTickCount64() + FrameAgeMs;
        if (shared.shutdown || !eligibleFrame(request, deadline, frameContext()))
            return;
        // One request may wait while the game holds the other slot (rendering it,
        // or Ready and waiting for its GPU work or for this loop), so the game finds
        // work as soon as it finishes a pair instead of drawing a frame the headset
        // never sees. A request the game has not taken yet is late-latched: it is
        // replaced by this loop's newer poses, so the game always renders from the
        // freshest prediction. Expiring/cancelled slots hold further requests.
        unsigned active = 0;
        int waiting = -1;
        for (unsigned i = 0; i != 2; ++i) {
            if (!outstanding[i].active)
                continue;
            ++active;
            if (outstanding[i].expired || shared.slot[i].cancelled) {
                ++fullSlots;
                return;
            }
            if (shared.slot[i].state == SlotState::Requested)
                waiting = int(i);
        }
        if (waiting >= 0) {
            auto &slot = shared.slot[waiting];
            outstanding[waiting] = {true, false, false, Expiry::None, deadline, request};
            slot.request = request;
            ++latched;
            SetEvent(channel.ready);
            return;
        }
        // A second request in flight only helps when the game is behind the
        // display; when it keeps up, it would render a pair that is discarded.
        if (active > 1 || (active == 1 && !allowSecond)) {
            ++fullSlots;
            return;
        }
        int index = -1;
        for (int i = 0; i < 2; i++)
            if (shared.slot[i].state == SlotState::Empty) {
                index = i;
                break;
            }
        if (index < 0)
            throw std::runtime_error("Admission found no owned empty slot");
        auto &slot = shared.slot[index];
        outstanding[index] = {true, false, false, Expiry::None, deadline, request};
        slot.request = request;
        slot.cancelled = 0;
        slot.presentation = 0;
        slot.presentationReserved = 0;
        slot.headClearance = {};
        slot.state = SlotState::Requested;
        SetEvent(channel.ready);
    }
    bool projection(XrCompositionLayerProjection &layer,
                    std::array<XrCompositionLayerProjectionView, 2> &views,
                    XrCompositionLayerProjection &insetLayer, std::array<XrCompositionLayerProjectionView, 2> &insetViews) {
        if (!cachedValid || !eye[0].ownership.previousImageAvailable() ||
            !eye[1].ownership.previousImageAvailable() ||
            (cachedFoveated && (!inset[0].ownership.previousImageAvailable() || !inset[1].ownership.previousImageAvailable())))
            return false;
        {
            Mutex lock(channel, 0);
            if (!lock || !eligibleFrame(cachedRequest, cachedDeadline, frameContext()))
                return false;
        }
        layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        layer.space = local;
        for (unsigned h = 0; h != 2; ++h) {
            views[h] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            views[h].pose = xrPose(cachedRequest.eye[h]);
            const auto &f = cachedRequest.fov[h];
            views[h].fov = {f.left, f.right, f.up, f.down};
            views[h].subImage = eye[h].subimage();
        }
        layer.viewCount = 2;
        layer.views = views.data();
        insetLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        if (cachedFoveated) {
            // Same pose and space as the base view: the inset is that view's own
            // full-resolution crop, so it stays aligned through reprojection.
            insetLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                    XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
            insetLayer.space = local;
            for (unsigned h = 0; h != 2; ++h) {
                insetViews[h] = views[h];
                const auto &f = cachedInset[h];
                insetViews[h].fov = {f.left, f.right, f.up, f.down};
                insetViews[h].subImage = inset[h].subimage();
            }
            insetLayer.viewCount = 2;
            insetLayer.views = insetViews.data();
        }
        return true;
    }
    bool finalProjectionEligible(const Input &input, bool validViews,const std::array<XrView,2>& views) {
        if (!cachedValid || !validViews || !input.focused || !input.headValid) {
            invalidateCachedPair();
            return false;
        }
        const Pose currentEyes[2]{pose(views[0].pose),pose(views[1].pose)};
        if(!headClearanceAllows(cachedClearance,cachedRequest,input,currentEyes,GetTickCount64())) {
            invalidateCachedPair();
            return false;
        }
        Mutex lock(channel, 0);
        if (!lock)
            return false;
        if (!eligibleFrame(cachedRequest, cachedDeadline, frameContext())) {
            invalidateCachedPair();
            return false;
        }
        return true;
    }
    bool admitFinalWorld(std::vector<const XrCompositionLayerBaseHeader *> &layers,
                         const XrCompositionLayerBaseHeader *world, const Input &input, bool validViews,
                         const std::array<XrView,2>& views, const XrCompositionLayerBaseHeader *insetWorld = nullptr) {
        if (std::find(layers.begin(), layers.end(), world) == layers.end())
            return false;
        if (finalProjectionEligible(input, validViews,views))
            return true;
        layers.erase(std::remove(layers.begin(), layers.end(), world), layers.end());
        if (insetWorld)
            layers.erase(std::remove(layers.begin(), layers.end(), insetWorld), layers.end());
        return false;
    }
    // --- VR options panel ---
    enum OptionsItem : unsigned {
        OptFoveation, OptInset, OptPeriphery, OptTurning, OptTurnSpeed, OptLasers, OptFireHaptic, OptDamageHaptic,
        OptScale, OptInstancing, OptClose, OptCount
    };
    bool instancingSetting() const { return loadSettings(settingsFile).singlePassInstancing; }
    std::vector<OptionsRow> optionRows() const {
        const auto percent = [](float v) { wchar_t b[16]; std::swprintf(b, 16, L"%d%%", int(std::lround(v * 100))); return std::wstring(b); };
        const auto &f = settings.foveation;
        std::vector<OptionsRow> rows(OptCount);
        rows[OptFoveation] = {L"Foveated rendering", f.mode == 0 ? L"Off" : f.mode == 1 ? L"Eye-tracked" : L"Fixed (centre)",
                              f.mode == 1 && !actions.gazeEnabled ? L"(no eye tracking: off)" : L""};
        rows[OptInset] = {L"Sharp region size", percent(f.inset)};
        rows[OptPeriphery] = {L"Periphery resolution", percent(f.periphery)};
        rows[OptTurning] = {L"Turning", settings.snapTurn ? L"Snap" : L"Smooth"};
        wchar_t speed[24];
        std::swprintf(speed, 24, L"%d deg/s", int(std::lround(settings.smoothTurnRate * 180 / Pi)));
        rows[OptTurnSpeed] = {L"Turn speed", speed};
        rows[OptLasers] = {L"Aim lasers", settings.lasers ? L"On" : L"Off"};
        rows[OptFireHaptic] = {L"Fire haptics", percent(settings.fireHaptic)};
        rows[OptDamageHaptic] = {L"Damage haptics", percent(settings.damageHaptic)};
        wchar_t scale[24];
        std::swprintf(scale, 24, L"%.2gx", double(settings.renderScale));
        rows[OptScale] = {L"Render scale", scale};
        rows[OptInstancing] = {L"Instanced single pass", instancingSetting() ? L"On" : L"Off", L"(experimental)"};
        rows[OptClose] = {L"Close", L""};
        return rows;
    }
    // Into SS2VR.user.ini (see userSettingsFile), which the game watches.
    void saveSetting(const wchar_t *section, const wchar_t *key, double value) {
        wchar_t text[32];
        std::swprintf(text, 32, L"%g", value);
        if (!WritePrivateProfileStringW(section, key, text, userSettingsFile(settingsFile).c_str())) {
            char line[160];
            std::snprintf(line, sizeof line, "VR options: could not save %ls/%ls", section, key);
            log.write(line);
        }
    }
    static float stepped(float value, int direction, float step, float low, float high) {
        const float next = std::round((value + direction * step) / step) * step;
        return next > high + step * .5f ? low : next < low - step * .5f ? high : std::clamp(next, low, high);
    }
    void changeOption(unsigned item, int direction) {
        auto &f = settings.foveation;
        switch (item) {
        case OptFoveation: f.mode = (f.mode + 3 + direction) % 3; saveSetting(L"Render", L"Foveation", f.mode); break;
        case OptInset: f.inset = stepped(f.inset, direction, .05f, .25f, .7f); saveSetting(L"Render", L"FoveationInset", f.inset); break;
        case OptPeriphery:
            f.periphery = stepped(f.periphery, direction, .05f, .25f, .8f);
            saveSetting(L"Render", L"FoveationPeriphery", f.periphery);
            break;
        case OptTurning: settings.snapTurn = !settings.snapTurn; saveSetting(L"Comfort", L"SnapTurn", settings.snapTurn); break;
        case OptTurnSpeed: {
            const float degrees = stepped(settings.smoothTurnRate * 180 / Pi, direction, 30, 30, 360);
            settings.smoothTurnRate = degrees * Pi / 180;
            saveSetting(L"Comfort", L"SmoothTurnDegreesPerSecond", degrees);
            break;
        }
        case OptLasers: settings.lasers = !settings.lasers; saveSetting(L"Laser", L"Enabled", settings.lasers); break;
        case OptFireHaptic:
            settings.fireHaptic = stepped(settings.fireHaptic, direction, .1f, 0, 1);
            saveSetting(L"Feedback", L"FireStrength", settings.fireHaptic);
            break;
        case OptDamageHaptic:
            settings.damageHaptic = stepped(settings.damageHaptic, direction, .1f, 0, 1);
            saveSetting(L"Feedback", L"DamageStrength", settings.damageHaptic);
            break;
        case OptScale: // Applies at once: the recommended eye size is republished every loop.
            settings.renderScale = stepped(settings.renderScale, direction, .1f, .5f, 2.f);
            saveSetting(L"Render", L"Scale", settings.renderScale);
            break;
        case OptInstancing: saveSetting(L"Render", L"SinglePassInstancing", instancingSetting() ? 0 : 1); break;
        default: break;
        }
        char line[96];
        std::snprintf(line, sizeof line, "VR options: changed item %u (direction %d)", item, direction);
        log.write(line);
    }
    void openOptions(const Input &input) {
        const float heading = horizontalHeading(input.head.q);
        const Quat facing = yaw(heading);
        options.anchor = {facing, input.head.p + rotate(facing, Vec3{0, -.12f, -OptionsDistance})};
        options.open = true;
        options.selected = 0;
        options.uploaded = false;
        options.stepX = options.stepY = 0;
        // Controls already held when it opens act only after release.
        options.triggerHeld[0] = input.trigger[0] > .5f; options.triggerHeld[1] = input.trigger[1] > .5f;
        options.buttonHeld = options.closeHeld = true;
        log.write("VR options opened");
    }
    // The wheel's options bubble: released while hovered (the game closes the wheel).
    void watchOptionsBubble(const Ui &ui, const Input &input) {
        const int32_t bubble = wheelBubbleHover(WheelBubble::VrOptions);
        for (unsigned h = 0; h != 2; ++h) {
            const bool open = ui.wheel[h].open != 0;
            if (options.wheelOpen[h] && !open && options.wheelHover[h] == bubble && !options.open && input.headValid)
                openOptions(input);
            static const bool trace = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_WHEEL_TRACE", v, 2) == 1 && v[0] == L'1'; }();
            if (trace && open && ui.wheel[h].hover != options.wheelHover[h]) {
                char line[96];
                std::snprintf(line, sizeof line, "Lab wheel hover hand=%u hover=%d count=%u", h, ui.wheel[h].hover, ui.wheel[h].count);
                log.write(line);
            }
            options.wheelOpen[h] = open;
            options.wheelHover[h] = open ? ui.wheel[h].hover : -1;
        }
    }
    // Navigation from this loop's input; the input the game sees is then neutral.
    void optionsInput(Input &input) {
        const ULONGLONG now = GetTickCount64();
        const auto axis = [&](unsigned c) {
            const float r = input.axis[1][c], l = input.axis[0][c];
            return std::abs(r) >= std::abs(l) ? r : l;
        };
        const auto edge = [&](float value, int &held, ULONGLONG &repeat) {
            const int d = value > .6f ? 1 : value < -.6f ? -1 : 0;
            int fire = 0;
            if (d && (d != held || now >= repeat)) { fire = d; repeat = now + (d != held ? 400 : 160); }
            held = d;
            return fire;
        };
        if (const int y = edge(axis(1), options.stepY, options.repeatY))
            options.selected = (options.selected + OptCount + (y > 0 ? OptCount - 1 : 1)) % OptCount;
        if (const int x = edge(axis(0), options.stepX, options.repeatX))
            if (options.selected != OptClose) changeOption(options.selected, x);
        bool step = false;
        for (unsigned h = 0; h != 2; ++h) {
            const bool held = input.trigger[h] > .75f;
            step = step || (held && !options.triggerHeld[h]);
            options.triggerHeld[h] = held ? true : input.trigger[h] > .5f && options.triggerHeld[h];
        }
        const uint32_t buttons = input.buttons[0] | input.buttons[1];
        const bool select = (buttons & (Use | Reload)) != 0, close = (buttons & (Menu | Jump)) != 0;
        step = step || (select && !options.buttonHeld);
        options.buttonHeld = select;
        if (step) {
            if (options.selected == OptClose) options.open = false;
            else changeOption(options.selected, 1);
        }
        if (close && !options.closeHeld) options.open = false;
        options.closeHeld = close;
        if (!options.open) log.write("VR options closed");
        for (unsigned h = 0; h != 2; ++h) {
            input.trigger[h] = 0;
            input.axis[h][0] = input.axis[h][1] = 0;
            input.buttons[h] = 0;
        }
    }
    void optionsLayer(const Input &input, XrCompositionLayerQuad &quad, std::vector<const XrCompositionLayerBaseHeader *> &output) {
        if (!options.open || !input.focused || !input.headValid || !comfortablePanel(options.anchor, input.head, .3f)) return;
        const auto rows = optionRows();
        bool same = options.uploaded && options.drawnSelected == options.selected && rows.size() == options.drawn.size();
        for (size_t i = 0; same && i < rows.size(); ++i) same = rows[i].value == options.drawn[i].value;
        if (!same) {
            options.bitmap.draw(rows, options.selected);
            static const bool dump = [] { wchar_t v[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_OPTIONS_DUMP", v, 2) == 1 && v[0] == L'1'; }();
            static unsigned dumps = 0;
            if (dump && dumps < 3) { // Lab: the panel image beside the host log.
                wchar_t name[32];
                std::swprintf(name, 32, L"options-%u.ppm", dumps++);
                if (FILE *f = _wfopen((settingsFile.substr(0, settingsFile.find_last_of(L"\\/") + 1) + name).c_str(), L"wb")) {
                    const auto &bm = options.bitmap;
                    std::fprintf(f, "P6\n%u %u\n255\n", bm.width, bm.height);
                    for (size_t i = 0; i < size_t(bm.width) * bm.height; ++i) {
                        const uint8_t rgb[3]{bm.bits[i * 4 + 2], bm.bits[i * 4 + 1], bm.bits[i * 4]};
                        std::fwrite(rgb, 1, 3, f);
                    }
                    std::fclose(f);
                }
            }
            options.uploaded = false;
            if (!options.chain.upload(context.p, options.bitmap.bits)) { ++imageTimeouts; return; }
            options.drawn = rows;
            options.drawnSelected = options.selected;
            options.uploaded = true;
        }
        if (!options.chain.ownership.previousImageAvailable()) return;
        quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        quad.space = local;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage = options.chain.subimage();
        quad.pose = xrPose(options.anchor);
        quad.size = {OptionsWidth, OptionsWidth * OptionsAspect};
        output.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&quad));
    }
    void wheelLayers(const Ui &ui, const Input &input, std::array<XrCompositionLayerQuad, 2> &layers,
                     std::vector<const XrCompositionLayerBaseHeader *> &output) {
        const ULONGLONG now = GetTickCount64();
        const bool fresh = ui.tickMs && now >= ui.tickMs && now - ui.tickMs <= UiFreshMs;
        for (unsigned h = 0; h != 2; ++h) {
            auto &wheel = wheels[h];
            const bool open = fresh && ui.gameplay && ui.wheel[h].open;
            if (!open) {
                wheel.reset();
                continue;
            }
            const WheelPointer pointer = wheelPointer(input, h);
            if (!wheel.anchored && pointer.valid) {
                wheel.anchor = compose(wheelRingFrame(pointer), Pose{{}, {0, 0, WheelCardLift}});
                wheel.anchored = true;
                wheel.uploaded = false;
            }
            // Within arm's reach by design; only a card behind or at the face is hidden.
            if (!wheel.anchored || !input.focused || !input.headValid || !input.handValid[h] ||
                !comfortablePanel(wheel.anchor, input.head, .15f))
                continue;
            if (!wheel.uploaded || !sameWheel(wheel.drawn, ui.wheel[h])) {
                wheel.bitmap.draw(ui.wheel[h], h);
                wheel.uploaded = false;
                if (!wheel.chain.upload(context.p, wheel.bitmap.bits)) {
                    ++imageTimeouts;
                    continue;
                }
                wheel.drawn = ui.wheel[h];
                wheel.uploaded = true;
            }
            if (!wheel.chain.ownership.previousImageAvailable())
                continue;
            layers[h] = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            layers[h].layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                   XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
            layers[h].space = local;
            layers[h].eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            layers[h].subImage = wheel.chain.subimage();
            layers[h].pose = xrPose(wheel.anchor);
            layers[h].size = {WheelCardWidth, WheelCardWidth / 4};
            output.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&layers[h]));
        }
        hresult(device->GetDeviceRemovedReason(), "D3D11 device removed during wheel upload", log);
    }
    // The pointer ring is its own small quad over the menu: moving it never
    // recopies or reuploads the (runtime-resolution sized) menu image.
    static constexpr uint32_t CursorSize = 64;
    bool cursorImage() {
        if (cursorUploaded) return cursorChain.ownership.previousImageAvailable();
        std::vector<uint8_t> ring(size_t(CursorSize) * CursorSize * 4, 0);
        const float centre = (CursorSize - 1) * .5f, outer = CursorSize * .5f - 1, inner = outer * .86f;
        for (uint32_t y = 0; y < CursorSize; ++y)
            for (uint32_t x = 0; x < CursorSize; ++x) {
                const float r = std::hypot(x - centre, y - centre);
                const float alpha = std::clamp(std::min(outer - r, r - inner) + .5f, 0.f, 1.f);
                uint8_t *p = &ring[(size_t(y) * CursorSize + x) * 4];
                p[0] = 255; p[1] = 235; p[2] = 40; p[3] = uint8_t(alpha * 255); // Same tint as the former ring.
            }
        if (!cursorChain.upload(context.p, ring.data())) return false;
        cursorUploaded = true;
        return cursorChain.ownership.previousImageAvailable();
    }
    void menuLayer(const Input &input, XrCompositionLayerQuad &layer, XrCompositionLayerQuad &cursor,
                   std::vector<const XrCompositionLayerBaseHeader *> &output) {
        if (!input.focused || !input.headValid)
            return;
        uint64_t sequence = 0;
        uint32_t width = 0, height = 0;
        uint32_t menuGeneration = 0;
        {
            Mutex lock(channel, 1);
            if (!lock)
                return;
            const auto &menu = channel.shared->menu;
            if (!menu.visible || GetTickCount64() - menu.tickMs > MenuHoldMs || !menu.width || !menu.height ||
                menu.width > MaxDimension || menu.height > MaxDimension)
                return;
            width = menu.width;
            height = menu.height;
            sequence = menu.sequence;
            menuGeneration = menu.interactionGeneration;
            if (!currentPixels(channel.shared->pixels) || !pixels.holds(menu.pixelGeneration, width, height))
                return;
            if (sequence != menuSequence || menuChain.width != width || menuChain.height != height) {
                menuSourcePixels.resize(size_t(width) * height * 4);
                std::memcpy(menuSourcePixels.data(), pixels.buffer(MenuPixelBuffer), menuSourcePixels.size());
            }
        }
        if (menuChain.width != width || menuChain.height != height) {
            const auto &limits = systemProperties.graphicsProperties;
            if (width > limits.maxSwapchainImageWidth || height > limits.maxSwapchainImageHeight)
                return;
            if (!drainGpu())
                throw std::runtime_error("GPU drain before menu resize failed");
            menuChain.destroy();
            menuChain.create(api, session, width, height);
            menuSequence = 0;
            cursorX = cursorY = -1;
        }
        if (!menuAnchor.update(input.head, true, GetTickCount64(), recenterUi, settings.comfort))
            return;
        const Pose panel = menuAnchor.panel({0, 0, -settings.menuDistance});
        if (!comfortablePanel(panel, input.head)) {
            menuAnchor.reset();
            return;
        }
        const float panelHeight = settings.menuWidth * float(height) / width;
        PanelHit hit;
        unsigned hand = 2;
        if (menuGeneration && !recenterHeld(input))
            for (unsigned candidate : {1u, 0u})
                if (input.handValid[candidate] && primaryActionEligible(input, candidate)) {
                    hit = pointAtPanel(input.hand[candidate], panel, settings.menuWidth, panelHeight);
                    if (hit.valid) {
                        hand = candidate;
                        break;
                    }
                }
        const int x = hit.valid ? std::min(int(width) - 1, int(hit.u * width)) : -1;
        const int y = hit.valid ? std::min(int(height) - 1, int(hit.v * height)) : -1;
        if (sequence != menuSequence) {
            if (!menuChain.upload(context.p, menuSourcePixels.data()))
                return;
            menuSequence = sequence;
        }
        cursorX = x;
        cursorY = y;
        if (!menuChain.ownership.previousImageAvailable())
            return;
        layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        layer.space = local;
        layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        layer.subImage = menuChain.subimage();
        layer.pose = xrPose(panel);
        layer.size = {settings.menuWidth, panelHeight};
        output.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&layer));
        if (hit.valid && cursorImage()) {
            const float diameter = sizeAtAngle(.7f * Pi / 180, settings.menuDistance);
            cursor = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            cursor.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
            cursor.space = local;
            cursor.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            cursor.subImage = cursorChain.subimage();
            cursor.pose = xrPose(compose(panel, Pose{{0, 0, 0, 1},
                {(hit.u - .5f) * settings.menuWidth, (.5f - hit.v) * panelHeight, .004f}}));
            cursor.size = {diameter, diameter};
            output.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&cursor));
        }
        if (hit.valid)
            presentedPointer = {input.sequence, GetTickCount64(), sequence, input.session, input.reference,
                                menuGeneration, 1, hand, hit.u, hit.v, input.trigger[hand],
                                input.primaryInputGeneration[hand]};
    }
    void feedback(const Ui &ui, const Input &input, bool menuVisible) {
        bool valid = !quit && running && hapticsAvailable && ui.gameplay && input.focused &&
                     input.headValid && !menuVisible && ui.tickMs && GetTickCount64() >= ui.tickMs &&
                     GetTickCount64() - ui.tickMs <= UiFreshMs;
        if (!valid) {
            if (feedbackSeeded)
                actions.stopFeedback(api, session);
            feedbackSeeded = false;
            return;
        }
        if (feedbackSeeded) {
            for (unsigned h = 0; h != 2; ++h) {
                bool hurt = ui.damageSequence != lastDamage;
                bool fired = ui.fireSequence[h] != lastFire[h];
                // A light tick as an open wheel's hover moves onto another item.
                const bool hovered = ui.wheel[h].open && ui.wheel[h].hover != -1 && ui.wheel[h].hover != lastHover[h];
                float amplitude =
                    std::max(hurt ? settings.damageHaptic : 0.f, fired ? settings.fireHaptic : 0.f);
                const bool tick = amplitude <= 0 && hovered;
                if (tick)
                    amplitude = .25f;
                if (hapticsAvailable && amplitude > 0 && input.handValid[h])
                    hapticsAvailable =
                        actions.pulse(api, session, h, amplitude,
                                      hurt && settings.damageHaptic > 0 ? 80'000'000 : tick ? 15'000'000 : 40'000'000);
            }
        }
        for (unsigned h = 0; h != 2; ++h) {
            lastFire[h] = ui.fireSequence[h];
            lastHover[h] = ui.wheel[h].open ? ui.wheel[h].hover : -1;
        }
        lastDamage = ui.damageSequence;
        feedbackSeeded = true;
    }
    void diagnostics() {
        const ULONGLONG now = GetTickCount64();
        if (now - lastDiagnostics < 5000)
            return;
        lastDiagnostics = now;
        char line[512];
        std::snprintf(line, sizeof(line),
                      "Frame totals: ipc_complete=%llu deadline=%llu slots_full=%llu discarded=%llu "
                      "image_wait=%llu invalid_views=%llu retired=%llu submitted=%llu reused=%llu "
                      "xr_end_success=%llu xr_layers=%llu xr_menu=%llu xr_world=%llu ui_pairs=%llu opaque_pairs=%llu",
                      static_cast<unsigned long long>(completed), static_cast<unsigned long long>(timeouts),
                      static_cast<unsigned long long>(fullSlots), static_cast<unsigned long long>(discarded),
                      static_cast<unsigned long long>(imageTimeouts),
                      static_cast<unsigned long long>(invalidViews), static_cast<unsigned long long>(retired),
                      static_cast<unsigned long long>(submissions), static_cast<unsigned long long>(reused),
                      static_cast<unsigned long long>(xrEndSuccess), static_cast<unsigned long long>(xrLayerSuccess),
                      static_cast<unsigned long long>(xrMenuSuccess), static_cast<unsigned long long>(xrWorldSuccess),
                      static_cast<unsigned long long>(uiPairs), static_cast<unsigned long long>(clearedPairs));
        log.write(line);
        if (perfLoops) {
            std::snprintf(line, sizeof(line),
                          "Host perf loops=%llu loop=%.2fms wait=%.2fms submit=%.2fms uploads=%llu upload=%.2fms pair_age=%.1fms zero_copy=%llu latched=%llu",
                          static_cast<unsigned long long>(perfLoops), perfLoop / perfLoops, perfWait / perfLoops,
                          perfSubmit / perfLoops, static_cast<unsigned long long>(perfUploads),
                          perfUploads ? perfUpload / perfUploads : 0., perfUploads ? perfPairAge / perfUploads : 0.,
                          static_cast<unsigned long long>(zeroCopyPairs), static_cast<unsigned long long>(latched));
            log.write(line);
        }
        if (mutexStats.longHolds) {
            std::snprintf(line, sizeof(line), "Host IPC holds over 1 ms=%llu max=%.2fms site=+%llx",
                          static_cast<unsigned long long>(mutexStats.longHolds), mutexStats.maxHoldMs,
                          static_cast<unsigned long long>(mutexStats.site - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))));
            log.write(line);
            mutexStats = {};
        }
        perfWait = perfUpload = perfLoop = perfSubmit = perfPairAge = 0;
        perfLoops = perfUploads = 0;
    }
    void run() {
        while (!quit && !api.lossPending) {
            if (WaitForSingleObject(gameProcess.h, 0) != WAIT_TIMEOUT) {
                quit = true;
                break;
            }
            events();
            if (quit || api.lossPending)
                break;
            if (!running) {
                Snapshot snapshot;
                publish(emptyInput(), snapshot);
                Sleep(10);
                continue;
            }
            XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
            XrFrameState frameState{XR_TYPE_FRAME_STATE};
            const LARGE_INTEGER waitStart = perfNow();
            if (perfLoopStart.QuadPart) { perfLoop += perfMs(perfLoopStart, waitStart); ++perfLoops; }
            perfLoopStart = waitStart;
            api.check(api.WaitFrame(session, &wait, &frameState), "xrWaitFrame");
            perfWait += perfMs(waitStart, perfNow());
            XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
            api.check(api.BeginFrame(session, &begin), "xrBeginFrame");
            Frame frame(api, session, frameState.predictedDisplayTime);
            std::vector<const XrCompositionLayerBaseHeader *> layers;
            if (api.lossPending) {
                worldSubmission.invalidate();
                publishWorldSubmission();
                frame.submit(layers);
                break;
            }
            applyReferenceChanges(frameState.predictedDisplayTime);
            std::array<XrView, 2> views;
            const bool validViews = viewsAt(frameState.predictedDisplayTime, views);
            viewsTracked = validViews;
            Input input = sample(frameState.predictedDisplayTime, views, validViews);
            if (options.open) optionsInput(input);
            const bool recenterHeldNow = recenterHeld(input);
            recenterUi = recenterHeldNow && !recenterWasHeld;
            recenterWasHeld = recenterHeldNow;
            if (!validViews || !input.focused || !input.headValid) {
                menuAnchor.reset();
                hudAnchor.reset();
                for (auto &wheel : wheels)
                    wheel.reset();
            }
            if (validViews)
                for (unsigned h = 0; h != 2; ++h) {
                    const auto &f = views[h].fov;
                    currentFov[h] = {f.angleLeft, f.angleRight, f.angleUp, f.angleDown};
                }
            Snapshot snapshot;
            const bool published = publish(input, snapshot);
            if (published && !api.lossPending)
                feedback(snapshot.ui, input, snapshot.menuVisible);
            if (published) watchOptionsBubble(snapshot.ui, input);
            if (snapshot.menuVisible) options.open = false;
            const bool hudPrepared = published && prepareHudAnchor(snapshot, input);
            std::array<XrCompositionLayerProjectionView, 2> projectionViews, insetViews;
            XrCompositionLayerProjection projectionLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            XrCompositionLayerProjection insetLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            std::array<XrCompositionLayerQuad, 2> quadLayers;
            XrCompositionLayerQuad menuQuad{}, cursorQuad{}, optionsQuad{};
            presentedPointer = {};
            if (published && !quit && !api.lossPending && frameState.shouldRender) {
                if (snapshot.renderer && !snapshot.menuVisible && validViews && input.focused &&
                    input.headValid) {
                    const auto &f = settings.foveation;
                    const uint32_t lowWidth = foveationExtent(snapshot.width, f.periphery),
                                   lowHeight = foveationExtent(snapshot.height, f.periphery),
                                   insetWidth = foveationExtent(snapshot.width, f.inset),
                                   insetHeight = foveationExtent(snapshot.height, f.inset);
                    const bool foveated = foveationActive && !sharedOpened &&
                        foveationFits(snapshot.width, snapshot.height, lowWidth, lowHeight, insetWidth, insetHeight);
                    // Zero-copy pairs: the game renders foveated itself (its single pass
                    // draws a reduced full view and a full-resolution inset per eye and
                    // returns full-size eyes), eye-tracked or at the view centre.
                    const bool rendered = !foveated && sharedOpened && renderFoveation();
                    if (foveated) {
                        eyeChains(lowWidth, lowHeight);
                        insetChains(insetWidth, insetHeight);
                    } else
                        eyeChains(snapshot.width, snapshot.height);
                    Request request{};
                    request.sequence = ++requestSequence;
                    request.predictedTime = frameState.predictedDisplayTime;
                    request.session = sessionGeneration;
                    request.reference = referenceGeneration;
                    request.width = snapshot.width;
                    request.height = snapshot.height;
                    request.input = input;
                    for (unsigned h = 0; h != 2; ++h) {
                        request.eye[h] = pose(views[h].pose);
                        const auto &v = views[h].fov;
                        request.fov[h] = {v.angleLeft, v.angleRight, v.angleUp, v.angleDown};
                    }
                    if (foveated || rendered) {
                        request.foveated = foveated ? 1 : 2;
                        request.lowWidth = lowWidth; request.lowHeight = lowHeight;
                        request.insetWidth = insetWidth; request.insetHeight = insetHeight;
                        Vec3 gaze;
                        const ULONGLONG now = GetTickCount64();
                        if (labGaze(input, gaze) || actions.gazeDirection(api, session, local, frameState.predictedDisplayTime, gaze)) {
                            lastGaze = gaze;
                            lastGazeTick = now;
                        }
                        // Hold the last gaze through blinks, then return to the view centre.
                        const bool gazeFresh = lastGazeTick && now - lastGazeTick < 500;
                        for (unsigned h = 0; h != 2; ++h) {
                            const Fov &fov = request.fov[h];
                            float tx = (std::tan(fov.left) + std::tan(fov.right)) * .5f;
                            float ty = (std::tan(fov.up) + std::tan(fov.down)) * .5f;
                            if (gazeFresh) gazeTangent(request.eye[h].q, lastGaze, fov, tx, ty);
                            request.insetCentre[h][0] = tx;
                            request.insetCentre[h][1] = ty;
                        }
                    }
                    if (hudPrepared) {
                        const Pose panel = hudAnchor.panel({0, 0, -settings.menuDistance});
                        if (comfortablePanel(panel, input.head)) {
                            request.uiPanel = panel;
                            request.uiWidth = settings.menuWidth;
                            request.uiHeight = settings.menuWidth * float(snapshot.height) / snapshot.width;
                            request.uiRequested = 1;
                        } else
                            hudAnchor.reset();
                    }
                    const uint64_t completedBefore = completed;
                    pollEyes();
                    requestEyes(request, completed == completedBefore);
                    if (projection(projectionLayer, projectionViews, insetLayer, insetViews)) {
                        layers.push_back(
                            reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projectionLayer));
                        if (insetLayer.viewCount)
                            layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&insetLayer));
                    }
                } else {
                    invalidateCachedPair();
                    cancelRequests();
                    discardEyeAcquisitions();
                }
                if (!api.lossPending) {
                    if (snapshot.menuVisible) {
                        hudAnchor.reset();
                        for (auto &wheel : wheels)
                            wheel.reset();
                        invalidateCachedPair();
                        cancelRequests();
                        menuLayer(input, menuQuad, cursorQuad, layers);
                    } else {
                        menuAnchor.reset();
                        wheelLayers(snapshot.ui, input, quadLayers, layers);
                        optionsLayer(input, optionsQuad, layers);
                    }
                }
            }
            if (!published || !frameState.shouldRender)
                discardEyeAcquisitions();
            if (quit || api.lossPending)
                layers.clear();
            const auto *world = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projectionLayer);
            const auto *insetWorld = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&insetLayer);
            // Only the game's own HUD is shown (composited into the stereo pair);
            // there is no substitute panel when a pair arrives without it.
            bool worldPresented = admitFinalWorld(layers, world, input, validViews,views, insetWorld);
            worldPresented=finalNativeUiLayers(quit,api.lossPending,layers,worldPresented);
            if (worldPresented) {
                ++submissions;
                if (cachedRequest.sequence == lastSubmittedSequence)
                    ++reused;
                lastSubmittedSequence = cachedRequest.sequence;
            }
            const LARGE_INTEGER submitStart = perfNow();
            const auto endResult = frame.submit(layers);
            perfSubmit += perfMs(submitStart, perfNow());
            worldSubmission.sample(GetCurrentProcessId(),cachedRequest,endResult==XR_SUCCESS &&
                worldPresented && std::find(layers.begin(),layers.end(),world)!=layers.end());
            publishWorldSubmission();
            if (endResult == XR_SUCCESS) {
                ++xrEndSuccess;
                if (worldPresented && std::find(layers.begin(),layers.end(),world)!=layers.end()) {
                    ++xrWorldSuccess;
                    static const bool labTrace=[] { wchar_t value[2]{}; return GetEnvironmentVariableW(L"SS2VR_LAB_TRACE",value,2)==1 && value[0]==L'1'; }();
                    static uint64_t lastSequence=0;
                    static uint32_t lastSession=0,lastReference=0;
                    static unsigned receipts=0;
                    if(labTrace && receipts<8192 && (lastSequence!=cachedRequest.sequence ||
                        lastSession!=cachedRequest.session || lastReference!=cachedRequest.reference)) {
                        ++receipts;lastSequence=cachedRequest.sequence;lastSession=cachedRequest.session;lastReference=cachedRequest.reference;
                        char receipt[256]{};
                        std::snprintf(receipt,sizeof(receipt),"Lab projection submitted request=%llu session=%u reference=%u tracking=%u presentation=%u",
                            static_cast<unsigned long long>(cachedRequest.sequence),cachedRequest.session,cachedRequest.reference,
                            cachedRequest.trackingGeneration,cachedPresentation);
                        log.write(receipt);
                    }
                }
                if (!layers.empty()) ++xrLayerSuccess;
                if (std::find(layers.begin(), layers.end(),
                    reinterpret_cast<const XrCompositionLayerBaseHeader *>(&menuQuad)) != layers.end()) ++xrMenuSuccess;
            }
            // Only point at the menu quad actually submitted in this XR frame.
            // Session/reference/focus and short expiry are checked again by the
            // native menu input dispatcher before any cursor or click mutation.
            if (std::find(layers.begin(), layers.end(),
                          reinterpret_cast<const XrCompositionLayerBaseHeader *>(&menuQuad)) == layers.end())
                presentedPointer = {};
            {
                Mutex lock(channel, 1);
                if (lock && channel.shared->hostPid == GetCurrentProcessId())
                    channel.shared->pointer = presentedPointer;
            }
            diagnostics();
        }
        if (api.lossPending) {
            failed = true;
            log.write("OpenXR session loss returned by frame/action API");
        }
        if (running && !api.lossPending)
            actions.stopFeedback(api, session);
        invalidate();
        log.write("Host stopped; tracking invalidated and outstanding requests cancelled");
    }
};
} // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc != 3 || std::wcscmp(argv[1], L"--channel") || !argv[2][0]) {
        std::fwprintf(stderr, L"Usage: ss2vr_host.exe --channel TOKEN\n");
        return 2;
    }
    const std::wstring token = argv[2];
    if (token.size() > 128 || !std::all_of(token.begin(), token.end(), [](wchar_t c) {
            return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ||
                   c == L'-' || c == L'_';
        })) {
        std::fwprintf(stderr, L"Invalid channel token\n");
        return 2;
    }
    try {
        const std::wstring directory = executableDirectory();
        Log log(directory);
        Host host(log);
        try {
            host.connect(token);
            host.initialize(directory);
            host.run();
        } catch (const std::exception &error) {
            host.failed = true;
            log.write(error.what());
            return 1;
        }
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
