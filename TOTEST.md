# To test by hand

The checks waiting for the user's hands in the headset, kept current as things are built. The game reads this file
(it's built in) and shows the unchecked items one at a time on a card over the left wrist, with VR SETTINGS > TEST
CHECKLIST on: white while waiting, green when done, then the next. An item ends in its completion:
`(auto: a, b)` the game's own events, all of them, while it's the current item (in any order), which pass it;
`(confirm)` the user's PASS on the card; both may be given. The card's buttons are chosen by pointing the gun hand
at one and holding it there a second: PASS; FAIL (red: it doesn't look right, or the action did nothing), kept for
review and off the card until fixed; SKIP, to the back, and it comes up again next run. Progress is kept on the
headset (`totest_done.txt` in the app's folder: `pass` or `fail`, a tab, the item's line). Once a failed item is
fixed, its line here gets `retest N` in its parenthesis (e.g. `(confirm; retest 2)`) so the card shows it again.
Checked here (`- [x]`) means confirmed and left out of the card. Lines must stay one item each.

## Shotguns (SPAS 12, Remington 870)
- [ ] Take a shell at the chest with the off hand's trigger (a pinch) (auto: shell_taken)
- [ ] No grenade thrown taking a shell; it sits between thumb and index finger (confirm)
- [ ] Load a shell at the port under the receiver (auto: shell_in)
- [ ] Rock the pump back quickly on the fore-end: it opens (auto: pump_open)
- [ ] Push the pump forward: it chambers (auto: pumped)
- [ ] Two-handed aim still works holding the fore-end (auto: fore_grip_shotgun; confirm)
- [ ] Let go with the pump back: it stays open; load a shell; take it and push forward (auto: pump_left_open, shell_in_open, pumped)
- [ ] Pump on a live round: it's thrown out (auto: pump_live_out)
- [ ] Fire right after pumping (auto: fire_after_pump)
- [ ] No rock animation after a shot (confirm)
- [ ] SPAS: right stick up switches to semi-auto (a click, a small buzz) (auto: spas_semi)
- [ ] SPAS semi-auto fires as fast as the trigger's pulled (auto: spas_rapid)
- [ ] The SPAS's shell looks right in the hand (confirm)
- [ ] The Remington's shell looks right in the hand, not flat (confirm)

## Launchers
- [ ] RPG empty: the tip "Grip over your left shoulder for a rocket" floats over the off hand (confirm)
- [ ] RPG: take a rocket over the left shoulder with the off hand's grip, held by the bar behind the warhead (auto: rocket_taken; confirm)
- [ ] RPG: the rocket's back end to the muzzle: it goes on, and fires (auto: rocket_in; confirm)
- [ ] Grenade launcher: the tube won't open before a shot (confirm)
- [ ] Grenade launcher: after a shot, push the tube forward quickly: it opens (auto: tube_open:GRL)
- [ ] Grenade launcher: take a grenade low by the waist with the grip (auto: grenade_taken:GRL)
- [ ] Grenade launcher: the grenade's tip into the back of the open tube (auto: grenade_in:GRL; confirm)
- [ ] Grenade launcher: pull the tube back to shut it, then fire (auto: tube_shut:GRL, fire_after_pump)
- [ ] M16+M203: the off hand's trigger fires the M203 only while that hand grips the gun, the grip held (also with TOGGLE grip) (confirm)
- [ ] M16+M203: one grenade per load; the trigger does nothing while it's empty (confirm)
- [ ] M16+M203: after its shot, push the M203's barrel forward quickly: it opens (auto: tube_open:B203)
- [ ] M16+M203: take a grenade low by the waist with the grip; it looks like a 40 mm grenade (auto: grenade_taken:B203; confirm)
- [ ] M16+M203: the grenade's tip into the back of the open barrel, pull it back to shut, fire it (auto: grenade_in:B203, tube_shut:B203; confirm)
- [ ] M16+M203: the rifle's magazine still reloads as before (auto: mag_in:BNS)

## Grenades (thrown by hand)
- [ ] Standing: the right grip low across the front of the waist takes a grenade; the gun goes away (confirm)
- [ ] The right grip at the right hip takes one too, standing and crouched; crouched, the waist band doesn't (confirm)
- [ ] Throw and let go: it flies from the hand at the hand's speed, the fuse starting then (confirm)
- [ ] The throw's sound plays as it leaves the hand, and not before (confirm)
- [ ] Hands empty after; a grip over the right shoulder brings the last gun back (confirm)
- [ ] The off hand's trigger no longer throws a grenade (but still fires the M203) (confirm)
- [ ] The M67 in the hand looks like the photos (no INERT), held fuze up, spoon to the palm (confirm)
- [ ] Thrown grenades (and the AI's) are drawn as the M67 in flight and on the ground (confirm)

## Magnum
- [ ] A lets the cylinder's latch go (auto: cyl_latch)
- [ ] Angle the gun down: the cylinder swings out to the left (auto: cyl_out_down; confirm)
- [ ] Roll the gun left: the cylinder swings out (auto: cyl_out_roll)
- [ ] It can't fire while open; the game's own reload never starts (confirm)
- [ ] Take a speed loader at the chest with the off hand's grip, held between thumb and fingers (auto: loader_taken; confirm)
- [ ] Seat the speed loader at the back of the cylinder (auto: loader_in)
- [ ] Swipe across the open cylinder: it spins and settles on a chamber (auto: cyl_spin; confirm)
- [ ] Shut it with the off hand's grip on it (auto: cyl_shut_push)
- [ ] Shut it with a clockwise flick of the gun hand (auto: cyl_shut_flick)
- [ ] A big fast counter-clockwise roll swings it out too, and the hand coming back doesn't shut it (confirm)
- [ ] No game animation takes over the magnum or the hands while it's out (after its first moment) (confirm)
- [ ] The cylinder never turns by itself (the game's spin with the off hand gone) (confirm)

## Off hands
- [ ] Uzi: the free off hand isn't turned (confirm)
- [ ] Uzi: take the fore-grip with the off hand (auto: fore_grip:SMG)
- [ ] Walther: the free off hand isn't turned (confirm)
- [ ] Walther: take the fore-grip with the off hand (auto: fore_grip:SNR)

## Walther
- [ ] Work the bolt handle after a shot (taken from up to 6 cm) (auto: rack:SNR)
- [ ] Fire right after working the bolt (auto: fire_after_rack:SNR)
- [ ] No rock animation after a shot (confirm)

## Every gun
- [ ] M249: the para stock (two bars, one over the other; the oval butt sits in the shoulder) (confirm)
- [ ] M249: its right side closed (receiver wall, plates, slot, port cover, handguard), matching the left (confirm)
- [ ] WA2000: its right side solid (the wooden stock, fore-end and receiver), matching the left (confirm)
- [ ] Walther: no rock in the hand after a shot (confirm)
- [ ] Walther: the magazine slides home once its top reaches the well's bottom (auto: mag_in:SNR; confirm)
- [ ] Walther: the off hand on the fore-end, nearer the trigger, at a natural angle (confirm)
- [ ] Grenade launcher: the off hand farther out on the tube (confirm)
- [ ] The fire-mode press (right stick up) gives a very small buzz in the gun hand (auto: mode_press; confirm)
- [ ] Fire-mode change on the AK or M16: no animation, no blur, the gun stays ready, its click plays (auto: mode_no_anim; confirm)
- [ ] Other guns' reloads unchanged: a magazine out and in, and a rack, on the M16 (auto: mag_in:M16, rack:M16)
- [ ] And on the AK (auto: mag_in:AK1, rack:AK1)

## Stocks
- [ ] M16: the stock's shading, colour and size against the receiver (auto: stock:M16; confirm)
- [ ] M249: the stock's shading, colour and size against the receiver (auto: stock:HVY; confirm)
- [ ] A stock comes about a second after the gun's drawn, with no hitch (confirm)
- [ ] The frame rate with the M16 or M249 out (confirm)

## Older, not yet confirmed by hand
- [ ] The aim at distance after the recoil fix (M16, M249, MP5, Uzi) (confirm)
- [ ] Pistol: rack the slide taking its top or back half (auto: rack:PST)

## Phase 7: UI
- [ ] The pause menu stays put in the room when you turn your head (confirm)
- [ ] Quitting a level: the game's menu comes up in front of you (confirm)
- [ ] The hud bigger and brighter, readable at low health (confirm)
