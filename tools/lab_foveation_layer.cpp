// Lab-only OpenXR API layer: offers the Steam Frame foveation extensions
// (XR_FB_foveation, XR_FB_foveation_configuration, XR_FB_swapchain_update_state,
// XR_META_foveation_eye_tracked) on a runtime without them, so the host's
// eye-tracked foveation path runs end to end. Its own structures are removed
// before anything reaches the runtime; the eye-tracked state reports the centre
// in SS2VR_LAB_META_CENTRE ("x,y" view NDC, both eyes; default -0.4,-0.3).
// Every call is logged to lab_foveation_layer.log beside the DLL.
//
// Build: x86_64-w64-mingw32-g++ -std=c++20 -O2 -shared -static -I<openxr>/include
//        tools/lab_foveation_layer.cpp -o lab_foveation_layer.dll
#include <windows.h>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
PFN_xrGetInstanceProcAddr nextGetInstanceProcAddr = nullptr;
PFN_xrGetSystemProperties nextGetSystemProperties = nullptr;
PFN_xrCreateSwapchain nextCreateSwapchain = nullptr;
FILE *logFile = nullptr;
const char *const Offered[]{XR_FB_FOVEATION_EXTENSION_NAME, XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME,
                            XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME, XR_META_FOVEATION_EYE_TRACKED_EXTENSION_NAME};
bool enabled = false;
unsigned profiles = 0, updates = 0, states = 0;

void say(const char *format, ...) {
    if (!logFile) {
        wchar_t path[MAX_PATH]{};
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&say), &self);
        GetModuleFileNameW(self, path, MAX_PATH);
        if (wchar_t *slash = wcsrchr(path, L'\\')) slash[1] = 0;
        wcscat_s(path, L"lab_foveation_layer.log");
        logFile = _wfopen(path, L"w");
        if (!logFile) return;
    }
    va_list args;
    va_start(args, format);
    std::vfprintf(logFile, format, args);
    va_end(args);
    std::fputc('\n', logFile);
    std::fflush(logFile);
}
bool offered(const char *name) {
    for (const char *o : Offered)
        if (!std::strcmp(o, name)) return true;
    return false;
}

