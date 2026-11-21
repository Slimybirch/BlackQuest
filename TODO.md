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

## Phase 3 (head tracking, locomotion, comfort)
- [x] head drives the game's look (VrLook 0x1E5FE1), eye matrix turns by the residual; snap/smooth turn (vr_snap_turn); auto-aim off in vr
- [x] user: tests 1-3, closed 2026-11-20
- [ ] locomotion relative to the off hand (option), real crouch done in Phase 2
- [ ] sim step 1/72 or pose-only re-render when the sim drops a frame
- [ ] comfort: vignette when moving, turn speed, seated mode
- [ ] camera shake / head bob off (option to keep a little); cutscene cameras on the theater screen

## Phase 4 (motion-controller aiming, gun in the hand)
- [x] gun list moved to the right controller (K in EyeMatrix), anchor from the muzzle (vr_gun.cpp)
- [x] player bullets from the muzzle along the gun (0x1DB7B0); flash sprite, gun effects, tracers on the barrel
- [x] game rumble → controller haptics
- [x] user test 1: gun too far back, shots from the head, flash random, melee hid the gun, arms together
- [x] fixes: renderware rows (mirror), shot hook keyed on 0x1DC0D6, gun locked to its muzzle frame, handles per
      weapon (kGrips), left arm split to the left controller, melee keeps the gun in the hand
- [x] user test 2: shots from the barrels, haptics felt, near shots right; AK grip 4" too far forward, AK left of
      centre at range and off when aiming in, reload blur unwanted
- [x] fixes: AK grip back 0.10 m, shot direction from the hand's aim (game spread kept), reload blur off in vr
- [x] user test 3: aiming in and range fine; rifle hands off; left arm stuck; melee elbow; no recoil; no rumble
- [x] fixes: arms by index range, left hand takes/lets go of the gun by grip, pistols split, left arm hidden in
      melee, the game's kick on the gun, per-shot buzz, rumble resent, GRIP FORWARD / HEIGHT rows
- [ ] check in the headset (needs light): left arm off and on, recoil per gun, buzz, melee elbow
- [x] user test 4: vibration felt; AK grip set by the user (0.68 back, baked in; vr_grips.txt cleared)
- [x] own hands first build: bones posed (IK elbows, shoulders by the head), grips from the game's palms, toggle
      grip, two-handed aim; not yet seen in the headset
- [ ] own hands: forearm twist following the hand; scale check vs the controllers; the left hand's open pose
      when off the gun (the game's is closed on a fore-grip)
- [ ] own hands (user, 2026-11-21): the game's arms mesh posed by our bones (it is GPU-skinned, bones from c-36):
      each hand on its controller, two-bone IK for the elbows, the game's finger poses kept. Grabbing a gun snaps
      the gun hand to its grip and the off hand to the game's own fore-grip hold, wherever the controller is (the
      game's hand bones on the gun give both, so no per-weapon grip table or GRIP rows once this works)
- [ ] reload by hand: which gun bones each reload animation moves (magazine, bolt...) are the parts to grab
- [ ] projectiles: RPG and grenade launcher from the gun
- [ ] hide the head-locked crosshair; aiming in (LT) in vr: zoom off, gun stays in the hand
- [ ] arms: forearms trailing the hands look odd (trim/hide option); the hand lets go briefly in melee; left-handed option
- [ ] two handed: the left arm only follows the left controller; aiming by the line between the hands still to do
- [ ] two-handed weapons (key, user 2026-11-20), pistols included. Long guns (rifles, shotguns, launchers): the
      off hand grips the fore-grip (grip button near it) and the gun aims along the line between the hands (gun
      hand the pivot, its roll kept). Pistols: the off hand cups the gun hand (grip near it); the hands are too
      close for a line to aim by, so the hold steadies the gun instead (less jitter, smoothed aim) and the off
      hand is drawn on the grip. Off the grip: one-handed, as now. Hold/toggle/proximity option; optional
      virtual stock for long guns
- [ ] picking weapons up by hand (key, user 2026-11-20): reach to a weapon on the ground or in the world and grip
      it to take it, instead of standing on it and pressing X. Needs the pickups' positions (world weapon list
      / the pickup test behind X) and a grab radius (~20 cm) around the hand; the grab then fires the game's own
      pickup for that item. Highlight in reach. X stays as a fallback
- [ ] scope as a lens (Wishlist); recoil kick on the hand model

