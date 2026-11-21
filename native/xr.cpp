// BlackQuest: OpenXR on Horizon OS. Phase 1 shows the game's finished frame on a quad ("theater mode"): no
// stereo yet, the runtime composites the quad in an empty dark space and handles head tracking, reprojection and
// recentering itself. Everything here runs on the Vulkan render thread except ApplyPad (game thread), Shutdown
// (sdl's thread) and the watch thread, which reads the runtime's events while the game is paused and draws nothing.
#include "xr.h"

#include <jni.h>
#include <vulkan/vulkan.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "kernel/kernel.h"

namespace xr {

namespace {

// the virtual screen: 16:9 (the game's widescreen picture; 4:3 is pillarboxed inside it), 3.2 m wide, 2.6 m out
// at eye height. about 63 degrees across: a big cinema screen, not a wall
constexpr uint32_t kScreenW = 1920;
constexpr uint32_t kScreenH = 1080;
constexpr float kScreenMetres = 3.2f;
constexpr float kScreenDistance = 2.6f;

XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_system = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_local = XR_NULL_HANDLE;
XrSwapchain g_screen = XR_NULL_HANDLE;
std::vector<VkImage> g_images;
// stereo (phase 2): a swapchain per eye at the runtime's recommended size; ScreenImages is then both eyes' images,
// the left eye's first
XrSwapchain g_eye_chain[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
uint32_t g_eye_images = 0;
uint32_t g_screen_images = 0;  // the quad screen's, after both eyes' in g_images
VkExtent2D g_eye_extent{};
// the views the frame being drawn is drawn with (located a frame ahead: draws run before the present that waits
// for the headset), and the ones just located for the next. the layer says which pose the pixels were drawn for,
// the compositor reprojects the rest
XrView g_render_views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
XrView g_next_views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
// the head's height in local space, latest located, for the game thread (vr_body.cpp)
std::atomic<float> g_head_y{0};
std::atomic<float> g_head_yaw{0}, g_head_pitch{0};
std::atomic<bool> g_head_valid{false};
bool g_views_valid = false;
VkFormat g_view_format = VK_FORMAT_R8G8B8A8_UNORM;
XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
bool g_running = false;
bool g_frame_begun = false;
XrTime g_display_time = 0;
// one owner of the session at a time: the render thread from BeginFrame to EndFrame, else the watch thread or
// Shutdown. paused in the background the game presents nothing, and a runtime asking us to stop or quit then
// would wait on us forever (the first headset run needed a force close)
std::timed_mutex g_session_lock;
bool g_frame_locked = false;  // render thread

PFN_xrGetVulkanGraphicsRequirements2KHR g_requirements = nullptr;
PFN_xrCreateVulkanInstanceKHR g_create_instance = nullptr;
PFN_xrGetVulkanGraphicsDevice2KHR g_graphics_device = nullptr;
PFN_xrCreateVulkanDeviceKHR g_create_device = nullptr;

// input: one action per control, left and right hand as subaction paths where both have it
XrActionSet g_set = XR_NULL_HANDLE;
XrPath g_hand[2] = {XR_NULL_PATH, XR_NULL_PATH};  // left, right
XrAction g_stick = XR_NULL_HANDLE, g_trigger = XR_NULL_HANDLE, g_grip = XR_NULL_HANDLE, g_thumb = XR_NULL_HANDLE;
XrAction g_a = XR_NULL_HANDLE, g_b = XR_NULL_HANDLE, g_x = XR_NULL_HANDLE, g_y = XR_NULL_HANDLE;
XrAction g_menu = XR_NULL_HANDLE;
XrAction g_pose = XR_NULL_HANDLE;  // grip poses, for the gestures (vr_body.cpp)
XrSpace g_hand_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};

// what the controllers last said, for the game thread
struct Controls {
    bool live = false;  // session focused and synced: else the pad gets nothing from us
    float stick[2][2] = {};
    float trigger[2] = {};
    float grip[2] = {};
    bool thumb[2] = {};
    bool a = false, b = false, x = false, y = false, menu = false;
    // local space, metres and metres a second: the hands' grips and the head's middle
    bool hand_ok[2] = {};
    float hand[2][3] = {}, hand_vel[2][3] = {};
    bool head_ok = false;
    float head[3] = {};
};
std::mutex g_controls_lock;
Controls g_controls;

const char* ResultName(XrResult r) {
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    if (g_instance && xrResultToString(g_instance, r, buf) == XR_SUCCESS) return buf;
    std::snprintf(buf, sizeof(buf), "%d", int(r));
    return buf;
}

// a call that must work: the log names it and what the runtime said
#define XR_OK(call)                                              \
    do {                                                         \
        XrResult _r = (call);                                    \
        if (XR_FAILED(_r)) {                                     \
            LOGE("xr: %s failed (%s)", #call, ResultName(_r));   \
            return false;                                        \
        }                                                        \
    } while (0)

template <typename F>
bool Proc(const char* name, F& out) {
    PFN_xrVoidFunction f = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(g_instance, name, &f)) || !f) {
        LOGE("xr: no %s", name);
        return false;
    }
    out = reinterpret_cast<F>(f);
    return true;
}

XrPath Path(const char* s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(g_instance, s, &p);
    return p;
}

bool MakeAction(const char* name, const char* label, XrActionType type, bool both, XrAction& out) {
    XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
    std::strncpy(ci.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(ci.localizedActionName, label, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    ci.actionType = type;
    if (both) {
        ci.countSubactionPaths = 2;
        ci.subactionPaths = g_hand;
    }
    XR_OK(xrCreateAction(g_set, &ci, &out));
    return true;
}

bool CreateActions() {
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(si.actionSetName, "gameplay");
    std::strcpy(si.localizedActionSetName, "Gameplay");
    XR_OK(xrCreateActionSet(g_instance, &si, &g_set));
    g_hand[0] = Path("/user/hand/left");
    g_hand[1] = Path("/user/hand/right");
    if (!MakeAction("stick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, true, g_stick) ||
        !MakeAction("trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT, true, g_trigger) ||
        !MakeAction("grip", "Grip", XR_ACTION_TYPE_FLOAT_INPUT, true, g_grip) ||
        !MakeAction("thumb", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT, true, g_thumb) ||
        !MakeAction("a", "A", XR_ACTION_TYPE_BOOLEAN_INPUT, false, g_a) ||
        !MakeAction("b", "B", XR_ACTION_TYPE_BOOLEAN_INPUT, false, g_b) ||
        !MakeAction("x", "X", XR_ACTION_TYPE_BOOLEAN_INPUT, false, g_x) ||
        !MakeAction("y", "Y", XR_ACTION_TYPE_BOOLEAN_INPUT, false, g_y) ||
        !MakeAction("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false, g_menu) ||
        !MakeAction("grip_pose", "Hand", XR_ACTION_TYPE_POSE_INPUT, true, g_pose))
        return false;
    // the touch profile: quest 3's touch plus controllers take it too
    const std::pair<XrAction, const char*> map[] = {
        {g_stick, "/user/hand/left/input/thumbstick"},          {g_stick, "/user/hand/right/input/thumbstick"},
        {g_trigger, "/user/hand/left/input/trigger/value"},     {g_trigger, "/user/hand/right/input/trigger/value"},
        {g_grip, "/user/hand/left/input/squeeze/value"},        {g_grip, "/user/hand/right/input/squeeze/value"},
        {g_thumb, "/user/hand/left/input/thumbstick/click"},    {g_thumb, "/user/hand/right/input/thumbstick/click"},
        {g_a, "/user/hand/right/input/a/click"},                {g_b, "/user/hand/right/input/b/click"},
        {g_x, "/user/hand/left/input/x/click"},                 {g_y, "/user/hand/left/input/y/click"},
        {g_menu, "/user/hand/left/input/menu/click"},
        {g_pose, "/user/hand/left/input/grip/pose"},            {g_pose, "/user/hand/right/input/grip/pose"},
    };
    std::vector<XrActionSuggestedBinding> b;
    for (const auto& [action, path] : map) b.push_back({action, Path(path)});
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = Path("/interaction_profiles/oculus/touch_controller");
    sb.countSuggestedBindings = uint32_t(b.size());
    sb.suggestedBindings = b.data();
    XR_OK(xrSuggestInteractionProfileBindings(g_instance, &sb));
    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &g_set;
    XR_OK(xrAttachSessionActionSets(g_session, &ai));
    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo sc{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        sc.action = g_pose;
        sc.subactionPath = g_hand[h];
        sc.poseInActionSpace.orientation.w = 1;
        if (XR_FAILED(xrCreateActionSpace(g_session, &sc, &g_hand_space[h]))) g_hand_space[h] = XR_NULL_HANDLE;
    }
    return true;
}

float FloatOf(XrAction a, XrPath hand) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, a, hand};
    XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(xrGetActionStateFloat(g_session, &gi, &s)) && s.isActive ? s.currentState : 0.0f;
}

bool BoolOf(XrAction a, XrPath hand = XR_NULL_PATH) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, a, hand};
    XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(xrGetActionStateBoolean(g_session, &gi, &s)) && s.isActive && s.currentState;
}

void ReadControls() {
    Controls c;
    if (g_state == XR_SESSION_STATE_FOCUSED) {
        XrActiveActionSet active{g_set, XR_NULL_PATH};
        XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
        si.countActiveActionSets = 1;
        si.activeActionSets = &active;
        c.live = xrSyncActions(g_session, &si) == XR_SUCCESS;
    }
    if (c.live) {
        for (int h = 0; h < 2; h++) {
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, g_stick, g_hand[h]};
            XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
            if (XR_SUCCEEDED(xrGetActionStateVector2f(g_session, &gi, &v)) && v.isActive) {
                c.stick[h][0] = v.currentState.x;
                c.stick[h][1] = v.currentState.y;
            }
            c.trigger[h] = FloatOf(g_trigger, g_hand[h]);
            c.grip[h] = FloatOf(g_grip, g_hand[h]);
            c.thumb[h] = BoolOf(g_thumb, g_hand[h]);
        }
        c.a = BoolOf(g_a);
        c.b = BoolOf(g_b);
        c.x = BoolOf(g_x);
        c.y = BoolOf(g_y);
        c.menu = BoolOf(g_menu);
        constexpr XrSpaceLocationFlags kPos = XR_SPACE_LOCATION_POSITION_VALID_BIT;
        for (int h = 0; h < 2 && g_local && g_display_time; h++) {
            if (!g_hand_space[h]) continue;
            XrSpaceVelocity v{XR_TYPE_SPACE_VELOCITY};
            XrSpaceLocation l{XR_TYPE_SPACE_LOCATION, &v};
            if (XR_FAILED(xrLocateSpace(g_hand_space[h], g_local, g_display_time, &l)) || !(l.locationFlags & kPos))
                continue;
            c.hand_ok[h] = true;
            c.hand[h][0] = l.pose.position.x, c.hand[h][1] = l.pose.position.y, c.hand[h][2] = l.pose.position.z;
            if (v.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT)
                c.hand_vel[h][0] = v.linearVelocity.x, c.hand_vel[h][1] = v.linearVelocity.y,
                c.hand_vel[h][2] = v.linearVelocity.z;
        }
        if (g_views_valid) {
            c.head_ok = true;
            c.head[0] = 0.5f * (g_next_views[0].pose.position.x + g_next_views[1].pose.position.x);
            c.head[1] = 0.5f * (g_next_views[0].pose.position.y + g_next_views[1].pose.position.y);
            c.head[2] = 0.5f * (g_next_views[0].pose.position.z + g_next_views[1].pose.position.z);
        }
    }
    std::lock_guard lock(g_controls_lock);
    g_controls = c;
}

// session lock held: everything given back to the runtime, so it never waits on a process that's gone
void Destroy() {
    if (g_session) xrDestroySession(g_session);
    if (g_instance) xrDestroyInstance(g_instance);
    g_session = XR_NULL_HANDLE;
    g_instance = XR_NULL_HANDLE;
    g_running = false;
}

// the runtime is done with us, or the player quit from the system menu. straight out from here: sdl's thread
// may be blocked on a paused activity and never see a quit event
[[noreturn]] void Quit(const char* why) {
    LOGR("xr: %s, closing the app", why);
    Destroy();
    kernel::Shutdown(0);
}

void PollEvents() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto& sc = reinterpret_cast<XrEventDataSessionStateChanged&>(ev);
            g_state = sc.state;
            LOGR("xr: session state %d", int(g_state));
            switch (g_state) {
                case XR_SESSION_STATE_READY: {
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    XrResult r = xrBeginSession(g_session, &bi);
                    g_running = XR_SUCCEEDED(r);
                    if (!g_running) LOGE("xr: xrBeginSession failed (%s)", ResultName(r));
                    break;
                }
                case XR_SESSION_STATE_STOPPING:
                    xrEndSession(g_session);
                    g_running = false;
                    break;
                case XR_SESSION_STATE_EXITING:
                    Quit("session exiting");
                case XR_SESSION_STATE_LOSS_PENDING:
                    Quit("session lost");
                default:
                    break;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            Quit("instance lost");
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

}  // namespace

bool Init() {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) {
        LOGR("xr: no activity, flat");
        return false;
    }
    JavaVM* vm = nullptr;
    env->GetJavaVM(&vm);
    // the loader and the instance keep the activity: a global reference, never released (one per run)
    jobject keep = env->NewGlobalRef(activity);
    env->DeleteLocalRef(activity);

