# BlackQuest

A standalone Meta Quest 3 VR port of *Black* (Criterion, 2006), the original Xbox version, running natively on
the headset. It is built on [black-android](https://github.com/ranggacasper/black-android): the game's x86 code is
statically recompiled to C++ with [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp) and built for arm64,
with OpenXR for the headset and Vulkan for drawing.

> [!IMPORTANT]
> This repository contains **no game data and no game code**. Every player builds their own APK on their own PC
> from their own disc of *Black* for the original Xbox. No APK is ever published, and the one you build contains
> code recompiled from your disc: keep it to yourself.

BlackQuest is in development. *Black* is a trademark of its respective owners; this project is not affiliated with
or endorsed by them.

## Features

- **Full stereo VR** at the headset's refresh rate, the view following your head, with the game's own HUD
  kept readable in front of you.
- **The gun in your hand**, held and aimed with the controller; your own hands with fingers that follow the
  controllers' touch sensors; the off hand grips the fore-end for two-handed aim; the gun pulls back instead of
  going through walls.
- **Manual reloads for every gun**: drop the magazine, take a fresh one from your chest, seat it, rack the slide or
  handle. Guns lock open on the last round where the real gun does. Shotguns load a shell at a time and pump,
  the revolver swings out its cylinder for a speed loader, the launchers take a grenade from the waist or a rocket
  from over the left shoulder, the M16's M203 opens and loads by hand.
- **Throw grenades** with your arm, **melee** with a swing, switch weapons by reaching behind your right shoulder,
  crouch with the right stick.
- **The silencer** as the game does it: picked up, it's fitted with the game's own animation; grip it to take it off,
  grip the end of the barrel to put it back on.
- **Haptics** tuned per weapon (heavier guns kick harder), light touches as your hands find the magazine, the slide
  and pickups.
- **The game's reticle dot** where the barrel points, in its usual colours (black for things that burn, red for
  enemies, blue for objectives, green for friends).
- **Comfort**: standing or seated play, smooth or snap turning, a vignette while moving, a first launch setup for
  stance, height and turning.
- **HD textures**: the Builder can convert the *BLACK HD Reimagined* texture pack, if you have it, for the
  headset (optional, not included).
- **Co-op** from black-seamless-coop, being brought to the headset (host or join over the internet or the same
  Wi-Fi).

## Controls (Touch controllers)

| Action | Control |
| --- | --- |
| Move | Left stick |
| Turn | Right stick left / right (smooth or snap, in the settings) |
| Fire | Right trigger |
| Two-handed aim | Left grip on the gun's fore-end |
| Fire mode | Right stick up |
| Medpack | Right stick down |
| Pick up weapons, use | Right grip |
| Switch weapon | Reach behind your right shoulder and grip |
| Throw a grenade | Throw with your arm |
| Melee | Swing a controller |
| Crouch | Click the right stick |
| Drop the magazine | A |
| Take a magazine, shell or round | Left grip at your chest (shells: left trigger; grenades at your waist, rockets over your left shoulder) |
| Rack the slide or handle | Left grip on it, pull back, let go |
| M16's M203 | Left trigger |
| Silencer off | Reach to the silencer and grip it with your left hand |
| Silencer on | Grip the end of the barrel with your left hand |
| Pause / back | Menu button: tap to pause, hold for back |
| Recentre | Hold the Meta button |

The settings panel, with the VR settings and cheats, opens beside the game's own menus.

## Building and installing (Windows)

