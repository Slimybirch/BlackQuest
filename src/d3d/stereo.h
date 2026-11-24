#pragma once
// blackquest: the headset's two eyes for the render thread. render_thread.cpp replays each batch once per eye;
// native/vulkan.cpp keeps a set of render targets per eye and the eye's matrix for world draws
namespace stereo {

// 2 while drawing both eyes for the headset, else 1 (flat, or theater mode)
int Eyes();
// render thread: the eye the commands from here on draw for (2: the scope's view, kScopeEye)
void BeginEye(int eye);
constexpr int kScopeEye = 2;
// render thread, each frame's right-eye replay: a scoped gun held near the eye, the frame replayed a third time from
// its scope (vr_gun.cpp VrScopeWanted)
bool Scope();

}  // namespace stereo