    PFN_xrInitializeLoaderKHR init_loader = nullptr;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", reinterpret_cast<PFN_xrVoidFunction*>(&init_loader));
    if (!init_loader) {
        LOGR("xr: no loader, flat");
        return false;
    }
    XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    li.applicationVM = vm;
    li.applicationContext = keep;
    if (XR_FAILED(init_loader(reinterpret_cast<XrLoaderInitInfoBaseHeaderKHR*>(&li)))) {
        LOGR("xr: loader init failed, flat");
        return false;
    }

    XrInstanceCreateInfoAndroidKHR ai{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    ai.applicationVM = vm;
    ai.applicationActivity = keep;
    const char* exts[] = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO, &ai};
    std::strcpy(ci.applicationInfo.applicationName, "BlackQuest");
    ci.applicationInfo.applicationVersion = 1;
    std::strcpy(ci.applicationInfo.engineName, "XboxXBE-Recomp");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = 2;
    ci.enabledExtensionNames = exts;
    XrResult r = xrCreateInstance(&ci, &g_instance);
    if (XR_FAILED(r)) {
        LOGR("xr: no instance (%d), flat", int(r));
        g_instance = XR_NULL_HANDLE;
        return false;
    }
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(g_instance, &ip);
    LOGR("xr: runtime %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
         XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

    XrSystemGetInfo gi{XR_TYPE_SYSTEM_GET_INFO};
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_OK(xrGetSystem(g_instance, &gi, &g_system));
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(g_instance, g_system, &sp);
    LOGR("xr: system %s", sp.systemName);
    if (!Proc("xrGetVulkanGraphicsRequirements2KHR", g_requirements) ||
        !Proc("xrCreateVulkanInstanceKHR", g_create_instance) ||
        !Proc("xrGetVulkanGraphicsDevice2KHR", g_graphics_device) ||
        !Proc("xrCreateVulkanDeviceKHR", g_create_device))
        return false;
    // asked before the device is made, as the extension requires
    XrGraphicsRequirementsVulkan2KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    XR_OK(g_requirements(g_instance, g_system, &req));
    LOGI("xr: vulkan %u.%u to %u.%u", XR_VERSION_MAJOR(req.minApiVersionSupported),
         XR_VERSION_MINOR(req.minApiVersionSupported), XR_VERSION_MAJOR(req.maxApiVersionSupported),
         XR_VERSION_MINOR(req.maxApiVersionSupported));
    return true;
}

bool Active() { return g_instance != XR_NULL_HANDLE && g_system != XR_NULL_SYSTEM_ID; }

bool CreateVkInstance(const VkInstanceCreateInfo& ci, VkInstance& out) {
    XrVulkanInstanceCreateInfoKHR xi{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xi.systemId = g_system;
    xi.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    xi.vulkanCreateInfo = &ci;
    VkResult vr = VK_SUCCESS;
    XR_OK(g_create_instance(g_instance, &xi, &out, &vr));
    if (vr != VK_SUCCESS) {
        LOGE("xr: vkCreateInstance failed (%d)", int(vr));
        return false;
    }
    return true;
}

bool PhysicalDevice(VkInstance instance, VkPhysicalDevice& out) {
    XrVulkanGraphicsDeviceGetInfoKHR gi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    gi.systemId = g_system;
    gi.vulkanInstance = instance;
    XR_OK(g_graphics_device(g_instance, &gi, &out));
    return true;
}

bool CreateVkDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo& ci, VkDevice& out) {
    XrVulkanDeviceCreateInfoKHR xi{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xi.systemId = g_system;
    xi.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    xi.vulkanPhysicalDevice = pd;
    xi.vulkanCreateInfo = &ci;
    VkResult vr = VK_SUCCESS;
    XR_OK(g_create_device(g_instance, &xi, &out, &vr));
    if (vr != VK_SUCCESS) {
        LOGE("xr: vkCreateDevice failed (%d)", int(vr));
        return false;
    }
    return true;
}

// a swapchain per eye like the screen's, at the recommended size; false leaves the quad screen alone
bool CreateEyes(XrSwapchainCreateInfo sc) {
    uint32_t n = 0;
    xrEnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr);
    if (n != 2) return false;
    XrViewConfigurationView v[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    if (XR_FAILED(xrEnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                    2, &n, v)))
        return false;
    sc.width = v[0].recommendedImageRectWidth;
    sc.height = v[0].recommendedImageRectHeight;
    std::vector<VkImage> all;
    for (int e = 0; e < 2; e++) {
        if (XR_FAILED(xrCreateSwapchain(g_session, &sc, &g_eye_chain[e]))) return false;
        uint32_t count = 0;
        xrEnumerateSwapchainImages(g_eye_chain[e], 0, &count, nullptr);
        std::vector<XrSwapchainImageVulkan2KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
        xrEnumerateSwapchainImages(g_eye_chain[e], count, &count,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
        // both eyes index one list: the same count each
        if (e && count != g_eye_images) return false;
        g_eye_images = count;
        for (const auto& im : images) all.push_back(im.image);
    }
    g_eye_extent = {sc.width, sc.height};
    // the theater screen stays for frames without a world (menus, films, loading): its images after the eyes'
    all.insert(all.end(), g_images.begin(), g_images.end());
    g_images = std::move(all);
    LOGR("xr: eyes %ux%u (max %ux%u), %u images each", sc.width, sc.height, v[0].maxImageRectWidth,
         v[0].maxImageRectHeight, g_eye_images);
    return true;
}

bool CreateSession(VkInstance instance, VkPhysicalDevice pd, VkDevice dev, uint32_t queue_family) {
    XrGraphicsBindingVulkan2KHR gb{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    gb.instance = instance;
    gb.physicalDevice = pd;
    gb.device = dev;
    gb.queueFamilyIndex = queue_family;
    gb.queueIndex = 0;
    XrSessionCreateInfo si{XR_TYPE_SESSION_CREATE_INFO, &gb};
    si.systemId = g_system;
    XR_OK(xrCreateSession(g_instance, &si, &g_session));

    // local space: the origin where the head was at start, recentred by the runtime (hold the meta button)
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation.w = 1;
    XR_OK(xrCreateReferenceSpace(g_session, &rs, &g_local));

    // srgb storage, written through unorm views: the game's colours are gamma encoded already, so the bytes go in
    // as they are and the compositor reads them as srgb. a plain unorm chain would show them too bright
    uint32_t n = 0;
    xrEnumerateSwapchainFormats(g_session, 0, &n, nullptr);
    std::vector<int64_t> formats(n);
    xrEnumerateSwapchainFormats(g_session, n, &n, formats.data());
    int64_t chosen = 0;
    for (int64_t want : {int64_t(VK_FORMAT_R8G8B8A8_SRGB), int64_t(VK_FORMAT_R8G8B8A8_UNORM)})
        if (!chosen && std::find(formats.begin(), formats.end(), want) != formats.end()) chosen = want;
    if (!chosen) {
        LOGE("xr: no rgba8 swapchain format");
        return false;
    }
    bool srgb = chosen == VK_FORMAT_R8G8B8A8_SRGB;
    g_view_format = VK_FORMAT_R8G8B8A8_UNORM;
    XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                    (srgb ? XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT : 0);
    sc.format = chosen;
    sc.sampleCount = 1;
    sc.width = kScreenW;
    sc.height = kScreenH;
    sc.faceCount = 1;
    sc.arraySize = 1;
    sc.mipCount = 1;
    XR_OK(xrCreateSwapchain(g_session, &sc, &g_screen));
    xrEnumerateSwapchainImages(g_screen, 0, &n, nullptr);
    std::vector<XrSwapchainImageVulkan2KHR> images(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    XR_OK(xrEnumerateSwapchainImages(g_screen, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())));
    g_images.clear();
    for (const auto& im : images) g_images.push_back(im.image);
    g_screen_images = n;
    LOGR("xr: screen %ux%u, %u images, %s", kScreenW, kScreenH, n, srgb ? "srgb" : "unorm");
    // debug: a file named stereo_off in the app's folder keeps theater mode
    if (std::filesystem::exists(app::ExeDir() / "stereo_off")) LOGR("xr: stereo off (stereo_off file)");
    else if (!CreateEyes(sc)) LOGR("xr: no eye swapchains, theater mode only");

    if (!CreateActions()) LOGR("xr: no controller input");
    // a frame takes the lock for one display period at most, so this gets in between frames
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            std::lock_guard lock(g_session_lock);
            if (!g_instance) return;
            PollEvents();
        }
    }).detach();
    return true;
}

