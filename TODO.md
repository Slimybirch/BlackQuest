# TODO

## To test by hand
- The user's hand checks live in TOTEST.md (also shown in the headset, one at a time: VR SETTINGS > TEST CHECKLIST).
- [ ] Install the in-game checklist build (vr_totest.cpp, built 2026-11-23, waiting for the user's go-ahead), then check
  the wrist card's place, its readability and the PASS / SKIP pointing in the headset.

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
- [x] Phase 4 committed (cbccf25); user: hands, melee, pistols, two-handed aim good
- [x] free off hand rolled 45°, forearm twist, OFF-HAND GRIP hold/toggle row (built, not yet seen)
- [ ] own hands: an open pose for the free off hand (the game's is closed on a fore-grip): needs a finger pose
      from somewhere (another animation's bones, or curl the finger bones ourselves)
- [ ] own hands (user, 2026-11-21): the game's arms mesh posed by our bones (it is GPU-skinned, bones from c-36):
      each hand on its controller, two-bone IK for the elbows, the game's finger poses kept. Grabbing a gun snaps
      the gun hand to its grip and the off hand to the game's own fore-grip hold, wherever the controller is (the
      game's hand bones on the gun give both, so no per-weapon grip table or GRIP rows once this works)
- [ ] later (user): the AK's ejection port and charging handle are modelled on its left (inside) side; real AKs
      have them on the right. Possible: draw the AK mirrored across its barrel's vertical plane (a reflection in
      its K; flip the cull winding for that draw), and send its casings out of the right side. Check which other
      guns are modelled mirrored, and that the sights and markings still read right
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
- [ ] grenades, the user's design (2026-11-21): an empty weapon slot to switch to (no gun in the hand); there the
      off hand's trigger at the chest (where the magazines are) takes a grenade, throw it and let go of the
      trigger to release it. The off hand's trigger otherwise fires the M16's under-barrel grenade launcher. The
      grip stays the reload's (magazines, slide, handle)
- [ ] locomotion: head- or hand-relative, movement hand swappable (left-handed), comfort vignette when moving
- [ ] HUD editor in the headset: scale, distance, and a wrist or on-gun ammo count (Phase 7)
- [ ] first-launch setup: height, handedness, turn style, comfort, before the first level
- [ ] performance first: reviews of Quest ports punish frame drops more than anything (Black's explosions and
      particles are the worst case); render scale and effect options in VR Settings, measured per level
- [ ] avoid "press A to interact" where a hand can do it (pickups by grabbing the weapon itself, doors)

## Phase 5 (manual reloading) — state 2026-11-21
- [x] research: reload is A; every reload start needs reserve rounds (0x1EF4D7, FUN_001BB340); the empty-gun
      refill (state 8); clip `[ammo+0x18]`, size `[[ammo+8]+0x60]`, reserve the inventory word by ammo type
- [x] the game's reloads held off (the state machine wrapper hides the reserve while it runs); RELOAD option
      (BY HAND / BUTTON); every magazine gun but shotguns, launchers and the magnum (SHG SH5 RM1 RM5 RPG GRL MGN
      MG5: the Remington and magnum were added 2026-11-22; they had been on by hand with no parts)
- [x] magazines: AK bone 81, PST 114 (measured middle/axis from the draw); eject on A (drops), pull out with the
      off hand's trigger and throw, take one at the chest with the trigger, guided slide-in along the well
- [x] slides: PST 117 (3.7 cm), locks back on empty, racked by the off hand's grip (AK racks with nothing drawn)
- [x] knock the old magazine out with the fresh one (AK technique), rock-in on the way in; the magazine in hand
      drawn by a second pass of the gun mesh (built 2026-11-21, not yet seen)
- [x] user test 1: a magazine came to the hand on the off hand's trigger almost anywhere (the fore-grip is next to
      the AK's well). Tightened (2026-11-21): no reload trigger while the off hand holds the gun; chest 15 cm, pull
      out 6 cm, lining up 3.5 cm off the axis / 5 cm below; rack on the off hand's trigger held at the slide or
      handle (7 cm); fore-grip 8 cm; weapon switch a 12 cm ball just above the right shoulder, behind the headset
- [x] user test 2: still couldn't rack; the pull-out still too far. Changed (2026-11-21): pull-out within 3.5 cm
      of the magazine's body (a line on its axis), the chest a column under the headset (9 cm radius, 15-50 cm
      down) with a light buzz on entering, the rack back on the off hand's grip within 2.5 cm of the charging
      handle / slide rear, measured from the meshes and placed by the render thread
- [x] user test 3: the AK still couldn't be racked reliably. Changed (2026-11-21): every reload action is the off
      hand's grip (chest, magazine off the gun, slide/handle); the AK's charging handle animated (its 12 vertices
      retagged onto spare bone 3, 7 cm travel); rack sounds (made in the mixer: a click at the back stop, a clack
      run forward) with buzzes; the fingers close on the slide/handle while held
- [x] user test 4: racking worked (log); a held magazine vanished after ~2 s (the hand's draw was built from the
      constants with the dropped one already collapsed); dropped ones never reached the ground; the pistol's never
      seated (lining up used the palm, which on a pistol is inside the gun hand). Fixed 2026-11-21 (NOTES)
- [x] the magazine in the hand held upright in the fist (top past the thumb, front forward); lining up by the held
      magazine's middle and axis; held indefinitely (seen with dev_maghand)
- [x] dropped magazines fall in the world, lie flat on the floor under the player and stay 60 s (a pass of their
      own, so they stay when the next one is seated)
- [x] racking with or without a magazine in; racking a loaded gun throws the chambered round out (clip - 1)
- [x] heard on the headset (user test 5): pistol rack sounds and the AK unload found; neither gun seated
- [x] seating by the held magazine's top within 2 cm of the well (mouth to seat), any angle; the well now follows
      the gun while the magazine is out (it was left where the gun was at the eject: why nothing ever seated)
- [x] the AK's charging handle zone: a line 7 cm out from the knob (pinched from its side), not a point
- [x] user test 6: both guns seat; the AK racks but the zone was missed at ~3 cm
- [x] the AK's zone: an invisible handle extension 3 cm out from the knob, reach 3.5 cm
- [x] the AK's charging handle: the mesh has none; its three rivets there drawn out into a tab that moves back
      with the rack (seen side on with dev_tab/dev_rackheld)
- [x] that tab was wrong: the AK has a real bolt carrier with its handle (bone 84, AssualtRifle_Slide), missed by
      dumps that held part of the draw; the tab is gone, the carrier racks (2026-11-22, built, not seen)
- [x] every rifle and pistol from the disc (the user's: the AK's way for rifles, the pistol's for pistols, not the
      magnum): bones by name from each gun's .db, magazine shapes and knobs from its vertices, the reload sounds
      from its wave dictionary (offsets read from it, each checked by a hash of its bytes; one-piece cock sounds
      split at their quiet gap). NOTES 2026-11-22 "every gun's parts and sounds from the disc"
- [ ] user test 7, every gun (dev_give cycles codes): the handle zones (the log's out/up/fwd), seating (the log's
      "held top ... from the well"), each gun's slide travel and knob (guesses: AK 7 cm), the sounds (the log names
      each sample found or not found), rounds riding with the magazine, the floor height, racking loaded