## VR controls (user, 2026-11-19)
- [x] right stick down → medpack (D-pad down)
- [x] right stick up → D-pad up (firing mode)
- [x] right grip → X (pick up weapons / action)
- [x] reach behind the right shoulder while gripping → switch weapon
- [x] a hitting motion with either controller → melee (presses B for now)
- [x] melee: the game's gun swing hidden 650 ms after a melee press, the hit still the game's (vr_body.cpp, hooks.cpp)
- [x] user: stick up/down, grip, shoulder switch, melee gesture all work; smooth turn preferred (now the default)
- [x] user: smoke in explosions perfect, melee with the gun hidden good; flash sprite wanted back (Phase 4)

## Wishlist (user, 2026-11-20)
- [ ] ragdoll deaths: the xbe has no ragdoll or physics engine (no Havok/ragdoll strings; deaths are canned
      animations). Doable as our own layer: at death take the body's bone matrices, run a small verlet ragdoll
      (joint limits, a ground and world collision through the game's ray queries), and write the bones back
      each frame in place of the death animation. Needs the skeleton/bone update and the death hook (co-op
      0x1DA240/0x1DA1A0 area); a big job, after Phase 6. In co-op each copy would run its own (cosmetic only).
- [ ] silencer by hand (Phase 5 style, with the reload work): hold the silencer in the off hand and twist it onto
      the barrel to fit it; twist the other way to take it off. Find the game's silencer toggle and call it when
      the twist completes; the button stays as a fallback.
- [ ] scoped weapons (Phase 4, with the gun in the hand): the scope as a lens. The zoomed view is rendered to a
      small target and drawn on the scope's glass, so aiming means bringing the scope up to your eye; never zoom
      the whole headset view. The game's own scope overlay and zoom are off in VR.
- [x] first cut (2026-11-20): left grip + menu opens a panel with the cheats and TURNING, title and play (NOTES)
- [x] user: couldn't move through it (imgui's gamepad nav was never enabled); now also the right controller's
      pointer, trigger clicks
- [ ] user: pointer, stick navigation, panel placement/readability, cheats from the title
- [ ] VR Settings window (Phase 7; replaces the planned VR options page): its own window on the virtual screen,
      opened from the pause menu and from beside the main menu (the same window). Settings that only apply in
      play (cheats, for one) are shown greyed out outside a level. Laser pointer and stick navigation. Pages:
      - cheats: the android port's cheats panel (menu.cpp rows), same options and toml
      - comfort: snap / smooth turn (vr_snap_turn), snap angle, smooth turn speed, vignette, seated mode, head bob
      - hands: weapon hand right / left (mirrors the grip, shoulder switch, sticks and melee)
      - height: "stand straight and press" calibration, view offset (now the fixed -10 cm), real crouch on/off
      - graphics: render scale, refresh rate 72/90/120, fixed foveation (XR_FB_foveation), bloom/filters, draw
        distance, motion smoothing (Application SpaceWarp, XR_FB_space_warp: needs motion vectors per draw, a
        Phase 7+ item; off by default)
      - HD textures on/off and the size cap (Phase 8), applied at the next level load

## Ideas from other VR mods and reviews (2026-11-20)
CoD4 VR (jplakon/CallOfDuty4_VR) is GPL-3.0: design facts only, never its code.
- [ ] manual reload (Phase 5), shaped like CoD4 VR's: steps ready → holding the loaded mag → ejected → holding a
      fresh one. Eject by a button or by pulling the mag out (option). The fresh mag comes from a pouch on the off
      hand's hip (option: left / right / off hand). Insert on letting go near the magazine well, or on touching it
      (option, ~15 cm radius). Then a rack/chamber step only when the gun was emptied (Black's empty reload).
      The game's reload runs when the mag goes in, with its animation hidden; the reload button stays as an option
- [ ] two-handed aiming: see Phase 4
- [ ] haptics: firing (by weapon), hits taken, reload steps, pickups; a strength slider
- [ ] gun against walls: the muzzle pulled back or the gun lowered when it would go into a wall (option)
- [ ] grenades from a belt (Phase 6): grab one from the chest/hip with the off hand, pull, throw
- [ ] locomotion: head- or hand-relative, movement hand swappable (left-handed), comfort vignette when moving
- [ ] HUD editor in the headset: scale, distance, and a wrist or on-gun ammo count (Phase 7)
- [ ] first-launch setup: height, handedness, turn style, comfort, before the first level
- [ ] performance first: reviews of Quest ports punish frame drops more than anything (Black's explosions and
      particles are the worst case); render scale and effect options in VR Settings, measured per level
- [ ] avoid "press A to interact" where a hand can do it (pickups by grabbing the weapon itself, doors)