const std::vector<VkImage>& ScreenImages() { return g_images; }
VkFormat ScreenViewFormat() { return g_view_format; }
bool Stereo() { return g_eye_images != 0; }
bool Hands(Hand& left, Hand& right, float head[3]) {
    Controls c;
    {
        std::lock_guard lock(g_controls_lock);
        c = g_controls;
    }
    if (!c.live || !c.head_ok) return false;
    Hand* out[2] = {&left, &right};
    for (int h = 0; h < 2; h++) {
        out[h]->ok = c.hand_ok[h];
        std::memcpy(out[h]->pos, c.hand[h], sizeof(c.hand[h]));
        std::memcpy(out[h]->vel, c.hand_vel[h], sizeof(c.hand_vel[h]));
    }
    std::memcpy(head, c.head, sizeof(c.head));
    return true;
}
bool HeadAngles(float& yaw, float& pitch) {
    if (!Stereo() || !g_head_valid.load(std::memory_order_relaxed)) return false;
    yaw = g_head_yaw.load(std::memory_order_relaxed);
    pitch = g_head_pitch.load(std::memory_order_relaxed);
    return true;
}
bool HeadHeight(float& y) {
    if (!Stereo() || !g_head_valid.load(std::memory_order_relaxed)) return false;
    y = g_head_y.load(std::memory_order_relaxed);
    return true;
}
int ImageEye(uint32_t index) {
    if (!Stereo()) return kTheater;
    return index < g_eye_images ? 0 : index < 2 * g_eye_images ? 1 : kTheater;
}
VkExtent2D ImageExtent(uint32_t index) {
    return ImageEye(index) == kTheater ? VkExtent2D{kScreenW, kScreenH} : g_eye_extent;
}
VkExtent2D ScreenExtent() { return Stereo() ? g_eye_extent : VkExtent2D{kScreenW, kScreenH}; }