- [ ] (user, 2026-11-23: now) guns that don't lock back (AK, M16, M249) show their handle held back when empty, as the
      pistol's slide (the real bolt's lock is on the gun's far side, not easily seen)
- [x] the game's own rack sounds: AK AkCock0/AkCock1, pistol PstBrch0/PstBbrch1, found by recording and played
      from where the game's draw played them (NOTES 2026-11-22)
- [ ] hear them on the headset (dev_racksound or a real rack); the other guns' are in (built 2026-11-22)
- [x] the magazine sounds: AkUnld0 / AkLd0, PstUnld0 / PstLd0 on eject (A, by hand, knocked) and seat (built
      2026-11-22, not yet heard)
- [ ] AkLd1 / PstLd0b / PstCockP1 if they fit a step (offsets unconfirmed: NOTES)
- [ ] later: the rack sound synced to the hand (the pull's part as it goes back, the release's when let go; held
      back longer, the release waits); the AK's is already two sounds (AkCock0, AkCock1)
- [ ] button prompts in the Quest's names (e.g. "RIGHT GRIP to pick up" for the game's "X"): find the prompt text
      and button glyph table, map the pad buttons to our VR controls
- [x] the magazine and slide bones for the other guns: named in each gun's .db (2026-11-22)
- [x] the magnum (MGN/MG5): its speed loader and six rounds by hand (built 2026-11-23: see below)
- [x] shotgun shells one at a time (2026-11-22: the SPAS 12 and Remington 870; built, tested with dev hands)
- [ ] the user's hand test of the shotguns: shells pinched on the off hand's trigger, the open pump, semi-auto's
  speed, no rock after shots (SPAS, Remington, Walther)
