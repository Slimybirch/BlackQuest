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
