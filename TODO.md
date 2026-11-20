# TODO

## Phase 0
- [x] install the Android toolchain (SDK 34, NDK 28.2.13676358, CMake 3.31.6, JDK 17) → `toolchain/`
- [x] baseline build with `build.bat`, install on the Quest 3 — runs flat (NOTES, "Baseline on the Quest 3")
- [x] baseline play: "played really well, no framerate drops" (user, 3 min session)
- [ ] pause/resume on headset removal, flat build (user, when convenient)
- [x] package rename to `com.slimybirch.blackquest` + app name
- [x] UpdateChecker → `Slimybirch/BlackQuest` releases, message says to update through the launcher
- [x] europe.patch out of the repo, fetched at build time
- [x] first commit 9a79952 (2026-11-19)

## Phase 1
- [x] first headset run of the OpenXR build: session, screen, colours, Touch mapping (user: all good)
- [x] exit: events read while paused, session destroyed on quit (checked with Home on the headset)
- [ ] headset off/on resume (untested: proximity sensor off in dev mode)
- [x] commit (user go 2026-11-19)
- [ ] Android dialogs (storage, update) → virtual screen (Phase 7); `appops` for now

## Later
- [ ] touch overlay hidden under XR (done in RenderUi); "PRESS ANYWHERE" text is the touch port's (Phase 7)
- [ ] xbe check: normalise the dump tool's region/media cert patch before hashing (game_files.cpp, Builder)
- [ ] find the fire ray builder (Phase 4) and the grenade spawn (Phase 6)
- [ ] `EnsureBackend`: stop cleanly when the renderer can't start instead of crashing in vkCreateImage

## Phase 2 (design in NOTES, "Phase 2 (true stereo): design"; double replay chosen by the user)
- [x] projection layer + per-eye swapchains (1680x1760), views a frame ahead
- [x] per-eye target sets, each batch replayed for the right eye before it's marked done (deferred while nobody waits)
- [x] per-eye VS matrix `VP·P_eye·V_eye·P_game⁻¹·VP⁻¹` in the VS epilogue, draw-list draws only (`d3d::SceneDraws`)
- [x] frames without world draws on the theater screen
- [x] user, first level test: depth good, 72 fps, menus float; gun off to the left, world culled off the aim,
      hud and effects doubled, view a little low
- [x] effects through the scene's projection stereo; gun hung off the head; hud on a head-locked plane; 160° cull
      fov; view raised 0.15 m
- [x] view follows real height, -10 cm default; real crouch holds the crouch button (vr_body.cpp)
- [x] user: second level test — all good but the flash and far cars
- [x] far cars: the distance view tan kept at the hip view's (ViewTan 0x2034CE / 0x203918); flash moved onto the barrel (headset checklist in the 2026-11-19 second-build notes)
- [ ] if effects still double: the log's `flat z` count (cpu-projected effects) → reproject pretransformed draws
- [ ] culling past ~±70° off the aim: the camera follows the head (Phase 3 decoupled view)
- [ ] hud as a real OpenXR quad layer (option), post passes checked per eye
- [ ] world scale (units per metre), render scale option
