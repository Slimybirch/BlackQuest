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
// are already gamma encoded), its size
const std::vector<VkImage>& ScreenImages();
VkFormat ScreenViewFormat();
VkExtent2D ScreenExtent();

// render thread, each present, in this order. BeginFrame polls the runtime's events, waits for the headset's next
// frame (this paces the game at the display's rate) and says whether to draw one. AcquireScreen gives the image
// to draw; after the draw's submit, ReleaseScreen; EndFrame always, with the screen shown when it was drawn
bool BeginFrame();
bool AcquireScreen(uint32_t& index);
void ReleaseScreen();
void EndFrame(bool drawn);

// the app quitting (sdl's thread): the session and instance handed back to the runtime first
void Shutdown();

// game thread, inside input::Poll: the Touch controllers onto port 0's pad
void ApplyPad(int port, input::Pad& pad);

}  // namespace xr
