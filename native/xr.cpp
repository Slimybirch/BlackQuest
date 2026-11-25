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

#include "common/options.h"
#include "app/app.h"
#include "common/menu.h"
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
// the settings panel's swapchain (user: beside the pause menu and the title's menus, no button combo): its images
// last in g_images, from g_panel_first
XrSwapchain g_panel = XR_NULL_HANDLE;
bool g_refresh_ext = false;  // XR_FB_display_refresh_rate enabled
size_t g_panel_first = SIZE_MAX;
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
std::atomic<uint32_t> g_no_render{0};  // frames the runtime said not to render (FrameReport)
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
// capacitive touch, for the hands' fingers: the thumb on anything (rest, stick, face buttons), the index on the trigger
XrAction g_thumb_touch = XR_NULL_HANDLE, g_trigger_touch = XR_NULL_HANDLE;
XrAction g_pose = XR_NULL_HANDLE;  // grip poses, for the gestures (vr_body.cpp)
XrSpace g_hand_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
XrAction g_aim = XR_NULL_HANDLE;  // aim poses: where the gun in the hand points (vr_gun.cpp)
XrSpace g_aim_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
XrAction g_haptic = XR_NULL_HANDLE;
std::atomic<uint32_t> g_rumble{0};  // left << 16 | right, the game's last, sent by the render thread
std::atomic<uint32_t> g_pulse[2];   // a buzz asked for: amplitude (0..255) << 16 | milliseconds
std::atomic<bool> g_left_grip_taken{false};  // the left grip holds the gun, not the white button

