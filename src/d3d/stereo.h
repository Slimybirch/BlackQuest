#pragma once
// blackquest: the headset's two eyes for the render thread. render_thread.cpp replays each batch once per eye;
// native/vulkan.cpp keeps a set of render targets per eye and the eye's matrix for world draws
namespace stereo {

// 2 while drawing both eyes for the headset, else 1 (flat, or theater mode)
int Eyes();
// render thread: the eye the commands from here on draw for
void BeginEye(int eye);

}  // namespace stereo