bool RenderEye(int eye, EyeView& out) {
    if (!g_views_valid) return false;
    const XrView& v = g_render_views[eye & 1];
    // debug: a file named stereo_nohead in the app's folder draws with the eyes' offset alone, no head pose, so
    // a headset lying on a desk still looks down the game's camera (screenshots over adb)
    static const bool nohead = std::filesystem::exists(app::ExeDir() / "stereo_nohead");
    if (nohead) {
        const XrVector3f& a = g_render_views[0].pose.position;
        const XrVector3f& b = g_render_views[1].pose.position;
        float half = 0.5f * std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
        out.pos[0] = eye ? half : -half;
        out.pos[1] = out.pos[2] = 0;
        out.quat[0] = out.quat[1] = out.quat[2] = 0;
        out.quat[3] = 1;
        out.tan[0] = std::tan(v.fov.angleLeft);
        out.tan[1] = std::tan(v.fov.angleRight);
        out.tan[2] = std::tan(v.fov.angleUp);
        out.tan[3] = std::tan(v.fov.angleDown);
        return true;
    }
    out.pos[0] = v.pose.position.x;
    out.pos[1] = v.pose.position.y;
    out.pos[2] = v.pose.position.z;
    out.quat[0] = v.pose.orientation.x;
    out.quat[1] = v.pose.orientation.y;
    out.quat[2] = v.pose.orientation.z;
    out.quat[3] = v.pose.orientation.w;
    out.tan[0] = std::tan(v.fov.angleLeft);
    out.tan[1] = std::tan(v.fov.angleRight);
    out.tan[2] = std::tan(v.fov.angleUp);
    out.tan[3] = std::tan(v.fov.angleDown);
    return true;
}

