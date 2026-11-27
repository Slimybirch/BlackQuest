#pragma once
// BlackQuest: the headset through OpenXR (xr.cpp). Phase 1, "theater mode": the game's finished 2D frame on a
// quad in front of the player, Touch controllers as the game's pad. Without an OpenXR runtime (a phone) Init fails
// and the flat Android path runs as before.
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace input {
struct Pad;
}

namespace xr {

// render thread, before Vulkan: loader, instance, system. False: no headset, stay flat
bool Init();
bool Active();

// Vulkan made through the runtime (XR_KHR_vulkan_enable2): it adds the extensions it needs and picks the GPU
bool CreateVkInstance(const VkInstanceCreateInfo& ci, VkInstance& out);
bool PhysicalDevice(VkInstance instance, VkPhysicalDevice& out);
bool CreateVkDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo& ci, VkDevice& out);

// once the device and its queue exist: session, input, the virtual screen's swapchain
bool CreateSession(VkInstance instance, VkPhysicalDevice pd, VkDevice dev, uint32_t queue_family);

// the virtual screen's swapchain: its images, the format to make their views with (raw bytes, the game's colours
// are already gamma encoded), its size. in stereo the eyes' swapchains instead: the left eye's images, then the
// right's, at one eye's size
const std::vector<VkImage>& ScreenImages();
VkFormat ScreenViewFormat();
VkExtent2D ScreenExtent();
bool Stereo();
// the head's height in local space (metres), any thread; false before the first located view or without stereo
bool HeadHeight(float& y);
// the head's look in local space (radians): yaw about +y, left positive; pitch, up positive
bool HeadAngles(float& yaw, float& pitch);
// the hands' grips and the head's middle in local space (metres, metres a second), from the last frame's sync
struct Hand {
    bool ok;
    float pos[3];
    float vel[3];
    bool aim_ok;     // the aim pose's orientation (x y z w): -z where the controller points, +y its top
    float aim[4];
    float grip;      // the grip button, 0..1
    float trigger;   // 0..1
    bool thumb_touch;    // the thumb on the rest, the stick or a face button
    bool trigger_touch;  // the index finger on the trigger
    bool primary;        // A (right) or X (left)
};
bool Hands(Hand& left, Hand& right, float head[3]);
// dev (dev_reload's script, game thread): hands Hands gives in place of the controllers' (positions, aims, buttons);
// null: the controllers again
void SetDevHands(const Hand* left, const Hand* right);
// the view's raise over the headset's own height (user: 15 cm was too much, 5 cm); shared by the renderer and the
// gun in the hand, which must agree on where the room is
constexpr float kEyeRaise = 0.05f;
// the paused game's menu (user 2026-11-26): a window fixed in the room ahead of the head as the pause comes up,
// whichever way the game has turned; 2.5 ft wide 5 ft away (user: half the 5 ft at 2.5 ft, twice as far), then 30%
// bigger (user 2026-11-26)
constexpr float kMenuDistance = 1.52f, kMenuHalfWidth = 0.381f * 1.3f;
// the settings panel beside it stays where the 5 ft menu at 2.5 ft had it (user: the panel is fine)
constexpr float kPanelMenuDistance = 0.76f, kPanelMenuHalfWidth = 0.762f;
// game thread: the game's rumble on the controllers (left motor the left hand, right the right), 0..65535
void Rumble(uint16_t left, uint16_t right);
// any thread: a short buzz on a hand (0 left, 1 right), over the game's rumble: a shot's kick
void Pulse(int hand, float amplitude, float seconds);
// game thread: the left grip holds the gun (the left hand is at it or on it): kept from the game's white button
void TakeLeftGrip(bool taken);
// game thread: the left trigger pinches a shotgun's shell (at the chest, or one held): kept from the game's grenade
void TakeLeftTrigger(bool taken);
// which an image of ScreenImages is: eye 0 or 1, or the theater screen; and its size
constexpr int kTheater = 2;
constexpr int kPanel = 3;  // the settings panel's own swapchain (beside the game's menus)
bool LastTheater();  // the last frame went to the theater screen (menus, films, loading), not the eyes
int ImageEye(uint32_t index);
VkExtent2D ImageExtent(uint32_t index);

// an eye as the frame being drawn sees it, in the local space (openxr: metres, y up, -z ahead): position,
// orientation (x y z w), tangents of the fov's left right up down angles (left and down negative)
struct EyeView {
    float pos[3];
    float quat[4];
    float tan[4];
};
// render thread; false before the headset is first tracked
bool RenderEye(int eye, EyeView& out);

// render thread, each present, in this order. BeginFrame polls the runtime's events, waits for the headset's next
// frame (this paces the game at the display's rate) and says whether to draw one. AcquireScreen gives the image
// to draw (eye 0 or 1, or kTheater); after the draw's submit, ReleaseScreen; EndFrame always, with the screen shown
// when it was drawn: the theater screen, or in stereo both eyes
bool BeginFrame();
bool AcquireScreen(int eye, uint32_t& index);
void ReleaseScreen(int eye);
void EndFrame(bool drawn, bool theater = true, bool panel = false, bool card = false, bool tip = false);
// the test checklist's card (vr_totest.cpp): imgui drawn into the screen swapchain's top left kCardW x kCardH, shown
// (card) on a small quad over the left wrist facing the head, when the settings panel isn't open; a hint (tip: the
// launchers' "grip over your left shoulder") the strip under it, kTipH high from kTipY, on a smaller quad just over
// the off hand
constexpr uint32_t kCardW = 768, kCardH = 432, kTipY = 448, kTipH = 112;
// render thread: where the right controller's ray meets the card (0..1 across and down); false: off it or not shown
bool CardPointer(float& u, float& v);
// the settings panel over play (stereo): the screen's swapchain on a quad 1 m ahead of where the head looked when it
// opened, alpha blended over the eyes
// render thread: the touch controllers for the settings panel's navigation (left stick, A), and the right
// controller's pointer: where its ray meets the quad imgui was last shown on (0..1 across and down; false: off it)
// and its trigger. false when not live
struct MenuInput {
    float stick[2];
    bool a;
    bool pointing;
    float u, v;
    bool trigger;
};
bool MenuControls(MenuInput& in);
// the game's own menus by a laser (user 2026-11-26, as the settings panel's): the paused menu's plane in play (its
// centre, its level forward x z, its half sizes; null: none), set by the renderer each frame it's up
void SetMenuPlane(const float* centre, const float* fwd, float half_w, float half_h);
// where the right aim ray meets the game's menu (the theater screen, or the paused menu's plane): 0..1 across and
// down; theater: the 16:9 screen (the game's frame inside it), else the frame itself. false when it points at the
// settings panel, or at neither
bool GameMenuPointer(float& u, float& v, bool& theater, bool& trigger);
// the laser's ends (local space) while a menu is up and the ray meets the game's menu plane or the settings panel
bool PointerBeam(float* from, float* to);

// the app quitting (sdl's thread): the session and instance handed back to the runtime first
void Shutdown();

// game thread, inside input::Poll: the Touch controllers onto port 0's pad
void ApplyPad(int port, input::Pad& pad);

}  // namespace xr