// what the controllers last said, for the game thread
struct Controls {
    bool live = false;  // session focused and synced: else the pad gets nothing from us
    float stick[2][2] = {};
    float trigger[2] = {};
    float grip[2] = {};
    bool thumb[2] = {};
    bool thumb_touch[2] = {}, trigger_touch[2] = {};
    bool a = false, b = false, x = false, y = false, menu = false;
    // local space, metres and metres a second: the hands' grips and the head's middle
    bool hand_ok[2] = {};
    float hand[2][3] = {}, hand_vel[2][3] = {};
    bool aim_ok[2] = {};
    float aim[2][4] = {};
    float aim_pos[2][3] = {};  // the aim pose's origin (the menu's pointer)
    bool head_ok = false;
    float head[3] = {};
};
std::mutex g_controls_lock;
Controls g_controls;
// the quad imgui was last shown on (the theater screen or the settings panel), for the pointer. render thread
XrPosef g_ui_pose{{0, 0, 0, 1}, {0, 0, 0}};
XrExtent2Df g_ui_size{};
// the test checklist's card as last shown (size 0: not shown). render thread
constexpr float kCardMetres = 0.17f, kTipMetres = 0.15f;
XrPosef g_card_pose{{0, 0, 0, 1}, {0, 0, 0}};
XrExtent2Df g_card_size{};

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
        !MakeAction("thumb_touch", "Thumb resting", XR_ACTION_TYPE_BOOLEAN_INPUT, true, g_thumb_touch) ||
        !MakeAction("trigger_touch", "Finger on trigger", XR_ACTION_TYPE_BOOLEAN_INPUT, true, g_trigger_touch) ||
        !MakeAction("grip_pose", "Hand", XR_ACTION_TYPE_POSE_INPUT, true, g_pose) ||
        !MakeAction("aim_pose", "Aim", XR_ACTION_TYPE_POSE_INPUT, true, g_aim) ||
        !MakeAction("haptic", "Rumble", XR_ACTION_TYPE_VIBRATION_OUTPUT, true, g_haptic))
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
        {g_thumb_touch, "/user/hand/left/input/thumbrest/touch"},  {g_thumb_touch, "/user/hand/right/input/thumbrest/touch"},
        {g_thumb_touch, "/user/hand/left/input/thumbstick/touch"}, {g_thumb_touch, "/user/hand/right/input/thumbstick/touch"},
        {g_thumb_touch, "/user/hand/left/input/x/touch"},          {g_thumb_touch, "/user/hand/left/input/y/touch"},
        {g_thumb_touch, "/user/hand/right/input/a/touch"},         {g_thumb_touch, "/user/hand/right/input/b/touch"},
        {g_trigger_touch, "/user/hand/left/input/trigger/touch"},  {g_trigger_touch, "/user/hand/right/input/trigger/touch"},
        {g_pose, "/user/hand/left/input/grip/pose"},            {g_pose, "/user/hand/right/input/grip/pose"},
        {g_aim, "/user/hand/left/input/aim/pose"},              {g_aim, "/user/hand/right/input/aim/pose"},
        {g_haptic, "/user/hand/left/output/haptic"},            {g_haptic, "/user/hand/right/output/haptic"},
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
        sc.action = g_aim;
        if (XR_FAILED(xrCreateActionSpace(g_session, &sc, &g_aim_space[h]))) g_aim_space[h] = XR_NULL_HANDLE;
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
            c.thumb_touch[h] = BoolOf(g_thumb_touch, g_hand[h]);
            c.trigger_touch[h] = BoolOf(g_trigger_touch, g_hand[h]);
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
        for (int h = 0; h < 2 && g_local && g_display_time; h++) {
            XrSpaceLocation l{XR_TYPE_SPACE_LOCATION};
            if (!g_aim_space[h] || XR_FAILED(xrLocateSpace(g_aim_space[h], g_local, g_display_time, &l)) ||
                !(l.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
                continue;
            c.aim_ok[h] = true;
            c.aim[h][0] = l.pose.orientation.x, c.aim[h][1] = l.pose.orientation.y;
            c.aim[h][2] = l.pose.orientation.z, c.aim[h][3] = l.pose.orientation.w;
            c.aim_pos[h][0] = l.pose.position.x, c.aim_pos[h][1] = l.pose.position.y;
            c.aim_pos[h][2] = l.pose.position.z;
        }
        // haptics: a shot's buzz, else the game's rumble. Sent again every 80 ms as 120 ms buzzes while it lasts:
        // the quest runtime cuts an infinite duration short, which left the game's rumble mostly unfelt
        {
            using clock = std::chrono::steady_clock;
            static clock::time_point until[2], resend[2];
            static uint32_t sent = 0;
            auto now = clock::now();
            uint32_t rumble = g_rumble.load(std::memory_order_relaxed);
            bool changed = rumble != sent;
            sent = rumble;
            for (int h = 0; h < 2; h++) {
                XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO, nullptr, g_haptic, g_hand[h]};
                XrHapticVibration vib{XR_TYPE_HAPTIC_VIBRATION};
                vib.frequency = XR_FREQUENCY_UNSPECIFIED;
                if (uint32_t p = g_pulse[h].exchange(0); p) {
                    vib.amplitude = float(p >> 16) / 255.0f;
                    vib.duration = int64_t(p & 0xFFFF) * 1000000;
                    until[h] = now + std::chrono::milliseconds(p & 0xFFFF);
                    xrApplyHapticFeedback(g_session, &hi, reinterpret_cast<XrHapticBaseHeader*>(&vib));
                    continue;
                }
                if (now < until[h]) continue;  // a buzz still running
                float amp = float(h ? rumble & 0xFFFF : rumble >> 16) / 65535.0f;
                if (amp <= 0) {
                    if (changed) xrStopHapticFeedback(g_session, &hi);
                    continue;
                }
                if (!changed && now < resend[h]) continue;
                resend[h] = now + std::chrono::milliseconds(80);
                vib.amplitude = std::min(amp, 1.0f);
                vib.duration = 120 * 1000000LL;
                xrApplyHapticFeedback(g_session, &hi, reinterpret_cast<XrHapticBaseHeader*>(&vib));
            }
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
    // (and the display's refresh rate, when the runtime offers it: VR SETTINGS)
    const char* exts[3] = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    uint32_t ext_count = 2;
    {
        uint32_t n = 0;
        xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
        std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
        xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data());
        for (const auto& p : props)
            if (!std::strcmp(p.extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
                exts[ext_count++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME, g_refresh_ext = true;
    }
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO, &ai};
    std::strcpy(ci.applicationInfo.applicationName, "BlackQuest");
    ci.applicationInfo.applicationVersion = 1;
    std::strcpy(ci.applicationInfo.engineName, "XboxXBE-Recomp");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = ext_count;
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
    // the settings panel's (the screen's size: its layout as the theater's)
    if (XR_SUCCEEDED(xrCreateSwapchain(g_session, &sc, &g_panel))) {
        uint32_t pn = 0;
        xrEnumerateSwapchainImages(g_panel, 0, &pn, nullptr);
        std::vector<XrSwapchainImageVulkan2KHR> pim(pn, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
        if (XR_SUCCEEDED(xrEnumerateSwapchainImages(g_panel, pn, &pn,
                                                    reinterpret_cast<XrSwapchainImageBaseHeader*>(pim.data())))) {
            g_panel_first = g_images.size();
            for (const auto& im : pim) g_images.push_back(im.image);
            LOGR("xr: settings panel %ux%u, %u images", kScreenW, kScreenH, pn);
        }
    }

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
        out[h]->aim_ok = c.aim_ok[h];
        std::memcpy(out[h]->aim, c.aim[h], sizeof(c.aim[h]));
        out[h]->grip = c.grip[h];
        out[h]->trigger = c.trigger[h];
        out[h]->thumb_touch = c.thumb_touch[h];
        out[h]->trigger_touch = c.trigger_touch[h];
        out[h]->primary = h ? c.a : c.x;
    }
    std::memcpy(head, c.head, sizeof(c.head));
    return true;
}
void Rumble(uint16_t left, uint16_t right) {
    uint32_t v = uint32_t(left) << 16 | right;
    if (v && g_rumble.load(std::memory_order_relaxed) != v) LOG_FIRST(8, "xr: game rumble %u %u", left, right);
    g_rumble.store(v, std::memory_order_relaxed);
}
void TakeLeftGrip(bool taken) { g_left_grip_taken.store(taken, std::memory_order_relaxed); }
std::atomic<bool> g_left_trigger_taken{false};
void TakeLeftTrigger(bool taken) { g_left_trigger_taken.store(taken, std::memory_order_relaxed); }
void Pulse(int hand, float amplitude, float seconds) {
    if (hand < 0 || hand > 1) return;
    uint32_t a = uint32_t(std::clamp(amplitude, 0.0f, 1.0f) * 255), ms = uint32_t(std::clamp(seconds, 0.0f, 1.0f) * 1000);
    if (a && ms) g_pulse[hand].store(a << 16 | ms);
}
namespace {
// v rotated by the unit quaternion q (or by its inverse)
XrVector3f Rotate(const XrQuaternionf& q, XrVector3f v, bool inverse = false) {
    float qx = inverse ? -q.x : q.x, qy = inverse ? -q.y : q.y, qz = inverse ? -q.z : q.z;
    float tx = 2 * (qy * v.z - qz * v.y), ty = 2 * (qz * v.x - qx * v.z), tz = 2 * (qx * v.y - qy * v.x);
    return {v.x + q.w * tx + (qy * tz - qz * ty), v.y + q.w * ty + (qz * tx - qx * tz),
            v.z + q.w * tz + (qx * ty - qy * tx)};
}
}  // namespace

// the right aim ray where it meets a quad (z = 0 in its own space, facing +z): 0..1 across and down
bool RayOnQuad(const Controls& c, const XrPosef& p, const XrExtent2Df& size, float& u, float& v) {
    if (!c.aim_ok[1] || size.width <= 0) return false;
    XrQuaternionf aq{c.aim[1][0], c.aim[1][1], c.aim[1][2], c.aim[1][3]};
    XrVector3f d = Rotate(p.orientation, Rotate(aq, {0, 0, -1}), true);
    XrVector3f o = Rotate(p.orientation,
                          {c.aim_pos[1][0] - p.position.x, c.aim_pos[1][1] - p.position.y,
                           c.aim_pos[1][2] - p.position.z},
                          true);
    if (o.z <= 0 || d.z >= -1e-3f) return false;
    float t = -o.z / d.z;
    u = (o.x + d.x * t) / size.width + 0.5f;
    v = 0.5f - (o.y + d.y * t) / size.height;
    return u >= 0 && u <= 1 && v >= 0 && v <= 1;
}

bool CardPointer(float& u, float& v) {
    Controls c;
    {
        std::lock_guard lock(g_controls_lock);
        c = g_controls;
    }
    return c.live && RayOnQuad(c, g_card_pose, g_card_size, u, v);
}

bool MenuControls(MenuInput& in) {
    Controls c;
    {
        std::lock_guard lock(g_controls_lock);
        c = g_controls;
    }
    if (!c.live) return false;
    in.stick[0] = c.stick[0][0];
    in.stick[1] = c.stick[0][1];
    in.a = c.a;
    in.trigger = c.trigger[1] > 0.5f;
    in.pointing = RayOnQuad(c, g_ui_pose, g_ui_size, in.u, in.v);
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
    if (index >= g_panel_first) return kPanel;
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

void ApplyRefreshRate();  // (below)

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
    ApplyRefreshRate();
    if (!fs.shouldRender) g_no_render.fetch_add(1, std::memory_order_relaxed);
    return fs.shouldRender;
}

// every 5 s: frames ended, how many had no layers (the compositor shows passthrough/home for those), how many the
// runtime said not to render, the session state. Why a capture shows the room while the game's focused
static void FrameReport(uint32_t layers) {
    static uint32_t ended, empty;
    static auto last = std::chrono::steady_clock::now();
    ended++;
    if (!layers) empty++;
    auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(5)) return;
    last = now;
    LOGR("xr: frames: %u ended, %u with no layers, %u not to render, session state %d", ended, empty,
         g_no_render.exchange(0), int(g_state));
    ended = empty = 0;
}

// the refresh rate VR SETTINGS asks for (0 the headset's own), when it changes and the runtime offers it: the
// nearest it has. Frame thread
void ApplyRefreshRate() {
    static uint32_t applied = ~0u;
    uint32_t want = game::Opt().vr_refresh;
    if (!g_refresh_ext || want == applied || !g_session) return;
    applied = want;
    if (!want) return;  // (the headset's own: left as it is)
    PFN_xrEnumerateDisplayRefreshRatesFB enumerate = nullptr;
    PFN_xrRequestDisplayRefreshRateFB request = nullptr;
    xrGetInstanceProcAddr(g_instance, "xrEnumerateDisplayRefreshRatesFB", reinterpret_cast<PFN_xrVoidFunction*>(&enumerate));
    xrGetInstanceProcAddr(g_instance, "xrRequestDisplayRefreshRateFB", reinterpret_cast<PFN_xrVoidFunction*>(&request));
    if (!enumerate || !request) return;
    uint32_t n = 0;
    enumerate(g_session, 0, &n, nullptr);
    std::vector<float> rates(n);
    enumerate(g_session, n, &n, rates.data());
    float best = 0;
    for (float r : rates)
        if (!best || std::fabs(r - float(want)) < std::fabs(best - float(want))) best = r;
    if (best > 0) {
        XrResult r = request(g_session, best);
        LOGR("xr: refresh rate %.0f Hz asked (%s; offered %u)", best, XR_SUCCEEDED(r) ? "ok" : ResultName(r), n);
    }
}

XrSwapchain Chain(int eye) {
    if (eye == kPanel) return g_panel;
    return Stereo() && eye != kTheater ? g_eye_chain[eye & 1] : g_screen;
}

bool AcquireScreen(int eye, uint32_t& index) {
    XrSwapchain chain = Chain(eye);
    if (!chain || (eye == kPanel && g_panel_first == SIZE_MAX)) return false;
    if (!g_frame_begun || XR_FAILED(xrAcquireSwapchainImage(chain, nullptr, &index))) return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(chain, &wi))) {
        xrReleaseSwapchainImage(chain, nullptr);
        return false;
    }
    if (eye == kPanel) index += uint32_t(g_panel_first);
    else if (Stereo()) index += eye == kTheater ? 2 * g_eye_images : uint32_t(eye & 1) * g_eye_images;
    return true;
}