bool BeginFrame() {
    g_frame_begun = false;
    g_session_lock.lock();
    g_frame_locked = true;
    if (!g_instance) return false;
    PollEvents();
    if (!g_running) {
        // no frames to wait for: the headset is off or the session is starting. don't let the game spin
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::lock_guard lock(g_controls_lock);
        g_controls = {};
        return false;
    }
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    XrResult r = xrWaitFrame(g_session, nullptr, &fs);
    if (XR_FAILED(r)) {
        LOG_FIRST(4, "xr: xrWaitFrame failed (%s)", ResultName(r));
        return false;
    }
    if (XR_FAILED(xrBeginFrame(g_session, nullptr))) return false;
    g_frame_begun = true;
    g_display_time = fs.predictedDisplayTime;
    if (Stereo()) {
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = fs.predictedDisplayTime;
        li.space = g_local;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        uint32_t n = 0;
        XrView v[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        constexpr XrViewStateFlags kTracked = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
        if (XR_SUCCEEDED(xrLocateViews(g_session, &li, &vs, 2, &n, v)) && n == 2 &&
            (vs.viewStateFlags & kTracked) == kTracked) {
            g_next_views[0] = v[0];
            g_next_views[1] = v[1];
            g_head_y.store(0.5f * (v[0].pose.position.y + v[1].pose.position.y), std::memory_order_relaxed);
            {
                // the head's look: forward = R (0, 0, -1); yaw about +y (left positive), pitch up positive
                const XrQuaternionf& q = v[0].pose.orientation;
                float fx = -2 * (q.x * q.z + q.w * q.y), fy = -2 * (q.y * q.z - q.w * q.x);
                float fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
                g_head_yaw.store(std::atan2(-fx, -fz), std::memory_order_relaxed);
                g_head_pitch.store(std::asin(std::clamp(fy, -1.0f, 1.0f)), std::memory_order_relaxed);
            }
            g_head_valid.store(true, std::memory_order_relaxed);
            // the very first frame has nothing drawn for any pose yet
            if (!g_views_valid) {
                g_render_views[0] = v[0];
                g_render_views[1] = v[1];
                g_views_valid = true;
            }
        }
    }
    ReadControls();
    return fs.shouldRender;
}

XrSwapchain Chain(int eye) { return Stereo() && eye != kTheater ? g_eye_chain[eye & 1] : g_screen; }

bool AcquireScreen(int eye, uint32_t& index) {
    XrSwapchain chain = Chain(eye);
    if (!g_frame_begun || XR_FAILED(xrAcquireSwapchainImage(chain, nullptr, &index))) return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(chain, &wi))) {
        xrReleaseSwapchainImage(chain, nullptr);
        return false;
    }
    if (Stereo()) index += eye == kTheater ? 2 * g_eye_images : uint32_t(eye & 1) * g_eye_images;
    return true;
}

