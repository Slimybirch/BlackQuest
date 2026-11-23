# BlackQuest notes

Addresses are x86 VAs in the **USA** `default.xbe` (image base 0x10000) unless said otherwise. Each entry says how
it was found. Sources: **bp** = black-pc `NOTES.md` (GPL-2.0, facts only, no code taken), **ba** = black-android
source (MIT, our base), **me** = checked here.

## 2026-11-18 — Phase 0

### Repo layout

- Repo root = black-android tree at `8e9c6fc` (2026-11-01, "fix: GENERAL options crashed the app"), copied without
  its git history: upstream history contains `ci/default.xbe.enc` (encrypted game code), which must never be in our
  git. Also dropped: `.github/workflows/build.yml` and `ci/` (the encrypted-xbe release CI, rule 3).
- `ref/` (gitignored, reading only):
  - `black-android` @ `8e9c6fc`
  - `XboxXBE-Recomp` @ `99078b1` — the commit black-android pins in both CMakeLists (`GIT_TAG 99078b11…`).
    The root and `tools/` CMakeLists now look in `ref/XboxXBE-Recomp` before `../XboxXBE-Recomp` and FetchContent,
    so the build uses this checkout (same commit).
  - `black-pc` @ `c065917` (Release 0.4.0, 2026-11-14) — GPL, read only.
  - `black-seamless-coop` @ `422eeaa` (2026-10-18) — matches the commit the prompt names.
- `build.bat` now takes `BLACK_ISO=<path>` or the first `*.iso` in `assets/`, `iso/`, then the repo root, so
  `Black.iso` stays where it is.

### The disc (me)

- `Black.iso`: XISO, game partition at offset 0 (`MICROSOFT*XBOX*MEDIA` at 0x10000).
- `default.xbe` from it (`tools/xdvdfs.py`): 2,813,952 bytes.
  - SHA-1   `82d0588124e70102ee650f8969c0a56374476234`
  - SHA-256 `42eb02fcd811f57e003e3c144dc2e1ee15445f53d172ed23711573dfbefb3cf3`
  - matches **neither** listed hash (NTSC-U `df2739c3…68f6`, PAL `67f35121…30f0`).
- Why: the certificate was patched by the dump tool. Title `Black` 0x45410083, version 2, xbe time 2006-01-09
  18:22:23, cert time 2006-01-24 05:01:34, entry 0x25A3F, XDK 5849 — all equal to black-pc's NTSC-U report
  (`notes/phase1-xbe-report.txt`). Different: **region 0x7** (was 0x1 NA) and **allowed media 0x400001FF** (was 0x202),
  the usual region-free + any-media patch.
- All 13 section SHA-1 digests in the header match the section data: code and data are untouched.
- Setting cert +0x9C = 0x202 and +0xA0 = 0x1 gives SHA-256 `df2739c3…68f6` exactly → **NTSC-U (USA)**.
  Stock US SHA-1 (for `kImageSha1`): `c5bfed25449c8212d7a976f15246e87ead57713f`.
- Effect today: none for a build from this ISO. CMake hashes whatever xbe is in `assets/` into `kImageSha1`, and
  the app on the headset hashes the xbe from the same ISO, so they match. But an APK built from a stock xbe would
  reject this ISO, and vice versa. TODO (Phase 9 Builder, and `game_files.cpp`): normalise those two cert fields
  before hashing, accept only if the section digests also check out.

### Baseline on the Quest 3 (2026-11-18)

- Native build (Gradle) 4 m 32 s after the recompile; APK 22.3 MB. `adb install -r` ok.
- Storage: `adb shell appops set com.slimybirch.blackquest MANAGE_EXTERNAL_STORAGE allow` works on Horizon OS
  (Android 14, UP1A.231005.007.A1); no in-app dialog needed after that. ISO pushed to `/sdcard/Black/Black.iso`
  (adb push 221 MB/s over USB). Game found it: "574 files", xbe sha1 82d05881… matched.
- Launched by `monkey` with the headset **asleep** (off the head): SDL window 1280x800 @ 72 Hz; activity went
  onResume → onPause → onStop within 0.1 s; `d3d: renderer init failed` with no `vulkan:` error line (so a silent
  early return in `VulkanBackend::Init`, likely surface/size), then **SIGSEGV in `vkCreateImage`** on the render
  thread: `EnsureBackend` (`src/d3d/d3d8.cpp:305`) logs the failed Init and carries on with a dead backend. Base
  port bug; harmless while awake if Init succeeds.
- **Headset worn (user, 2026-11-18): it runs.** Flat window from Unknown Sources; GPU Adreno 740, Vulkan 1.3.295,
  driver 80345009; display reported 1280x800 @ 72 Hz. Session 44 s, quit through the port's exit menu, `Exit (0)`.
  Log line `quit: ConfirmWarning, FEAudio failed` (`src/game/exit_menu.cpp:173`): the quit page's sound clips
  weren't found; the quit still works. Cosmetic, not chased.
- So the asleep crash is only the launch-while-off case. TODO: bail out cleanly (or wait for a surface) when
  `Init` fails instead of crashing — moot once OpenXR owns the session (Phase 1).
- User, 2026-11-18, second session (3 min, played): "played really well on the headset, no framerate drops".
  Log clean apart from the FEAudio quit line; `Exit (0)`.
- **europe.patch out of the repo** (user, 2026-11-18: nothing close to game files shipped). It is black-android's
  PAL→US xbe patch, built from the game's bytes. `src/common/europe.patch` deleted and gitignored; `CMakeLists.txt`
  takes it from `ref/black-android` or downloads it from raw.githubusercontent at black-android `8e9c6fc7…` and
  checks SHA-256 `3cb0f57b…c390`, into the CMake build dir. Fallback: empty patch = US disc only, with a warning.
  README tells builders this. The Builder (Phase 9) will do the same fetch.

### Package rename (user ok 2026-11-18)

- `com.racas.black` → `com.slimybirch.blackquest` (Gradle `namespace`/`applicationId`, Java package dir, JNI
  names `Java_com_slimybirch_blackquest_BlackActivity_*` in `native/input.cpp`). App name "BlackQuest" (strings.xml,
  storage dialogs, FPS watermark, `settings.gradle`). Device data now in
  `Android/data/com.slimybirch.blackquest/files`. Game folder unchanged: `/storage/emulated/0/Black`.
