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
struct Mutex {
    Channel &channel;
    bool held = false;
    Mutex(Channel &c, DWORD wait) : channel(c) {
        const DWORD result = WaitForSingleObject(c.mutex, wait);
        if (result == WAIT_ABANDONED) {
            ReleaseMutex(c.mutex);
            throw std::runtime_error("IPC mutex abandoned; refusing possibly partial payload");
        }
        if (result == WAIT_FAILED)
            throw std::runtime_error("IPC mutex wait failed");
        held = result == WAIT_OBJECT_0;
    }
    ~Mutex() {
        if (held)
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
        recenter{}, sprint{};
    XrSpace aimSpace[2]{}, gripSpace[2]{};
    XrPath viveProfile = XR_NULL_PATH, currentProfile[2]{};
    ActionStream primaryStream[2], zoomStream[2], gripPoseStream[2];
    SqueezeAdmission squeeze[2];
    void invalidateStreams() {
        for (unsigned h = 0; h != 2; ++h) {
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
    void create(Api &api, XrInstance instance, XrSession session, bool frameController) {
        hand[0] = path(api, instance, "/user/hand/left");
        hand[1] = path(api, instance, "/user/hand/right");
        viveProfile = path(api, instance, "/interaction_profiles/htc/vive_controller");
        XrActionSetCreateInfo create{XR_TYPE_ACTION_SET_CREATE_INFO};
        std::strcpy(create.actionSetName, "ss2vr");
        std::strcpy(create.localizedActionSetName, "Serious Sam 2 VR");
        api.check(api.CreateActionSet(instance, &create, &set), "xrCreateActionSet");
        aim = action(api, "aim", "Aim pose", XR_ACTION_TYPE_POSE_INPUT);
        grip = action(api, "grip", "Weapon attachment pose", XR_ACTION_TYPE_POSE_INPUT);
        haptic = action(api, "feedback", "Weapon and damage feedback", XR_ACTION_TYPE_VIBRATION_OUTPUT);
        trigger = action(api, "trigger", "Fire trigger", XR_ACTION_TYPE_FLOAT_INPUT);
        stick = action(api, "stick", "Move turn and wheel axis", XR_ACTION_TYPE_VECTOR2F_INPUT);
        wheel = action(api, "wheel_click", "Weapon wheel grip click", XR_ACTION_TYPE_BOOLEAN_INPUT);
        wheelValue = action(api, "wheel_value", "Weapon wheel grip value", XR_ACTION_TYPE_FLOAT_INPUT);
        use = action(api, "use", "Use", XR_ACTION_TYPE_BOOLEAN_INPUT);
        jump = action(api, "jump", "Jump / Vive right sniper zoom", XR_ACTION_TYPE_BOOLEAN_INPUT);
        menu = action(api, "menu", "Pause and menu", XR_ACTION_TYPE_BOOLEAN_INPUT);
        recenter = action(api, "recenter_chord", "Recenter with both grips", XR_ACTION_TYPE_BOOLEAN_INPUT);
        sprint = action(api, "sprint", "Sprint / sniper zoom", XR_ACTION_TYPE_BOOLEAN_INPUT);
        reload = action(api, "reload", "Reload weapon", XR_ACTION_TYPE_BOOLEAN_INPUT);
        netricsa = action(api, "netricsa", "Netricsa / objectives", XR_ACTION_TYPE_BOOLEAN_INPUT);
        suggest(api, instance, frameController);
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
                bind(profile.analogGrip ? wheelValue : wheel, h,
                     profile.analogGrip ? "squeeze/value" : "squeeze/click");
            }
            const std::string click = std::string(profile.axis) + "/click";
            bind(sprint, 0, click);
            bind(recenter, 0, click); // Same button, activated only with both grips held.
            switch (profile.kind) {
            case 0:
                bind(sprint, 1, click);
                bind(reload, 0, "x/click");
                bind(use, 1, "a/click");
                bind(netricsa, 0, "y/click");
                bind(jump, 1, "b/click");
                bind(menu, 0, "menu/click"); // Touch has no right menu component.
                break;
            case 1:
                bind(sprint, 1, click);
                bind(reload, 0, "a/click");
                bind(use, 1, "a/click");
                bind(jump, 1, "b/click");
                bind(menu, 0, "b/click");
                bind(netricsa, 0, "trackpad/force");
                break;
            case 2:
                bind(menu, 0, "menu/click");
                bind(use, 1, "menu/click");
                bind(jump, 1, "trackpad/click");
                break;
            case 3:
                bind(sprint, 1, click);
                bind(use, 0, "trackpad/click");
                bind(jump, 1, "trackpad/click");
                bind(menu, 0, "menu/click");
                bind(menu, 1, "menu/click");
                break;
            case 4:
                // Valve Frame has a left D-pad/view and right ABXY/menu.
                // Keep the established action semantics; no invented left XY.
                bind(sprint, 1, click);
                bind(reload, 0, "dpad_down/click");
                bind(use, 1, "a/click");
                bind(netricsa, 0, "dpad_up/click");
                bind(jump, 1, "b/click");
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
                gripPoseStream[h].sample(false);
                input.wheelInputEpoch[h] = squeeze[h].epoch;
            }
            return;
        }
        api.check(result, "xrSyncActions");
        uint32_t squeezeSources[2]{};
        bool squeezeValuesValid[2]{};
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
            const auto rawSqueeze = squeezeSample(wheelClick.isActive != XR_FALSE, wheelClick.currentState,
                wheelAnalog.isActive != XR_FALSE, wheelAnalog.currentState);
            squeezeSources[h] = rawSqueeze.sources;
            squeezeValuesValid[h] = rawSqueeze.valid;
            if (rawSqueeze.down) input.buttons[h] |= Wheel;
            if (boolean(api, session, use, h))
                input.buttons[h] |= Use;
            const auto jumpState = booleanState(api, session, jump, h);
            const auto sprintState = booleanState(api, session, sprint, h);
            if (jumpState.isActive && jumpState.currentState)
                input.buttons[h] |= Jump;
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

// GDI renders labels into a top-down DIB. Pixels outside the disc remain alpha 0;
// the disc, sectors and antialiased text use straight-alpha opaque BGRA.
struct WheelBitmap {
    HDC dc = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ oldBitmap = nullptr;
    HFONT label = nullptr, ammo = nullptr;
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
        if (!label || !ammo)
            throw std::runtime_error("GDI wheel fonts unavailable");
        SetBkMode(dc, TRANSPARENT);
    }
    void text(const wchar_t *value, float x, float y, HFONT font, COLORREF color, float width) {
        const HGDIOBJ old = SelectObject(dc, font);
        SetTextColor(dc, color);
        RECT rectangle{LONG(x - width / 2), LONG(y), LONG(x + width / 2), LONG(y + size * .052f)};
        const int result =
            DrawTextW(dc, value, -1, &rectangle, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, old);
        if (!result)
            throw std::runtime_error("GDI wheel label drawing failed");
    }
    void stats(const Ui &ui) {
        GdiFlush();
        std::memset(bits, 0, size_t(size) * height * 4);
        // Compact rectangular card, with independent lines rather than one long weapon string.
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < size; ++x) {
                auto *p = bits + (size_t(y) * size + x) * 4;
                p[0] = 24;
                p[1] = 18;
                p[2] = 15;
                p[3] = 220;
            }
        wchar_t line[96];
        std::swprintf(line, 96, L"HEALTH %d    ARMOR %d", ui.health, ui.armor);
        text(line, size * .5f, height * .06f, label, RGB(255, 255, 255), size * .94f);
        std::swprintf(line, 96, L"LEFT   %ls    %d", weaponName(ui.currentWeapon[0]), ui.currentAmmo[0]);
        text(line, size * .5f, height * .36f, label, RGB(190, 225, 255), size * .94f);
        std::swprintf(line, 96, L"RIGHT   %ls    %d", weaponName(ui.currentWeapon[1]), ui.currentAmmo[1]);
        text(line, size * .5f, height * .66f, label, RGB(255, 225, 170), size * .94f);
        GdiFlush();
        // GDI clears text alpha; keep the whole card's straight alpha predictable.
        for (size_t i = 0; i < size_t(size) * height; i++)
            bits[i * 4 + 3] = 220;
    }
    void draw(const WheelUi &ui, unsigned hand) {
        if (!GdiFlush())
            throw std::runtime_error("GDI flush failed");
        const uint32_t count = std::min(ui.count, WeaponCount);
        const float center = size * .5f, outer = size * .47f, inner = size * .205f;
        const float step = count ? 2 * Pi / count : 2 * Pi;
        for (uint32_t y = 0; y < size; ++y)
            for (uint32_t x = 0; x < size; ++x) {
                const float dx = x + .5f - center, dy = center - (y + .5f);
                const float radius = std::sqrt(dx * dx + dy * dy);
                auto *pixel = bits + (size_t(y) * size + x) * 4;
                pixel[0] = pixel[1] = pixel[2] = pixel[3] = 0;
                if (radius > outer)
                    continue;
                bool highlight = false;
                if (radius >= inner && count) {
                    float angle = std::atan2(dx, dy);
                    if (angle < 0)
                        angle += 2 * Pi;
                    const int sector = int(std::floor((angle + step * .5f) / step)) % int(count);
                    highlight = ui.hover == sector;
                }
                pixel[0] = highlight ? 32 : 31;
                pixel[1] = highlight ? 116 : 27;
                pixel[2] = highlight ? 204 : 23;
                pixel[3] = 255;
            }
        HPEN pen = CreatePen(PS_SOLID, std::max(1, int(size / 400)), RGB(120, 128, 138));
        if (!pen)
            throw std::runtime_error("GDI wheel pen unavailable");
        const auto oldPen = SelectObject(dc, pen);
        const auto oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Ellipse(dc, int(center - inner), int(center - inner), int(center + inner), int(center + inner));
        for (uint32_t i = 0; i < count; ++i) {
            const float boundary = step * (i + .5f);
            MoveToEx(dc, int(center + std::sin(boundary) * inner), int(center - std::cos(boundary) * inner),
                     nullptr);
            LineTo(dc, int(center + std::sin(boundary) * outer), int(center - std::cos(boundary) * outer));
        }
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
        for (uint32_t i = 0; i < count; ++i) {
            const float angle = step * i;
            const float x = center + std::sin(angle) * outer * .78f;
            const float y = center - std::cos(angle) * outer * .78f;
            wchar_t name[48], ammunition[48];
            // Stock names are verified against the native parameter-path switch.
            std::swprintf(name, 48, L"%ls", weaponName(ui.weapon[i]));
            if (ui.ammo[i] >= 0)
                std::swprintf(ammunition, 48, L"%d", ui.ammo[i]);
            else
                std::swprintf(ammunition, 48, L"--");
            text(name, x, y - size * .033f, label, RGB(255, 255, 255), size * .11f);
            text(ammunition, x, y + size * .010f, ammo, RGB(220, 225, 230), size * .11f);
        }
        text(hand ? L"RIGHT" : L"LEFT", center, center - size * .130f, ammo, RGB(210, 220, 230), size * .34f);
        if (ui.hover >= 0 && uint32_t(ui.hover) < count) {
            text(weaponName(ui.weapon[ui.hover]), center, center - size * .045f, label, RGB(255, 255, 255),
                 size * .36f);
            wchar_t amount[48];
            if (ui.ammo[ui.hover] >= 0)
                std::swprintf(amount, 48, L"Ammo %d", ui.ammo[ui.hover]);
            else
                std::swprintf(amount, 48, L"Ammo --");
            text(amount, center, center + size * .010f, ammo, RGB(235, 240, 250), size * .34f);
            text(L"Release to select", center, center + size * .090f, ammo, RGB(235, 240, 250), size * .38f);
        } else {
            text(L"Choose weapon", center, center - size * .040f, label, RGB(255, 255, 255), size * .36f);
            text(L"Center to cancel", center, center + size * .050f, ammo, RGB(230, 235, 240), size * .38f);
        }
        if (!GdiFlush())
            throw std::runtime_error("GDI wheel draw flush failed");
        // GDI overwrites alpha. Restore alpha from the disc mask after drawing.
        for (uint32_t y = 0; y < size; ++y)
            for (uint32_t x = 0; x < size; ++x) {
                const float dx = x + .5f - center, dy = y + .5f - center;
                auto *pixel = bits + (size_t(y) * size + x) * 4;
                if (dx * dx + dy * dy <= outer * outer)
                    pixel[3] = 255;
                else
                    pixel[0] = pixel[1] = pixel[2] = pixel[3] = 0;
            }
    }
};

bool sameWheel(const WheelUi &a, const WheelUi &b) {
    return a.open == b.open && a.count == b.count && a.hover == b.hover &&
           !std::memcmp(a.weapon, b.weapon, sizeof(a.weapon)) && !std::memcmp(a.ammo, b.ammo, sizeof(a.ammo));
}
struct WheelLayer {
    Swapchain chain;
    WheelBitmap bitmap;
    bool previousOpen = false, anchored = false, uploaded = false;
    Pose anchor{};
    WheelUi drawn{};
    void reset() {
        previousOpen = anchored = uploaded = false;
    }
};

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
    Actions actions;
    WorldSubmissionStream worldSubmission;
    Swapchain eye[2];
    WheelLayer wheels[2];
    WheelLayout wheelLayout;
    ComfortAnchor menuAnchor, hudAnchor;
    VrSettings settings;
    uint32_t lastFire[2]{}, lastDamage = 0;
    bool feedbackSeeded = false, hapticsAvailable = true;
    Fov currentFov[2];
    bool recenterWasHeld = false, recenterUi = false;
    Swapchain menuChain, hudChain;
    WheelBitmap hudBitmap;
    std::vector<uint8_t> menuPixels, menuSourcePixels;
    uint64_t menuSequence = 0;
    MenuPointer presentedPointer;
    int cursorX = -1, cursorY = -1;
    Ui drawnHud{};
    bool hudUploaded = false;
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
    uint64_t completed = 0, timeouts = 0, fullSlots = 0, discarded = 0, imageTimeouts = 0, invalidViews = 0;
    uint64_t retired = 0, submissions = 0, reused = 0, lastSubmittedSequence = 0;
    ULONGLONG lastDiagnostics = 0, lastMutexDiagnostic = 0;

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
            menuChain.abandon();
            hudChain.abandon();
            drainQuery.abandon();
            context.abandon();
            device.abandon();
            api.loader.h = nullptr;
            return;
        }
        for (auto &chain : eye)
            chain.destroy();
        for (auto &wheel : wheels)
            wheel.chain.destroy();
        menuChain.destroy();
        hudChain.destroy();
        actions.destroy(api);
        if (viewSpace)
            api.cleanup(api.DestroySpace(viewSpace), "xrDestroySpace VIEW");
        if (local)
            api.cleanup(api.DestroySpace(local), "xrDestroySpace LOCAL");
        if (session)
            api.cleanup(api.DestroySession(session), "xrDestroySession");
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
        settings = loadSettings(directory + L"SS2VR.ini");
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
        const char *enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME,frameExtension};
        XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
        std::strcpy(create.applicationInfo.applicationName, "Serious Sam 2 VR");
        std::strcpy(create.applicationInfo.engineName, "SS2VR host");
        create.applicationInfo.applicationVersion = create.applicationInfo.engineVersion = 1;
        // All used core calls are 1.0; this admits 1.0 runtimes with the required extension.
        create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        create.enabledExtensionCount = frameController?2:1;
        create.enabledExtensionNames = enabled;
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
        api.check(api.GetSystemProperties(instance, system, &systemProperties), "xrGetSystemProperties");
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
        actions.create(api, instance, session, frameController);
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
            wheel.chain.create(api, session, wheelSize, wheelSize);
            wheel.bitmap.create(wheelSize);
        }
        hudChain.create(api, session, wheelSize, wheelSize / 4);
        hudBitmap.create(wheelSize, wheelSize / 4);
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
        wheelLayout.reset();
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
        reapLocked();
        snapshot = {shared.width, shared.height, shared.rendererReady != 0, shared.ui,
                    shared.menu.visible && GetTickCount64() - shared.menu.tickMs < 250};
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
            return false;
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
        for (auto &chain : eye)
            if (chain.ownership.acquired && chain.waitImage()) {
                // Release changes the runtime's most-recent image even without a write.
                invalidateCachedPair();
                chain.releaseWaited();
            }
    }
    void finishSessionAcquisitions() {
        invalidate();
        hudUploaded = false;
        menuSequence = 0;
        const uint64_t deadline = GetTickCount64() + 500;
        for (;;) {
            bool retiredAll = true;
            for (auto &chain : eye)
                retiredAll = chain.releaseAcquired() && retiredAll;
            for (auto &wheel : wheels)
                retiredAll = wheel.chain.releaseAcquired() && retiredAll;
            retiredAll = menuChain.releaseAcquired() && retiredAll;
            retiredAll = hudChain.releaseAcquired() && retiredAll;
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
            Mutex lock(channel, 0);
            if (!lock)
                return;
            reapLocked();
            for (int i = 0; i < 2; i++)
                if (outstanding[i].active && !outstanding[i].expired &&
                    channel.shared->slot[i].state == SlotState::Ready)
                    candidate = i;
        }
        if (candidate < 0) {
            discardEyeAcquisitions();
            return;
        }
        // No IPC lock across XR waits. Ready remains owned until BOTH images are writable.
        const bool left = eye[0].waitImage(), right = eye[1].waitImage();
        if (!left || !right) {
            ++imageTimeouts;
            return;
        }
        if (!bothImagesReady(eye[0].ownership, eye[1].ownership))
            throw std::runtime_error("Stereo wait barrier failed");
        Request reply;
        HeadClearance clearance;
        uint64_t deadline = 0;
        uint32_t presentation = 0;
        bool accepted = false;
        const uint8_t *source[2]{};
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
                    accepted = currentPixels(channel.shared->pixels) &&
                        pixels.matches(slot.pixelGeneration, reply.width, reply.height);
                    for (unsigned h = 0; h < 2; h++)
                        source[h] = accepted ? pixels.buffer(slotPixelBuffer(candidate, h)) : nullptr;
                    slot.state = SlotState::Empty;
                    slot.cancelled = 0;
                    slot.presentation = 0;
                    slot.presentationReserved = 0;
                    slot.headClearance = {};
                    tracked = {};
                    if (accepted) ++completed;
                }
            }
        }
        if (!accepted) {
            discardEyeAcquisitions();
            return;
        }
        invalidateCachedPair(); // A failure between releases must never expose a mixed pair.
        eye[0].uploadWaited(context.p, source[0]);
        eye[1].uploadWaited(context.p, source[1]);
        hresult(device->GetDeviceRemovedReason(), "D3D11 device removed", log);
        if (!api.lossPending) {
            cachedRequest = reply;
            cachedClearance = clearance;
            cachedDeadline = deadline;
            cachedPresentation = presentation;
            cachedValid = true;
        }
    }
    void requestEyes(Request request) {
        Mutex lock(channel, 0);
        if (!lock)
            return;
        reapLocked();
        for (const auto &pending : outstanding)
            if (pending.active) {
                ++fullSlots;
                return; // Includes Ready retries and cancelled native Rendering.
            }
        auto &shared = *channel.shared;
        request.trackingGeneration = trackingEpoch(shared);
        uint64_t deadline = GetTickCount64() + FrameAgeMs;
        if (shared.shutdown || !eligibleFrame(request, deadline, frameContext()))
            return;
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
                    std::array<XrCompositionLayerProjectionView, 2> &views) {
        if (!cachedValid || !eye[0].ownership.previousImageAvailable() ||
            !eye[1].ownership.previousImageAvailable())
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
                         const std::array<XrView,2>& views) {
        if (std::find(layers.begin(), layers.end(), world) == layers.end())
            return false;
        if (finalProjectionEligible(input, validViews,views))
            return true;
        layers.erase(std::remove(layers.begin(), layers.end(), world), layers.end());
        return false;
    }
    void wheelLayers(const Ui &ui, const Input &input, std::array<XrCompositionLayerQuad, 2> &layers,
                     std::vector<const XrCompositionLayerBaseHeader *> &output) {
        const ULONGLONG now = GetTickCount64();
        const bool fresh = ui.tickMs && now >= ui.tickMs && now - ui.tickMs <= UiFreshMs;
        const bool anyOpen = fresh && ui.gameplay && (ui.wheel[0].open || ui.wheel[1].open);
        if (!anyOpen)
            wheelLayout.reset();
        if (anyOpen && !wheelLayout.reserved && input.focused && input.headValid)
            wheelLayout.reserve(input.head, currentFov);
        for (unsigned h = 0; h != 2; ++h) {
            auto &wheel = wheels[h];
            const bool open = fresh && ui.gameplay && ui.wheel[h].open;
            if (!open) {
                wheel.reset();
                continue;
            }
            if (!wheel.anchored && wheelLayout.reserved) {
                wheel.anchor = wheelLayout.anchor[h];
                wheel.anchored = true;
                wheel.uploaded = false;
            }
            wheel.previousOpen = true;
            if (!wheel.anchored || !input.focused || !input.headValid || !input.handValid[h] ||
                !comfortablePanel(wheel.anchor, input.head, 1.f))
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
            layers[h].size = {wheelLayout.diameter, wheelLayout.diameter};
            output.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&layers[h]));
        }
        hresult(device->GetDeviceRemovedReason(), "D3D11 device removed during wheel upload", log);
    }
    void menuLayer(const Input &input, XrCompositionLayerQuad &layer,
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
            if (!menu.visible || GetTickCount64() - menu.tickMs > 250 || !menu.width || !menu.height ||
                menu.width > MaxDimension || menu.height > MaxDimension)
                return;
            width = menu.width;
            height = menu.height;
            sequence = menu.sequence;
            menuGeneration = menu.interactionGeneration;
            if (!currentPixels(channel.shared->pixels) || !pixels.matches(menu.pixelGeneration, width, height))
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
        if (sequence != menuSequence || x != cursorX || y != cursorY) {
            menuPixels = menuSourcePixels;
            if (hit.valid) {
                const int radius = std::max(4, int(width * sizeAtAngle(.7f * Pi / 180,
                                                                    settings.menuDistance) /
                                                  settings.menuWidth * .5f));
                for (int dy = -radius; dy <= radius; ++dy)
                    for (int dx = -radius; dx <= radius; ++dx) {
                        int px = x + dx, py = y + dy;
                        const int r2 = dx * dx + dy * dy;
                        if (px < 0 || py < 0 || px >= int(width) || py >= int(height) ||
                            r2 > radius * radius || r2 < (radius - 2) * (radius - 2))
                            continue;
                        size_t index = (size_t(py) * width + px) * 4;
                        menuPixels[index] = 255;
                        menuPixels[index + 1] = 235;
                        menuPixels[index + 2] = 40;
                        menuPixels[index + 3] = 255;
                    }
            }
            if (!menuChain.upload(context.p, menuPixels.data()))
                return;
            menuSequence = sequence;
            cursorX = x;
            cursorY = y;
        }
        if (!menuChain.ownership.previousImageAvailable())
            return;
        layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        layer.space = local;
        layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        layer.subImage = menuChain.subimage();
        layer.pose = xrPose(panel);
        layer.size = {settings.menuWidth, panelHeight};
        output.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&layer));
        if (hit.valid)
            presentedPointer = {input.sequence, GetTickCount64(), sequence, input.session, input.reference,
                                menuGeneration, 1, hand, hit.u, hit.v, input.trigger[hand],
                                input.primaryInputGeneration[hand]};
    }
    void hudLayer(const Ui &ui, const Input &input, XrCompositionLayerQuad &layer,
                  std::vector<const XrCompositionLayerBaseHeader *> &output) {
        const ULONGLONG now = GetTickCount64();
        if (!ui.gameplay || !input.focused || !input.headValid || !ui.tickMs || now < ui.tickMs ||
            now - ui.tickMs > UiFreshMs)
            return;
        bool changed = !hudUploaded || ui.health != drawnHud.health || ui.armor != drawnHud.armor ||
                       std::memcmp(ui.currentWeapon, drawnHud.currentWeapon, sizeof(ui.currentWeapon)) ||
                       std::memcmp(ui.currentAmmo, drawnHud.currentAmmo, sizeof(ui.currentAmmo));
        if (changed) {
            hudBitmap.stats(ui);
            hudUploaded = false;
            if (!hudChain.upload(context.p, hudBitmap.bits))
                return;
            drawnHud = ui;
            hudUploaded = true;
        }
        if (!hudChain.ownership.previousImageAvailable() || !hudAnchor.initialized)
            return;
        const Pose panel = hudAnchor.panel({0, -.38f, -settings.hudDistance});
        if (!comfortablePanel(panel, input.head)) {
            hudAnchor.reset();
            return;
        }
        layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        layer.space = local;
        layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        layer.subImage = hudChain.subimage();
        layer.pose = xrPose(panel);
        layer.size = {settings.hudWidth, settings.hudWidth / 4};
        layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                           XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        output.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&layer));
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
                float amplitude =
                    std::max(hurt ? settings.damageHaptic : 0.f, fired ? settings.fireHaptic : 0.f);
                if (hapticsAvailable && amplitude > 0 && input.handValid[h])
                    hapticsAvailable =
                        actions.pulse(api, session, h, amplitude,
                                      hurt && settings.damageHaptic > 0 ? 80'000'000 : 40'000'000);
            }
        }
        for (unsigned h = 0; h != 2; ++h)
            lastFire[h] = ui.fireSequence[h];
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
                      "xr_end_success=%llu xr_layers=%llu xr_menu=%llu xr_world=%llu",
                      static_cast<unsigned long long>(completed), static_cast<unsigned long long>(timeouts),
                      static_cast<unsigned long long>(fullSlots), static_cast<unsigned long long>(discarded),
                      static_cast<unsigned long long>(imageTimeouts),
                      static_cast<unsigned long long>(invalidViews), static_cast<unsigned long long>(retired),
                      static_cast<unsigned long long>(submissions), static_cast<unsigned long long>(reused),
                      static_cast<unsigned long long>(xrEndSuccess), static_cast<unsigned long long>(xrLayerSuccess),
                      static_cast<unsigned long long>(xrMenuSuccess), static_cast<unsigned long long>(xrWorldSuccess));
        log.write(line);
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
            api.check(api.WaitFrame(session, &wait, &frameState), "xrWaitFrame");
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
            if (wheelLayout.reserved)
                for (unsigned h = 0; h != 2; ++h)
                    if (wheels[h].previousOpen && !comfortablePanel(wheelLayout.anchor[h], input.head, 1.1f))
                        input.blockedWheels |= 1u << h;
            const bool recenterHeldNow = recenterHeld(input);
            recenterUi = recenterHeldNow && !recenterWasHeld;
            recenterWasHeld = recenterHeldNow;
            if (!validViews || !input.focused || !input.headValid) {
                menuAnchor.reset();
                hudAnchor.reset();
                wheelLayout.reset();
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
            const bool hudPrepared = published && prepareHudAnchor(snapshot, input);
            std::array<XrCompositionLayerProjectionView, 2> projectionViews;
            XrCompositionLayerProjection projectionLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            std::array<XrCompositionLayerQuad, 2> quadLayers;
            XrCompositionLayerQuad menuQuad{}, hudQuad{};
            presentedPointer = {};
            if (published && !quit && !api.lossPending && frameState.shouldRender) {
                if (snapshot.renderer && !snapshot.menuVisible && validViews && input.focused &&
                    input.headValid) {
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
                        const auto &f = views[h].fov;
                        request.fov[h] = {f.angleLeft, f.angleRight, f.angleUp, f.angleDown};
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
                    pollEyes();
                    requestEyes(request);
                    if (projection(projectionLayer, projectionViews))
                        layers.push_back(
                            reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projectionLayer));
                } else {
                    invalidateCachedPair();
                    cancelRequests();
                    discardEyeAcquisitions();
                }
                if (!api.lossPending) {
                    if (snapshot.menuVisible) {
                        hudAnchor.reset();
                        wheelLayout.reset();
                        for (auto &wheel : wheels)
                            wheel.reset();
                        invalidateCachedPair();
                        cancelRequests();
                        menuLayer(input, menuQuad, layers);
                    } else {
                        menuAnchor.reset();
                        wheelLayers(snapshot.ui, input, quadLayers, layers);
                    }
                }
            }
            if (!published || !frameState.shouldRender)
                discardEyeAcquisitions();
            if (quit || api.lossPending)
                layers.clear();
            const auto *world = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projectionLayer);
            bool worldPresented = admitFinalWorld(layers, world, input, validViews,views);
            const bool canDrawHud = published && !quit && !api.lossPending && frameState.shouldRender &&
                                    !snapshot.menuVisible;
            bool fallbackDrawn = false;
            if (canDrawHud && (!worldPresented || cachedPresentation != NativeUiComplete)) {
                hudLayer(snapshot.ui, input, hudQuad, layers);
                fallbackDrawn = true;
            }
            // A fallback upload may wait for an image. Recheck the world after it
            // completes so an expired pair is never resubmitted.
            worldPresented = admitFinalWorld(layers, world, input, validViews,views);
            if (canDrawHud && !worldPresented && !fallbackDrawn) {
                hudLayer(snapshot.ui, input, hudQuad, layers);
                fallbackDrawn = true;
                worldPresented = admitFinalWorld(layers, world, input, validViews,views);
            }
            worldPresented=finalNativeUiLayers(quit,api.lossPending,layers,worldPresented);
            if (worldPresented) {
                ++submissions;
                if (cachedRequest.sequence == lastSubmittedSequence)
                    ++reused;
                lastSubmittedSequence = cachedRequest.sequence;
            }
            const auto endResult = frame.submit(layers);
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