void ReleaseScreen(int eye) { xrReleaseSwapchainImage(Chain(eye), nullptr); }

void EndFrame(bool drawn, bool theater) {
    struct Unlock {
        ~Unlock() {
            if (g_frame_locked) g_session_lock.unlock();
            g_frame_locked = false;
        }
    } unlock;
    if (!g_frame_begun) return;
    g_frame_begun = false;
    if (Stereo() && !theater) {
        // the eyes as drawn: the views they were drawn with, then the next frame's draws take the ones just located
        XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                  {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        for (int e = 0; e < 2; e++) {
            pv[e].pose = g_render_views[e].pose;
            pv[e].fov = g_render_views[e].fov;
            pv[e].subImage.swapchain = g_eye_chain[e];
            pv[e].subImage.imageRect = {{0, 0}, {int32_t(g_eye_extent.width), int32_t(g_eye_extent.height)}};
        }
        XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        proj.space = g_local;
        proj.viewCount = 2;
        proj.views = pv;
        const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&proj)};
        XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
        ei.displayTime = g_display_time;
        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        ei.layerCount = drawn && g_views_valid ? 1 : 0;
        ei.layers = layers;
        XrResult r = xrEndFrame(g_session, &ei);
        if (XR_FAILED(r)) LOG_FIRST(4, "xr: xrEndFrame failed (%s)", ResultName(r));
        g_render_views[0] = g_next_views[0];
        g_render_views[1] = g_next_views[1];
        return;
    }
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.space = g_local;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = g_screen;
    quad.subImage.imageRect = {{0, 0}, {int32_t(kScreenW), int32_t(kScreenH)}};
    quad.pose.orientation.w = 1;
    quad.pose.position = {0, 0, -kScreenDistance};
    quad.size = {kScreenMetres, kScreenMetres * kScreenH / kScreenW};
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad)};
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = g_display_time;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = drawn ? 1 : 0;
    ei.layers = layers;
    XrResult r = xrEndFrame(g_session, &ei);
    if (XR_FAILED(r)) LOG_FIRST(4, "xr: xrEndFrame failed (%s)", ResultName(r));
    g_render_views[0] = g_next_views[0];
    g_render_views[1] = g_next_views[1];
}

