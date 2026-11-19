# TODO

## Phase 0
- [x] install the Android toolchain (SDK 34, NDK 28.2.13676358, CMake 3.31.6, JDK 17) → `toolchain/`
- [x] baseline build with `build.bat`, install on the Quest 3 — runs flat (NOTES, "Baseline on the Quest 3")
- [ ] baseline fps in mission 1, pause/resume on headset removal (user, when convenient)
- [x] package rename to `com.slimybirch.blackquest` + app name
- [x] UpdateChecker → `Slimybirch/BlackQuest` releases, message says to update through the launcher
- [ ] first commit (waiting on the user's go)

## Later
- [ ] xbe check: normalise the dump tool's region/media cert patch before hashing (game_files.cpp, Builder)
- [ ] find the fire ray builder (Phase 4) and the grenade spawn (Phase 6)