void ReleaseScreen(int eye) { xrReleaseSwapchainImage(Chain(eye), nullptr); }

// the settings panel's quad to the right of a menu at `at` (its centre, upright, its yaw) of half width `half`, at
// the same height, turned to face the head
XrPosef PanelBeside(const XrPosef& at, float half, float panel_w) {
    const XrQuaternionf& q = at.orientation;
    float rx = 1 - 2 * (q.y * q.y + q.z * q.z), rz = 2 * (q.x * q.z - q.w * q.y);  // the menu's +x
    float l = std::sqrt(rx * rx + rz * rz);
    if (l < 1e-3f) rx = 1, rz = 0, l = 1;
    rx /= l, rz /= l;
    // (dev_panelfront: over the menu itself, for screenshots from a headset that can't turn)
    static const bool front = std::filesystem::exists(app::ExeDir() / "dev_panelfront");
    float d = front ? 0.0f : half + 0.08f + panel_w * 0.5f;
    XrPosef p;
    p.position = {at.position.x + rx * d, at.position.y, at.position.z + rz * d};
    float hx = 0.5f * (g_render_views[0].pose.position.x + g_render_views[1].pose.position.x);
    float hz = 0.5f * (g_render_views[0].pose.position.z + g_render_views[1].pose.position.z);
    float yaw = std::atan2(hx - p.position.x, hz - p.position.z);
    p.orientation = {0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)};
    return p;
}
constexpr float kPanelBesideHud = 1.0f, kPanelBesideScreen = 1.6f;  // its width beside the paused hud, the theater

