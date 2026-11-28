# Endless mode

A replayable survival mode on the campaign's maps, its own project inside BlackQuest (user, 2026-11-24; made a
project of its own 2026-11-27). Add ideas under **Ideas** as they come; once one is agreed it moves into **Design**,
and the work goes into **Plan**. It comes after co-op (Phase 9) is solid.

## Ideas (the user's, as given)

- (2026-11-24) Replayability: mission 1's map first, started by a hidden switch in the level or a front-end entry.
- (2026-11-24) 3 lives a player; over when every player is out (each player's own count, a spectate while out).
- (2026-11-24) Waves of enemies at random spawn points, more each wave; the next wave when the last enemy dies.
- (2026-11-24) Random weapon pickups at random points, better weapons as the waves go (a tier table by wave).
- (2026-11-27) For each level: remove the cinematics, triggered events and encounters (an empty map to fill).
- (2026-11-27) An in-game tool to mark where enemies spawn, where weapons spawn and where health packs spawn.
- (2026-11-27) Players spawn, and respawn, at the level's start.
- (2026-11-27) Random weapons and random enemies spawn, more as the waves go on.
- (2026-11-27) Text on screen: game start, each wave, a life counter, and so on.
- (2026-11-27) Co-op: health packs shared. Reach your hand out to your buddy and press the health pack button
  (right stick down): the pack goes to them.

## Design (to agree)

- **Levels**: each campaign level as an arena. Its scripted content off: cinematics, trigger volumes (the type 11
  triggers coop_control.cpp lists), placed encounters and their spawners, objectives and the mission's flow (fail /
  complete requests, coop_world.cpp's in-level state). Doors the script opens stay as they are when the level loads.
- **Spawn markers** (the editor): in a level, a mode from the VR settings panel (EDIT ENDLESS SPAWNS). Point the
  laser at the floor and pull the trigger to place a marker of the selected kind (enemy, weapon, health pack);
  grip on a marker to move it, B to delete it. Markers drawn as coloured posts in the world. Saved per level in the
  app's folder (endless_<level>.toml: kind, position, facing), so a player's own layouts survive updates; shipped
  defaults for each level can come later.
- **Players**: spawn and respawn at the level's start (where the mission begins), not a checkpoint reload. A life
  counter per player; out of lives, spectate (the co-op down player's view) until the next wave or the game ends.
- **Waves**: wave N spawns more enemies (a count curve) of tougher kinds (a table by wave) at random enemy markers,
  out of the players' sight where possible; the next wave when the last enemy dies, after a short break.
- **Pickups**: each wave, random weapons from a tier table (pistols early, rifles, then launchers) at random weapon
  markers; health packs at the health markers, a few per wave.
- **On screen** (the game's own message box and HUD): GAME START, WAVE N, WAVE CLEARED, lives left, enemies left,
  GAME OVER with the wave reached.
- **Co-op health** (every mode, not only endless: TODO.md Phase 9): with a stored health pack, the right stick down while the left hand is reached out within
  10-20 cm of a partner's body gives them the pack (a co-op event, as magazines are handed over: weapons::AddMagazines'
  path); otherwise it heals you as now.

## Needs from the code (research)

- The game's enemy spawn: its actor/spawn function, the kinds (the friendly soldiers' and enemies' models and AI
  types), and how a spawned enemy is given a weapon and woken.
- The pickup spawn for weapons and health packs (the level's placed pickups: NOTES "suppressor pickup hunt" found
  their records and the take step).
- The player's death and respawn without a checkpoint reload (coop_life.cpp already holds a death; the down/revive
  path may serve).
- Turning off a level's scripted content: its trigger list, cinematics (the film player and in-engine cutscenes),
  placed encounters.
- A HUD counter (lives, wave): the hud's message box (0x12B800) or our own text on the hud plane.

## Plan

Not started. First step when it starts: the research above on mission 1, then the spawn editor (markers saved and
drawn), then waves of one enemy kind.