// Touch → the Xbox pad. sticks and triggers as they are; A B X Y on the same letters; grips as the black (right)
// and white (left) buttons; stick clicks as the thumb buttons. the left menu button is START on a tap and BACK when
// held half a second (touch has no back button and no d-pad: the game's menus move with the stick)
void ApplyPad(int port, input::Pad& p) {
    if (port != 0 || !Active()) return;
    Controls c;
    {
        std::lock_guard lock(g_controls_lock);
        c = g_controls;
    }
    using clock = std::chrono::steady_clock;
    static clock::time_point menu_down;
    static bool menu_was = false, back_sent = false;
    static clock::time_point start_until;
    auto now = clock::now();
    bool start = false, back = false;
    if (c.menu && !menu_was) {
        menu_down = now;
        back_sent = false;
    }
    if (c.menu && !back_sent && now - menu_down >= std::chrono::milliseconds(500)) back_sent = true;
    if (c.menu && back_sent) back = true;
    // a tap: START held for a few frames after the release, so the game's poll sees it
    if (!c.menu && menu_was && !back_sent) start_until = now + std::chrono::milliseconds(100);
    if (now < start_until) start = true;
    menu_was = c.menu;
    if (!c.live) return;
    p.connected = true;
    auto axis = [](float v) { return int16_t(std::clamp(v, -1.0f, 1.0f) * 32767.0f); };
    auto analog = [](float v) { return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
    // the pad's own values win when pushed further: a bluetooth controller still works next to touch
    auto further = [](int16_t a, int16_t b) { return std::abs(int(b)) > std::abs(int(a)) ? b : a; };
    p.lx = further(p.lx, axis(c.stick[0][0]));
    p.ly = further(p.ly, axis(c.stick[0][1]));
    p.rx = further(p.rx, axis(c.stick[1][0]));
    p.ry = further(p.ry, axis(c.stick[1][1]));
    const uint8_t a[8] = {uint8_t(c.a ? 255 : 0), uint8_t(c.b ? 255 : 0), uint8_t(c.x ? 255 : 0),
                          uint8_t(c.y ? 255 : 0), analog(c.grip[1]), analog(c.grip[0]),
                          analog(c.trigger[0]), analog(c.trigger[1])};
    for (int i = 0; i < 8; i++) p.analog[i] = std::max(p.analog[i], a[i]);
    if (start) p.buttons |= 1 << 4;
    if (back) p.buttons |= 1 << 5;
    if (c.thumb[0]) p.buttons |= 1 << 6;
    if (c.thumb[1]) p.buttons |= 1 << 7;
}

void Shutdown() {
    if (!g_instance) return;
    // a render thread stuck in a frame doesn't keep the app open: the process ends either way
    std::unique_lock lock(g_session_lock, std::chrono::milliseconds(500));
    if (!lock) {
        LOGR("xr: session busy, exiting without it");
        return;
    }
    LOGR("xr: app quit, session closed");
    Destroy();
}

}  // namespace xr