XrResult XRAPI_CALL getSystemProperties(XrInstance instance, XrSystemId system, XrSystemProperties *properties) {
    // Unlink our property structure, answer it after the runtime fills the rest.
    XrBaseOutStructure *previous = reinterpret_cast<XrBaseOutStructure *>(properties), *ours = nullptr;
    for (auto *node = previous->next; node; previous = node, node = node->next)
        if (node->type == XR_TYPE_SYSTEM_FOVEATION_EYE_TRACKED_PROPERTIES_META) {
            ours = node;
            previous->next = node->next;
            break;
        }
    const XrResult result = nextGetSystemProperties(instance, system, properties);
    if (ours) {
        ours->next = previous->next;
        previous->next = ours;
        reinterpret_cast<XrSystemFoveationEyeTrackedPropertiesMETA *>(ours)->supportsFoveationEyeTracked = XR_TRUE;
        say("xrGetSystemProperties: reported supportsFoveationEyeTracked");
    }
    return result;
}
XrResult XRAPI_CALL createSwapchain(XrSession session, const XrSwapchainCreateInfo *info, XrSwapchain *swapchain) {
    XrSwapchainCreateInfo copy = *info;
    bool foveation = false;
    for (auto *node = reinterpret_cast<const XrBaseInStructure *>(info->next); node; node = node->next)
        if (node->type == XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB) foveation = true;
    if (foveation) {
        // Only the foveation info is chained by the host; drop the whole chain otherwise untouched.
        const auto *first = reinterpret_cast<const XrBaseInStructure *>(info->next);
        copy.next = first && first->type == XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB ? first->next : info->next;
    }
    const XrResult result = nextCreateSwapchain(session, &copy, swapchain);
    say("xrCreateSwapchain %ux%u foveationInfo=%d result=%d", info->width, info->height, int(foveation), int(result));
    return result;
}
XrResult XRAPI_CALL createFoveationProfile(XrSession, const XrFoveationProfileCreateInfoFB *info, XrFoveationProfileFB *profile) {
    bool level = false, tracked = false;
    for (auto *node = reinterpret_cast<const XrBaseInStructure *>(info->next); node; node = node->next) {
        level = level || node->type == XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB;
        tracked = tracked || node->type == XR_TYPE_FOVEATION_EYE_TRACKED_PROFILE_CREATE_INFO_META;
    }
    *profile = reinterpret_cast<XrFoveationProfileFB>(uintptr_t(0x5100 + ++profiles));
    say("xrCreateFoveationProfileFB level=%d eyeTracked=%d -> %u", int(level), int(tracked), profiles);
    return level && tracked ? XR_SUCCESS : XR_ERROR_VALIDATION_FAILURE;
}
XrResult XRAPI_CALL destroyFoveationProfile(XrFoveationProfileFB) {
    say("xrDestroyFoveationProfileFB");
    return XR_SUCCESS;
}
XrResult XRAPI_CALL updateSwapchain(XrSwapchain, const XrSwapchainStateBaseHeaderFB *state) {
    const bool foveation = state && state->type == XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB;
    say("xrUpdateSwapchainFB foveation=%d (%u)", int(foveation), ++updates);
    return foveation ? XR_SUCCESS : XR_ERROR_VALIDATION_FAILURE;
}
XrResult XRAPI_CALL foveationEyeTrackedState(XrSession, XrFoveationEyeTrackedStateMETA *state) {
    static float x = -.4f, y = -.3f;
    static const bool parsed = [] {
        char value[32]{};
        if (GetEnvironmentVariableA("SS2VR_LAB_META_CENTRE", value, sizeof value)) std::sscanf(value, "%f,%f", &x, &y);
        return true;
    }();
    (void)parsed;
    if (!state || state->type != XR_TYPE_FOVEATION_EYE_TRACKED_STATE_META) return XR_ERROR_VALIDATION_FAILURE;
    for (auto &centre : state->foveationCenter) centre = {x, y};
    state->flags = XR_FOVEATION_EYE_TRACKED_STATE_VALID_BIT_META;
    if (++states == 1 || states % 500 == 0) say("xrGetFoveationEyeTrackedStateMETA %u: centre %.2f,%.2f", states, x, y);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL getInstanceProcAddr(XrInstance instance, const char *name, PFN_xrVoidFunction *function) {
    struct Entry { const char *name; PFN_xrVoidFunction fn; };
    const Entry ours[]{
        {"xrGetInstanceProcAddr", reinterpret_cast<PFN_xrVoidFunction>(getInstanceProcAddr)},
        {"xrGetSystemProperties", reinterpret_cast<PFN_xrVoidFunction>(getSystemProperties)},
        {"xrCreateSwapchain", reinterpret_cast<PFN_xrVoidFunction>(createSwapchain)},
        {"xrCreateFoveationProfileFB", reinterpret_cast<PFN_xrVoidFunction>(createFoveationProfile)},
        {"xrDestroyFoveationProfileFB", reinterpret_cast<PFN_xrVoidFunction>(destroyFoveationProfile)},
        {"xrUpdateSwapchainFB", reinterpret_cast<PFN_xrVoidFunction>(updateSwapchain)},
        {"xrGetFoveationEyeTrackedStateMETA", reinterpret_cast<PFN_xrVoidFunction>(foveationEyeTrackedState)}};
    for (const auto &e : ours)
        if (!std::strcmp(name, e.name) && (enabled || !std::strstr(name, "FB") && !std::strstr(name, "META"))) {
            *function = e.fn;
            return XR_SUCCESS;
        }
    return nextGetInstanceProcAddr(instance, name, function);
}
XrResult XRAPI_CALL createApiLayerInstance(const XrInstanceCreateInfo *info, const XrApiLayerCreateInfo *layerInfo,
                                          XrInstance *instance) {
    std::vector<const char *> names;
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
        if (offered(info->enabledExtensionNames[i])) enabled = true;
        else names.push_back(info->enabledExtensionNames[i]);
    }
    XrInstanceCreateInfo copy = *info;
    copy.enabledExtensionCount = uint32_t(names.size());
    copy.enabledExtensionNames = names.data();
    XrApiLayerCreateInfo next = *layerInfo;
    next.nextInfo = layerInfo->nextInfo->next;
    nextGetInstanceProcAddr = layerInfo->nextInfo->nextGetInstanceProcAddr;
    const XrResult result = layerInfo->nextInfo->nextCreateApiLayerInstance(&copy, &next, instance);
    if (XR_SUCCEEDED(result)) {
        nextGetInstanceProcAddr(*instance, "xrGetSystemProperties", reinterpret_cast<PFN_xrVoidFunction *>(&nextGetSystemProperties));
        nextGetInstanceProcAddr(*instance, "xrCreateSwapchain", reinterpret_cast<PFN_xrVoidFunction *>(&nextCreateSwapchain));
    }
    say("xrCreateApiLayerInstance: foveation extensions requested=%d result=%d", int(enabled), int(result));
    return result;
}
} // namespace

extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo *loaderInfo, const char *, XrNegotiateApiLayerRequest *request) {
    if (!loaderInfo || !request || loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION)
        return XR_ERROR_INITIALIZATION_FAILED;
    request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    request->layerApiVersion = XR_CURRENT_API_VERSION;
    request->getInstanceProcAddr = getInstanceProcAddr;
    request->createApiLayerInstance = createApiLayerInstance;
    say("negotiated");
    return XR_SUCCESS;
}