- [ ] the user's check of the Uzi's and Walther's off hand (the M16's angle, at a fore-grip: 2026-11-23)
- [x] launchers (2026-11-23): the RPG and grenade launcher by hand (built, run with dev hands)
- [x] the M16's M203 (BNS) grenades by hand (2026-11-23, B203, NOTES): run with dev hands; its look is the user's
- [ ] the M433 grenade: the user inspects it in hand from the waist (not seen drawn in dev screenshots: NOTES "Open")
- [ ] magazines at the chest drawn on the body
- [ ] hovering icons where things are taken (user, 2026-11-23): a bullet icon at the chest for ammo, a grenade icon
  low near the waist for grenades (launcher grenades and thrown ones both taken there, below the chest); the icon
  moves (lifts, grows) when the hand hovers over it, with the buzz
- [x] multi-gun pass on the headset with dev hands (2026-11-22): sounds from the disc when unplayed, the P90's well
  turned down; dev markers removed
- [ ] Make the guns work for AR platforms and physical gun stocks (user, 2026-11-22)
- [x] The magnum (built 2026-11-23, to test in TOTEST.md: the swing's side and pivot, the loader in the hand, spin, shut): the cylinder swings out to the side, a speed loader
  (a ring of rounds) from the chest put in, swung back in; the cylinder spun by hand (user)
- [x] (built 2026-11-23: FireModeAnim hook, NOTES) No fire-mode change animation on any gun; the mode still changes on its button (right stick up, the pad's
  d-pad up), and no blur with it (user)

## Polish (user, 2026-11-23: moved here from Phase 5)
- [ ] the P90's magazine is held upright in the fist like the others (it lies flat on the gun: held along the
      forearm would suit it); the M249's belt stays in the gun with the box out