You need a Windows PC with about 15 GB free, your *Black* disc dumped to an `.iso` (USA or Europe release; region
patched dumps are fine; an XISO or a full disc dump both work, and a dump named `.xiso` only needs renaming to
`.iso`), and a Quest 3 in [developer mode](https://developers.meta.com/horizon/documentation/native/android/mobile-device-setup/).

### The easy way: BlackQuest Builder

1. Download `BlackQuest-<version>.zip` from [Releases](https://github.com/Slimybirch/BlackQuest/releases) and unzip
   it to a short path such as `C:\BlackQuest`. It holds this source, `BlackQuestBuilder.exe` and the recompiler
   prebuilt in `tools\bin`, so **no Visual Studio is needed**.
2. Start `BlackQuestBuilder.exe`. Pick your disc image and your Quest (plugged in by USB with USB debugging
   allowed, or over Wi-Fi by its address), and the HD texture pack if you have it.
3. Press **BUILD & INSTALL**. It downloads what is missing into a tools folder (Java, the Android SDK, Python, git),
   asking first; builds the game from your disc; installs it; and copies the disc to the headset. The first build
   takes 30 to 90 minutes, later ones are quick.
4. On the headset, start **BlackQuest** from the Library's **Unknown Sources**.

Your APK is signed with your PC's own Android key (`%USERPROFILE%\.android\debug.keystore`): later builds from the
same PC install over it and keep your settings and saves. A build from another PC must be uninstalled first.

### Updating

When a new release is out, the headset says so at launch (it checks over Wi-Fi) with how long the update takes.
Start `BlackQuestBuilder.exe` on your PC: it checks too, shows what's new and offers **UPDATE & INSTALL**. That
downloads the release over your folder, rebuilds (a few minutes, or 30 to 90 when the game itself must be
recompiled) and installs it over the old one, keeping your settings and saves. A git clone updates with `git pull`.

### By hand

1. Clone this repository and put your disc image in its root folder (or in `assets\` or `iso\`, or set
   `BLACK_ISO` to its path). Git ignores it; never commit it.
2. Tools: Android SDK Platform 34, NDK `28.2.13676358` and CMake `3.31.6`; JDK 17; Python 3; git. To build the
   recompiler yourself, Visual Studio 2022 or 2026 (Build Tools are enough) with the C++ workload, plus Clang 20
   or newer with Visual Studio 2026; or put the release zip's `tools\bin` in place and skip it. A `toolchain\`
   folder in the repository (`toolchain\jdk17`, `toolchain\android-sdk`, `toolchain\llvm`) is used first.
3. Run `build.bat`. It extracts `default.xbe`, builds (or takes) the recompiler, recompiles the game into
   `generated\` and builds `app\build\outputs\apk\release\app-release.apk`.
4. **Europe discs** are turned into the USA one at runtime by black-android's `europe.patch`. It is made from the
   game's bytes, so it is not in this repository: the build fetches it from black-android at a pinned commit and
   checks its SHA-256. Without it the APK accepts only the USA disc.
5. Install and copy the disc:
   ```
   adb install -r app\build\outputs\apk\release\app-release.apk
   adb shell appops set com.slimybirch.blackquest MANAGE_EXTERNAL_STORAGE allow
   adb shell mkdir -p /sdcard/Black
   adb push Black.iso /sdcard/Black/Black.iso
   ```

Settings, saves and logs are in `Android/data/com.slimybirch.blackquest/files` on the headset. The game looks for
the disc there (in `assets`) and in `/storage/emulated/0/Black`.

To build the Builder: `dotnet build builder -c Release`. To pack a release zip: `python -I tools/package.py`.

## Credits

- [black-android](https://github.com/ranggacasper/black-android) by Rangga Casper (MIT): the Android port this is
  built on.
- [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp) by TGP482 (MIT): the static recompiler and the
  portable runtime (Xbox kernel, XDK libraries and Direct3D 8), which used [xemu](https://github.com/xemu-project/xemu)
  and [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded) as references.
- [black-seamless-coop](https://github.com/MuriloDagustin/black-seamless-coop) by Murilo D'agustin (MIT): the co-op
  (`src/game/coop*`, its licence in `src/game/COOP_LICENSE`).
- [black-pc](https://github.com/Spark198rus/black-pc): its reverse-engineering notes (facts and addresses only,
  no code).
- [SDL](https://github.com/libsdl-org/SDL), [glslang](https://github.com/KhronosGroup/glslang),
  [toml++](https://github.com/marzer/tomlplusplus), [Dear ImGui](https://github.com/ocornut/imgui),
  [Zydis](https://github.com/zyantific/zydis) and the [OpenXR](https://www.khronos.org/openxr/) loader.
- [bagasromadon](https://github.com/bagasromadon): the touch controls' button skin from black-android.

## Licence

MIT, see [LICENSE](LICENSE). The licences of the projects above apply to their parts.