- UpdateChecker → `Slimybirch/BlackQuest` releases; the dialog no longer offers an APK download (none can exist:
  each APK is built from the player's disc) and says to run the PC launcher to rebuild and reinstall.
- Credits: SUPPORT row (black-android author's donation link) removed at the user's request, 2026-11-18; the
  author's credit, YouTube and GitHub rows stay.
- Wireless adb works (the headset was paired already, mDNS `adb-2G0YC5ZG6L0989-…._adb-tls-connect._tcp`):
  `adb install -r` over Wi-Fi keeps the storage appop.

### HD texture pack inventory (me, counted 2026-11-18)

`BLACK_HD_Reimagined_Texture_Pack/` — "BLACK HD Reimagined", PCSX2 replacement pack for the **PS2 USA release
SLUS-21376**.

- `SLUS-21376/replacements/`: 1,324 textures, flat folder, named by PCSX2 hashes
  `<tex hash>-<clut hash>-<tex0 bits>` (e.g. `108d09fad126df42-f541a70889ace26c-00001dd4.dds`).
  - 1,204 DXT5 + 7 DXT1 `.dds` (1,211) and 113 `.png`.
  - sizes: mostly 2048 px (709) and 1024 px (431), a few up to 4096.
  - 4.5 GB. Plus a Windows `desktop.ini` in the folder (ignore).
- `BLACK HD Reimagined.ini` is **not** a texture setting: it is a **ReShade** preset. Techniques, in order:
  Sepia/Tint (strength 0.10, tint 0.55/0.43/0.42), qUINT sharpen (0.294, depth + RMS mask), PD80 Bonus LUT
  (#2, intensity 0.513, chroma 0.245), Vignette (amount -1, radius 2, ratio 2.342), PD80 Cinetools LUT (#7, full),
  BaBa FakeHDR (preset 2, 0.555). Nothing the texture loader needs. A light grade like this could be an optional
  post effect later; not planned.
- Two install PDFs (EN, PT).
- The port runs the **Xbox** code with Xbox textures (swizzled, DXT/palettised), so PS2 hashes will not match:
  Phase 8 matches images visually and re-keys them by our own Xbox hash.

### Toolchain on this PC (me)

- Found: git 2.x, Python 3.11, adb 1.0.41 (34.0.5, `C:\Users\codyp\platform-tools`), VS Build Tools 2026
  (MSVC 14.51, at `S:\Microsoft Visual Studio18BuildTools`; vswhere finds it with VC.Tools.x86.x64, so `build.bat`'s
  check passes), VS Community 2022 (no C++ workload).
- Missing: Android SDK (platform 34), NDK 28.2.13676358, CMake 3.31.6 (SDK package; also gives ninja), JDK 17–21.
  No Clang in VS: `build.bat` falls back to the NDK's clang-cl for the recompiler, fine.
- adb: no device connected at the time.
- Installed (user ok, 2026-11-18) into `toolchain/` (gitignored), portable, nothing system-wide:
  `toolchain/jdk17` = Temurin 17.0.21+9 (Adoptium API zip); `toolchain/android-sdk` = cmdline-tools
  `commandlinetools-win-13114758` + platforms;android-34, build-tools;34.0.0, platform-tools, ndk;28.2.13676358,
  cmake;3.31.6 (SDK licences accepted). `build.bat` prefers these when present. This is also the layout the Phase 9
  Builder can download to.
- Baseline `build.bat` run started 2026-11-18; log in `out/build-baseline.log`.
- **Build fixes for VS Build Tools 2026** (2026-11-18, found by running the recompiler build by hand; `build.bat`
  sends that step's output to nul):
  - MSVC 14.51's STL defines its "doom function" as `__builtin_verbose_trap` under clang; the NDK's clang-cl
    19.0.1 doesn't have it → 7 errors in `xmemory`. `tools/CMakeLists.txt` defines
    `_MSVC_STL_USE_ABORT_AS_DOOM_FUNCTION` (the STL's own switch, `__msvc_doom_core.hpp`).
  - **Not enough**: MSVC 14.51's `yvals_core.h` refuses any clang below 20 (STL1000, "expected Clang 20 or newer")
    and its `type_traits` uses clang-20 builtins. The NDK's clang-cl is 19.0.1. MSVC `cl.exe` can't build the
    recompiler either (the SDK's `tools/recomp/*.cpp` use `__attribute__`). Needs a Clang ≥ 20 for Windows: VS
    "C++ Clang tools" component (build.bat already prefers `%VSDIR%\VC\Tools\Llvm`) or LLVM 20.1.8 portable.
    The abort() define stays: harmless and still needed with a newer STL under clang.
  - **Fix**: LLVM 20.1.8 portable (`clang+llvm-20.1.8-x86_64-pc-windows-msvc.tar.xz`, 940 MB, llvm/llvm-project
    GitHub release; user ok 2026-11-18) in `toolchain/llvm`; `build.bat` prefers it. Recompiler builds; recompile
    of the xbe: 15,042 functions, 680,140 instructions, 457 jump tables, 64 partitions (`generated/game`, 68 files).
    Logs a few hundred "unimplemented" rare opcodes (aas, les, iretd…): data decoded as code in gap fill, expected.
  - `NoDefaultCurrentDirectoryInExePath=1` in this environment: cmd doesn't run `gradlew.bat` from the current
    folder by bare name ("not recognized"). `build.bat` now calls `"%~dp0gradlew.bat"`.
  - Git Bash rewrites `/sdcard/...` into `C:/Program Files/Git/sdcard/...` for adb: use `MSYS_NO_PATHCONV=1`.
  - Its `VsDevCmd.bat` calls `vswhere.exe` by name: `build.bat` puts the VS Installer folder on PATH first.
  - Rewriting `build.bat` with Python's text mode turned it into LF; `.gitattributes` wants CRLF for `*.bat`, and
    cmd's `goto` labels need it. Edit it with a CRLF-preserving tool.

### VR plan — where things are

**Camera, view, projection**
- Game code reads D3D device fields directly: transforms at device +0xC60 + State·0x40, Xbox D3DTS order VIEW,
  PROJECTION, TEXTURE0-3, WORLD0-3; viewport {X,Y,W,H,MinZ,MaxZ} at +0xEE0 (bp). In ba the device copy is written
  by `D3DDevice_SetTransform` (`src/d3d/d3d8.cpp:725`, `XDK_CDevice_Transforms`), also into
  `g_state.transforms[]` that the render thread copies (`render_thread.cpp:87-90`).
- **World shaders get a combined WVP in VS constants**: FUN_0016F770 copies VIEW/PROJECTION/viewport into the
  renderer context (`*0x2D67F4` +0x70/+0xB0/+0x170), multiplies (FUN_00022424) and uploads WVP with
  SetVertexShaderConstant4 at slot 10 (= c-86) (bp). In ba, the four VS-constant HLE entry points
  (`d3d8.cpp:899-927`) write `g_state.vsc[]`. → Phase 2 per-eye correction goes in clip space on draws using the
  main camera: `P_eye · T_eye · P_game⁻¹`, or rebuild WVP from WORLD·VIEW_eye·PROJ_eye where WORLD is known.
- First-person view matrix: player object (ebx in FUN_001E5FA0, also `*0x2D45D4 + 0x30`) builds it in FUN_001DFD90
  (only caller 0x1E5FE1): yaw `[[obj+0x324]+0x18]` deg, pitch `+0x1C`, smoothed position +0xA0, eye height +0x104 =
  y + [obj+0x2E0] - 0.2 (store at 0x1DFFEC). Rows at +0xD0 **left** (right-handed), +0xE0 up, +0xF0 forward,
  +0x100 eye (bp). ba uses the same offsets (`hooks.cpp` kPlayerForward 0xF0, kPlayerEye 0x100).
- Camera manager `*0x2D12B8` runs 14 controllers in FUN_002033A0; FUN_002007D0 copies the player matrix into its
  quaternion +0x760 / position +0x770, built back into +0x7E0..+0x810 (bp). FOV in degrees at camera +0x750
  (horizontal, near 1.2, aspect +0x74) (bp). ba hooks the FOV at 0x1BB2D6 (`FieldOfView`) and zoom at 0x1BB2BA.
  → Phase 3 head pose: likely inject after FUN_001DFD90 / at FUN_002007D0 so body yaw drives movement and the head
  pose only the rendered view. Co-op's spectate also writes these rows (player +0xD0) — Phase 11 overlap.
- Gun is placed from the view matrix (0x14DE0 at 0x1E6150), pulled out of walls by line test FUN_001DAFF0 (bp).

**Viewmodel (gun + arms)**
- Draw list 5 (`kGunDrawList`, `src/game/layout.h:46`) = first-person models, drawn through FUN_00176F60 (ba hooks it
  in `hooks.cpp:209`: `d3d::ViewScale(g_gun)` around the list). bp: list at renderer `*0x2D67F4 + 0xCAE4`, drawn by
  FUN_001EE820 at 0x1EE97C (or 0x1EEB2B after the effects pass when `[[0x2D1990]+0x830D4] == 1`).
- Muzzle flash / locators: ba moves them for the viewmodel FOV — `FlashSprite` 0x16B312, `GunEffect` 0x176C6E,
  `GunEffectStart` 0x162A20, `TracerStart` 0x1E285F, muzzle matrix FUN_001D2860 (tracer callers 0x1DBAAB,
  0x1DC4D4). Same points move effects to the hand in Phase 4.
- Sway FUN_001E5FA0 eases the weapon (+0x4CC/+0x4D0) toward look values (bp); ba scales it at 0x1E6392/0x1E644A.

**Aim and fire**
- Player input FUN_001EC450 (ecx = player controller; binding table `*0x2F3BD4`): look +0xD8/+0xDC, then
  FUN_001BD3F0(vertical, horizontal) integrates turn rates; aim flag +0x45, toggle mode +0x103 (bp).
- Auto-aim pulls the view (FUN_001D1E40, 30 m query FUN_001CDA30 along camera matrix `[ebx+0x8C]+0xD0`) — not bullet
  correction (bp). ba's `AimAssist` hooks 0x1ECE64/0x1DCDAB. Turn it off in VR.
- Fire: weapon state machine FUN_001EEFD0 (state +0xD8, wait +0xDC; ba wraps it at `hooks.cpp:359`); ammo decrement
  at 0x1EFBBB (clip, word +0x18) / 0x1EFBC5 (total +0x14) (bp+ba). Player fire request at player +0x32C (ba).
- Bullets start from the game's own muzzle matrix (ba comment: "the game's own stays where bullets start").
  **Not yet found**: the function that builds the fire ray (origin + direction) for hitscan and projectiles —
  Phase 4 work, start from the shot struct (`bullet +0x70` → shot, shot +4 → weapon) and callers of FUN_001D2860.

**Reload**
- Weapon controller (char +0x29C) state +0xD8: 0 idle, 1 firing, 4 magazine reload (~1.5 s, entered at 0x1EF4E9 /
  0x1BB3AC), 8 refill-to-end (refill is an animation event, handler near 0x1E89BE → FUN_001BAF70), 5/6 shotgun
  shell-by-shell (0x1BB385, flag +0xC7) (bp). Jump table 0x1F0150, byte index 0x1F0188.
- Ammo object at weapon +0xF4 (alt/underbarrel +0xF8): total +0x14, clip +0x18 (ba `hooks.cpp:322-326`).
- Reload blur: post controller `*0x2D1990 + 0x830A0` mode 1, started by FP animation handler FUN_0017B130 for names
  ending `101`/`501` (not `FP_RPG`) and `180` (shotgun loop start); `182` ends it (bp). ba switches it at 0x17B239
  (`ReloadBlur`) / 0x17B31C (`ReloadBlurHold`).

**Grenades**
- Count: char +0x286 (uint16) (ba `cheats.cpp`).
- Character animation event FUN_001E8830 (64-bit hash, then pointer to the character); the throw event takes a
  grenade at 0x1E8934 (ba). **Spawn (position/velocity/fuse) not yet found** — Phase 6: follow from 0x1E8934.
- Grenades are host-synced world objects in co-op (Phase 11).

**Screen-space passes** (bp, mission 2 frame of reference: ~520 scene draws)
1. scene (stereo), 2. depth downsample (640x480 Z24S8 read as A8R8G8B8) into 160x120/80x60, 3. blur ping-pong
160x120↔80x60, 4. **DOF composite** (SRCALPHA into the back-buffer alias), 5. **first-person weapon**, 6. **bloom**
(ONE/SRCALPHA add), 7. **colour grade** (dot(t0, green) × tint, alpha 0.1), 8. **HUD**.
- **Light shafts / lamp cones**: drawn into 160x120 0x02DB0000 with the scene's view-projection, depth-tested
  against a 160x120 depth copy — world-space geometry at low res: needs per-eye matrices too.
- **Reload blur** = post mode 1 (above). **Fades and brightness** = the game's gamma ramp, FUN_001762B0 per frame
  (bp); ba has an `Op::Gamma` on the render thread.
- **HUD**: not Apt; the game's 2D overlay node list walked by FUN_00021260 (from FUN_000DA4B0), pre-transformed
  640x480 `DrawVerticesUP` through the sprite flush 0x2AC1E < 0xD4CA1 (bp). Sniper scope (WpnScope.bin) and hit ring
  (Ring.bin) are Apt. ba: `HudItem` hook at 0x2139D, `DrawOverlay` 0x20F8BF. → separate quad layer.
- **FMV**: film player at game object `*0x2D45F0` + 0x2026C, state +0x1A8 (bp; ba `SkipVideo` 0x20F0DE). XMV
  decoder. → virtual screen.
- **Menus**: Apt (`FEMain.bin`, `PseMenu.bin` for pause); ba `MenuTick` 0x124351 etc. → virtual screen.

**Frame step**
- Fixed world step chosen at start-up (NTSC 1/30 at 0x2F3A38) (bp). ba instead keeps 1/30 and scales timers to the
  real frame time (`FrameTime`, timer hook FUN_000BE3F0, physics `PhysicsTime` 0x1F11CD and many per-step fixes).
  So any display rate already plays at speed in ba; 72 Hz needs no new step constant, but each "per frame" leftover
  must keep using `FrameShare()`.

**Render pipeline risk for stereo (ba)**
- `render_thread.cpp` records calls on the game thread and replays them on a Vulkan thread, reading vertex, texture
  and surface memory **at replay time**. Replaying a frame twice (per eye) means the second replay may see memory the
  game already changed for the next frame. Phase 2 must either replay both eyes in one pass per draw (multiview /
  two passes per command before the fence releases) or snapshot dynamic buffers. Multiview (VK_KHR_multiview,
  Vulkan 1.1 core) with per-eye matrices in a push constant is the cleanest fit.

## 2026-11-18 — Phase 1 (OpenXR bring-up, theater mode)

- Loader: Khronos `org.khronos.openxr:openxr_loader_for_android:1.1.63` (Apache-2.0) from Maven Central, as a
  Gradle prefab package; CMake `find_package(OpenXR CONFIG)` → `OpenXR::openxr_loader`; `libopenxr_loader.so` in
  the APK. Newest on Maven at the time: 1.1.65.
- Manifest: `android.hardware.vr.headtracking` (required, v1), `org.khronos.openxr.permission.OPENXR(_SYSTEM)`,
  `<queries>` for the runtime broker providers and the runtime/API-layer services (Android 11 package visibility),
  `com.oculus.supportedDevices` = quest3|quest3s|quest2|questpro, `com.oculus.vr.focusaware`, launcher intent also
  `com.oculus.intent.category.VR`, orientation `landscape`.
- `native/xr.cpp` / `xr.h`: loader init (XrLoaderInitInfoAndroidKHR with the activity as a global ref), instance
  (XR_KHR_android_create_instance + XR_KHR_vulkan_enable2, API 1.0), HMD system, Vulkan instance/physical
  device/device made through the runtime, session, LOCAL space, one 1920x1080 swapchain for the virtual screen.
  - Colour: swapchain `R8G8B8A8_SRGB` with MUTABLE_FORMAT, views `R8G8B8A8_UNORM`: the game's gamma-encoded bytes go
    in unchanged and the compositor reads them as sRGB (a plain UNORM chain would be read as linear = too bright).
  - The screen is an `XrCompositionLayerQuad`, 3.2 m wide, 2.6 m in front at eye height in LOCAL space (≈63°),
    no projection layer: the runtime's empty dark space is the "room"; it does reprojection and recentring.
  - Frame loop on the Vulkan render thread in `VulkanBackend::Present`: PollEvents → xrWaitFrame (paces the game at
    the display rate) → xrBeginFrame → acquire/wait image → `DrawFrame` into it (same blit/filters/ImGui as the flat
    path; final layout COLOR_ATTACHMENT_OPTIMAL) → Submit → release → xrEndFrame with the quad.
  - Session states: READY → xrBeginSession(PRIMARY_STEREO); STOPPING → xrEndSession; EXITING/LOSS_PENDING →
    SDL_EVENT_QUIT. Not running: no frame wait, 10 ms sleep so the game doesn't spin.
  - Fallback: `xr::Init` fails without a runtime → the flat Android path runs unchanged.
- `native/vulkan.cpp`: `xr_` flag; under XR no window surface, no VK_KHR_swapchain, GPU from
  xrGetVulkanGraphicsDevice2KHR, any graphics queue; `CreateSwapchain` fills `swap_images_/views/fmt/extent` from
  the XR swapchain so `DrawFrame`, ImGui and the filter pipelines are untouched.
- Input (`xr::ApplyPad`, called in `input::Poll` before the sticks are shaped), Touch → Xbox pad:
  sticks → sticks; triggers → LT/RT; A B X Y → A B X Y; right grip → black, left grip → white; stick clicks →
  L3/R3; left menu: tap = START, hold ≥ 0.5 s = BACK. No D-pad on Touch (menus move with the stick). A paired
  Bluetooth pad still works: the further-pushed value wins. No haptics yet (Phase 4).
- Not handled yet: Android dialogs (storage access, update notice) don't show in an immersive app — storage is
  granted by `appops` for now; the update notice needs moving onto the virtual screen (Phase 7).
- Build: 3 m 04 s incremental. Wireless adb dropped ("offline", then no answer) while the headset was charging,
  probably asleep.
- **First XR run (2026-11-18, installed over Wi-Fi, launched by monkey, proximity sensor off by the user):**
  loader found no runtime broker (`Null cursor` for both content providers) and fell back to the system manifest
  `/odm/etc/openxr/1/active_runtime.aarch64.json` — fine, that is Meta's runtime. Runtime "Oculus 209.174.0",
  system "Meta Quest 3", screen swapchain sRGB, 3 images. Session READY → SYNCHRONIZED → VISIBLE → FOCUSED,
  app load 1.23 s to BeginSession. VrApi: `FPS=72/72, Stale=0, Tear=0`, App 1.8 ms, GPU% 0.20, CPU% 0.55,
  1 layer. SDL now reports the display as 4128x2208 @ 90 Hz (the immersive panel; not our frame rate).
  The `XR_ERROR_CALL_ORDER_INVALID` lines in logcat at launch are another process (pid 4409), not ours.
- **User's headset run (2026-11-19):** "all good" — picture, Touch controls, menus, mission 1. Headset removal
  not tested (proximity sensor off in dev mode). Exiting: the first run had to be force closed; the second run quit
  from the game's own Quit → Yes and closed cleanly (`black.log`: `Exit (0)`, no tombstone or ANR in dropbox).
  The `quit: ConfirmWarning, FEAudio failed` line before it is the base port's quit page failing to borrow the
  title menu's click sounds; harmless, the quit still works.
- **Exit fix (me, 2026-11-19).** Cause of the hang, from the code: the runtime's events were only read in
  `xr::BeginFrame`, on the render thread, inside a present. In the background (`SDL_EVENT_WILL_ENTER_BACKGROUND`)
  the game thread parks in `game::WaitWhileUnfocused` before presenting, so nothing read STOPPING/EXITING; and the
  old EXITING handler pushed `SDL_EVENT_QUIT` to SDL's thread, which SDL blocks while the activity is paused
  (`SDL_HINT_ANDROID_BLOCK_ON_PAUSE`, default on). Now:
  - a watch thread polls the runtime's events every 20 ms under `g_session_lock`, which the render thread holds from
    BeginFrame to EndFrame (one display period at most), so STOPPING → xrEndSession happens even while paused;
  - EXITING / LOSS_PENDING / instance loss → destroy session + instance and `kernel::Shutdown(0)` from that thread;
  - the app's own quit (`SDL_EVENT_QUIT`/`TERMINATING` in `Dispatch`) → `xr::Shutdown()` first (500 ms timed lock,
    then destroy session + instance), so the runtime never waits on a dead process;
  - session state changes are logged at report level now (they're in `black.log`).
  Checked on the headset: launch → states 1-5 (FOCUSED); Home → VISIBLE, SYNCHRONIZED, STOPPING, IDLE, then
  Horizon finishes the activity → `xr: app quit, session closed`, process died 0.3 s after
  `IMMERSIVE_APP_EXITED`, no crash/ANR. On Quest, Home closes an immersive app (no background state to resume).
  Runtime is now 210.135.0 (updated overnight).

## 2026-11-19 — Phase 2 (true stereo): design

What the code says (me, reading `render_thread.cpp`, `native/vulkan.cpp`, the SDK's `gpu/shaders.cpp`):
- The title's thread records calls into batches of 64 commands (`Threaded`); the render thread executes a batch,
  then sets `done_` to its id. Every title-side wait (fence, busy resource, read-back, frame) is a wait on `done_`,
  so **guest memory a batch reads stays valid until that batch is marked done**.
- Each `Draw` reads vertex/index/texture memory when it runs (`VulkanBackend::Draw` copies vertices into the upload
  ring then), and the state arrives as byte deltas applied to `state_`.
- Vertex shaders end in the SDK's `kVsEpilogue`: the program's screen-space `oPos` (pixels, depth units, clip w) is
  turned back into Vulkan clip space `q`, then `o.pos = q`. That's the one place a per-eye clip-space matrix goes:
  `o.pos = mul(E[eye], q)`, with `E = F · P_eye · T_eye · P_game⁻¹ · F` (F = diag(1,−1,1,1), the y flip the
  epilogue applies), P_game from `transforms[D3DTS_PROJECTION]` of the draw, P_eye from the OpenXR per-eye FOV
  with the game's near/far. E = identity for draws that aren't the world camera (2D, HUD, post passes, an
  orthographic or missing projection).
- Pixel shaders sample render targets as `Texture2D` (`tex%d.Sample`), so making targets array layers
  (multiview) would mean rewriting generated pixel shaders and every copy/blit/read-back path.

Two ways to get two eyes:
- **A. Multiview** (VK_KHR_multiview): every target a 2-layer image, render passes with viewMask 0b11, the VS
  matrix chosen by `SV_ViewID`, pixel shaders sampling targets as `Texture2DArray` at the view index; copies,
  read-backs, depth copies, shrinks, feedback copies and clears all made layer-aware. One pass, cheapest GPU, but
  it touches the shader generator's output and ~15 backend paths.
- **B. Replay each batch twice** (chosen): the render thread executes a batch for the left eye, keeps it, and
  replays it for the right eye with the right-eye target set and matrices before marking it done, so the right eye
  reads the same guest memory the left did. Targets are looked up through a per-eye map, so render-to-texture
  passes (shadows, DOF, light shafts, bloom) stay per eye with no shader change beyond the VS matrix. Ops with side
  effects outside the GPU run once (read-back `Sync`, `RecordLate`, `Flush`, reports, `Gamma`); `Present` after
  both eyes. To keep eye switches (render pass store/load on the Adreno) down, the right-eye replay is deferred
  while nobody waits: batches pile up with the left eye done, and the right eye catches up when the title waits on
  one of them or at Present — about 1–2 switches per frame instead of one per batch.
- OpenXR side: a projection layer with one 2-layer (or two) swapchain(s) at the recommended size × a scale option;
  the per-eye `XrView` pose/FOV from `xrLocateViews` each frame; the quad screen stays for menus/FMV (Phase 1).
- Order of work: (1) projection layer + per-eye swapchains showing the mono frame in both eyes; (2) per-eye target
  sets and the double replay; (3) the VS matrix with P_game⁻¹ and IPD from the views; (4) pass classification —
  HUD to a quad layer, post passes checked per eye; (5) world scale (units per metre) and render scale option.

## 2026-11-19 — Phase 2, first stereo build (me)

- `adb exec-out screencap -p` on the Quest 3 returns the compositor's output for both eyes (4128x2208, lens
  warped): stereo can be checked over Wi-Fi without wearing the headset. `adb shell input keyevent KEYCODE_ENTER`
  reaches the game's menus (keyboard Enter = confirm), but the main menu remembers its last choice (CREDITS after
  one run), so Enter alone doesn't reliably reach a mission.
- OpenXR: two swapchains at the recommended 1680x1760 per eye (max 8192), projection layer in LOCAL space with
  the views located one frame ahead (draws run before the present that waits for the headset), the quad screen
  kept: `xr::ScreenImages()` = left eye's 3, right eye's 3, screen's 3.
- **glslang trap**: `[[vk::push_constant]] EyePc eye_pc;` (a struct variable) compiles to a member of the `$Global`
  uniform block, so the matrix read zero and every vertex collapsed (black eyes). `[[vk::push_constant]] cbuffer
  EyePc { row_major float4x4 eye_m; };` is a real push constant (checked with the NDK's glslc + spirv-dis).
- **Which draws get the eye matrix**: depth test alone was wrong — menu/title draws (Apt, their own camera)
  went through the device projection of another camera and left the screen. Now only draws made inside the game's
  draw-list function `sub_00176F60` (hooked in `hooks.cpp` already for the viewmodel) count: `d3d::SceneDraws`
  marks them in transform slot 4 (unused by D3D8; sent to the render thread with view/projection), and the draw must
  also be depth tested, not ZFUNC ALWAYS, not pretransformed, with a perspective projection.
- Frames with no marked world draw (menus, films, loading, briefings) go on the theater screen quad as in Phase 1;
  frames with any go to the stereo layer. Menus checked over screencap: on the screen, both eyes, 72/72 fps.
- Logged every 5 s at report level: `stereo: N draws, W world, U unmoved; front WxH; proj ...; eye ...`.
- Debug files in the app folder (`Android/data/com.slimybirch.blackquest/files`), read at start:
  `stereo_off` (theater only), `stereo_nohead` (eye offsets only, no head pose: desk screenshots),
  `stereo_noeye` (both eyes, matrices left identity).
- Not verified yet: a level in stereo (needs the user), the gun (draw list 5 is drawn with `d3d::ViewScale`, which
  changes the viewport constants the eye matrix doesn't know about: may sit wrong until Phase 4), shadow maps (if a
  light's camera is perspective and drawn through a draw list, it would wrongly get the eye matrix), and world
  scale (1 unit = 1 m assumed, `kUnitsPerMetre` in vulkan.cpp).

## 2026-11-19 — Phase 2, second build: pass classification (me, after the user's first level test)

User's level test: depth good, smooth head look, 72 fps, menus float. Problems: gun far off to the left; buildings
and enemies missing when the head turns off the aim (enemies still shoot); hud and effects (dust, bullets,
explosions) doubled, not fused; view a little low.
- **Culling**: the game culls by its own camera, 70° hip FOV widened to ~83 x 51° here (proj 1.132 / 2.116), while
  the eye shows ~104 x 96° anywhere the head points. black-pc NOTES: the FOV at camera +0x750 is horizontal and
  widening it (their --fov 100) widens culling with no gaps. In stereo the FOV hook (`FieldOfView`, 0x1BB2D6) now sets
  160° (about 145° tall on the game's 16:9 view); the drawn view doesn't change since the eye matrix undoes the
  game's projection. Scripted cameras write their own FOV after this, so cutscenes still cull narrow. Head turns past
  ~70° off the aim still cull: the real fix is the Phase 3 decoupled camera following the head. Watch for lower LODs
  if the game picks them by projected size.
- **Effects**: black-pc NOTES name the effects pass FUN_00178C80 (particles FUN_0015B380) and shell casings
  FUN_00173650, outside the draw-list function, so they had no scene mark and kept identity in both eyes. Now a
  draw is world when it is marked (draw lists) **or** uses the exact projection of the last marked world draw of the
  same eye's replay, and is depth tested, not pretransformed, perspective. Light shafts (scene projection, 160x120)
  take the same path. The remembered projection is cleared after a theater frame.
- **Gun**: `SceneDraws` now marks the gun list 2 (world lists 1). The gun's eye matrix uses only each eye's offset
  from the head's middle, not the head's position in the room, so it no longer slides when the player stands away
  from where the session started (LOCAL space origin), the likely cause of "off to the left". In stereo
  `d3d::ViewScale` is left at 1 (g_gun = 1): the viewmodel FOV scale would have widened the gun's view-space place.
- **HUD**: the 2D layer's Begin/End (sub_000D99A0 / sub_000D3740, hud.cpp) at the screen's width, game or Flash, set
  `d3d::Overlay` (transform slot 4, [1]). In a frame after a world frame those draws get `HudMatrix`: the target's
  clip position becomes a point on a head-locked plane 1.5 m ahead, ±30° wide, the screen's 16:9 tall, seen from each
  eye's offset; depth 1e-4 so it's in front. Before, each eye showed it at the same spot of its own asymmetric
  frustum, which doesn't fuse.
- **Height**: the view follows the headset up and down (local space, origin where the session started) plus
  `kEyeRaise` 0.05 m (user: 0.15 was too high, down 10 cm).
- **Real crouch** (`src/game/vr_body.cpp`, user asked for it with the button kept): the game thread's pad filter
  (port 0) reads the head height (`xr::HeadHeight`, from the located views) against the highest seen; 0.35 m under
  it holds the crouch button (left stick click, the default binding, action 12 per black-pc) until back within
  0.25 m. A toggle-crouch game stays down after the release, so 0.6 s later, still down, one 150 ms press stands it.
  While that crouch is the headset's, `game::VrViewLift` = standing stance − stance (player +0x2E0, read 1.65
  standing) is added back to the view, so the view drops by the player's real crouch only. The button alone crouches
  the game's way. Logged every 5 s: `vr body: head, top, stance, stand, crouch real/ours, lift`.
- The stats line (every 5 s) now counts `world (effects)`, `gun`, `hud`, `flat z` (pretransformed, depth tested,
  off the 2D layer: CPU-projected effects would show there) and `unmoved`.
- Checked here: builds, menus/films on the theater screen, no crash. One run reached the first level over adb
  (Enter x4 from the title, Enter x3 over the films): ~20k effect draws, 1440 gun, 8640 hud, 0 `flat z`, 0 unmoved
  per 5 s, cull projection 0.140 / 0.261. Later runs fell back to the title's attract films; the crouch isn't
  tested in play (desk headset).

Headset checklist (second level test):
1. Look around a street: buildings/enemies stay drawn within ~70° of the aim; note where they pop.
2. Dust, bullets, tracers, explosions, muzzle flash, smoke: single and at depth, not doubled.
3. HUD (ammo, health, prompts, pause menu over play): one sharp image, readable, not doubled.
4. The gun: where it sits, size, whether it moves when you step sideways.
5. Height feels right; shadows/light shafts look the same in both eyes.
6. After, pull the log (stats lines) for the `flat z` and `unmoved` counts.

## 2026-11-19 — Phase 2, third build: far cars, muzzle flash (me, after the user's second level test)

User: height right, real crouch works, nothing culled looking away, effects single, HUD sharp, gun in place. Left:
the muzzle flash 4-5 inches left of the barrel; cars far off not drawn (their shadows sometimes were).
- **Far cars**: the view setups store tan(half view) at camera manager `*0x2D12B8 + 0x16A0` (0x2034CE in
  FUN_002033A0, 0x203918 in FUN_00203610; computed fov x 0.5 x pi/180, fptan, read off the recompiled listing). The
  LOD pick multiplies the object's distance by it (FUN_001C8360 at 0x1C8419), as do ~15 other readers (particles,
  character detail, aim assist 0x1DCDBF). With the 160° cull view that value went from ~0.88 to 5.67, so things
  dropped detail and vanished 6x nearer. New hook `ViewTan` at both stores: in stereo, a value over tan 75° (only the
  vr cull view is that wide) is replaced by the hip view's tan, which `FieldOfView` saves before widening. Culling
  still uses the wide view (the setups' frustum comes from the stack copy, not +0x16A0).
- **Muzzle flash**: a sprite on the aim line near the screen centre (black-pc NOTES), made for the flat picture; in
  vr the gun is drawn where it really sits. `FlashSprite` in stereo moves a copy of its matrix 0.115 m right (minus
  the player's left row +0xD0). The game's own matrix stays (bullets start there).
- Checked here: recompiled (hook emitted before the store at 0x2034CE), builds, starts. Not seen in play (user
  trusts the fixes).

## 2026-11-19 — Phase 3, first build: the head drives the look (me)

Phase 2 committed (7256333, user's go-ahead). Design choice for Phase 3 item 1: rather than keep the game's camera
on the body yaw (which would bring back the culling of anything ~70° off it), the **head drives the game's look**
and the right stick turns a body offset. The game's view, culling, aim and walking direction follow the head; the
renderer turns each eye only by what the head did since the frame was simulated. Phase 4 moves the aim to the hand.
- Hook `VrLook` at 0x1E5FE1 (the call to FUN_001DFD90, the player view builder, in FUN_001E5FA0; ebx the character,
  checked == `*0x2D45D4 + 0x30`). Look angles at `[ebx+0x324]` +0x18 yaw / +0x1C pitch, degrees (black-pc NOTES);
  the listing shows the recoil kick `[ebx+0x4C4]` taken off the pitch just before the call and put back after.
  Yaw rows (c,0,-s)(0,1,0)(s,0,c) with forward (sin, 0, cos): yaw grows turning left, the same way as the head's yaw
  about local +y. Written: yaw = body + head yaw, pitch = sign x head pitch. Body = its first offset, then plus
  whatever the game turned since the last write (smooth turning, scripted turns), wrapped to ±180.
- The pitch's sign isn't read off the code: each frame the player's built forward row (+0xF0, y) is compared with
  the head pitch written last time, and the sign flips when they disagree (logged `vr look: ... sign`).
- `d3d::VrLookUsed` puts the head yaw/pitch used into transform slot 4 [2], [3] (flag [4]); `EyeMatrix` replaces the
  eye's rotation and position by Uᵀ R and Uᵀ p, U = Ry(yaw) Rx(pitch): what's left is the head's motion since the
  sim (latency), its roll and the room-scale offset. The gun's offset is turned the same way, so the gun stays at
  the face, pointing where the head does.
- Right stick in play (view built in the last 250 ms): Y dropped (the head pitches); X a snap turn of
  `vr_snap_turn` degrees (black.toml, default 45; 0 = the game's smooth turning), re-armed under a third of the throw.
- Auto-aim (`AimAssist`, 0x1ECE64 / 0x1DCDAB) off in stereo: its pull would turn the body.
- Checked: recompiled and builds. Not seen in play (adb didn't reach a level this time).

## 2026-11-19 — Phase 3, second build: explosions, flash, touch scheme (me, after the user's Phase 3 test)

User: smooth; view, aim, up/down and walking follow the head; far cars back. Explosions lost part of their effect
(first report: a car went up in smoke with no fire; then: the fire shows, not the smoke). Flash ~2 inches right.
- **Explosions**: the change since the build where effects were right was `ViewTan`, which replaced the stored
  tan(half view) (`+0x16A0`) for all ~15 readers. They all scale a distance by it (0x16087D particle counts, 0x17C00D
  effect priority, 0x17DAC3 effect distance pick, 0x1BDF55, 0x1C4DD8, 0x1C5647 character detail, 0x1CCAC7/0x1CCB47,
  0x1DCDBF aim assist), so some effect choice went another way. Not pinned down (adb couldn't hold a level long
  enough to watch one). `ViewTan` is gone; only the LOD pick in FUN_001C8360 is compensated, at its compare 0x1C8427
  (`LodDistance` = the old `DistanceShorter` x hip tan / tan 80°). Cars far off should stay; if the smoke is still
  missing, the effect isn't the tan's.
- Flash: 0.065 m right instead of 0.115.
- **Touch scheme** (user's): in play, right stick up/down = d-pad up/down (firing mode / health pack); right grip = X
  (pick up, use; the grip had been the black button, which the game's controls don't use); right grip closed with
  the hand behind the face's plane, right of the head, higher than 0.35 m under it (head yaw frame) = d-pad right,
  a weapon switch; either hand moving away from the head faster than 2.5 m/s = B (melee), re-armed after 0.6 s.
  Hand grips come from a new OpenXR pose action (`/input/grip/pose`) located in local space with velocities each
  frame (`xr::Hands`).
- User: the melee should not play the game's animation, the arms' own swing is the animation, but still hit as a
  melee. Not done: needs the melee's damage call (weapon state machine FUN_001EEFD0 / the melee animation event) so
  the hit can run without the animation. For now the swing presses B.
- A bug caught here: the snap-turn block cleared the right stick's Y before the controls read it.

## 2026-11-19 — Phase 3, third build: smoke, flash, melee, smooth turn (me, after the user's tests)

User: smoke still missing (fire shows), far cars fine; flash 4 in right at 0.065 m; snap turn works but smooth is
preferred; stick up/down, grip, shoulder switch and melee gesture work ("not too easy or too hard"); melee should
not play the game's animation, the hit lands where you aim.
- **Smoke**: particle setup FUN near 0x16DF00 scales each type's fade distances (+0xC0 near start, +0xC4 far cut,
  +0xC8/+0xCC ramps) by the renderer view's `[[*0x2D67F4+0xD590]+0x50]`. The 30° setup at 0x15ABA0..0x15ABF5 shows
  what that field is: 1 / (near x tan(half view)), the same struct. The 160° vr view made it ~6x smaller, so a near
  start floored by the view's +0x84 could pass the scaled ramp and the near fade went negative: invisible. Hook
  `ParticleView` at 0x16DF4D (xmm1 just loaded, used to 0x16DFFB) divides it by `VrDistance()` (hip tan / tan 80°).
- **Flash**: three offsets gave 4-5 in left, 2 in right, 4 in right: not a fixed sideways error. The gun list has
  its own muzzle mesh on the barrel, so in vr the sprite's axes are scaled by 1e-4 (drawn as nothing).
- **Smooth turn** default: `vr_snap_turn` = 0 (black.toml can set degrees).
- **Melee**: the button sets controller +0x4B on the press edge (0x1ECB3C reads binding slot 32, `[edi+0x80]`);
  FUN_001F0650 (weapon requests: aim +0x45, melee +0x4B, +0x44) passes it to FUN_001F0210. The hit most likely lands
  on the animation's event, so the animation runs; in vr the gun list (draw list 5) isn't drawn for 650 ms after any
  melee press, so the player's own swing is the only one seen and the game's hit still lands where they aim.

## 2026-11-20 — Phase 3 closed (user's retest)

User: explosions perfect (smoke back); melee good, the 650 ms hide is long enough for the arm's own swing to stand in
for the game's; smooth turning as the default. The flash sprite (hidden) wanted back: "I like the game's muzzle
flash", at the barrel's end. The cause of the three earlier offsets, found for Phase 4: the gun list is drawn with
the head's eye (only the eyes' half separation) and the sprite with the room's eye (the head's place in the room +
the 5 cm raise), so the sprite sat off by wherever the player stood. Fixed with the gun in the hand (Phase 4 notes).

## 2026-11-20 — Phase 4, first build: the gun in the hand (me)

- **How**: the game still poses its first person list (gun, arms, muzzle mesh) ahead of its camera. The renderer
  draws that list through one rigid transform K, from the gun's grip in the camera's frame (the anchor) to the
  right controller, seen with the room's eye like the world (vulkan.cpp EyeMatrix, `hand`). Frames: C = the game
  camera's in openxr axes; a local point q is U^T (q + raise) in C, U from the head angles the camera was built from.
  K: v → U^T A (v − anchor) + U^T (hand + raise), A the aim pose's orientation, the grip pose's position.
- **Anchor**: the player's muzzle each frame from FUN_001D2860 (thiscall, ecx the player, out [esp+4]: a locator of
  the model at [player+0x328] times [player+0x420] times the camera rows at player+0xD0; called with CallGuest from
  the gun list's hook), into C with the rows (right, up, forward, eye at +0xD0/+0xE0/+0xF0/+0x100), 0.28 m behind and
  0.07 m under it (one guess for all weapons), followed with a 0.25 s lag (a kick or swing still shows, a weapon
  switch jumps). Passed to the renderer in transform slot 4 [5..8] (`d3d::VrGunUsed`). Log `vr gun: muzzle ...`.
- **Shots**: the player's bullet spawn 0x1DB7B0 (ecx origin; stack gun, dir, range, speed, flags; one call at
  0x1DC0D1, black-seamless-coop's notes) is wrapped: origin = the muzzle moved by K, direction = the game's
  (spread kept) turned by K's rotation. Projectiles (RPG, launcher) not done yet.
- **Flash and effects**: the sprite (FlashSprite 0x16B312), the gun's effects (GunEffect, GunEffectStart) and the
  tracers' muzzle matrix go through the same K (`game::VrGunMap`), so the game's own flash is back, on the barrel.
- **Haptics**: the game's rumble (port 0) to the controllers, left motor left hand, right motor right
  (`/output/haptic`, `xr::Rumble`).
- Unchanged: the view and walking follow the head; melee hides the list for 650 ms; the hud's crosshair is still
  head-locked (meaningless now; to hide).

## 2026-11-20 — Phase 4, second build: handles, the gun's own frame, left arm, shots, flash (me, after the user's test)

User (first build): gun too far back (hand at the action, not the handle; each gun differs); it points along the
controller but shoots where the head looks; the flash in a random place; melee still hides the gun; both arms on
the right controller, wanted apart. Tested here without the user: the headset on the desk, adb screenshots
(`adb shell screencap -p`, the compositor's two eyes) and the dev markers below.
- **The camera rows are renderware's**: det(rows) = +1 and the first row is screen-left (right = at × up). The first
  build read it as right, so every game-thread mapping (flash, effects, tracers, shot) was mirrored sideways: the
  "random" flash. `ToCamera`/`ToWorld` flip it.
- **Shots**: the owner check read the wrong field (the spawn's first argument isn't the weapon: owner 0x0064003B),
  so no shot was ever moved. Now keyed on the spawn's return address 0x1DC0D6 (its one player call, checked in the
  recompiled code); log `vr gun: shot from the hand`.
- **The gun's own frame**: the muzzle matrix from FUN_001D2860 is renderware too (x left, y up, z along the barrel)
  and moves with the gun's animation (raise, kick, the melee swing: seen 31° in a shot). K now maps that frame onto
  the controller, so the game's animation of the gun is taken out: steady in the hand through melee (screenshots), so
  melee no longer hides it in the hand (still hidden head-locked, no controller).
- **Handles per weapon**: `weapons::HeldCode()` (the table code, weapons.cpp); `kGrips` in vr_gun.cpp: muzzle → handle
  along the barrel and under it, and → fore-grip, from real guns at about viewmodel size (pistol 0.13 back, AK 0.45,
  M16 0.50, shotguns 0.55-0.58...). Checked on the tutorial's pistol side on (dev_fakehands_side): the hand on the
  grip. The flash locator sits exactly on the muzzle in the gun's frame (0.081 -0.070 -0.433 against 0.081 -0.071
  -0.436), so it is drawn on the barrel's tip.
- **Left arm**: the gun list is two draws, the gun (5905 indices) and both arms (2927, one mesh: hidden by a dev file
  in turn). On two handed guns the arms draw is drawn twice, each kept by a clip plane (SV_ClipDistance0 from a plane
  pushed after the eye matrix: push constants 80 bytes, `shaderClipDistance` on), the gun hand's part with the gun's K,
  the left arm's with the left controller's K (its rest the fore-grip). The plane stands along the gun from 6 cm
  left of the handle to 5 cm right of the fore-grip. Pistols stay one piece (the left hand cups the right).
- Gun frame to the renderer: transform slot 4 [5..15] (handle, forward, up, fore-grip distance, flags).
- Dev markers (files in the app's folder, testing without the controllers in hand): `dev_autostart` presses A through
  the menus, `dev_autofire` / `dev_automelee` pulse RT / B in play, `dev_fakehands` holds both hands still in front of
  the view (`dev_fakehands_side` turns the gun side on), `dev_twohand` splits any gun's arms. Remove them after.

## 2026-11-20 — VR settings panel (cheats in the headset) (me)

User: the cheats are needed for testing each level; a trimmed version of the planned VR settings, cheats toggleable,
open from the main menu and in play (paused).
- **Open/close**: left grip held + the menu button, anywhere (title, pause, play); while open the menu button or B
  closes it and the game's pad gets nothing (xr::ApplyPad).
- **Where**: in menus (theater frames) imgui already drew over the game's frame on the theater screen; in stereo it
  had drawn into the left eye only. Now imgui never draws into an eye: over play the screen's swapchain is drawn with
  imgui alone over a transparent clear and submitted as a second, alpha blended quad layer (premultiplied: imgui over
  zero), 1.6 m wide, 1.2 m ahead of where the head looked when it opened, upright, facing it (xr::EndFrame `panel`).
- **Navigation**: the left stick moves (up/down) and steps values (left/right), A presses (ui::NewFrame, when no SDL
  controller), through ImGui's gamepad nav.
- **Content**: the base port's cheats panel (CHEATS, titled VR SETTINGS in the headset) with a VR heading on top:
  TURNING smooth / snap 30 / 45 / 90 (`vr_snap_turn`). Cheat toggles work anywhere (unlock missions from the title);
  give-a-weapon and the supplies are greyed outside a level.
- Dev marker `dev_menu`: opens the panel 5 s into play (with dev_autostart) or 20 s after boot (the title).
- Not yet seen in the headset: the headset slept before the build could be installed.

## 2026-11-20 — Phase 4, third build: shot aim, AK grip, pointer, reload blur (me, after the user's test)
User test: shots leave the barrels and hit up close; haptics felt. At range the AK lands left of centre, and
aiming in doesn't shoot where the barrel points. AK hand ~4" too far forward. The panel couldn't be navigated.
- **Shot direction**: was the game's direction mapped by K. The game aims from its camera along the view (spread
  added), but the viewmodel's barrel isn't parallel to the view (toe-in, and different again when aiming in), so K
  turned the shot a constant angle off the drawn barrel: right up close, a growing miss with range. Now the
  direction is taken into C (where the view is −z) and turned by the gun hand's aim orientation (AC), which is
  the drawn barrel's forward: `VrGunAim`. The origin still goes through K (the muzzle as drawn); the spread is
  kept, as the offset from the view.
- **AK grip**: AK1/AK5 back 0.45 → 0.55 m from the muzzle.
- **Reload blur**: the `ReloadBlur` hook returns false while xr is active, whatever the option says.
- **Panel input**: `ImGuiConfigFlags_NavEnableGamepad` was never set (the Android menu was driven by touch), so
  imgui dropped the pad keys. Set now. Added a pointer: the right aim pose's ray is taken into the quad imgui was
  last shown on (the theater screen, or the panel over play; xr.cpp records its pose and size each EndFrame), the
  hit becomes the imgui mouse, the right trigger its button, and imgui draws the cursor (`MouseDrawCursor`), only
  while the menu is open. The aim pose's position is now read as well (`aim_pos`).
- Checked in the headset with fake hands (side on), AK1 given by `dev_give`, trigger held by `dev_firehold`:
  the hand sits on the pistol grip just behind the magazine, and the game's flash bursts from the muzzle. Hip fire
  with the hand straight ahead hit around the view's middle at the far windows.
- New dev markers (app folder): `dev_give` holds a weapon code (`AK1`), given ~4 s into a level and retried till
  it's in hand; `dev_firehold` holds the trigger in play and turns infinite ammo on.

## 2026-11-21 — Phase 4, fourth build: left arm free, recoil, haptics, grip rows (me, after the user's test)
User test: aiming in shoots right, long range better, the menu pointer works. Rifles' hand still off; the left arm
never leaves the gun; the left elbow rises in melee; no recoil at all; no vibration felt.
- **Left arm stuck on rifles**: the split matched the arms by one index count, 2927, the pistol's arms. Logged per
  weapon (`vulkan: gun list draws a mesh of N indices`, with dev_give cycling weapons): arms PST 2927, AK1 2921,
  SHG 2923, M16 2921, SMG 2919; guns 5156 to 6773. Textures move between frames (streamed), so the arms are now
  any gun list mesh of 2880 to 2970 indices (`IsArms`).
- **Left hand on and off the gun**: off, the left arm follows the left controller (as the split meant). A fresh
  left grip press within 15 cm of the fore-grip as drawn takes hold: the left arm is then drawn with the gun hand's
  K, where the game holds it; releasing the grip lets go (`LeftHold`, game thread). While near or holding, the
  left grip is kept from the game's white button (`xr::TakeLeftGrip`). Pistols split too now (all kGrips two
  handed), with the plane along the barrel 2.5 cm left of the grip (the old across-the-gun plane degenerated to
  one across the barrel when the fore-grip sits at the handle).
- **Melee elbow**: the arms are skinned, so the swing animates the left arm whatever K does; its piece is not
  drawn while a swing is on (`VrMeleeHidden`).
- **Recoil**: the game keeps each gun's view kick at player +0x4C4 (degrees, dying away; RecoilDecay's value,
  the no recoil cheat zeroes it). In vr the view is the head's, so it never showed. Now the gun hand turns up about
  the grip by 1.5x the kick and comes back 6 mm a degree (`HandInCamera`); the shot direction uses the unkicked
  hand, since the game's own direction already carries the kick. The strongest kick per gun goes to the log
  (`vr gun: AK1 kicked up to N degrees`) for tuning.
- **Haptics**: a 60 ms buzz on the gun hand each shot, 0.45 plus 0.08 a degree of kick, and on the left hand at 70%
  when it holds the gun. The game's rumble is now resent as 120 ms buzzes every 80 ms instead of one infinite
  duration. In ~50 s of held fire the game sent no rumble at all (`xr: game rumble` never logged): its firing
  rumble may be off or absent; the per-shot buzz doesn't depend on it.
- **Grip rows**: VR SETTINGS has GRIP FORWARD and GRIP HEIGHT (1 cm a step) for the gun in hand, kept in
  `vr_grips.txt` in the app folder, to be baked into kGrips.
- New dev markers: `dev_fakeleft_low` (the left hand by the hip), `dev_lefton` (the left hand held on the gun);
  `dev_give` takes a list (10 s each). Not verified in the headset: its cameras saw too little light ("images too
  dark", 3DoF), every screencap black.

## 2026-11-21 — Own hands: first look (me, after the user's test)
User: vibration felt; rifle grip changed in VR SETTINGS (vr_grips.txt, to bake into kGrips once the headset is
reachable again); moving the gun hand's grip offsets the hands (the arms mesh is rigid to the gun, so moving the
gun in the hand moves the arms with it). Wants the game's arms replaced by our own that look the same, with
articulated elbows, and the hands to work the parts each reload animation uses.
- Diagnostic built (not yet run): for each gun list mesh the vertex shader's inputs, length and constants set, and
  for the arms whether their vertices change frame to frame. VS-skinned (bone matrices in constants, weights and
  indices in the stream) means our hands can be the game's own arms mesh posed by our bones; CPU-skinned means
  reading the mesh's bind pose ourselves.
- **Result: the gun list is skinned on the GPU.** All gun list meshes (guns and arms) share one vertex shader
  (hash 28B8D86D1CECC165, 56 instructions). Stride 36: v0 position float3 +0, v1 weights PBYTE4 +12, v4 bone
  indices (bytes) +16, v2 normal packed +24, v9 uv float2 +28. The arms' vertices don't change (2 of 600 draws), so
  the animation is all in the constants. Bone matrices are 3 rows (4x3) each from **c-36**; a vertex's index byte
  is the register offset (0, 9, 12, ... in steps of 3). The AK's guns also come as bones, so the reload parts
  (magazine, bolt) can be found the same way.
- AK arms (2921 indices, 1465 vertices; model space metres-ish, x right, z forward): index 0 the root (845
  vertices, identity); right arm 9 upper arm (z -0.19), 12 forearm (-0.02), 15 wrist (0.13), 18 hand (0.18), 21-63
  fingers and thumb (0.20-0.31); left arm 66 upper arm (x -0.12, z -0.18), 69 forearm (0.11), 72 hand (0.21),
  90-129 fingers. The view-projection rows sit at c-86..c-83, the camera at c-82..c-79.
- Dumps (`dev_dumpgun`: the program, constants, indices and vertices of each gun list mesh, once each) are game
  data: they stay on the headset and in the scratchpad, never in git.

## 2026-11-21 — Own hands, first build (me)
- **The bone indices are SHORT4** (D3DVSDT 0x45 at +16), not bytes; read as shorts, the skin is the plain palette
  `c[-36 + index]`, 3 rows a bone, model-space vertices (joint gap 0.025 at base c-36, 0.08+ anywhere else).
  The vertex shader (disassembled with a small nv2a reader, scratchpad `vsh.py`): `arl a0.x, v4` then 4 weighted
  `mul/mad c[a0.x-36..-34]`, `dp4` with v0, projection rows c-86..c-83; c-82..c-80 the camera's axes for the
  environment map. Bones output the game's view space G = C with z flipped.
- Rig (AK 2921 and pistol 2927 the same): right 9 upper arm, 12 forearm, 15 forearm twist, 18 hand, 21-63
  fingers; left 66 upper arm, 69 forearm, 72 hand, 75-129 fingers. Pivots (mesh space): right elbow
  (0.128 -0.004 -0.107), wrist (0.116 0.005 0.137); left elbow (-0.128 -0.004 -0.107), wrist (-0.118 -0.015
  0.145). Forearm 0.245 m: life size.
- `VrPoseArms` (vr_gun.cpp), called by the renderer on the arms' draw with a copy of the constants: the draw keeps
  the gun's K, and each posed bone becomes D⁻¹ Δ D B (D: G → C, z flip then K). The gun hand keeps the game's hand
  on the grip; the off hand on the gun likewise, off it moved by its controller's K over the gun's. Shoulders from
  the head's middle (17 cm out, 24 down, 6 back, the head's yaw); upper arm 0.28 m; out of reach the shoulder
  comes to the hand. Elbow by two-bone IK, pole down/out/back; upper arm and forearm bones turned onto the new lines
  about their joints (minimal rotation: no twist yet); hand and fingers moved together.
- Grip points from the game's own hands: each frame the arms are drawn, the palms (S·B·palm, C before K) become
  the rests VrGunPose puts on the controllers (eased, held during a melee swing). kGrips is only the first frame's
  fallback now; the GRIP rows and vr_grips.txt are gone. The clip-plane split is gone.
- Off hand: **toggle grip** (a press near where it holds the gun takes hold, the next press lets go); on a long
  gun, holding aims the barrel from the gun hand toward the off hand (the shot too); pistols are held, not aimed.
- Not yet seen in the headset (it went offline after the build).
- **Mirror**: the arms came out on the wrong sides with the gun 20 cm off. The shader's projection (c-86) has x
  -1.13, the d3d projection transform (what EyeMatrix inverts) +1.13, so G is x left: C = G with x **and** z
  flipped, and the mesh's +x arm (bones 9-63, the one with the twist bone) is the left. With that, the game's
  palms land within 2-8 cm of the old kGrips handles (AK right palm 0.065 -0.224 -0.162 vs handle -0.016 -0.243
  -0.182; pistol 0.183 -0.231 -0.166 vs 0.207 -0.263 -0.247).
- Seen in the headset with fake hands (AK): free left arm ends at the left controller, right arm up to the grip;
  with dev_lefton both hands on the gun (left on the handguard), the barrel aimed from the right hand toward the
  left, the flash at the muzzle. Elbows bend down and out. Not seen: pistols, twist, real controllers.

## 2026-11-21 — Phase 4 closed; rough edges (me, after the user's test)
User: hands, melee, pistols and two-handed aim (any grip angle) all good. Committed Phase 4 (cbccf25, the
user's go-ahead). Then: the free off hand turned 45° clockwise; off-hand grip **hold** by default with a VR
SETTINGS row OFF-HAND GRIP (HOLD / TOGGLE, `vr_grip_toggle` in black.toml).
- Free off hand: AC · Rz(-45°) in VrGunPose for hand 0 off the gun, a roll about the controller's pointing axis
  (clockwise looking down the arm). If the user meant another axis, this is the one constant (kFreeHandRoll).
- Forearm twist: after the minimal rotation, each forearm bone turns about the elbow→wrist line by a share of how
  much more the hand rolls against the forearm than in the game's pose (bone y axes laid flat across the line):
  left arm 12 30%, 15 (the twist bone) 80%; right arm 69 50%.
- Built, not yet seen (the headset was off adb).

## 2026-11-21 — Fingers from the controllers (me)
User asked for open free-hand poses that close naturally on the gun: grip closes middle/ring/little, trigger the
index, thumb by the thumb rest. Looked at RoboHands-UnityXR (no licence file; some models paid third-party; a
Unity/Blender rig) and stewdio/vr-hands (MIT, but WebXR hand tracking with no poses or angles): neither usable, so
the poses are procedural on the game's own finger bones.
- Inputs: new OpenXR actions `thumb_touch` (thumbrest, stick, A/B/X/Y touch) and `trigger_touch`; xr::Hand has
  trigger, thumb_touch, trigger_touch.
- Fingers are found once per arms mesh from its vertices (`FindFingers`): bones sharing vertices are joined, the
  pivot between two is where they share; chains of up to 3 from the hand bone; the thumb is the chain rooted nearest
  the wrist, index to little by distance from the thumb's root (the left hand's finger bones differ between the AK
  and the pistol, so nothing is hard-coded but the ranges).
- `CurlFingers`: each joint turned about the knuckle line (little→index root, oriented so the game's grip reads
  as flexed) from the game's angle to open (6/8/5°) .. closed (the game's own pose when the hand holds the gun, a
  fist 80/95/55° when free). Thumb: down = the game's pose; up = lifted 40° at the root, the joints above eased
  straight. Index rests half closed with the finger on the trigger. Eased (~55 ms).
- `dev_curl` in the app folder: "grip trigger thumb" for both hands. Built; not yet seen.
- User test: the thumb was backwards (down without a touch, up on the rest) and twisted on the mesh. Fixed: the
  lift goes the other way about the knuckle line; a 12° twist about the thumb's own line at its root (clockwise
  down the thumb on the right hand, mirrored on the left: kThumbTwist).
- Gun hand (user's choice): its middle/ring/little fingers close on the gun by a **toggle**: a right grip press
  that isn't the over-the-shoulder switch opens or closes them (closed with each new weapon); the gun never
  leaves the hand, the grip still does use/pick-up. The off hand keeps the OFF-HAND GRIP option.
- User test: gripping must close every finger on the gun whatever is pressed; the free thumb's rest looked broken
  (laid flat). Now a hand that grips (the gun hand by its toggle, the off hand while it holds) has all five fingers
  in the game's pose on the gun; otherwise they follow the controller. A free thumb on a touch comes down 60% of
  the way (kThumbRest), not to the game's flat pose.

## 2026-11-21 — Phase 5 begun: manual reloading (me)
Phase 4 follow-ups committed (3630144, the user's go-ahead). Phase 5 groundwork:
- **Reload start** in the weapon state machine FUN_001EEFD0 (ebp the weapon controller, char +0x29C; recompiled in
  x86_recomp.52.cpp): at 0x1EF4A5 `mov al, [esp+0x28]` reads the reload request (the function's 5th stack
  argument, passed through from FUN_001F0210); skipped while the state is 5/6 (shotgun shells); then, unless
  forced (`[esp+0x18]`, the 1st argument slot reused: controller +0xF0 → +0xC4 == 2), it needs reserve
  (`word [[ctrl+0xFC] + type*2] > 0`, type = `[[ammo+8]+0x64]`) and a clip not full (`word [ammo+0x18] !=
  [[ammo+8]+0x60]`, ammo = ctrl+0xF4); then 0x1EF4E9 sets state 4 and calls FUN_001EEF50. So the reserve is a
  per-ammo-type word array at `[ctrl+0xFC]`, the clip's size `[[ammo+8]+0x60]`.
- **Gate for manual mode**: a hook at 0x1EF4A5 with `on_false = 0x1EF4F8` (the code after the reload start) vetoes
  every reload (button or empty clip) while manual mode is on; the clip is then filled by us when a magazine is
  seated (clip = min(size, clip + reserve), reserve less what went in). Needs the recompile step.
- **Magazine bones**: dev_dumpreload (vr_body taps X every 6 s; the renderer writes each gun list mesh's vertices
  once to dumps/mesh_<n>.bin and every gun list draw's constants for 2.5 s after a tap to dumps/reload.bin), to
  find the bones a reload moves (magazine, bolt, the shells). Built, not yet run.
- User: magazines come from the chest with the **trigger**.
- **Bug (mine)**: dev_firehold set `Opt().infinite_ammo = true` at runtime and the options are saved, so the
  user's black.toml had infinite ammo on (the clip never emptied, no reload could start). Set back to false on the
  headset; the marker no longer touches options.
- **Reload is A** (the touch layout's reload is the pad's A; X is use). A tap during a burst is ignored (a firing
  gun takes no reload), so dev_dumpreload taps between dev_autofire's bursts.
- **The AK's magazine is gun bone 87.** The AK gun mesh (5878 indices, vertices 1465.. of the buffer it shares with
  the arms: the arms are the first 1465) has two bones: 75 the body (2445 vertices) and 87 (72 vertices, centroid
  0.003 0.052 0.099, at the receiver). Over a reload (~1.6 s from the tap) the off hand (bone 18: mesh +x = the
  left) leaves the gun to 0.54 m and comes back, bone 87 travelling with it (0.49 m); the gun hand stays; idle,
  bone 87 sits exactly on the body. The AK has no separate bolt or charging handle bone: racking would be a gesture
  only.
- Screenshots of the game's AK reload (side on, dev_dumpreload): the curved magazine leaves the gun in the off
  hand and comes back, matching bone 87's travel. Its vertices as dumped from the stream's start look like a 1 cm
  sliver at the receiver, and the gun draw's indices (base -1465) reach the whole 3982-vertex buffer: the dumped
  bytes aren't what's drawn for that mesh (a stream offset not accounted for). Bones and constants are reliable;
  vertex positions for the gun mesh are not yet. The magazine's middle is taken as (0, -0.03, 0.12) in the mesh
  (below the receiver, the mesh's lowest point -0.083 its bottom) until the gun's real vertices are read.
- **Built (vr_reload.cpp, VrPoseGun)**: no recompile needed: the weapon state machine's existing wrapper
  (hooks.cpp) clears the reload request (the 5th argument, `[esp+0x14]` on entry) for the player's controller
  while manual reload is on and the gun's magazine is known, and puts state 4 back to idle if a reload starts some
  other way. A (gun hand) ejects: the clip's rounds go back to the reserve (none lost), the clip is 0, the
  magazine falls 0.6 s and is hidden (its bone collapsed far below). The off hand's trigger within 25 cm of the
  chest (35 cm below the head, 12 cm ahead) takes one (fingers closed on it, drawn on the palm turned with the
  hand); within 8 cm of the well it seats: clip = min(size, reserve), reserve less that, a buzz in both hands;
  let go of the trigger elsewhere and it falls. VR SETTINGS: RELOAD (BY HAND / BUTTON), `vr_manual_reload`.
  Only the AK so far (kMagBone). xr::Hand.primary = A (right) / X (left). dev_eject presses A once.
- Slides (the user wants each gun's slide or bolt worked by hand and animated): the AK has no bone for it, so it
  needs its vertices moved onto a spare bone (index 3: c-33..-31 is an unused identity), which needs the gun's
  real vertices first.
- First headset runs (dev_eject, fake hands): the block missed at first. (1) The character's +0x29C is not the
  state machine's controller; and the state machine runs for the other slot's gun too, so the controller is now the
  inventory's held weapon (`weapons::HeldWeapon`, player +0x278 → +0x24), checked against the wrapper's argument.
  (2) An empty gun goes by itself to **state 8** (refill) ~2 s after it runs dry, not through the request: the
  wrapper now undoes 4 and 8, and while the magazine is out the tick keeps the clip at 0 (anything refilled goes
  back to the reserve). (3) The magazine (bone 87) is drawn with the **arms** mesh as well: VrPoseGun runs on the
  arms' draw too.
- Seen: after the eject the HUD reads 000 / 360 (the 60 rounds back in the reserve), the AK without its magazine,
  no reload by the game. Not seen: the chest grab and the seat (they need the controllers). Screenshots lag the log
  by many seconds here (a frame from 10+ s earlier at times): judge by the log's times.
- User test: after A the game's reload ran over and over (the wrapper undid states 4/8 only after each had
  started, so the animation restarted each frame); the pistol (no magazine bone known) reloaded by the game, its
  hand putting a magazine in from anywhere. Fixed by holding the reloads off instead of undoing them: every reload
  start checks the reserve first (0x1EF4D7 in the state machine; FUN_001BB340's 0x1BB35E, called only from inside
  the state machine at 0x1EFD08/0x1EFFA9/0x1F0006), so the wrapper shows the player's gun an empty reserve while
  the state machine runs and puts it back after (plus anything the call added). The HUD reads it outside, so it
  shows the real count. Manual reload now covers every gun but shotguns and launchers (SHG SH5 RPG GRL keep the
  game's reload for now); a gun without a known magazine bone seats at the gun hand's grip (12 cm), its magazine
  not drawn moving yet. dev_dumpreload turns the hold-off off (to record the game's reload).
- Seen after the fix (fake hands, dev_eject, autofire on the AK): the weapon stays in state 0, the clip at 0, no
  reload animation.
- **Pistol (PST) recorded** (dev_dumpreload, dev_give PST): its gun mesh bones (vertices 1465.. of the buffer)
  are 117 (1903 vertices: the slide and top, 22 cm), 111 (131: the hammer, up 0.054-0.113 in the slide's frame),
  114 (84: the magazine, in the grip, middle 0 -0.046 -0.062; leaves ~0.6 m down in the reload and comes back up
  into the grip), 108 (48) and 120 (56): small frame parts. Against the slide, 108/114/120 move +0.037 along z
  at each shot and stay there through a reload: the slide goes back 3.7 cm, locked through the reload.
- **Gun parts table** (vr_reload.cpp kParts): magazine bone, slide bone and its travel (back along the mesh's -z),
  the magazine's middle: AK 87/none, PST 114/117 0.037. VrPoseGun hides/moves the magazine and holds the slide
  back by VrSlide (0..1).
- **Slide**: locks back when the clip reaches 0 (also after an eject of an empty gun); a magazine seated then
  keeps its rounds pending until a rack; the off hand's grip within 12 cm of the gun hand (only while the slide
  needs it) takes the slide, pulling back along the gun hand's aim (6 cm a full pull); let go past 70% (or from a
  locked slide) and it runs forward, the pending rounds go into the clip, a buzz. The AK racks the same way with
  nothing drawn moving (its handle isn't a bone).
- Seen (PST, fake hands, side on): after the eject the HUD reads 000 / 045 and the slide sits locked back.
- User test: the magazine never went into the gun (the well was a guess); on the AK they want to take the
  magazine out by hand and throw it aside. Now:
  - The magazine's middle and up (the way it goes into its well) are **measured** once per mesh from the draw's
    own vertices (`a.buffers[0]`, the indices with their base: exactly what's drawn), those weighted mostly to its
    bone; up runs from its middle to its top tenth. Logged: `vr reload: magazine bone N of mesh M: ...`. The table's
    middle is only the fallback.
  - Seating is guided: with the palm within 7 cm of the well's axis and up to 18 cm below it, the magazine is drawn
    on the axis, out of the well by the hand's distance, and seats when pushed within 2 cm of home.
  - The off hand's trigger within 10 cm of the magazine in the gun pulls it out into the hand (its rounds back to
    the reserve); let go away from the well and it's thrown with the hand's velocity (falling 1.2 s, then gone);
    A drops it straight down.
- Measured in the headset: PST magazine (bone 114, 138 vertices in the gun's draw) middle 0 -0.022 -0.057, up
  0 0.96 0.28 (the grip's rake); dev_magslide (the magazine held 6 cm out along the well's line after an eject)
  shows it sticking out of the bottom of the grip on that line. The AK's bone 87 measures as 72 vertices at the
  receiver (0.003 0.052 0.099), and moving it leaves the curved magazine where it is: **the AK's magazine is part
  of its body (bone 75)**. Its vertices as read from the draw don't plot as an AK, so they can't be picked out
  yet. The AK reloads by hand with nothing drawn moving (well at the table's guess, under the receiver);
  kParts.measured false for it.
- **The AK's magazine is bone 81.** Found by collapsing bones in the gun list draws (dev_hidebone, several numbers
  and ranges; collapsed onto the body's origin, not sent away: vertices carry small weights on other bones and a
  far translation dragged the whole gun off), side on: all but 75 → no magazine; 0..63 → magazine stays; 66..72
  and 78..129 → gone; 78/81/84 → gone; 114 → stays; 78 → stays; 81 → gone. Measured from the gun draw: 288
  vertices, middle 0 -0.054 0.127, up 0 0.94 -0.35. dev_magslide shows it 6 cm out along that line. (Bone 87, the
  72 vertices at the receiver, is something else; the retag-a-box path is kept for guns whose magazine really is
  in the body, unused now.)

## 2026-11-21 — Phase 5 state (consolidated; the entries above are the trail)
**Files**: src/game/vr_reload.cpp (game thread: the reload's state, ammo, the hold-off), src/game/vr_gun.cpp
(render thread VrPoseGun / VrHandMag: the magazine and slide bones; the arms' posing), hooks.cpp (the weapon
state machine wrapper hides the reserve), native/vulkan.cpp (calls the posing on the gun list draws; the second
pass for the magazine in hand).
**Ammo**: the held weapon `weapons::HeldWeapon()` (inventory +0x24) is the state machine's controller; its ammo
`[ctrl+0xF4]`: clip word +0x18, kind +0x08 (size +0x60, ammo type +0x64); reserve = inventory word by type.
Reload is A. Every reload start checks the reserve (0x1EF4D7; FUN_001BB340 at 0x1BB35E, called only from inside
the state machine), so hiding the reserve during the state machine stops them all, the empty-gun refill (state 8)
too. Shotguns and launchers (SHG SH5 RPG GRL) keep the game's reload.
**Bones**: magazine AK 81, PST 114; slide PST 117 (3.7 cm along the mesh's -z). AK 87 is a small receiver piece,
not the magazine. Bone matrices are 3 rows from c-36 by the index short; G = the game's view space, x left.
Measured from the draw's own vertices (`a.buffers[0]` with the indices and base): magazine middle and up.
**States** (VrMag): 0 seated; 1 out (dropped/thrown/knocked: falls 1.2 s, then collapsed); 2 in the off hand,
well empty (lined up within 7 cm of the axis and 18 cm below: drawn on the axis, rocked 15° at 6 cm out, seats
within 2 cm); 3 a fresh one in hand with the old one in (a strike within 12 cm of the well faster than 0.6 m/s
knocks the old out). Off hand's trigger: at the chest (35 cm below the head, 12 cm ahead, 25 cm reach) takes one;
at the magazine in the gun (10 cm) pulls it; let go away from the well: thrown with the hand's speed. The slide
locks back at clip 0; a seated magazine waits (pending) for a rack: the off hand's grip within 12 cm of the gun
hand, pull 6 cm back, let go.
**Dev markers**: dev_eject (A once), dev_magslide (held 6 cm out on the well line), dev_magfresh (a fresh one in
hand, the old in), dev_dumpreload (records the game's reload; the hold-off off), dev_hidebone (collapse bones:
numbers, "a -b" ranges), dev_magbox (the retag box), dev_give (weapon codes in turn), dev_fakehands(_side),
dev_autostart, dev_autofire, dev_firehold, dev_lefton, dev_fakeleft_low, dev_curl. Remove them after a run.
**Headset rules** (user): ask before driving it over adb; never send the vrpowermanager prox broadcasts (the user
sets the proximity sensor in SideQuest); find the wireless port with `adb mdns services`; screenshots lag the
log by up to 10+ s.

## 2026-11-21 — Phase 5: tighter zones, rack on the trigger (user test 1)
User: the off hand's trigger brought a magazine up almost anywhere; wanted it only at the chest, the magazine to
snap to the well only centimetres away, the rack on the trigger (the grip was too near the fore-grip and the
weapon switch), smaller grip and switch zones. Cause of "anywhere": on the AK the fore-grip sits next to the well,
so a trigger press while holding the gun pulled the magazine out (10 cm reach), and the chest ball was 25 cm.
Now (vr_reload.cpp, vr_gun.cpp kReach, vr_body.cpp shoulder):
- no reload trigger action while the off hand holds the gun (VrLeftOnGun)
- chest 15 cm; pull-out 6 cm; lining up within 3.5 cm of the well's axis and 5 cm below it, home at 1.5 cm
- rack: the off hand's trigger held within 7 cm of a rack point (per gun, from the gun hand's grip along its aim:
  AK 12 cm forward 5 up, pistol 1 back 7 up: guesses), pull back 6 cm, let go; only while the slide is locked,
  and it wins over a pull-out
- fore-grip take 8 cm (was 15)
- weapon switch: the right grip with the hand in a 12 cm ball 15 cm right, 8 cm behind, 8 cm below the head's
  middle (was: anywhere right of the head and behind the face, above the chest)
Not yet seen in the headset.

## 2026-11-21 — Phase 5: grip rack at the parts, 1-2 cm zones (user test 2)
User: still couldn't rack; the trigger still grabbed the magazine from too far; wanted the pull-out 1-2 cm from
the magazine, a light buzz entering the chest zone, the chest under the headset (not 15 cm out), and the rack on
the off hand's grip within 1-2 cm of the charging handle. Noted for polish: the elbow rolls over counter-clockwise
reaching to the chest.
- rack points from the meshes' vertices (scratchpad dumps): the AK gun mesh is narrow (x ±0.018); a block on its
  +x side (the game's inside, where the user saw the port) at z 0.172, y 0.043..0.057 is the charging handle
  (muzzle at +z: the front sight at z 0.54, the stock to -0.32, the pistol grip at z -0.08). Rack point (0.037,
  0.05, 0.172) on body bone 75 (the palm 2 cm out). Pistol: slide 117 spans z -0.094..0.129, top y 0.071
  (hammer 111 at z -0.098..-0.079: the rear is -z); rack point (0, 0.085, -0.078) on 117, so it follows the
  slide when it's locked back
- the render thread (VrPoseGun, gun draws only) puts the rack point into C each frame (VrRackAt); the off hand's
  grip within 2.5 cm of it takes the slide (any time in state 0; it only chambers if it was locked or pulled most
  of the way). Near it, the fore-grip can't take that press (VrRacking covers "near" too)
- pull-out: the trigger within 3.5 cm of the magazine's body: a line along its axis from 8 cm below its middle to
  2 cm above
- chest: a column 9 cm in radius, 4 cm ahead of the head's middle, from 15 to 50 cm below it; entering it with a
  magazine to take (state 0/1, reserve, the hand off the gun) buzzes the off hand lightly
Not yet seen in the headset.

## 2026-11-21 — Phase 5: all on the grip, the AK's handle animated, rack sounds (user test 3)
User: still couldn't reliably rack the AK; wanted the charging handle to move back with its sounds (at least the
sound, to know it's done); every reload action on the off hand's grip (the trigger is for grenades from an empty
weapon slot and the M16's under-barrel launcher: TODO); every finger closing on what the hand grips; the thumb on
the polish list.
- grip for all: chest, the magazine off the gun (3.5 cm of its body), the slide/handle (2.5 cm). The chest, the
  magazine and the rack point count as "near" for the fore-grip (VrRacking), which then can't take that press; the
  left grip isn't the game's white button meanwhile (TakeLeftGrip)
- the AK's charging handle: the 12 receiver vertices at x > 0.0155, y 0.040..0.059, z 0.169..0.175 on body bone 75
  (the only part of the receiver not mirrored in x) are retagged onto spare bone 3 (RetagMag, gun draws only: the
  arms' mesh numbers its bones differently), which is the AK's slide: 7 cm back along -z at full pull. The rack
  point is on bone 3 now, so it follows the handle. The log line "vr reload: mesh N: K body vertices of bone 75
  moved onto bone 3" says whether the box caught it
- rack sounds: sound::Cue in mixer.cpp, made there (noise + tone clicks, mono into the front pair): a light double
  click when the handle reaches the back (or on the grab when it's locked back), a heavier clack with a low thump
  when it runs forward. The game's own reload sounds weren't searched for yet (TODO)
- fingers close while holding the slide/handle (VrRackHeld), as with a magazine or the fore-grip
Not yet seen in the headset.

## 2026-11-21 — Phase 5: magazines held upright, dropped ones land, loaded racks (user test 4)
User: a magazine from the chest stayed in the hand ~2 s then vanished; wanted it held indefinitely, upright in the
fist with the fingers round it (a reference photo: pistol magazine in a fist, its top out past the index/thumb);
dropped magazines should hit the ground; the pistol's never slid in; rack with or without a magazine, and racking a
loaded gun should throw a round out (one fewer). Gave the go-ahead to install and test on the headset.
- the vanish: VrHandMag drew from the gun's posed constants, where the dropped magazine's bone had been collapsed
  after 1.2 s; its pass now copies the game's own constants (vulkan.cpp, sv.vsc)
- the hold: the fist's axis is the aim pose's -z tilted 55 deg up toward its +y (kRodTilt); the magazine's up runs
  along it, its front (the mesh's +z, toward the muzzle) forward-down, its middle `hold` up the fist from the grip
  point (pistol 2.5 cm, AK 5 cm). HeldMagOf (vr_reload.cpp, real controllers) and VrHeldMag (vr_gun.cpp, HandPose,
  so dev_fakehands holds one too) compute the same frame. The magazine's own frame in the mesh is its measured
  middle and up with the mesh's +z squared to it (MagFrame)
- lining up: the held magazine's middle within 3.5 cm of the well's axis, 0..5 cm below, its up within ~35 deg of
  the axis (kAlignTurn 0.82). The pistol's well is inside the gun hand, so the palm could never get there
- drops: the out path now hides the gun's own magazine and starts a world drop (StartDrop): the camera rows and the
  floor (eye y - (stance 0x2E0 - 0.2)) come from the game thread (VrGunFrame snapshot), the fall is analytic to the
  floor (+1.5 cm), turning about its horizontal axis to lie flat as it lands, drawn by a third pass of the gun mesh
  (VrDropMag, everything else collapsed) for 60 s, whatever the gun's own magazine is doing. Seen in the log:
  "magazine dropped from ... feet -0.33, lands in 0.51 s"
- racking: in state 0 or 1 (the off hand empty); released after > 70 % pull with the slide not locked and rounds in
  the clip: one round out (clip - 1, not to the reserve)
- the arms' draw no longer calls VrPoseGun: the arms' mesh has no gun parts, and its bones under those numbers are
  fingers (81, 114, 117 and 3 would have moved finger bones)
- dev_maghand: 2 s after an eject a magazine is in the off hand, held, never lined up (with dev_eject, dev_fakehands)
Headset (dev hands, pistol): eject, the drop logged, the magazine held in the left fist 30+ s, its top out of the
fist. The floor landing wasn't in view (the dev view looks ahead).

## 2026-11-22 — The guns' real reload sounds: names found on the disc
User: find the game's real rack sounds (syncing them to the hand's motion is a later TODO).
- Disc layout (XDVDFS, walked recursively with a scratch lister): `/levels/level_NN/fpguns/bg1_<gun>.db` (+`.wdd`)
  per first-person gun; `/sound/banks/Anim.awd` (character foley: ChngWpn0-2, Silencer_On/Of, melee, steps: no
  reloads); `/sound/streams/*.bks` music/ambience; `/levels/*/stg_0001/AIWpns.awd` the AI's guns.
- Each gun's `.db` carries its reload animation's triggers: `animtrigger:weapon reload`, and
  `animtrigger:gunaudio <name>...`, the sounds the animation plays, plus a wave dictionary naming them (entries:
  name, then a size field: AkCock0 32000 bytes, AkCock1 20000, AkLd1 24000; a "04 01 12 00" field that looks like
  4-bit (Xbox ADPCM) mono). Gun audio per gun (level_01's files):
  - AK1/AK5: AkUnld0, AkLd0, AkLd1, AkCock0, AkCock1 (cock in two parts: pull and release), SbmgnPin0/1, FireSelect
  - PST/PS5: PstUnld0, PstLd0, PstLd0b, PstCockP1, PstBrch0, PstBbrch1 (breech open / release), PstFire
  - GK1/GK5: GkUnld0, Gkld0, GkCock0, GkFire
  - M16/M45/BNS: M16UnLd0, M16Ld0, M16Ld1, M16Cock0
  - MP1/MP5: Mp1Unld0, Mp1Ld0, Mp1Cock0; P90/P95: P90Unld0, P90Ld0, P90Ld1, P90Cock0
  - HVY/HV5: HvMgnULd0/1, HvMgnLd0, HvMgnCk0/1; MGN/MG5: MgnUnLd0, MgnLd0-3, MgnCock0
  - none: shotguns, RPG, GRL, RM1/RM5 (shells / launchers), AS5/ASR (AssRflFire), SM3/MC5/SMG/SM5 (FireSelect)
- Next (needs the headset): dev_dumpsound (buffers.cpp: each buffer Play logged with address, length, format, a
  hash, and each new sample saved to dumps/ on the headset) during dev_dumpreload (which now also stands our
  reload aside so the game's own runs) gives each named sound's address, length (matching the dictionary sizes)
  and rate; then find the dictionary entry in memory by name to play them at the rack.

## 2026-11-22 — Rack sounds: the game's own samples (recorded on the headset)
Recorded with dev_dumpsound + dev_dumpreload (+ dev_autofire so the game reloads), AK1 and PST, dev hands.
- Guns' sounds are Xbox ADPCM (tag 0x69) mono buffers. Each gun's `.db` wave dictionary: entries marked by the
  u32 0x0043CDC0, then rate, 4, length (bytes). **An entry's fields come just before its name** (so a
  name belongs to the rate/length printed before it); checked against what plays:
  AK draw = AkCock0 then AkCock1, AK reload starts with AkUnld0, each shot SbmgnPin0/1, pistol draw = PstBrch0
  then PstBbrch1, pistol reload PstUnld0, PstLd0, PstLd0b.
- The samples themselves are the gun's `.wdd`, loaded as one block (the first entry, the fire loop, at its
  start): AK1 base 0x81813A80 in that run, AkCock0 at +0x7C00, AkCock1 at +0x6B00 (the same offsets as in the
  file). AK5/PS5 use the same samples.
- Rack sounds (length, rate): AK1/AK5 pull AkCock0 3420 B @ 20000 Hz, release AkCock1 4212 B @ 24000 Hz; PST/PS5
  pull PstBrch0 5436 B @ 32000, release PstBbrch1 5112 B @ 32000. Others for later: AkUnld0 3492@24000,
  AkLd0 4248@20000, AkLd1 2880@20000; PstUnld0 9036@32000, PstLd0 6552@24000, PstLd0b 5652@32000, PstCockP1
  5400@24000.
- Playing them: mixer.cpp remembers every ADPCM mono sample the game plays (Seen, by length and rate, with a hash
  of its start to drop it once the memory is reused); the gun's draw plays its cock sounds, so they're known by
  the time it's held. vr_reload.cpp PlayRack: the pull's sample at 40 % of the pull (a locked slide: on the
  grab), the release's when the hand lets go, decoded and resampled into our cues (CueSample, gain 0.8); our
  clicks only when the sample hasn't been seen. dev_racksound: 10 s in, pull then release (to hear them).
- Not yet heard on the headset (the install of this build wasn't approved yet).
- (later the same day) magazine sounds too: AK eject/pull-out/knock AkUnld0 (wdd +0x4000, 3492 @ 24000), seat
  AkLd0 (+0x4E00, 4248 @ 20000); pistol PstUnld0 (+0x7400, 9036 @ 32000), PstLd0 (+0xC500, 6552 @ 24000). The game
  only plays these in its own reload, so they're found from the pull's sample (seen at the draw) by their .wdd
  offsets, checked to look like ADPCM (every block's step index <= 88, reserved byte 0). Offsets from the
  recordings (base = the fire loop's address), every block header checked in the files. Unrecorded: AkLd1
  (2880 @ 20000, most likely +0x5F00, the only gap that fits) and PstCockP1 (5400 @ 24000: +0x9800, 0xDF00 or
  0xE800); PstLd0b (+0xAE00) follows PstLd0 in the game's empty reload (likely the slide release). The headset was
  put to sleep (KEYCODE_SLEEP, at the user's word) with this build not yet installed.

## 2026-11-22: seating by the magazine's top (user test 5)

- User test 5: neither gun ever seated a magazine (the log: many held magazines dropped, no "magazine in").
  The pistol's rack sounds were found and played (PstBrch0 at 817E1880, PstBbrch1 at 817E0480), the AK's
  unload found from its pull (817DFA80); the pistol's unload wasn't (its draw hadn't played the pull yet).
- The cause: the well (the seated magazine's middle and axis in C) was only published while the magazine was in
  the gun (states 0 and 3), so once it was out the target stayed where the gun had been at the eject while the
  gun moved on with the hand. Lining up also wanted the held magazine's middle within 1.5 cm of the seated
  middle and turned within 35 degrees: on a pistol that puts the off fist inside the gun hand.
- Now (the user's rule): vr_gun.cpp publishes the well every gun draw (the game's own pose of the magazine bone
  is still in the gun), with the magazine's top and bottom measured along its up (MeasureMag). vr_reload.cpp
  seats when the held magazine's top (its middle + up x top) is within 2 cm (kSeatReach) of the line from the
  well's mouth to where the top sits seated; the mouth is the seat less GunParts depth (AK 4 cm, a guess) or, 0,
  the whole magazine but its 1.5 cm base (the pistol's grip). The hand's angle no longer matters. It then slides
  home along the well in 0.1 s (g_insert counting down; the renderer draws it on the axis) and seats.
  Diagnostic: "held top D from the well, B below its seat" twice a second within 15 cm.

## 2026-11-22: the AK's charging handle zone

- User: the pistol's slide racks fine, the AK's handle couldn't be taken. Its zone was a 2.5 cm ball 2 cm out
  from the knob (mesh x 0.037), fine for a slide taken palm over (the controller sits right on it) but a knob is
  pinched from its side, which puts the controller's grip point several cm out past that ball (the log had no AK
  rack at all).
- Now a rack zone can be a line: GunParts rack_out/rack_len. The AK's runs from the knob's tip (x 0.03, y 0.05,
  z 0.172 on the spare bone) 7 cm out along mesh +x (the side it's on); the hand within 2.5 cm of that line takes
  it. vr_gun.cpp publishes both ends in C (VrRackAt). The pistol's stays a point. Racking works with the
  magazine in or out (states 0 and 1) as before.
- Diagnostic: "off hand D from the rack zone (state, on the gun)" twice a second within 12 cm, not racking.

## 2026-11-22: user test 6 (seating works; the handle zone, the handle not moving)

- Seating worked on both guns: the pistol at 2.7 cm then in (its top 10 cm below the seat, in the grip), the AK at
  1.6-2.9 cm. The game's load sounds played (PstLd0 at 817E7F80, AkLd0 at 817E0880). The AK racked five times;
  a rack after seating on a locked (empty) gun chambered the pending 60 (no round thrown, correctly).
- The AK's handle zone: while trying, the off hand sat 2.3-3.5 cm from the 7 cm line (reach 2.5), so most
  presses missed. The user's idea: an invisible extension of the handle, a cylinder 2-3 cm out from the knob. Now
  the zone is 3 cm out from the knob's tip along mesh +x, taken within 3.5 cm of it (GunParts rack_reach); the
  diagnostic adds where the hand is from the knob (out, up, toward the muzzle, cm) to fit it by.
- The handle didn't move on the headset although the 18 vertices are retagged (the log) and the slide (bone 3)
  is offset by VrSlide; the vertices are written before the draw's upload, so the cause isn't visible from the
  code. dev_rackheld (VrSlide always 1) and the retag's log line now giving the moved vertices' bounds are there
  to look at it side on (dev_fakehands_side).

## 2026-11-22: the AK's charging handle made (headset test, fake hands, with the user's okay)

- Why it never moved: the AK's mesh models **no charging handle** (it's painted in the texture). The 12 vertices
  (18 entries) the retag box caught, x 0.0162-0.0172, y 0.041-0.058, z 0.171-0.173 on bone 75, are three tiny
  rivet bevels (each a 1 mm slope from an inner edge to an outer one) ahead of the magazine. They were moved back
  with the rack all along, unseen. The receiver is +-0.0173 wide; nothing on it sticks out (aklow/ak.csv dumps).
- The fix: RetagMag (vr_gun.cpp) also rewrites those vertices' positions each draw (GunParts tab, AK 0.04): the
  rivets' inner edges stay, their outer edges go out to x = 0.04 and forward 1.2 cm, their heights spread to meet
  (one tab 1.7 cm tall, 2.3 cm out). Forward, not back: the bevels face out and to the rear, and raked back the
  face turns inward and isn't drawn (culled: a 15 cm tab raked back was invisible from every side).
- Seen: dev_tab "0.15 0.12" (a tab 15 cm out, lifted 12 cm) drew a big slab off the gun, so the vertex writes
  reach the draw; dev_tab "0.06 0.04" side on (dev_fakehands_side) showed the slab ahead of the receiver, and with
  dev_rackheld the same slab ~7 cm back. At its real size it's small and as dark as the receiver side-on; from
  the player's place (behind, the port side, the gun's left: shells go left) it faces the eye.
- The handle zone is on the tab's tip now (rack point x 0.04), 3 cm out from it, reach 3.5 cm.
- Dev markers added: dev_rackheld (the slide/handle held back), dev_tab ("reach [lift]": the tab's x, and raised,
  to see it). Screenshots: `adb shell screencap -p` to a file then pull (PowerShell's `>` mangles the PNG).

## 2026-11-22: every gun's parts and sounds from the disc (user: the AK's way for rifles, the pistol's for pistols)

Done offline from the disc (the user away; no headset run), with scratch scripts reading `/levels/*/fpguns/bg1_<gun>.db`
and `.wdd` (every level's copy of each gun file is the same, checked by hash).
- **Bones by name.** Each gun's `.db` holds its skeleton's names, NUL-terminated, from `World`: World, Extra, Root,
  the left arm, the right arm, the gun's own bones, the right thumb. A bone's palette row is 3 x its place in that
  list. This gives the AK's 81 (AssualtRifle_Magazine) and 87 (Bullets), the pistol's 111/114/117 (Hammer,
  Magazine, Slide), all found earlier on the headset, and the magnum's speed loader at 9 before its arm.
- **Vertices.** The gun's mesh is in the `.db` raw, stride 36 as drawn (position, PBYTE4 weights, SHORT4 rows): the
  largest skinned run (twice, two copies). The AK's is 3528 + 288 + 94 + 72 = 3982 vertices, its gun draw's
  buffer. A copy of MeasureMag run on them gives the AK magazine's middle 0 -0.054 0.127, up 0.03 0.94 -0.33, as
  measured on the headset.
- **The AK has a real charging handle.** Bone 84 AssualtRifle_Slide (94 vertices: the bolt carrier, x 0.005-0.031,
  z -0.024-0.136), its knob tip at (0.031, 0.065, 0.132). The csv dumps that showed "no handle" held only bones 75
  and 87 (part of the draw). The rivet tab, the retag path, dev_tab and dev_magbox are gone; the AK racks bone 84.
- **Parts** (vr_reload.cpp kParts; knob = the slide bone's outermost vertices; all on mesh +x but the M16's T):

| gun | magazine | rounds | slide/handle | rack point | magazine middle / up (table) |
|---|---|---|---|---|---|
| AK1 AK5 (AK-47) | 81 | 87 | 84 | 0.031 0.065 0.132 | measured |
| PST PS5 (pistol) | 114 | - | 117 | 0 0.085 -0.078 (palm over) | measured |
| GK1 GK5 (Glock) | 117 | 114 | 120 | 0 0.077 -0.074 (palm over) | measured |
| ASR AS5 (G36C) | 78 | - | 81 | 0.025 0.076 0.100 | 0 -0.055 0.075 / 0 0.99 -0.14 |
| M16 M45, BNS | 78 | 81 | 84 (T handle) | 0 0.095 -0.100, zone back along -z | 0 -0.010 0.077 / 0 1 0 |
| MP1 MP5 (MP5) | 78 | 81 | 84 (lever) | 0.030 0.097 0.208 | 0 -0.032 0.094 / 0 0.94 -0.34 |
| P90 P95 | 78 (on top) | - | 81 | 0.025 0.028 0.102 | 0 0.067 -0.164 / 0 -1 0 (in downward) |
| SMG SM5 (Uzi) | 114 (grip) | - | 117 | 0.029 0.051 0.029 | 0 -0.070 -0.028 / 0 1 0 |
| SM3 MC5 (MAC-10) | 111 (grip) | 114 | 117 | 0.0365 0.0736 0 | 0 -0.038 -0.025 / 0 1 0 |
| SNR SN5 (WA2000) | 81 | - | 84 | 0.025 0.045 0.120 | 0 -0.002 0.092 / 0 1 0 |
| HVY HV5 (M249) | 99 (box) | - | 114 | 0.035 0.077 0.082 | 0.025 0.035 0.128 / 0 1 0 |

  Where the measure's up (toward the highest tenth) wasn't the well's, the table's is the magazine's long axis
  (principal axis of its vertices) or straight up (the P90's lies flat; the WA2000's is a short box; the M249's box
  is wider than tall); those guns use the table's middle, up, top and bottom. Rifle handles: the zone 3 cm on from
  the knob, 3.5 cm round it (the AK's user-tested values). hold ~0.6 of the top over the middle; depth from the
  lowest receiver vertex beside the magazine (rifles 3-5 cm; grip magazines all but the base). Slide travels are
  guesses (rifles 6-7 cm, Glock 3.5). The rounds bone of a magazine rides with it (in hand, seating, dropped).
- **Sounds.** A wave dictionary entry: u32 0x0043CDC0, rate, 4, length, ..., **the sample's offset in the .wdd at
  +0x40**, then its name. This reproduces every offset recorded for AK1 and PST. The sample bytes the game played
  on the headset (snd_ dumps: AkCock0, AkCock1) are the disc's exactly. Each role sound (pull, release, unload,
  load) is found from any of the gun's samples the game has played (its fire loop every shot, its cock at the draw)
  and played only if a hash of all its bytes matches the disc. Guns share lengths and rates (M16UnLd0 = P90Ld0's;
  AS5's UnLd0 = Gkld0's), and the silver ones move the same sounds. **PS5's samples sit elsewhere than PST's**
  (PstUnld0 +0x7E00, PstLd0 +0xCF00): the old table played PST's offsets for it.
- One-piece cock sounds split where they go quiet (loudness every 10 ms, whole 36-byte blocks): GkCock0 at 0.09 s,
  G36 Cock 0.25, M16Cock0 0.10-0.42 / 0.46-, P90Cock0 0.235, Uzi/MAC-10 Cock 0-0.20 / 0.24-. MP5: Mp1Cock0 back,
  Mp1Tap0 (the slap) forward. M249: HvMgnCk0 back, HvMgnCk1 from 0.28 s forward; HvMgnULd0 out, HvMgnLd0's first
  0.4 s in. WA2000: Cock0 is one burst, played whole on the release (no pull sound); its Unld0 from 0.3 s (silence
  before).
- RM1/RM5 (Remington 870, shells) and MGN/MG5 (the magnum: the user's exception) now keep the game's reload; they
  had been on by hand with no parts (seated at the gun hand).
- Built; not run on the headset (no okay this turn).

## 2026-11-22: first multi-gun headset run (cut short), switch fixes

- Installed (the package is `com.slimybirch.blackquest`; its folder `Android/data/com.slimybirch.blackquest/files`)
  and run with dev_autostart, dev_fakehands(_side), dev_rackheld, dev_give. Seen in the log before the headset
  dropped off Wi-Fi (asleep, off the head): the pistol's magazine measured as before (bone 114, mesh 5905, middle
  0 -0.022 -0.057, up 0 0.96 0.28) and the AK's (81, mesh 5878, 0 -0.054 0.127, up 0 0.94 -0.35). No screenshots.
- dev_give can't give a gun already carried (the pistol at the start: ten tries, then the list stops): list the
  held gun first.
- Git Bash turns `/sdcard/...` arguments to adb into Windows paths: MSYS_NO_PATHCONV=1.
- Fixed (found reading the switch paths): the slide's lock and pending rounds belonged to no gun, so an AK left
  locked empty (or with a magazine seated on the lock) passed its lock and rounds to the next gun. Now kept per gun
  code across switches (g_kept); a kept lock with rounds in the gun is dropped as stale. The dev markers' timers
  (dev_eject, dev_magfresh, dev_racksound) now count from each switch, so one dev_give run covers every gun.
- Fixed: the measured magazine shapes were cached by bone number alone, which guns share (AK/WA2000 81, pistol/Uzi
  114); a change of parts now clears them, and ends a dropped magazine (its pass draws the held gun's mesh, so it
  would have turned into the new gun's).
- Seen on the headset (below): both fixes ran through the whole cycle.

## 2026-11-22: every gun run on the headset (dev hands, with the user's okay)

- Runs: dev_autostart, dev_fakehands(_side), dev_rackheld, dev_give `PST AK1 GK1 ASR M16 BNS MP1 P90 SMG SM3 SNR HVY`
  (10 s each), then again with dev_eject, dev_magslide, dev_racksound (eject 8 s into each gun, the magazine back
  6 cm out on the well line, the rack's pull and release). Screenshots every ~2.5 s (scratchpad only).
- Every gun was given; each magazine bone found its vertices where the disc table has them (log "magazine bone N of
  mesh M": AK 81/5878, pistol 114/5905, Glock 117/4118, G36 78/6856, M16 78/5679, M203 78/6966, MP5 78/7042, P90
  78/4985, Uzi 114/6773, MAC-10 111/4758, WA2000 81/6464, M249 99/8345). Each eject dropped its magazine (landing
  logged), ammo went back to the reserve, no game refill.
- Sounds: every gun's unload, pull and release came from the game's memory, but the pistol's and (in one run) the
  Uzi's: none of their samples had played yet (no shots, and their draws play no cock). **Fixed**: then the gun's
  `.wdd` is read from the disc once (weapons::GunWaves, the first level that has it) and the sample played from it
  after the same whole-sample hash (sound::CueAdpcm). Seen: "PST unload: from the disc's .wdd at +7400", pull +5E00,
  release +4A00.
- Seen: the G36's magazine out below the well on its line, the M249's box below its feed (the belt goes with it).
- **Fixed: the P90's magazine went into the gun.** It lies on top and goes in downward, but its table had the way
  in as mesh +y (every other gun's), so out was down into the receiver (hidden) and seating aimed upside down. Now
  up 0 -1 0, top 0.015 / bottom -0.013 (its faces swapped). Seen after: the magazine lifted above the gun on its
  line.
- Not checked (needs hands): the zones, the travels, seating by hand, the P90 held in the fist. Dev markers removed.

## 2026-11-22: user test 7 (every gun by hand), and the fixes from it

User: reloads and handles good on every gun but the pistols (wants the top and back half of the slide); the arms
glitch holding the Uzi and the M249; the M16, M16+M203 and M249 need buttstocks (the viewmodels have none: first
person never showed them; from the user's photos); shots go low at distance on the M249, M16, MP5 and Uzi; the
WA2000's bolt should be worked by hand after every shot, the game's animation gone. Next: shotguns, the magnum.
- Log of the test: every eject, seat, pull-out and rack logged as meant on all twelve guns, the game's sounds
  found (the pistol's from the disc copy once).
- **Arms on the Uzi and M249**: the arm rig (vr_gun.cpp kRig) had fixed bone rows from the AK's and pistol's
  skeletons. Each gun's .db names its own (World first, row = 3 x place): the Uzi's left hand lacks
  Finger4_Tip_lft, so its right arm is a row early (63/66/69, fingers 72-129); the M249 lacks the left
  Joint_wrist_twist and that fingertip (left 9/12/-/15, fingers 18-60; right 63/66/69, fingers 72-123); the
  magnum's speed loader bones come first (left 30/33/36/39, fingers 42-84; right 87/90/93, fingers 96-129). Every
  other gun matches the table. RigOf(code) now gives each its own; the fingers' cache is by gun and mesh. Seen
  with test hands: the M249's arms hold it right.
- **Aim low at range**: the drawn gun kicks up 1.5x the game's view kick, but the shot followed the unkicked hand
  ("the game's direction carries its kick"). Logged against the game's camera, it doesn't: the M249's shot 0.9 deg
  up of the camera at kick 0, 0.8 at kick 3.5 (that constant is the game's muzzle-to-crosshair convergence). So
  sustained fire went under the rising barrel by ~1.5x the kick (the M249 to 7.5 deg, Uzi 4.4, M16 3.2, AK 2.2):
  the guns the user named kick hardest. Shots now follow the kicked hand. Also checked: each gun's muzzle locator
  is exactly its root bone's +z (logged, all twelve), so the barrel and the aim agree.
- **Pistol slides**: the zone is a line 1.4 cm over the slide's top from its rear end to its middle, 4 cm round
  it (pistol slide z -0.094..0.129, top y 0.066; glock -0.090..0.101, top 0.060; from the disc).
- **WA2000 bolt** (GunParts bolt): a shot (the clip down since the last tick) leaves the rest pending, the clip at
  0, the bolt forward (g_spent); a rack (pulled > 70 %) chambers them; a magazine seated on a spent chamber waits
  for the rack too. The game's cycling after each shot (its fire animation: animtrigger:weapon "Shell Eject" and
  gunaudio SnpRfl-Cock0, bg1_snr.db 0x9B010) isn't drawn: while the slide bone (84) is off the root (75) the slide
  is put back on the root and the arms keep their last pose relative to the root; the game's own play of Cock0
  (6156 B @ 32000) is muted (sound::Mute, buffers.cpp Play) while it's held by hand. Seen: after a shot the HUD 0,
  the rest waiting. Not seen with hands: the rack after each shot.
- **Fixed (found reading)**: rounds pending on a locked slide were lost when that magazine was ejected, knocked or
  pulled out (the next seat overwrote them); they go back to the reserve now.
- **Stocks** (vr_gun.cpp StockOf, a second draw of the gun draw that holds its pistol grip, vulkan.cpp): the M16,
  M45, BNS get a carbine's collapsible stock (a buffer tube r 1.55 cm on the bore's line from the receiver's back
  at z -0.115 to -0.33, the stock z -0.215..-0.355, 4.4 cm wide, butt y -0.062..0.090); the M249 its fixed stock
  (z -0.125..-0.435, 4.2 cm wide, top 0.104..0.094, butt down to -0.078). On the root bone (75, the M249's 90),
  vertices as the gun's (36 bytes: position, weights, bones, normal packed 11:11:10, uv), uv a point in the gun's
  own black pistol grip (M16 0.0817 -0.159, M249 0.4785 -0.0369, from the disc), both windings. Seen drawn (test
  hands, M16 and M249), flat black; their shapes not yet judged in the hand.

## 2026-11-22: the stocks textured from the handguards

- User: the M16's stock in the texture of its handguard; the M249's likewise (its own handguard: the M16's
  coordinates would read another part of the M249's texture sheet).
- Found on the disc: the M16's handguard (bone 75, z 0.13..0.31, r 0.032 round the bore at y 0.0715) repeats every
  4.6 cm along its length, u 0.309 (z 0.150) to 0.170 (0.195) and back; round it v -0.62 at its sides to -0.84 at
  its top and bottom, a quarter turn (5 cm of arc). The M249's handguard side (bone 90, x 0.024..0.030) is mapped
  flat: u = 0.134 - 2.27 (z - 0.341), v = -0.254 - 5.26 (y - 0.028), over z 0.341..0.371, y 0.028..0.070.
- StockFace now cuts each face into cells of that patch's size (its own s, t: along z and up the sides, along the
  edge and across the rims, along and round the tube) and textures each cell with the patch, mirrored in odd cells
  as the handguard repeats; the side outlines are cut at their one inward corner into two convex pieces.
- Seen (test hands, side on): the M16's stock and tube in the handguard's ribs; the M249's in its patch, which
  holds repeating oval shapes (as on its handguard side), mirrored cell to cell: a busy, quilted look.
- Added to TODO (user): no fire-mode change animation, the mode kept on right stick up (the pad's d-pad up), no blur;
  no blur for objectives and tips; the title cards' text without its black box, and the head not moving opposite
  to the player in a level's first seconds.

## 2026-11-22: detailed stocks from the user's photos, plain black

- User: plain black stocks (the handguard texture's cooling holes showed on the M249's), and new photos: an M4
  6-position stock for the M16, the FN M249 stock; as detailed as possible.
- tools/stocks.py lofts them from cross-sections (smooth normals) and writes src/game/vr_stocks.h (positions,
  normals, triangles); with a folder argument it also renders shaded side and three-quarter previews, which were
  checked against the photos. M16 (1734 vertices, 2852 triangles): the end plate and castle nut, the buffer tube
  (r 1.55 cm) on the bore's line, the stock z -0.200..-0.368 (round top over the tube, two rib grooves along each
  side, a web deepening from nothing at the front to the toe at y -0.060, 4.4 cm wide at the butt), a pad to
  -0.380, the release lever under the front, the sling mount at the toe. M249 (3114 vertices, 5712 triangles), from
  the side-on photo at 1180 px = 30 cm (z = -0.125 - (px - 160) x 0.000254, y = 0.105 - (py - 140) x 0.000254):
  the mounting flange with its lugs, the slim wrist, the comb rising to the flat top, the underside's rounded chin,
  the rubber pad; superellipse sections 3.7-4.9 cm wide.
- Colour: one point of the gun's own texture. The pistol grip's black (the first try) was too dark for the light to
  show any shape. dev_stockuv (the app folder: "u v" pairs, each for 3 s) cycled candidates on the headset: the
  back of each receiver shows the shape as a dark grey-black with a sheen: M16 u 0.99 v -0.40 (its strip at
  u 0.978..0.999), M249 u 0.6752 v -0.7416.
- Seen with test hands, side on (the shapes and the colour candidates); installed with the chosen colours.

## 2026-11-22: the M16 stock in more detail (user: the M249's is perfect, keep it)

- tools/stocks.py m16(): four grooves along each side of the housing (smooth dips 1.4 mm deep, 6 degrees wide at
  12, -2, -16 and -30 degrees, fading over the first and last centimetre), a 1.2 mm collar at its front, the castle
  nut's four notches; the release latch swept along a curve from its hinge (two bosses) under the housing's front
  back 4.5 cm, a flat lever 1.2 cm wide ending in a thicker finger tab curling down, in its pocket; the web now
  starts behind the latch (z -0.25); the sling slot's rim (raised 1.3 mm, crisp: a recess let into the smooth loft
  shaded as a smudge in the preview) on each side, z -0.300..-0.346; ridges across the butt pad's face; a sling loop
  under the toe. 8020 vertices, 13828 triangles. Previews checked; installed and seen with test hands (the
  test view is from behind and below, against a lit window: the pad's ridges and the loop show in silhouette, the
  sides' details are in shadow there).

### 2026-11-22: the M16 stock traced off the photo
- The user wanted the M16 stock exact to their photos. The old one was a loft from my reading of the photos (round
  housing with grooves); the photos (an M4 stock, side-on and three-quarter, only 204 x 192 px) show something
  else: a flat-sided housing with 3 rows of 5 windows, a web with a sling slot through it, a lever under it with its
  tab hanging down, a lock-pin knob, a raked butt pad, a sling loop under the toe. Now traced: the side-on photo
  upscaled with a pixel grid (scratchpad up.py), every outline read off it in photo px, mapped by p16() at 0.95 mm
  a px (188 px = 17.9 cm; the front at z -0.200, the tube's middle on y 0.066). Flat-faced extruded profiles
  (slab(): ear-clipped sides, tapered 1.1 to 1.5 cm half width front to butt), the windows as a frame of rails
  and posts 2.5 mm proud of the housing's sides, the slot as the web built in bands round it. 2086 vertices. The
  side preview matches the photo's outline. Built, not installed yet.
- Revised (user, marked on the render): the housing is a round tube (r 1.85 cm; the windows' frame now curved
  strips of its wall over a 1.65 cm core), the butt pad oval seen from behind (straight sides, round ends 4.2 cm
  across, edges rounded over, the ridges cut to its width; the web stops at py 156 inside its round bottom), the
  sling loop lying flat across the stock under the toe. 3208 vertices. Built, not installed.
- The sling loop (user): a rectangle of rod 2.6 cm across the stock, hanging 1.3 cm below the pad's bottom from a
  hinge block at the toe (px 21..30, py 157..166). Installed on the headset.
- dev_bright (vulkan.cpp, the frame filter's post.z): a gamma lift of the whole frame for test shots, the value
  in the file (2 if empty). Shot at 2.2 and 4: the scene lifts well, but the stock barely does: its colour is the
  pistol grip's texel, near pure black, so a lift has little to work on. The windows' frame, the pad's oval edge
  and the hanging loop show; the faces stay black. Markers removed, the game stopped.

### 2026-11-22: the stock's sliver, and the stocks' greys
- The line the user circled by the M16's stock (a thin strap from the castle nut down to the sling loop, a faint
  wedge beside it) was the stock's own draw: the gun's pipeline draws triangle strips (vulkan.cpp, a.strip; the
  M16's gun draw is one), and the stock's indices are a list, so each triangle was joined to the next. Most joins
  were small and hidden; the join from the list's last triangle (the loop) to the reversed copy's first (the
  castle nut) was the sliver. Found by elimination on the headset: not the sleeves, the cord's bones (87 90 99 102),
  the left arm or the gun's mesh (dev_hidebone); gone with the stock drawn empty; the drawn data dumped and matching
  vr_stocks.h exactly; only whole-stock draws had it. Fix: when the gun's draw is a strip, the stock goes as a strip
  of lone triangles (a a b c c, the joins without area), both windings (a strip's culling flips with each
  triangle's place). dev_stockcut (vr_gun.cpp: "from to" fractions of the stock's triangles, each drawn alone in
  turn for 3 s) kept for finding parts; parts of the M16 stock: tube 0-0.078, housing 0.078-0.685, web -0.712,
  lever -0.729, pad and ridges -0.892, loop -1.
- Colours (user: grey like the receiver): M16 u 0.91 v -0.33 (the lower receiver's back, verts 299-303 of the
  mesh), M249 u 0.70 v -0.76 (its receiver's flat side). Picked by single-colour runs; dev_stockuv's cycling log
  lags the screen, so its labels can't be trusted for a burst. Seen at normal brightness: both match their
  receivers, the M16's line gone. Test markers removed.
- dev_bright (vulkan.cpp) stays as a test-shot tool.
- Textured instead (user: the colour to change with the light as the receiver's does): the stocks now carry a plain
  patch of their own receiver's texture, alpha included (the gun textures' alpha is the worn-shine mask). The gun
  textures are in each .db: DXT5, 512 x 256, at offset 512, the size at 440 (scratchpad guns/dxt.py decodes them);
  texel (x, y) is u = x/512, v = y/256 - 1. tools/stocks.py: split_long() cuts every edge over 1.2 cm at its middle
  (shared cuts, no cracks), texture() projects each vertex square to its normal's main axis and tiles the patch
  mirrored. Patches, found by searching for the evenest shade and checked texel by texel: M16 x 358..390 y 192..207
  (its lower receiver between the pins and the bottom edge; at 600 texels/m, stretched 2x so the folds sit 2.5 cm
  apart: a thinner strip folded faster than the triangles and smeared), M249 x 248..296 y 120..152 at 1200/m (its
  receiver's scale, measured off its vertices). M16 18676 vertices 32782 triangles, M249 7730 / 14768. Textured
  previews (scratchpad texprev.py) clean. dev_stockuv removed. The strip form of the stock's indices is made once
  per list. Built, not installed (needs the user's okay).
- The M249 stock retraced (user: model it exactly, smaller, its front lined up with the receiver): off the user's
  new side photo (968 x 729), scaled so its flange (py 85..350) is the receiver's back end in the mesh (z -0.144,
  y 0.039..0.105, 4.4 cm wide; the old one, 0.0254 cm a px off a bigger crop, made the flange 12 x 6 cm): 0.249 mm
  a px, 20 cm long, 3 flange heights as in the whole gun's photo. Wrist, comb, the cheek rest bevelled in above its
  crease, the chin, the pad with 16 ridges, the sling swivel; the flange the receiver's size, squarer. Textured as
  before (its receiver's patch). 7265 vertices. Installed and seen lit on the headset: the flange flush with the receiver's back, the M16's textured stock clean, no line. Test markers removed.
- The M16 stock scaled up (user, with a photo of a whole carbine for scale): against that photo's receiver (its
  back to the handguard's rear, 0.24 m in the mesh: 1.05 mm a px) the stock is 21 cm long and 15 cm tall, its front
  6 cm behind the receiver's end. tools/stocks.py: K16 = 1.2 times every size (S16 1.14 mm a photo px, the housing
  r 2.22 cm, the web, lever, pad 5 cm across, loop), the front moved to z -0.178, still on the bore's line. 21438
  vertices. Built; not installed (the install was refused this turn without a fresh okay).
- The M16 stock's own texture (user: the stretched receiver patch came out too light; a new texture modeled on the
  gun's). Made at run time, so nothing game-derived is in the repo: vulkan.cpp decodes the gun's stage-0 texture
  (DecodeLevel, once per texture address), shades the receiver patch the stock's uvs use (vr_gun.cpp
  VrStockTexture: x 358..390 y 192..207) down to the mean of the gun's own plastic (the pistol grip, x 35..85
  y 120..200: x0.73 on the M16), uploads it (PixelImage, outside the pass) and binds it in stage 0's place for the
  stock's draw only (its own descriptor set; the gun's set back after). Alpha (the shine) kept. Log line "vulkan:
  stock texture from ..". The M249 keeps the gun's texture as it is. Built with the 1.2x M16 stock; not installed
  yet (needs the user's okay this turn).
- Tested on the headset (lit): the first build made no texture: DecodeLevel leaves DXT as blocks (its Host format
  is BC3), so the size check failed quietly; now DecodeDxt turns the blocks into BGRA first. Log: "stock texture:
  stage 0 .. 512x256 format 0f" then "stock texture from 83cb7200, the patch shaded by 0.73". The stock now reads
  as dark as the receiver, 1.2x bigger, no line. Test markers removed, the game stopped.

### 2026-11-22: the stocks' painted shading
- User: the game's guns have shadows and darkened edges painted into their textures; the stocks should too. Shorter
  normals do nothing (tried with a dev switch: the game's lighting normalises), so it's in a texture of the stocks'
  own. tools/stocks.py atlas(): every triangle unshared with its own cell (shelf-packed, 600 texels/m, 2 texels of
  rim; M16 1024x1024, M249 1024x512), per corner its atlas uv and st (metres on the plane square to its triangle's
  main axis, so the patch tiles without the old smearing across folds), per triangle its sharp edges (35 degrees,
  or open). No game texel in it: vr_gun.cpp BakeStock paints the cells at run time from the gun's texture
  (vulkan.cpp decodes it, DXT to BGRA): the receiver patch mirrored at each repeat (bilinear), times the plastic
  tone (M16 x0.73, the grip's mean; M249 1), times occlusion (disks of the stock's own surface about 1 cm across,
  by area, within 3 cm, in front of the texel and facing it: area cos cos / (pi r^2 + area), x1.1, floor 0.35;
  worked out once a 3x3 block) and sharp-edge darkening (0.35, falling off over 1.5 mm). Alpha shaded the same.
  On its own thread (about 0.7 s on the headset for either); the stock waits for it (not drawn till its texture is
  up). Log: "vr gun: stock texture baked, ..". Seen lit on the headset: the M16's window frame and housing edges
  shaded like the gun's, the M249's edges and crease. First tries took 17 s and 10.6 s on the render thread
  (6 mm disks; a long thin strip cut into thousands): disks by area and the thread fixed it.
- The M249's stock darker (user, with a photo of the gun): its tone now from its own pistol grip (x 235..290
  y 170..245, x0.86 of the patch) and 0.8 more on top (StockSrc::darker): about 0.69 in all; the painted shading
  stays on top of it. Installed and seen lit on the headset ("patch shaded by 0.69"); markers removed.
- Darker still (user): the M249's darker 0.8 -> 0.58, about 0.5 of the patch in all; the M16 the same 0.58 on its
  grip tone (0.42 in all). Installed and seen lit on the headset: both darker, their edges and crevices still show.
- A bit darker again (user, "almost there"): both darker 0.58 -> 0.46: the M16 0.33 of its patch in all, the M249
  0.39. Installed and seen lit on the headset; edges and crevices still show. Markers removed.
- Darker again (user): both 0.46 -> 0.36: the M16 0.26 of its patch, the M249 0.31 ("patch shaded by" in the
  log). Seen lit on the headset; each step shows less than its number (the game's light on the gun is mostly flat
  ambient, which evens out the texture's shade). Markers removed.

### 2026-11-22: shotguns, a shell at a time
- User: the SPAS 12 (SHG/SH5) and Remington 870 (RM1/RM5) loaded a shell at a time, pumped after each shot, a
  shell loadable after any number of shots up to the game's own count, shells pinched between the off hand's
  thumb and index finger. Off the game's reload (kGameReload) and onto ours (vr_reload.cpp GunParts .shells).
- Skeletons (.db): both Shotgun_Root 108. SPAS: Shotgun_Pumpgrip 111 (z 0.212..0.454), Shotgun_Shell 114 (a mesh
  of its own, 275 vertices, drawn by the game only in its reload: z 0.129..0.177, round y -0.054, r 7.5 mm; its
  texture u 0.947..1.0, hull v -0.067..-0.225, brass v -0.001..-0.067; the SPAS texture is 256 x 256),
  Shotgun_Trigger 117. Remington: Door 111 (loading gate), Shotgun_Door 114 (ejection cover), Shotgun_Pumpgrip 117
  (z 0.229..0.434), Shotgun_Shell 120 (in the gun's main mesh, z 0.12..0.177 under the receiver).
- Sounds (.db dictionaries, scratchpad guns/gen2.py): SPAS Cock0 (0.56 s, split at 0.23 s: pump back / forward),
  Ld1 a shell in; Remington Cock0 / Cock1, Ld0. The game's own per-shot pump is muted: sound::Mute has two slots
  now (the Remington's back and forward are two samples).
- How it plays: the shell is the gun's own shell bone (mag), carried in the off hand from the chest (grip), loaded
  when its tip comes within 3 cm of the loading port's line (kShellReach; the port = the shell bone's rest, its
  axis the barrel's way, 5 cm deep): into the chamber if it's live, else waiting (g_pending) for the pump. Each shot
  (the bolt rule, as the walther) puts the rest waiting and the chamber empty; pumping (the off hand's grip anywhere
  on the pump: a 17 cm zone along it, 5 cm round, worked 8 cm back) chambers them; pumping a live round throws it
  out (as a real gun); pumping on nothing leaves the chamber empty (the next shell waits for a pump). No drop, no
  eject button, no lock on the last shot; a shell let go of away from the gun goes back (nothing taken). The game's
  pump animation after a shot isn't drawn (vr_gun.cpp WorkedGun: the pump back on the root, the arms kept, as the
  walther's bolt). The fingers pinch a held shell (thumb 0.85, index 0.7, the rest loose).
- The SPAS's shell is ours (vr_gun.cpp MakeSpasShell, the same size and place, its texture where the game's is),
  drawn as the gun's extra mesh (the stocks' draw; no texture of its own: the gun's) and in the hand pass too
  (vulkan.cpp: the extra mesh drawn with the hand's constants when VrStockShell). The shell is hidden in the gun
  except while going in.
- dev_shells (vr_reload.cpp): from 6 s into a shotgun, every 6 s: a shell in the hand (0.5 s), out along the port
  (0.5 s), loaded, then the pump worked back (1.5 s) and forward; it logs each phase. Tested on the headset with
  dev hands, both guns: shells load ("shell in, clip 6"), the pump moves back (seen) and cycles with the game's
  sounds, the per-shot pump is quiet. The held shell is drawn where the real controllers are (they lay on the
  desk; dev hands only fake the arms' pose), so its place in the pinch needs the user's hands.
- SPAS modes and pumping from the fore-grip (user): the game's SPAS has no second mode (dev_modeflip: the d-pad's
  up pressed every 6 s, the weapon's words diffed round each press: nothing flips with it; the game pumps after
  every shot whatever is pressed). So ours: the fire-mode button (the right stick up) on the SPAS switches pump /
  semi-auto (vr_reload.cpp, kept per gun; a click: sound::Cue). Any gun: the button's press buzzes the gun hand very
  lightly (vr_body.cpp, 0.12 for 20 ms); a gun the game gives modes still plays its own sound. Pump mode is read off
  the game: a shot needs the hand's pump only if the game starts its own pump within 0.7 s of it (vr_gun.cpp
  g_cycled_ms, when WorkedGun's part is off its rest); semi-auto: the next round's in.
- The pump is the fore-grip now (no rack zone for shells): the off hand holding the fore-end aims two-handed; a shot
  that needs the pump puts it in pump mode, the hand locked on the pump from its most forward point: a clear rock
  back, 7 cm (kPumpStroke) with the hand at least 0.5 m/s back along the barrel (kPumpSpeed) when it gets there,
  counts (the back sound, a buzz); forward again under a quarter of the stroke chambers (the forward sound) and it's
  aiming again. A slower or shorter pull moves the pump but counts for nothing; letting go of the fore-end cancels.
  The drawn off hand goes back with the pump (vr_gun.cpp PumpBack, into the arms' left delta).
- Tested on the headset with dev hands (dev_lefton, dev_autofire, dev_shells, dev_modeflip): pump mode "fired, pump
  to chamber" then "pumped, clip 6" every cycle with a shell loaded between; with the mode button "mode semi-auto",
  shots "fired semi-auto, clip 5, 4", back to pump "pump to chamber". Markers removed.

### 2026-11-23: the user's shotgun test, fixes
- Play log: a shell was taken twice (SPAS and Remington state 2, a second each) and let go: the take was the off
  hand's grip, and the user pinched (its trigger). Shells are taken and held on the off hand's trigger now
  (vr_reload.cpp take), and xr::TakeLeftTrigger keeps that trigger from the game (its grenade) while the off hand
  is at the chest with a shotgun or holds a shell.
- The pump stays open (user: to load shells): worked back and let go of, it stays back (pump_open); shells loaded
  then wait in the tube; taken again, the hand is locked on it at the back, and forward chambers. Opening throws out
  a live round and puts the rest in the tube. A clear rock back on the fore-end opens it at any time, not only
  after a shot that needs it.
- The shot clock (the weapon controller's +0xDC, a float: set to the kind's interval at a shot, counting down; the
  game fires only at 0): found with dev_dumpkind (the ammo kind's words: +0x20 is the interval, SPAS 1.315 s,
  Remington 1.67, Walther 1.999, pistol 0.3; the kind is shared by every gun of the type, enemies' too, so it's left
  alone) and dev_shotclock (the controller's clocks every 0.1 s). The SPAS in semi-auto clears it whenever the fire
  trigger is up (vr_body.cpp VrFireHeld, the pad's analog 7): dev_rapid (pulls every 0.3 s, over the controller's
  trigger, which lay pressed on the desk) fired every 0.3 s, where before it was every 1.33 s. A pump or bolt
  worked by hand clears it too (the Remington fired 0.5 s after a pump).
- The rock after a shot: a worked gun's root (SPAS, Remington, Walther) is held where it rests (vr_gun.cpp
  RootRest: taken 1.5 s after the gun is out and not cycling, or 3 s: the walther's handle never sits on its root),
  and the arms kept on that root while the game's root moves. Logged "root held at rest" for all three.
- Walther: the handle's reach 3.5 -> 6 cm (the play log had the hand 5-6 cm off it when the user reached for it).
- Not found yet: the off hand turning on the Uzi and Walther (the log shows nothing for it; test screenshots were
  washed out by the boundary showing on the desk).
- The off hand on the uzi and walther (user: turned when free, and neither could be held): dev_lefthand logs the
  game's left hand relative to the gun's root, once a gun. M16 rows 0.035 0.532 -0.846 | -0.974 0.206 0.089 |
  0.222 0.821 0.526, palm 0.003 0.034 0.167 (under the handguard); AK and SPAS within a few degrees of it. Uzi: palm
  0.231 -0.470 -0.336 (hidden half a metre off), Walther: turned, palm 7 cm out at the stock's side. The free hand's
  angle and the fore-grip's point both come from that hand (VrGunPose's rest, g_rest from the palm). So on the Uzi
  and Walther (SMG SM5 SNR SN5) the left hand and its fingers are put at the M16's angle to the root, the palm at
  the gun's kGrips fore-grip (vr_gun.cpp VrPoseArms). After: Uzi left palm 0.205 -0.471 -0.458 (8 cm ahead of the
  handle), Walther 20 cm ahead. Test shots with dev hands show the hand under each gun's front with dev_lefton;
  the views were washed by the boundary again, so the user's hands are the check.

### 2026-11-23: the magnum, reloaded by hand
- User: the cylinder released on the mag-drop button (A), swung out by angling the gun down or rolling it
  counter-clockwise; a speed loader from the chest between the thumb and fingers; the cylinder spun by hand.
- Its .db: Pistol_Magazine_loader 9 (the speed loader, z -0.019..-0.008 behind the cylinder), Bullet1..6 12..27 (the
  rounds, in the chambers to z 0.028), Pistol_Root 111, Pistol_Hammer 114, Pistol_Magazine_Pin 117 (the crane arm
  before the cylinder, down to y 0.015), Pistol_Magazine 120 (the cylinder, its axis x 0.001 y 0.045). Sounds:
  MgnUnLd0 (out), MgnLd0 (the rounds in), MgnLd3 (shut), MgnLd1/2, MgnCock0.
- vr_reload.cpp (GunParts .revolver, kMagnum; off the game's reload): A lets the latch go; out it swings when the
  gun's muzzle is 45 degrees under level or it's rolled 45 counter-clockwise (from the gun hand's aim pose); its
  rounds wait (none fire). Out, the off hand's grip at the chest takes a speed loader; it seats with its rounds'
  tips within 2 cm of the chambers' line (the magazines' seating, the well from the swung cylinder): what was in
  it goes back to the reserve, six in. A swipe within 8 cm spins it (the swipe's speed about its axis over 2 cm,
  slowing, settling on a chamber); the off hand's grip on it, or a clockwise flick of the gun hand (6 rad/s),
  shuts it and the rounds can fire. vr_gun.cpp: the cylinder and crane turn 80 degrees out about the crane's pin
  (x -0.002 y 0.018), the cylinder about its axis with the spin, its rounds in it while it has any; the loader
  hidden but in the hand (the hand pass takes its six rounds too) or going in; the fingers round the loader.
- dev_revolver: from 6 s into the magnum, every 10 s: latch, out, a loader in the hand, seated, spun, shut.
  Run on the headset with dev hands (2026-11-23): two cycles of latch, out ("6 in it"), loader in ("6 rounds,
  reserve 30"), spin, shut ("clip 6") with the game's own sounds; the screenshots were washed by the boundary
  again and taken side on, so the swing's side and the loader in the hand need the user's eyes.
- Fixes from the magnum's test (user: it looked to go both ways): the speed loader's and rounds' bones are parked
  out of view by the game but in its reload (the rounds were drawn high over the player), so the rounds and the
  loader's seat are drawn on the cylinder's bone (in the mesh they sit in its chambers). The swing went right: the
  mesh's +x is the player's left (C -x: logged, "mesh +x is C -1.00 0 0"), so it turns the other way about a pin at
  x +0.002 y 0.018. Measured (vr gun: "cylinder swung", once): angled down 60 degrees, -0.027 -0.010 0.017 (C:
  left and down); rolled 60 counter-clockwise, -0.000 -0.033 -0.003 (the gun's left, which points down rolled).
  dev_fakegun ("pitch roll" degrees) turns the test hands' gun, and the revolver's swing waits for those angles.

## 2026-11-23: the in-game test checklist (TOTEST.md on the left wrist)

- `TOTEST.md` (repo root) is the single list of what waits for the user's hands; `tools/embed.py` builds it into the
  library (`<build>/game/vr_totest_md.h`, CMake), `src/game/vr_totest.cpp` parses its `- [ ]` lines: the text, then a
  last parenthesis `(auto: a, b; confirm)`. An item with no events is confirm-only.
- Shown one at a time on a card over the left wrist (VR SETTINGS > TEST CHECKLIST, on by default): white while
  waiting, green and DONE once all its events came while it was current (and PASS if it asks for it), then after
  1.5 s the next. Its events show under it, each `[x]` green once seen.
- PASS / SKIP: the gun hand's aim ray held on one for 1 s (a fill shows it; a buzz on choosing; the ray must leave
  before the next). PASS also overrides a missed event. SKIP sends it to the back for this run only.
- Passes are appended to `totest_done.txt` in the app folder (`pass<TAB>line`), and left out next run (delete the file
  to start over). Log lines: `vr totest: ...`.
- Drawing: the settings panel's path. When the panel is shut and an item is waiting, imgui is drawn into the screen
  swapchain (cleared transparent) with only the card window in its top left 768x432 (`xr::kCardW/H`), and
  `xr::EndFrame(..., card)` adds a 0.17 m quad of that corner: 9 cm above the grip, 7 cm back toward the wrist (the
  left aim pose's frame), turned to face the head (yaw, then tilt). `xr::CardPointer` is the right aim ray on it
  (`RayOnQuad`, shared with the panel's pointer).
- Events (`TestEvent(name)`, any thread; `x:CODE` names take the variant codes as their first, e.g. SN5 → SNR):
  vr_reload.cpp: shell_taken, shell_in, shell_in_open, pump_open, pumped, pump_left_open, pump_live_out,
  fore_grip:CODE, fore_grip_shotgun, fire_after_pump / fire_after_rack:CODE (a shot within 3 s of the hand's pump
  or rack), spas_semi, spas_rapid (three shots each within 0.8 s in the SPAS's semi-auto), cyl_latch, cyl_out_down /
  cyl_out_roll, cyl_spin (a swipe over 6 rad/s), loader_taken, loader_in, cyl_shut_push / cyl_shut_flick,
  mag_in:CODE, rack:CODE; vr_body.cpp: mode_press; vr_gun.cpp: stock:CODE (its bake done).
- Not yet seen in the headset (built only, 2026-11-23).
- FAIL (user, same day): a third button, red, between PASS and SKIP. A failed item turns red ("FAILED: kept for
  review"), is written `fail<TAB>line` to totest_done.txt and stays off the card; the card's header counts the
  failed. To bring one back after a fix, its TOTEST.md line gets `retest N` in its parenthesis (a new line, a new
  key; the parser ignores parts that aren't `auto:` or `confirm`). Skips aren't written: they come back next run.

## 2026-11-23: launchers by hand (RPG, grenade launcher), the fire-mode blur hunt

- **Bones** (each `.db`, as before): RPG root 108, RPG_Rocket 111 (only the part out of the tube is modelled: z
  0.132..0.607 on the bore at y 0.056; its back flange r 0.039 at the muzzle z 0.134, a narrow bar r 0.021 about z
  0.18..0.28, the warhead r 0.043 to 0.36, the nose to 0.607), RPG_Site 114. The grenade launcher (GRL) is built on
  the shotgun skeleton: Shotgun_Root 105, Shotgun_Clip 108, Shotgun_Pumpgrip 111 (the tube, z 0.074..0.550),
  Shotgun_Shell 114 (the grenade, z 0.109..0.199, tip forward), Site 117, Small_Site 120. The M16+M203 (BNS):
  BottomLead 87, Flap_Bone 90, G_Slide 93 (the M203's barrel, z 0.200..0.572), GrenadeShell 96, MidLead 99, TopLead 102.
- **Ours** (vr_reload.cpp kRpg, kGrl; GunParts .launcher, .take, .root): one round at a time, shells' rules otherwise
  (a start with more in the gun puts the rest in the reserve). Taken with the off hand's grip, not the pinch: the
  rocket over the left shoulder (a 14 cm ball, the weapon switch's mirrored), grenades low by the waist (a column
  45-85 cm under the head, 18 cm round). The RPG's rocket goes in backward (mesh -z), held by its bar (hold 0); the
  GRL's tube opens forward only after a shot (kTubeStroke 10 cm at the pump's 0.5 m/s; dir -1 through the pump code,
  travel -0.10 so the bone and the off hand go forward), stays open let go, the grenade goes into its back tip
  first, back again shuts it. The round is drawn on the root bone while loaded (moving with an open tube), gone when
  empty (the game hides it, so the held one is posed from the root too). The game's reload for both is off
  (kGameReload is empty now).
- **Hints** over the off hand (vr_totest.cpp DrawTip; SetTip in vr_reload.cpp): "Grip over your left shoulder for a
  rocket", "Push the tube forward to open it", "Grip low by your waist for a grenade". The strip under the checklist
  card in the screen swapchain (y 448, 112 high), its own 15 cm quad 4 cm ahead of the grip and 4 cm up, facing the
  head (xr::EndFrame's tip).
- **Fixed**: MagFrame's front for a part whose up is the barrel's way (shotgun shells, rockets, grenades) was a zero
  vector, so the held shell drew flat: now the mesh's top.
- **dev_shells** works the tube: open at phases 0-2, the grenade taken at 3, seated 4-5, shut at 6.
- **The fire-mode blur**: not the reload blur (FUN_0017B130's, already off in VR: ReloadBlur jumps past it). The
  post controller's mode (`*0x2D1990 + 0x830D4`) is set by several others: 0x1C6850 (9, a zoom's), 0x1F6C67/0x1F6C88
  (3 and 7, damage), 0x1FDBB2, 0x149227, 0x20DFB2, 0x2030BB/0x2031D9. "SetFireRateMode" (0x2910BC) is a HUD event
  (0x13495C, the fire mode byte at `[[player+0x2CC]+0xF4]+0x20`). **dev_modelog** logs every first-person animation
  through 0x17B239 by name, and the post mode and weapon state as they change with the time since the mode press:
  one run with dev_modeflip on a gun with modes (AK, M16) names both.
- Not yet: the M203 (BNS) grenades by hand: how the game fires and reloads them (its alt ammo at weapon +0xF8) needs
  a run.
- **The fire-mode change, found** (dev_modelog run, 2026-11-23): on every gun with modes the press goes through
  FUN_001BB3C0 (esi the controller): if the kind has modes (`[ctrl+0xEC]+0xC4`) it calls FUN_001BAE70 (ecx the ammo
  object: its mode byte +0x20 becomes (mode + 1) % 3, mode 2 sets +0x14 = 1, and the HUD's fire-rate word), then
  sets state 30 (0x1BB3DB). State 30 (the jump table's case at 0x1EF1DD) asks for animation 8, `FP_<gun>_S_105`, and
  holds the gun in state 31 for about 2 s, back to 0 with `FP_<gun>_S_001`. The post blur's mode didn't change with
  it (the blur seen is the animation's, the gun pulled up to the face). So the mode changes at the press, the state
  is only the animation: hook **FireModeAnim** at 0x1BB3DB (on_false 0x1BB3E5, the ret) skips it for the player's gun
  in the headset and plays the gun's FireSelect itself (the animation's `gunaudio` trigger played it). Tested with
  dev_modeflip: AK, M16 and M16+M203 change mode (+0x20 0→1→2), no _105, no state 30/31, FireSelect heard in the log.
  The M16+M203's mode press only cycles the rifle's modes: its grenades are another input.
- **Sounds by name** (PlayNamedSound): the gun's `.db` dictionary entries hold the name at +0x60 (not the first letters
  after +0x44: AK1's FireSelect entry has a 'd' in a field before it), some with the gun's prefix ("Rpg-Ld0",
  "Grdlch-Cock", "Shtg01-Cock0"): matched after the dash. The launchers (no kGunAudio entry) play Cock (open), C0
  (shut), Ld0 (load) from it.
- **Launcher runs** (dev hands, dev_shells + dev_autofire + dev_lefton): RPG "shell in, clip 1" after each shot, five
  times to an empty reserve, the rocket drawn on its front; GRL: fired → "the tube to open" → open (Grdlch-Cock) →
  grenade in (waiting 1, Grdlch-Ld0) → shut (Grdlch-C0) → clip 1 → fired, to an empty reserve. The checklist card and
  hint are hidden under dev_fakehands (the desk's controllers put them in the face). The pale jagged shape in the
  dev-hand shots is the level's (it stays with the card hidden).

## 2026-11-23: the M203 (M16+M203, BNS) by hand

- User: the M203 fires on the off hand's trigger, and should only with that hand gripping the gun. Ours: on BNS the
  pad's left trigger is held back from the game (xr::TakeLeftTrigger) unless VrLeftOnGun.
- Its ammo is the weapon's second object (+0xF8), its reserve by its kind's slot as the first's. The game's own M203
  reload is held off as the rifle's is: the state machine wrapper (hooks.cpp) hides both reserve words
  (VrHideReserve2).
- The gun switches parts (vr_reload.cpp, pseudo code "B203", kM203Tube): from when the M203 is empty with grenades
  in the reserve (and the off hand's free) until it's loaded and its barrel shut; the rifle's magazine the rest of
  the time. B203 is a launcher like the GRL: barrel G_Slide 93 forward 10 cm to open (an empty launcher with a tube
  counts as fired now: the M203's shot was the rifle's code's), the grenade from the waist into the barrel's back
  tip first, back to shut. Its sounds from the BNS files (Ld0 the load; no open/shut samples: our clicks).
- The grenade is ours (GrenadeShell 96 has no mesh): vr_gun.cpp MakeM203Grenade, a 40 mm round z 0.200..0.300 on the
  bore (y 0.0055), merged into the BNS's extra mesh with the stock (each its own bone), textured with one texel of
  the stock's bake; in the hand pass the extra mesh draws with the stock's texture (vulkan.cpp). In the rifle's turn
  bone 96 sits on the root while loaded, else gone.
- dev_altfire: the off hand's trigger pulled 0.3 s every 4 s in play. dev_modelog also logs the second ammo's clip,
  size and reserve as they change.
- **Run with dev hands (2026-11-23)**: the M203's shots count down its ammo's **+0x14** (an int), not the clip word
  (+0x18 stays 1: a shot needs it non-zero). Its alt fire (state 2, animation `FP_bns_S_126`, request 12) is in the
  state machine at 0x1EFDBE: +0x14 > 0 fires (0x1EFEED dec), else state 3 refills it at once to the kind's +0x98
  (3) and the reserve (slot 5) is never touched: the stock game's M203 is endless. The hand-grenade count (character
  +0x286) was 0 throughout: the off hand's trigger on BNS is the M203's alone. Ours: one at a time (the game's 3 at the
  start: 2 to the reserve), +0x14 is B203's clip (Ammo.clip), each load one from the reserve; the trigger is held back
  from the game while it's empty (a pull would be the endless refill) and unless the off hand grips the gun with its
  grip held (user: also in toggle grip, where the hand stays on let go; dev_lefton counts as held). Seen: fired →
  "B203 empty, the tube to open" → open → grenade in (reserve 7→6) → shut "clip 1" → fired, five times to the end;
  the barrel forward in the shots. dev_altfire takes a hold in ms (the file), skips while the trigger's held back;
  dev_nohide2 turns the M203's reserve hiding off (it made no difference: the game never reads it there).
- **The M203's grenade is an M433 HEDP** (user's photo, 2026-11-23; vr_gun.cpp MakeM203Grenade, PaintM433): 40 mm
  across, 121.5 mm long, measured off the photo's sides at 0.164 mm a pixel (the band edges sag toward the front:
  the camera looks down, so the printing is placed against the nearest edge at the front). Rim 0..7.4 (dark green,
  chamfered), case to 60 (olive), a dark joint to 61, the silver body to 93, the gold ogive (a half ellipse 19.4 round,
  28 up) and the fuze's pin. Printed in 5 x 7 letters: FUZE, PIBD, M550 / LOT 2213425 (gold), PROJ 40MM / M433 HEDP
  (silver), CTG40MM HEDP / AMMLOT GA-WS-14-1 (the case, a shade darker), centred on the mesh's +x. Painted into a
  512 x 512 strip under the M16 stock's atlas (the BNS's bake only: its stock's v scaled to make room). The M203's
  barrel opens 13 cm (the round goes in whole; tubes now open their own parts' travel).
- **The M16+M203's own texture is 256 x 256** (not the M16's 512 x 256), so its stock had never baked (BakeStock
  refused it; it drew with the gun's texture). Now a StockSrc of its own: the receiver's flat 180..208 x 198..208, the
  pistol grip 20..30 x 150..175 (read off a dump: dev_dumptex writes tex_<code>.bgra when a bake is refused), 300 px/m;
  baked 1024 x 1536 with the M433's strip, the stock seen textured on the headset.
- **Open**: the M433 isn't seen drawn yet. dev_grenadeout puts it 11 cm over the gun (logged there, between the
  muzzle and the stock in C) but no screenshot shows it; the stock in the same extra mesh draws. dev_grenaderoot
  builds it on the root bone (75) instead of 96, to tell bone 96's constants from its drawing: that run was stopped
  (user: will test the grenade later).
- dev_grenadeout and dev_grenaderoot removed (user: no placing it to be seen; it's taken from the waist and inspected
  in the hand when the user tests). The grenade is only ever in the barrel (loaded), going in, or in the off hand.

## 2026-11-23: Phase 6, the grenade's spawn (from the code)

- The throw is an animation event (FUN_001E8830, the character in edi): 0x1E8925 takes one from the count (char
  +0x286), then for the player (`[char]+0xC4 == 2`) FUN_001E32A0 (ebx the weapon controller, char +0x29C), for the
  AI FUN_001E3000; then weapon state 14 (0x1E897C).
- FUN_001E32A0: a random spread (the LCG at 0x2D6848..0x2D684C, FUN_00011C10) turned by the character's camera rows
  (`[ctrl+0xF0]+0xD0`, FUN_00012670), scaled into the throw's velocity (esp+0x10, a vector); the hand's matrix (4 x 4)
  from `[[[char+0x328]+0x50]+0x4C]+0x40` (or 0x5655C0) into esp+0x30..0x6F; then **FUN_001DD000(matrix*, velocity*,
  3, 0)** at **0x1E3464** (edi = `[0x2D1998]`): the grenade made. A hook there sees [esp] the matrix (its row 3 the
  place), [esp+4] the velocity, to replace with the real hand's (world units, as the dropped magazines' conversion).
- **Piece 2 (built, not run)**: no hook needed. FUN_001DD000 takes only edi (the pool, `[0x2D1998]`: 32 slots, a
  slot's kind at +0x3600, in use at +0x3700) and its four arguments (stdcall, `ret 0x10`): mtx (row 3 the place),
  velocity, 3, 0. hooks.cpp ThrowGrenade, run in the weapon state machine's wrapper for the player each frame: a
  throw asked for (vr_grenade.cpp VrRequestThrow(hand)) takes one from the count (char +0x286) and calls the maker
  on a frame of ours below the stack (identity turn, the hand's grip point; its velocity × kThrowGain 1.2), the
  registers put back after. The hand in the world: vr_gun.cpp VrHandWorld (HandInCamera, then CamToWorld: the world
  is metres, as the dropped magazines found). Nothing calls VrRequestThrow yet (piece 3: the release).
- **Piece 3 (built, not run)**, vr_grenade.cpp VrGrenadeGrip (vr_body.cpp's pad poll, before the right grip's other
  uses): the right hand holds the gun, a grenade, or nothing. The right grip takes a grenade (if the count has one)
  at the right hip (12-32 cm right of the head, ±15 cm fore and aft, 55-85 cm down standing, 40-70 cm crouched:
  vr_body's real crouch) or, standing only, a band across the front of the waist (±20 cm, 5 cm back to 25 cm ahead,
  48-75 cm down), all in the head's yaw frame (user: under and in line with the headset; no grab by a crouch). Held
  while the grip is; let go, VrRequestThrow(1). Then empty till the right grip over the right shoulder (the weapon
  switch's ball) brings the gun back (no switch). While away: the first-person list isn't drawn (hooks.cpp
  sub_00176F60: the arms with it, till the grenade hand of piece 4), the trigger's zeroed, the reload tick stands
  aside. The off hand's trigger (the game's own throw) is kept from the game in the headset except on the M16+M203.
- **Piece 4 (built, not run)**: while the gun's away, VrPoseGun collapses every gun bone to a point (the
  first-person list is drawn again, so the arms show) and, a grenade held, puts bone 3 ("Extra": no gun mesh uses it)
  at the right palm (HandInCamera; the fuze up the controller's top, the spoon on mesh +x to the palm, its middle 3 cm
  ahead of the grip point). VrStock returns our M67 (vr_gun.cpp MakeM67: a ball 64 mm across, the fuze's body r 10 mm
  to 50 mm up, the spoon a 14 mm strip down its side, the ring pin 26 mm across) on the gun's biggest draw, and
  vulkan.cpp binds its own texture (PaintM67, 512 x 256: the olive body with GRENADE,HAND,FRAG,DELAY and COMB B
  IP-51-514D in yellow facing the player, no INERT (user); the fuze, spoon and ring's colours in patches). The 5 x 7
  letters are shared now (Font5x7, with N and Y).
- **Piece 5 (built, not seen)**: a dev run (dev_throw: a throw every 6 s, the count topped to 3, from half a metre
  ahead of the eyes when the hand isn't tracked; dev_drawdiff: the world draws' kinds new in the 2 s after a throw)
  made grenades (the first level gives none) and found the game's grenade: **stride 24, 5280 vertex bytes (220
  vertices), 479 indices** (the rest new then were stride-32 quad batches: its smoke and sparks). Its 24-byte vertex
  taken as the guns' 36 less their bone words (position, packed normal, uv: not yet checked by eye). vulkan.cpp
  draws our M67 for that draw instead (vr_gun.cpp VrM67World: MakeM67's vertices cut to 24 bytes; a strip's indices
  as the stock's), with the M67's texture (made once, shared with the hand's).