std::atomic<bool> g_last_theater{true};
bool LastTheater() { return g_last_theater.load(std::memory_order_relaxed); }

void EndFrame(bool drawn, bool theater, bool panel, bool card, bool tip) {
    g_last_theater.store(theater || !Stereo(), std::memory_order_relaxed);
    struct Unlock {
        ~Unlock() {
            if (g_frame_locked) g_session_lock.unlock();
            g_frame_locked = false;
        }
    } unlock;
    if (!g_frame_begun) return;
    g_frame_begun = false;
    // the theater screen goes in front of the head each time play hands over to it (user: after quitting a level the
    // menu was off to the side, where the room's forward was at the start): from play's last views, upright, its
    // yaw; before any play, ahead of the room's origin
    static bool from_play = false;
    static XrPosef screen_at{{0, 0, 0, 1}, {0, 0, -kScreenDistance}};
    if (Stereo() && !theater) {
        from_play = true;
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
        // the settings panel beside the paused game's menu: the hud plane's place in the room (vulkan.cpp HudMatrix:
        // 1.5 m ahead of the head's level forward as the pause came, 1.02 m each side), the panel to its right
        static bool placed = false;
        static XrPosef at{};
        if (!panel) placed = false;
        if (panel && !placed && g_views_valid) {
            placed = true;
            const XrPosef& h = g_render_views[0].pose;
            const XrQuaternionf& q = h.orientation;
            float fx = -2 * (q.x * q.z + q.w * q.y), fz = -(1 - 2 * (q.x * q.x + q.y * q.y));  // -z rotated
            float yaw = std::atan2(-fx, -fz), l = std::sqrt(fx * fx + fz * fz);
            if (l < 1e-3f) fx = 0, fz = -1, l = 1;
            XrPosef hud;
            hud.position = {0.5f * (h.position.x + g_render_views[1].pose.position.x) + fx / l * 1.5f,
                            0.5f * (h.position.y + g_render_views[1].pose.position.y),
                            0.5f * (h.position.z + g_render_views[1].pose.position.z) + fz / l * 1.5f};
            hud.orientation = {0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)};
            at = PanelBeside(hud, 1.02f, kPanelBesideHud);
        }
        XrCompositionLayerQuad over{XR_TYPE_COMPOSITION_LAYER_QUAD};
        over.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // imgui over clear: premultiplied
        over.space = g_local;
        over.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        over.subImage.swapchain = g_panel;
        over.subImage.imageRect = {{0, 0}, {int32_t(kScreenW), int32_t(kScreenH)}};
        over.pose = at;
        over.size = {kPanelBesideHud, kPanelBesideHud * kScreenH / kScreenW};
        // the test checklist's card over the back of the left wrist, a little up, and the hint just over the off hand:
        // each turned to face the head
        g_card_size = {};
        XrPosef tip_pose{{0, 0, 0, 1}, {0, 0, 0}};
        XrExtent2Df tip_size{};
        if ((card || tip) && !panel) {
            Controls c;
            {
                std::lock_guard lock(g_controls_lock);
                c = g_controls;
            }
            if (c.hand_ok[0] && c.aim_ok[0] && c.head_ok) {
                XrQuaternionf aq{c.aim[0][0], c.aim[0][1], c.aim[0][2], c.aim[0][3]};
                auto facing = [&](XrVector3f off, float up) {
                    XrVector3f o = Rotate(aq, off);
                    XrVector3f p{c.hand[0][0] + o.x, c.hand[0][1] + o.y + up, c.hand[0][2] + o.z};
                    float dx = c.head[0] - p.x, dy = c.head[1] - p.y, dz = c.head[2] - p.z;
                    float yaw = std::atan2(dx, dz), tilt = -std::atan2(dy, std::sqrt(dx * dx + dz * dz));
                    XrQuaternionf qy{0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)};
                    float sx = std::sin(tilt * 0.5f), cx = std::cos(tilt * 0.5f);
                    // qy * qx (the tilt about the quad's own x, then the turn)
                    return XrPosef{{qy.w * sx, qy.y * cx, -qy.y * sx, qy.w * cx}, p};
                };
                if (card) {
                    g_card_pose = facing({0, 0.02f, 0.07f}, 0.09f);  // toward the wrist from the grip
                    g_card_size = {kCardMetres, kCardMetres * kCardH / kCardW};
                }
                if (tip) {
                    tip_pose = facing({0, 0, -0.04f}, 0.04f);  // ahead of the grip, just over the hand
                    tip_size = {kTipMetres, kTipMetres * kTipH / kCardW};
                }
            }
        }
        XrCompositionLayerQuad wrist{XR_TYPE_COMPOSITION_LAYER_QUAD};
        wrist.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        wrist.space = g_local;
        wrist.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        wrist.subImage.swapchain = g_screen;
        wrist.subImage.imageRect = {{0, 0}, {int32_t(kCardW), int32_t(kCardH)}};
        wrist.pose = g_card_pose;
        wrist.size = g_card_size;
        XrCompositionLayerQuad hint = wrist;
        hint.subImage.imageRect = {{0, int32_t(kTipY)}, {int32_t(kCardW), int32_t(kTipH)}};
        hint.pose = tip_pose;
        hint.size = tip_size;
        const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&proj),
                                                        reinterpret_cast<XrCompositionLayerBaseHeader*>(&over),
                                                        nullptr};
        XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
        ei.displayTime = g_display_time;
        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        ei.layerCount = drawn && g_views_valid ? (panel && placed ? 2 : 1) : 0;
        if (panel && placed) g_ui_pose = over.pose, g_ui_size = over.size;
        if (ei.layerCount == 1 && g_card_size.width > 0)
            layers[ei.layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&wrist);
        if (ei.layerCount >= 1 && !(panel && placed) && tip_size.width > 0)
            layers[ei.layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&hint);
        ei.layers = layers;
        FrameReport(ei.layerCount);
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
    if (from_play && g_views_valid) {
        from_play = false;
        const XrPosef& h = g_render_views[0].pose;
        const XrQuaternionf& q = h.orientation;
        float fx = -2 * (q.x * q.z + q.w * q.y), fz = -(1 - 2 * (q.x * q.x + q.y * q.y));  // -z rotated
        float l = std::sqrt(fx * fx + fz * fz);
        if (l < 1e-3f) fx = 0, fz = -1, l = 1;
        float yaw = std::atan2(-fx, -fz);
        screen_at.position = {0.5f * (h.position.x + g_render_views[1].pose.position.x) + fx / l * kScreenDistance,
                              0.5f * (h.position.y + g_render_views[1].pose.position.y),
                              0.5f * (h.position.z + g_render_views[1].pose.position.z) + fz / l * kScreenDistance};
        screen_at.orientation = {0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)};
        LOGR("xr: the theater screen placed ahead of the head (yaw %.0f deg)", yaw * 57.3f);
    }
    quad.pose = screen_at;
    quad.size = {kScreenMetres, kScreenMetres * kScreenH / kScreenW};
    g_ui_pose = quad.pose, g_ui_size = quad.size;
    // the settings panel beside the title's menus: to the right of the theater screen
    XrCompositionLayerQuad side = quad;
    side.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    side.subImage.swapchain = g_panel;
    side.size = {kPanelBesideScreen, kPanelBesideScreen * kScreenH / kScreenW};
    if (panel && g_views_valid) {
        side.pose = PanelBeside(screen_at, kScreenMetres * 0.5f, kPanelBesideScreen);
        g_ui_pose = side.pose, g_ui_size = side.size;
    }
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad),
                                                    reinterpret_cast<XrCompositionLayerBaseHeader*>(&side)};
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = g_display_time;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = drawn ? (panel && g_views_valid ? 2 : 1) : 0;
    ei.layers = layers;
    FrameReport(ei.layerCount);
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
    // the settings panel (blackquest's cheats and vr settings) comes with the game's own menus, beside them (user:
    // no button combo; vr_body.cpp), and the game keeps its input
    if (c.menu && !menu_was) {
        menu_down = now;
        back_sent = false;
    }
    if (c.menu && !back_sent && now - menu_down >= std::chrono::milliseconds(500)) back_sent = true;
    if (c.menu && back_sent) back = true;
    // a tap: START held for a few frames after the release, so the game's poll sees it
    if (!c.menu && menu_was && !back_sent) start_until = now + std::chrono::milliseconds(100);
    if (now < start_until) start = true;
    {
        // dev_pause: the menu button's tap (START) once, 40 s after the first poll (the pause menu without the
        // controllers)
        static const bool dev_pause = std::filesystem::exists(app::ExeDir() / "dev_pause");
        static const auto first = now;
        static bool tapped = false;
        static std::chrono::steady_clock::time_point dev_until{};
        if (dev_pause && !tapped && now - first > std::chrono::seconds(40)) {
            tapped = true;
            dev_until = now + std::chrono::milliseconds(150);
            LOGR("xr: dev_pause: START");
        }
        if (now < dev_until) p.buttons |= 1 << 4;  // (here: without live controllers the rest isn't reached)
    }
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
                          uint8_t(c.y ? 255 : 0), analog(c.grip[1]),
                          analog(g_left_grip_taken.load(std::memory_order_relaxed) ? 0.0f : c.grip[0]),
                          analog(g_left_trigger_taken.load(std::memory_order_relaxed) ? 0.0f : c.trigger[0]),
                          analog(c.trigger[1])};
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
