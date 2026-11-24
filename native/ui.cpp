#include <string>
#include <mutex>
#include <jni.h>
// ImGui's input on Android: SDL's touches, mouse and keys into ImGui's IO (main thread, under app::UiLock), and the
// first controller for the settings panel's navigation, read per frame on the render thread
#include <SDL3/SDL.h>

#include <cfloat>
#include <chrono>

#include <imgui.h>

#include "common/menu.h"
#include "host.h"
#include "xr.h"

namespace ui {

namespace {

ImGuiKey KeyOf(SDL_Scancode s) {
    switch (s) {
        case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
        case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
        case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
        case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
        case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
        case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
        case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
        case SDL_SCANCODE_HOME: return ImGuiKey_Home;
        case SDL_SCANCODE_END: return ImGuiKey_End;
        case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
        case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
        case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
        case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
        case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
        case SDL_SCANCODE_ESCAPE: return ImGuiKey_Escape;
        case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
        case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
        case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
        case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
        case SDL_SCANCODE_LALT: return ImGuiKey_LeftAlt;
        case SDL_SCANCODE_RALT: return ImGuiKey_RightAlt;
        default: return ImGuiKey_None;
    }
}

int MouseButton(uint8_t b) {
    switch (b) {
        case SDL_BUTTON_LEFT: return 0;
        case SDL_BUTTON_RIGHT: return 1;
        case SDL_BUTTON_MIDDLE: return 2;
        default: return -1;
    }
}

SDL_FingerID g_finger = 0;  // the touch that is ImGui's mouse
bool g_fingered = false;

}  // namespace

void OnEvent(const SDL_Event& e) {
    if (!ImGui::GetCurrentContext()) return;
    ImGuiIO& io = ImGui::GetIO();
    int w, h;
    host::WindowPixels(w, h);
    switch (e.type) {
        case SDL_EVENT_FINGER_DOWN:
            if (g_fingered) break;
            g_finger = e.tfinger.fingerID;
            g_fingered = true;
            io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
            io.AddMousePosEvent(e.tfinger.x * float(w), e.tfinger.y * float(h));
            io.AddMouseButtonEvent(0, true);
            break;
        case SDL_EVENT_FINGER_MOTION:
            if (!g_fingered || e.tfinger.fingerID != g_finger) break;
            io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
            io.AddMousePosEvent(e.tfinger.x * float(w), e.tfinger.y * float(h));
            break;
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED:
            if (!g_fingered || e.tfinger.fingerID != g_finger) break;
            g_fingered = false;
            io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
            io.AddMouseButtonEvent(0, false);
            // a lifted finger hovers nothing
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (e.motion.which == SDL_TOUCH_MOUSEID) break;
            io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
            io.AddMousePosEvent(e.motion.x, e.motion.y);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.which == SDL_TOUCH_MOUSEID) break;
            if (int b = MouseButton(e.button.button); b >= 0) {
                io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
                io.AddMouseButtonEvent(b, e.button.down);
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            io.AddMouseWheelEvent(e.wheel.x, e.wheel.y);
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            io.AddKeyEvent(ImGuiMod_Ctrl, (e.key.mod & SDL_KMOD_CTRL) != 0);
            io.AddKeyEvent(ImGuiMod_Shift, (e.key.mod & SDL_KMOD_SHIFT) != 0);
            io.AddKeyEvent(ImGuiMod_Alt, (e.key.mod & SDL_KMOD_ALT) != 0);
            if (ImGuiKey k = KeyOf(e.key.scancode); k != ImGuiKey_None) io.AddKeyEvent(k, e.key.down);
            break;
        case SDL_EVENT_TEXT_INPUT:
            io.AddInputCharactersUTF8(e.text.text);
            break;
        default:
            break;
    }
}

void NewFrame(float width, float height) {
    ImGuiIO& io = ImGui::GetIO();
    using clock = std::chrono::steady_clock;
    static clock::time_point last = clock::now();
    auto now = clock::now();
    float dt = std::chrono::duration<float>(now - last).count();
    last = now;
    io.DeltaTime = dt > 0 ? dt : 1.0f / 60;
    io.DisplaySize = ImVec2(width, height);
    io.DisplayFramebufferScale = ImVec2(1, 1);

    // the first controller steps through the panel
    io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    SDL_Gamepad* pad = count ? SDL_GetGamepadFromID(ids[0]) : nullptr;
    SDL_free(ids);
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;  // without it imgui ignores every pad key for moving
    xr::MenuInput in{};
    if (xr::Active() && xr::MenuControls(in)) {
        // the headset: the right controller points and its trigger clicks; the left stick moves and steps, A presses
        // (B closes the panel: xr.cpp). imgui draws the cursor itself: there's no system one in the headset
        // (the panel is beside the game's own menus now: the sticks and A stay theirs, the pointer is the panel's)
        static bool pointed = false, held = false;
        in.pointing = in.pointing && app::MenuOpen();  // no cursor over the game's own screens
        io.MouseDrawCursor = in.pointing;
        if (in.pointing) {
            io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
            io.AddMousePosEvent(in.u * width, in.v * height);
        } else if (pointed) {
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        }
        pointed = in.pointing;
        bool down = in.trigger && (in.pointing || held);  // a press starts on the quad, a release lands anywhere
        if (down != held) io.AddMouseButtonEvent(0, down);
        held = down;
        return;
    }
    if (!pad) return;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    struct Map {
        ImGuiKey key;
        SDL_GamepadButton button;
    };
    constexpr Map kMap[] = {
        {ImGuiKey_GamepadStart, SDL_GAMEPAD_BUTTON_START},       {ImGuiKey_GamepadBack, SDL_GAMEPAD_BUTTON_BACK},
        {ImGuiKey_GamepadFaceDown, SDL_GAMEPAD_BUTTON_SOUTH},    {ImGuiKey_GamepadFaceRight, SDL_GAMEPAD_BUTTON_EAST},
        {ImGuiKey_GamepadFaceLeft, SDL_GAMEPAD_BUTTON_WEST},     {ImGuiKey_GamepadFaceUp, SDL_GAMEPAD_BUTTON_NORTH},
        {ImGuiKey_GamepadDpadLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {ImGuiKey_GamepadDpadRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
        {ImGuiKey_GamepadDpadUp, SDL_GAMEPAD_BUTTON_DPAD_UP},    {ImGuiKey_GamepadDpadDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {ImGuiKey_GamepadL1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},  {ImGuiKey_GamepadR1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    };
    for (const Map& m : kMap) io.AddKeyEvent(m.key, SDL_GetGamepadButton(pad, m.button));
    auto stick = [&](ImGuiKey neg, ImGuiKey pos, SDL_GamepadAxis a) {
        float v = SDL_GetGamepadAxis(pad, a) / 32767.0f;
        io.AddKeyAnalogEvent(neg, v < -0.3f, v < -0.3f ? -v : 0);
        io.AddKeyAnalogEvent(pos, v > 0.3f, v > 0.3f ? v : 0);
    };
    stick(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, SDL_GAMEPAD_AXIS_LEFTX);
    stick(ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, SDL_GAMEPAD_AXIS_LEFTY);
}

}  // namespace ui

// BlackActivity (a Quest): an update's notice, shown on the settings panel's top line (menu.cpp), since an Android
// dialog isn't seen once the game's in the headset
namespace {
std::mutex g_notice_lock;
std::string g_notice;
}  // namespace
namespace app {
std::string UpdateNotice() {
    std::lock_guard lock(g_notice_lock);
    return g_notice;
}
}  // namespace app
extern "C" JNIEXPORT void JNICALL Java_com_slimybirch_blackquest_BlackActivity_nativeUpdateNotice(JNIEnv* env, jclass,
                                                                                         jstring text) {
    const char* c = env->GetStringUTFChars(text, nullptr);
    {
        std::lock_guard lock(g_notice_lock);
        g_notice = c ? c : "";
    }
    if (c) env->ReleaseStringUTFChars(text, c);
}