- [ ] a round flying out of the port when racked loaded (a shell/round model to throw: find the game's casing)
- [ ] a dropped magazine stays drawn only while the same gun is held (one drop at a time)
- [ ] the thumb: rests too flat/broken-looking on the thumb rest, and its twist (curl, kThumbRest,
      kThumbTwist in vr_gun.cpp); every finger wrapping on whatever the hand grips
- [ ] the elbow rolls over counter-clockwise when the hand reaches to the chest (the IK's pole)
- [ ] the magazine in the hand held vertical (it sits sideways now): turn HandMag's pose so its up runs along the
      hand's fingers-to-wrist line, top toward the thumb
- [ ] No blur when objectives or tips come up (user)
- [ ] Title cards (the time and place at a level's start): the text only, no black box behind it; and the player's
  view set up so the head doesn't move opposite to the player's own in those first seconds (user)

## Phase 6 (grenade throwing; user, 2026-11-23: in pieces, low usage, no test or install without the user's say)
- The user's design: a grip low across the waist (below and in line with the headset; never reached crouching by
  accident) takes a grenade: the gun leaves the hand, the grenade is in it, not armed; let go while throwing: it
  flies from the hand with the hand's speed and the game's fuse starts then. Hands empty after the throw until a grip
  over the right shoulder brings back the last weapon held. The grenade modelled exactly as the user's photos (M67:
  olive sphere, yellow "GRENADE,HAND,FRAG,DELAY / COMB B IP-51-514D" (no "INERT": user); fuze, spoon, ring pin); every
  grenade seen in the game drawn as it, the pickup box kept.
- [x] 1. the spawn, from the code (NOTES 2026-11-23 "Phase 6")
- [x] 2. the throw on our release (built, not run): VrRequestThrow(hand) → the wrapper makes the grenade itself
      (FUN_001DD000) with the hand's place and speed; no animation, no recompile
- [x] 3. (built, not run) the right grip at the right hip (and standing, a band across the waist) takes a grenade,
      the gun hidden (the first-person list: the hands too, till piece 4) and not fired; let go, thrown; empty till a
      grip over the right shoulder. The game's own trigger throw is off in the headset (the M203 keeps it)
- [x] 4. (built, not run) the M67 in the right hand while one is held; the gun's bones collapsed, the arms drawn again
- [x] the throw's sound as the grenade leaves the hand, only then (built, not heard): the game's GrdPin.dav
- [x] 5. (built, not seen) the world's grenade draw (stride 24, 220 vertices, 479 indices: found by a dev throw)
      drawn as the M67 with its texture
- [ ] co-op (user, 2026-11-23): giving ammo to another player: the magazine taken at the chest can be dropped on the
      ground for them (any time, whatever gun is out); better, handed over: it stays in the hand till their hand
      meets it
- [ ] sounds (user, 2026-11-23): a magazine landing on the ground, and one taken from the chest (from the game's files
      if they have them, else open-source files the user finds)

## From the user's checklist and notes (2026-11-24)
- [x] (built) a grenade's throw: the swing's peak speed (last 100 ms, × 1.5), the gun back at once after
- [x] (built) the chest zone 13 cm round, its middle under the head (deeper toward the chest, its front as before)
- [x] (built) the Walther's fore-grip 6 cm nearer the trigger, the grenade launcher's 8 cm farther out
- [x] (built) the SPAS's shell port placed by the root (its shell bone is parked: the port read 10-13 cm low)
- [x] (built) launcher rounds and the hand grenade held with the fingers wrapped round, not pinched or closed through
- [x] (built) the grenade launcher's round is our M433 (on bone 3, its own texture; the tube opens 13 cm)
- [x] (built) the magnum: a fast counter-clockwise flick swings it out too (a big roll wraps past -180); the
      flick that shuts it waits 0.6 s after it swings out
- [x] (built) the magnum: the game's off hand spinning the cylinder no longer shows (cylinder and crane posed
      from the gun's root, not their own bones); every game animation on it held off once it settles (its root and arms held)
- [x] the M249's right side: its left mirrored (seen on the headset from the right)
- [x] the M249 remodelled as the Para: scrapped (user); the game's M249 back
- [x] the guns' right sides report (NOTES 2026-11-24): every texture shares its left's texels; only the M249 and
      the WA2000 lack right-side geometry
- [x] the M249's para stock: two bars one over the other, an oval butt dished for the shoulder
- [x] the M249's right side: options A and B built and shown (dev_right_b switches to B)
- [x] the user picked A for the M249 (its upper band fixed); B's switch (dev_right_b) to go once A's confirmed
- [x] the WA2000's right side (A: its missing left faces copied across)
- [x] the Walther's rock after a shot (its frame held at rest too), its magazine's catch from the well's bottom, its
      off hand 6 cm nearer the trigger (and the GRL's 8 cm out: both had been moved the wrong way)
- [ ] the Walther's off hand's angle on the fore-end: the user's look (test hands cup it from under)
- [ ] the WA2000's right side the same way
- [ ] the Walther: its magazine's and the off hand's angle; the magazine hard to seat; the rock after a shot (fail)
- [ ] the Uzi and Walther free off hand still turned, the Uzi's fore-grip not taken (fails)
- [ ] fails to look at: the SPAS's and Remington's shells in the hand; the RPG's rocket in the hand; grenades into the
      GRL's and M203's open tubes and shutting them; the magnum opening by angling down, its speed loader in the
      hand; the M16's magazine and rack; the M16 and M249 stocks' look; the aim at distance; the hand grenade
      pieces (taken, thrown, the M67 in the hand: marked before there were grenades, user)

