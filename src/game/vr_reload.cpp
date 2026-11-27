// blackquest phase 5: reloading by hand. In vr with manual reload on (VR SETTINGS), for a gun whose magazine is
// known, the game's own reload never starts (the weapon state machine's reload request is cleared: hooks.cpp); the
// player ejects the magazine with A on the gun hand (or pulls it out with the off hand's grip), takes a fresh one at
// the chest with the off hand's grip and seats it at the magazine well, then racks with the grip at the slide or
// charging handle (the user keeps the off hand's trigger for grenades and the m16's launcher). Seating fills the
// clip from the reserve. The magazine is one of the gun's bones (each gun's own, named in its model: kParts); the
// renderer moves or hides it (vr_gun.cpp VrPoseGun) as this says
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "app/app.h"
#include "common/options.h"
#include "core/log.h"
#include "game/coop.h"
#include "game/coop_world.h"
#include "game/layout.h"
#include "xdk/hle.h"

#include "xr.h"

using namespace kernel;
using namespace layout;

namespace sound {
void Cue(int kind);  // mixer.cpp: a host sound, the hands' mechanics
uint32_t FindSample(uint32_t len, uint32_t rate);                         // mixer.cpp: a game sample seen played
void CueSample(uint32_t addr, uint32_t len, uint32_t rate, float gain);  // and played by us
void CueAdpcm(const uint8_t* p, uint32_t len, uint32_t rate, float gain);  // one read from the disc
void Mute(uint32_t len, uint32_t rate, int slot);  // a game sample not played (mixer.cpp; two slots)
}
namespace weapons {
std::string HeldCode();  // weapons.cpp
bool GunWaves(const std::string& code, std::vector<uint8_t>& out);  // the gun's .wdd from the disc
bool GunDb(const std::string& code, std::vector<uint8_t>& out);     // its .db
uint32_t HeldWeapon();
}

namespace game {

bool VrHandWorld(int hand, float* pos, float* vel);  // vr_gun.cpp: a hand's grip in the game's world, metres

// co-op: the off hand within reach of a partner's chest (its eye less 0.4 m, within 0.55 m): the magazine is handed
// to that partner (coop_world.cpp SendAmmo); game thread
bool HandOver() {
    if (!coop::Networked()) return false;
    float p[3], v[3];
    if (!VrHandWorld(0, p, v)) return false;
    for (int player = 0; player < coop::kMaxPlayers; player++) {
        if (player == coop::LocalPlayer() || !coop::Connected(player)) continue;
        coop::PartnerScope scope(player);
        float view[4][3];
        if (!coop::PartnerView(view) || coop::PartnerDown()) continue;
        float d[3] = {p[0] - view[3][0], p[1] - (view[3][1] - 0.4f), p[2] - view[3][2]};
        if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 0.55f * 0.55f) {
            coop::world::SendAmmo(player);
            return true;
        }
    }
    return false;
}
float VrViewLift();                           // vr_body.cpp
int64_t VrGameCycledMs();                     // vr_gun.cpp: the game last cycled the held gun's pump or bolt
int VrModePresses();                          // vr_body.cpp: the fire-mode button's presses
bool VrFireHeld();                            // vr_body.cpp: the fire trigger is held
bool VrLeftOnGun();                           // vr_gun.cpp
float VrLeftFore();                           // vr_gun.cpp: the off hand from the fore-grip (m)
bool VrForeAt(float* c);                      // vr_gun.cpp: where the off hand takes the gun (C)
bool VrLeftOnGun();
void VrM203CaseOut();                         // vr_gun.cpp: the m203's spent case falls out
void TestEvent(const std::string& name);      // vr_totest.cpp: the test checklist
bool VrGunAway();                             // vr_grenade.cpp: a grenade in the hand or empty
bool VrLookAngles(float& yaw, float& pitch);  // vr_body.cpp

namespace {
// when the hand last pumped or racked (ms), for the test checklist's shot right after
int64_t g_worked_ms = 0;
int64_t NowMsR() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// the weapon controller and its ammo (NOTES, Reload): the ammo object at +0xF4 has the clip (word
// +0x18) and its kind at +0x08 (the clip's size +0x60, the reserve's slot +0x64); the reserves are words at
// [controller +0xFC] by slot
constexpr uint32_t kCtrlAmmo = 0xF4, kCtrlReserves = 0xFC, kCtrlState = 0xD8, kCtrlOwner = 0xF0;
constexpr uint32_t kCtrlAmmo2 = 0xF8;  // the second ammo: the M203's on the M16 (BNS)
// the M203's rounds: its ammo's +0x14 (an int), not the clip word: a shot (0x1EFEED) takes one while its clip word
// (+0x18, 1) isn't 0; pulled empty, the game reloads it at once (state 3, to its kind's +0x98: 3) without touching
// the reserve (its slot 5 never goes down: endless). Ours: one at a time, each from the reserve
constexpr uint32_t kAmmoRounds2 = 0x14;
constexpr uint32_t kCtrlShotClock = 0xDC;  // float: seconds till it may fire again, from the interval down (NOTES)
constexpr uint32_t kAmmoClip = 0x18, kAmmoKind = 0x08, kKindSize = 0x60, kKindSlot = 0x64;

// the guns' moving parts: bones of their own in each gun's model, named there (AssualtRifle_Magazine, Pistol_Slide,
// SMG_Bullets, ...: a gun's .db lists its skeleton from World, a bone's row 3 x its place in that list; this gives
// the AK's 81 and the pistol's 114 and 117 found on the headset: NOTES 2026-11-22). The slide (a pistol's, or a
// rifle's bolt carrier with its handle) goes back along the mesh's -z by its travel. The magazine's middle, up (the
// way it goes into its well) and its top and bottom along up are measured from its vertices in the draw, or are the
// table's (from the same vertices on the disc: its middle, and its long axis where the measure's up, toward its
// highest tenth, isn't the well's)
struct GunParts {
    int mag = -1, slide = -1;
    float travel = 0;
    float mid[3] = {}, up[3] = {0, 1, 0};
    float top = 0.05f, bottom = -0.05f;
    bool measured = false;
    // the rounds on its top, a bone of their own: they go where it goes (-1 none)
    int bullets = -1;
    // where the off hand takes the slide or its handle: a point in the mesh, on this bone (so it follows the slide)
    int rack_bone = -1;
    float rack[3] = {};
    // held in the fist (upright, its top out past the thumb): its middle this far from the grip point along the fist
    // (about 0.6 of its top over its middle)
    float hold = 0.03f;
    // how far its top goes into the gun past the well's mouth (0: all but its base, a pistol's grip)
    float depth = 0;
    // how near the line up the well the held top must come to slide home (0: kSeatReach)
    float seat_reach = 0;
    // the rack zone runs from the rack point this far along this (mesh) direction, an invisible extension of a knob
    // (the user's: a knob is too small to close a fist on), taken within rack_reach of it (0: kRackReach); a slide
    // taken palm over is the point alone
    float rack_out[3] = {};
    float rack_len = 0, rack_reach = 0;
    // the bolt is worked by hand after every shot (the walther: the user's): a shot's next round waits for a rack
    bool bolt = false;
    // a revolver (the magnum: the user's): mag is the speed loader (its six rounds the bones after it), the cylinder
    // swings out on its crane, loaded from the chest, spun by hand, shut (vr_reload.cpp kCyl..)
    bool revolver = false;
    // fed a shell at a time (the shotguns: the user's): mag is the shell the off hand carries (the gun's own shell
    // bone, its reload's), loaded at its place in the gun after any number of shots up to the game's count; no
    // magazine to drop, no lock on the last shot (each shot waits for the pump, bolt above)
    bool shells = false;
    // a launcher (the user's): one round at a time (mag the rocket or grenade, shells' rules otherwise), taken with
    // the grip at `take`, drawn in the gun on the root bone while loaded. With a slide it's a tube that opens
    // forward (travel negative), worked only after a shot: forward kTubeStroke at kPumpSpeed opens it, the round
    // goes into its back, back again shuts it. Without one (the rpg) the round goes straight in and it's loaded
    bool launcher = false;
    // its action holds open on the last round, as the real gun's (user: the m16, g36, wa2000 and pistols); the rest
    // close on an empty chamber (the ak, mp5, p90; the uzi, mac-10 and m249 fire from an open bolt that rides
    // forward), a fresh magazine's rounds then waiting for a rack all the same
    bool hold_open = false;
    // the part the lock holds back, when it isn't the slide (the m16's bolt: its charging handle stays home, user);
    // -1 the slide
    int lock_bone = -1;
    int take = 0;  // where it's taken: kTakeChest, kTakeShoulder (over the left), kTakeWaist (low, near the waist)
    int root = -1;  // the gun's root bone (the round's place in the gun when the game has hidden it)
};
constexpr int kTakeChest = 0, kTakeShoulder = 1, kTakeWaist = 2;
// a rifle's handle: the zone 3 cm on from the knob's tip, 3.5 cm round it (user tests 6 and 7). Knobs from each
// slide bone's outermost vertices; mesh +x is the player's side (the game's guns eject to the left). Travels are
// guesses (the game's own reloads would show them); the pistols' frame parts move 3.7 cm against the slide
// (user 2026-11-26: wider and longer, every grab should find it: 6 cm on from the knob, 5 cm round it)
constexpr float kKnobLen = 0.06f, kKnobReach = 0.05f;
// AK-47: magazine 81 (measured on the headset: up 0.94 -0.33), its rounds 87, the bolt carrier 84 with the handle's
// knob at its front on the port side (the vertex dumps that had it painted on held only part of the gun's draw)
const GunParts kAk = {.mag = 81, .slide = 84, .travel = 0.07f, .mid = {0, -0.054f, 0.127f}, .up = {0, 0.94f, -0.33f},
                      .top = 0.100f, .bottom = -0.103f, .measured = true, .bullets = 87, .rack_bone = 84,
                      .rack = {0.031f, 0.065f, 0.132f}, .hold = 0.05f, .depth = 0.04f, .rack_out = {1, 0, 0},
                      .rack_len = kKnobLen, .rack_reach = kKnobReach};
// the pistols' slides are taken anywhere over the top of their back half, or its sides (user): the zone a line 1.4 cm
// over the slide's top from its rear end to its middle, 4 cm round it
constexpr float kSlideReach = 0.05f;
// pistol: magazine 114 in the grip, slide 117 (measured on the headset; z -0.094..0.129, its top y 0.066)
const GunParts kPistol = {.mag = 114, .slide = 117, .travel = 0.037f, .mid = {0, -0.022f, -0.057f},
                          .up = {0, 0.98f, 0.19f}, .top = 0.044f, .bottom = -0.051f, .measured = true, .rack_bone = 117,
                          .rack = {0, 0.080f, -0.094f}, .hold = 0.025f, .rack_out = {0, 0, 1}, .rack_len = 0.111f,
                          .rack_reach = kSlideReach, .hold_open = true};
// glock: magazine 117 in the grip, its rounds 114, slide 120 (z -0.090..0.101, its top y 0.060)
const GunParts kGlock = {.mag = 117, .slide = 120, .travel = 0.035f, .mid = {0, -0.012f, -0.047f},
                         .up = {0, 0.95f, 0.31f}, .top = 0.050f, .bottom = -0.078f, .measured = true, .bullets = 114,
                         .rack_bone = 120, .rack = {0, 0.074f, -0.090f}, .hold = 0.03f, .rack_out = {0, 0, 1},
                         .rack_len = 0.096f, .rack_reach = kSlideReach, .hold_open = true};
// g36c: magazine 78, the handle on 81
const GunParts kG36 = {.mag = 78, .slide = 81, .travel = 0.07f, .mid = {0, -0.055f, 0.075f}, .up = {0, 0.99f, -0.14f},
                       .top = 0.069f, .bottom = -0.069f, .rack_bone = 81, .rack = {0.025f, 0.076f, 0.100f},
                       .hold = 0.04f, .depth = 0.03f, .rack_out = {1, 0, 0}, .rack_len = kKnobLen,
                       .rack_reach = kKnobReach, .hold_open = true};
// m16: magazine 78, its rounds 81, the T handle 84 at the receiver's rear: its zone runs back from the T
// (user 2026-11-26: couldn't pull it back: the zone 1.5 cm higher, 10 cm back from the T, 7 cm round it)
const GunParts kM16 = {.mag = 78, .slide = 84, .travel = 0.07f, .mid = {0, -0.010f, 0.077f}, .up = {0, 1, 0},
                       .top = 0.056f, .bottom = -0.085f, .bullets = 81, .rack_bone = 84, .rack = {0, 0.110f, -0.100f},
                       .hold = 0.035f, .depth = 0.05f, .rack_out = {0, 0, -1}, .rack_len = 0.10f,
                       .rack_reach = 0.07f, .hold_open = true, .lock_bone = 90};
GunParts M203() {  // the m16a2 + m203: the m16's, its magazine a little apart
    GunParts p = kM16;
    p.mid[1] = -0.012f, p.mid[2] = 0.078f, p.top = 0.057f, p.bottom = -0.083f;
    return p;
}
// mp5: magazine 78, its rounds 81, the cocking lever 84 on the tube over the handguard
const GunParts kMp5 = {.mag = 78, .slide = 84, .travel = 0.07f, .mid = {0, -0.032f, 0.094f}, .up = {0, 0.94f, -0.34f},
                       .top = 0.084f, .bottom = -0.104f, .bullets = 81, .rack_bone = 84,
                       .rack = {0.030f, 0.097f, 0.208f}, .hold = 0.05f, .depth = 0.035f, .rack_out = {1, 0, 0},
                       .rack_len = kKnobLen, .rack_reach = kKnobReach};
// p90: magazine 78 lying on top (it goes down onto the gun), the handle 81 at the front of its rail
// (it lies on top and goes in downward: its way into the gun is mesh -y, its top the face against the receiver)
const GunParts kP90 = {.mag = 78, .slide = 81, .travel = 0.06f, .mid = {0, 0.067f, -0.164f}, .up = {0, -1, 0},
                       .top = 0.015f, .bottom = -0.013f, .rack_bone = 81, .rack = {0.025f, 0.028f, 0.102f},
                       .hold = 0.01f, .depth = 0.03f, .rack_out = {1, 0, 0}, .rack_len = kKnobLen,
                       .rack_reach = kKnobReach};
// uzi: magazine 114 in the grip, the handle 117
const GunParts kUzi = {.mag = 114, .slide = 117, .travel = 0.06f, .mid = {0, -0.070f, -0.028f}, .up = {0, 1, 0},
                       .top = 0.080f, .bottom = -0.080f, .rack_bone = 117, .rack = {0.029f, 0.051f, 0.029f},
                       .hold = 0.05f, .depth = 0.08f, .rack_out = {1, 0, 0}, .rack_len = kKnobLen,
                       .rack_reach = kKnobReach};
// mac-10: magazine 111 in the grip, its rounds 114, the knob 117
const GunParts kMac10 = {.mag = 111, .slide = 117, .travel = 0.06f, .mid = {0, -0.038f, -0.025f}, .up = {0, 1, 0},
                         .top = 0.046f, .bottom = -0.128f, .bullets = 114, .rack_bone = 117,
                         .rack = {0.0365f, 0.0736f, 0}, .hold = 0.03f, .depth = 0.08f, .rack_out = {1, 0, 0},
                         .rack_len = kKnobLen, .rack_reach = kKnobReach};
// walther 2000: magazine 81 (a short box behind the grip), the handle 84
const GunParts kWa2000 = {.mag = 81, .slide = 84, .travel = 0.07f, .mid = {0, -0.002f, 0.092f}, .up = {0, 1, 0},
                          .top = 0.026f, .bottom = -0.045f, .rack_bone = 84, .rack = {0.025f, 0.045f, 0.120f},
                          // (user: it slides home once its top reaches the well's bottom: the mouth 2.6 cm under the
                          // seated top, the line on 2.4 cm past it, 3 cm round it)
                          .hold = 0.02f, .depth = 0.05f, .seat_reach = 0.03f, .rack_out = {1, 0, 0}, .rack_len = kKnobLen,
                          .rack_reach = 0.07f, .bolt = true, .hold_open = true};  // the user's hand 5-6 cm off it (play log 2026-11-23)
// m249: the box 99 at the side under the feed (its belt, bones 102-111, stays in the gun), the handle 114
const GunParts kM249 = {.mag = 99, .slide = 114, .travel = 0.07f, .mid = {0.025f, 0.035f, 0.128f}, .up = {0, 1, 0},
                        .top = 0.031f, .bottom = -0.045f, .rack_bone = 114, .rack = {0.035f, 0.077f, 0.082f},
                        .hold = 0.02f, .depth = 0.03f, .rack_out = {1, 0, 0}, .rack_len = kKnobLen,
                        .rack_reach = kKnobReach};
// the shotguns (user: a shell at a time, pumped after each shot): the shell the hand carries is the gun's own (its
// reload's, lying along the gun at the loading port under the receiver: its up the barrel's way, where it slides
// in), the pump the fore-end's bone, worked back 8 cm along the barrel; taken anywhere along it (a 17 cm zone,
// 5 cm round). SPAS 12: shell 114, pump 111 (z 0.212..0.454 under the barrel)
constexpr float kPumpReach = 0.05f;
// pumping (user): with the off hand holding the fore-end (the fore-grip: two-handed aim), a shot that needs the pump
// puts it in pump mode, the hand locked on the pump: a clear rock back toward the player, at least kPumpStroke and
// at least kPumpSpeed (m/s along the barrel) at some point on the way, works it; forward again (back under a quarter
// of the stroke) chambers and it's aiming again. A slower or shorter pull moves the pump but doesn't count
constexpr float kPumpStroke = 0.07f, kPumpSpeed = 0.5f;
// a shot's pump is needed if the game starts its own pump within this long after it (the SPAS's semi-auto mode
// doesn't: user)
constexpr int64_t kCycleWait = 700;
const GunParts kSpas = {.mag = 114, .slide = 111, .travel = 0.08f, .mid = {0, 0.046f, 0.153f}, .up = {0, 0, 1},
                        .top = 0.024f, .bottom = -0.024f, .rack_bone = 111, .rack = {0, 0.010f, 0.250f},
                        .hold = 0.02f, .depth = 0.05f, .rack_out = {0, 0, 1}, .rack_len = 0.17f,
                        .rack_reach = kPumpReach, .bolt = true, .shells = true, .root = 108};
// (user 2026-11-26: both shotguns' loading points raised 10 cm, they were still too far under the receiver)
// (and the shell held 9 cm out along its axis, was 5: pinched at its brass, most of it past the fingers as in the
// user's photo; it sat deep in the fist)
// (its root: the game parks its shell bone outside its own reload, so the port's place is the root's: user, the
// entrance was 10-13 cm under the receiver)
// remington 870: shell 120, pump 117 (z 0.229..0.434); its loading gate (111) and ejection cover (114) stay put
const GunParts kRemington = {.mag = 120, .slide = 117, .travel = 0.08f, .mid = {0.001f, 0.082f, 0.148f},
                             .up = {0, 0, 1}, .top = 0.029f, .bottom = -0.029f, .rack_bone = 117,
                             .rack = {0, 0.020f, 0.250f}, .hold = 0.02f, .depth = 0.05f, .rack_out = {0, 0, 1},
                             .rack_len = 0.17f, .rack_reach = kPumpReach, .bolt = true, .shells = true};
// the magnum: the speed loader 9 (behind the cylinder, z -0.019..-0.008 round the cylinder's axis x 0.001 y 0.045)
// with its six rounds 12..27 (into the chambers to z 0.028), the cylinder 120, the crane arm 117 before it, the
// hammer 114 (bones from its .db). The loader goes in along the barrel's way, its rounds' tips first
const GunParts kMagnum = {.mag = 9, .travel = 0, .mid = {0.001f, 0.045f, -0.0135f}, .up = {0, 0, 1}, .top = 0.0415f,
                          .bottom = -0.0055f, .bullets = 12, .hold = 0.03f, .depth = 0.02f, .revolver = true};
// the tube opens this far forward (user: farther than a shotgun's pump, the same push)
constexpr float kTubeStroke = 0.10f;
// rpg-7: the rocket 111 out of the tube's front (z 0.132..0.607 on the bore at y 0.056: its back flange at the
// muzzle, the narrow bar behind the warhead z 0.18..0.28, the warhead to 0.36, the nose); root 108. Held by the bar
// (user), its back end out past the thumb, the flange to the muzzle (it goes in backward, mesh -z)
const GunParts kRpg = {.mag = 111, .travel = 0, .mid = {0, 0.056f, 0.23f}, .up = {0, 0, -1}, .top = 0.098f,
                       .bottom = -0.377f, .hold = 0, .depth = 0.04f, .shells = true, .launcher = true,
                       .take = kTakeShoulder, .root = 108};
// the grenade launcher: the shotgun's skeleton (its .db): root 105, the tube Shotgun_Pumpgrip 111 (z 0.074..0.550),
// the grenade Shotgun_Shell 114 (z 0.109..0.199, its tip forward). The tube slides forward to open, the grenade
// goes into its back tip first (mesh +z)
// (its round is our M433, 12.2 cm, on bone 3 (vr_gun.cpp): the tube opens 13 cm for it)
const GunParts kGrl = {.mag = 3, .slide = 111, .travel = -0.13f, .mid = {0, 0.018f, 0.170f},
                       .up = {0, 0, 1}, .top = 0.061f, .bottom = -0.061f, .hold = 0.02f, .depth = 0.05f,
                       .shells = true, .launcher = true, .take = kTakeWaist, .root = 105};
// the M16's M203 (BNS), while it's empty or being loaded (the gun's pseudo code "B203"; the rifle otherwise): its
// barrel G_Slide 93 (z 0.200..0.572 round y 0.0055, r 0.035) slides forward to open; the grenade is ours (its
// GrenadeShell bone 96 has no mesh: vr_gun.cpp MakeM203Grenade, z 0.200..0.300 in the barrel's back), root 75. Its
// ammo is the weapon's second (+0xF8)
// (it opens 13 cm: the round is 12.2 cm long and goes in whole)
const GunParts kM203Tube = {.mag = 96, .slide = 93, .travel = -0.13f, .mid = {0, 0.0055f, 0.261f},
                            .up = {0, 0, 1}, .top = 0.061f, .bottom = -0.061f, .hold = 0.02f, .depth = 0.05f,
                            .shells = true, .launcher = true, .take = kTakeWaist, .root = 75};
// the silver and silenced ones are the same models (their bones checked on the disc)
const std::map<std::string, GunParts> kParts = {
    {"AK1", kAk},     {"AK5", kAk},       {"PST", kPistol}, {"PS5", kPistol}, {"GK1", kGlock}, {"GK5", kGlock},
    {"ASR", kG36},    {"AS5", kG36},      {"M16", kM16},    {"M45", kM16},    {"BNS", M203()}, {"MP1", kMp5},
    {"MP5", kMp5},    {"P90", kP90},      {"P95", kP90},    {"SMG", kUzi},    {"SM5", kUzi},   {"SM3", kMac10},
    {"MC5", kMac10},  {"SNR", kWa2000},   {"SN5", kWa2000}, {"HVY", kM249},   {"HV5", kM249},
    {"MGN", kMagnum}, {"MG5", kMagnum}, {"SHG", kSpas},   {"SH5", kSpas},     {"RM1", kRemington}, {"RM5", kRemington},
    {"RPG", kRpg},    {"GRL", kGrl},      {"B203", kM203Tube},
};
// the user's zones are tight (1-2 cm past where the hand closes on the part), so the fore-grip and the magazine
// don't overlap them; reaches are from the controller's grip point
constexpr float kRackReach = 0.025f;  // metres from the rack point the off hand's grip takes the slide (or handle)
constexpr float kRackPull = 0.06f;    // metres back the hand pulls for a full rack

// the chest: a column under the headset, its top this far below the head's middle, its bottom, its radius, and
// its middle this far ahead of the head's middle
// (user, 2026-11-24: wider, and deeper toward the chest, its front edge where it was: 13 cm ahead)
constexpr float kChestTop = 0.15f, kChestBottom = 0.50f, kChestRadius = 0.13f, kChestAhead = 0.0f;
// pulling the magazine in the gun out: the palm within this far of its body (a line along its axis, from its
// middle this far down to this far up) - about its half thickness plus 2 cm
constexpr float kPullReach = 0.035f, kPullDown = 0.08f, kPullUp = 0.02f;
// the hands' sounds: the game's own, from each gun's wave data (its .wdd, loaded as one block; the game plays them by
// name from its .db's gunaudio triggers). The .db's dictionary gives each sample's place in that block, its length
// and rate (NOTES 2026-11-22). mixer.cpp remembers where every sample the game plays lives, by length and rate; any
// of the gun's own found there gives its block (its draw plays its cock, every shot its fire loop), and a sound is
// played only if its bytes are what the disc has (whole-sample hash: guns share lengths and rates, and the silver
// ones keep the same sounds at other places). A one-piece cock sound is split where it goes quiet between the pull
// and the release (its loudness every 10 ms); long silences before a sound are cut
struct GunSound {
    uint32_t off, len, rate;  // the sample: its place in the block, bytes, Hz (len 0: none)
    uint32_t from, part;      // the part played: bytes from its start, whole adpcm blocks
    uint32_t hash;            // fnv-1a of all its bytes
};
struct WaveRef {
    uint32_t off, len, rate;
};
enum Sound { kPull, kRelease, kUnload, kLoad, kSounds };
struct GunAudio {
    std::array<GunSound, kSounds> sound;
    std::vector<WaveRef> waves;  // all its samples, to find its block by
};
// generated from the disc's dictionaries (pull, release, unload, load; then the samples)
const std::map<std::string, GunAudio> kGunAudio = {
    // AK1: AkCock0 / AkCock1, AkUnld0, AkLd0
    {"AK1", {{{{0x7C00, 3420, 20000, 0, 3420, 0x2E2819F6}, {0x6B00, 4212, 24000, 0, 4212, 0x92805E13}, {0x4000, 3492, 24000, 0, 3492, 0x5D2F0A77}, {0x4E00, 4248, 20000, 0, 4248, 0x50A04752}}},
            {{0x0000, 14076, 16000}, {0x3700, 2088, 16000}, {0x4000, 3492, 24000}, {0x4E00, 4248, 20000}, {0x5F00, 2880, 20000}, {0x6B00, 4212, 24000}, {0x7C00, 3420, 20000}, {0x8A00, 3492, 32000}, {0x9800, 2088, 32000}, {0xA100, 2304, 32000}}}},
    // AK5: AkCock0 / AkCock1, AkUnld0, AkLd0
    {"AK5", {{{{0x7C00, 3420, 20000, 0, 3420, 0x2E2819F6}, {0x6B00, 4212, 24000, 0, 4212, 0x92805E13}, {0x4000, 3492, 24000, 0, 3492, 0x5D2F0A77}, {0x4E00, 4248, 20000, 0, 4248, 0x50A04752}}},
            {{0x0000, 14076, 16000}, {0x3700, 2088, 16000}, {0x4000, 3492, 24000}, {0x4E00, 4248, 20000}, {0x5F00, 2880, 20000}, {0x6B00, 4212, 24000}, {0x7C00, 3420, 20000}, {0x8A00, 3492, 32000}, {0x9800, 2088, 32000}, {0xA100, 2304, 32000}}}},
    // AS5: Cock split, UnLd0, Ld0
    {"AS5", {{{{0x3100, 10080, 32000, 0, 4428, 0xE20062FF}, {0x3100, 10080, 32000, 4428, 5652, 0xE20062FF}, {0x5900, 8028, 32000, 0, 8028, 0x1CD8F19D}, {0x7900, 9468, 32000, 0, 9468, 0x3E04C083}}},
            {{0x0000, 9864, 16000}, {0x2700, 2484, 16000}, {0x3100, 10080, 32000}, {0x5900, 8028, 32000}, {0x7900, 9468, 32000}, {0x9E00, 2304, 32000}}}},
    // ASR: Cock split, UnLd0, Ld0
    {"ASR", {{{{0x3E00, 10080, 32000, 0, 4428, 0xE20062FF}, {0x3E00, 10080, 32000, 4428, 5652, 0xE20062FF}, {0x6600, 8028, 32000, 0, 8028, 0x1CD8F19D}, {0x8600, 9468, 32000, 0, 9468, 0x3E04C083}}},
            {{0x0000, 13212, 16000}, {0x3400, 2484, 16000}, {0x3E00, 10080, 32000}, {0x6600, 8028, 32000}, {0x8600, 9468, 32000}, {0xAB00, 2304, 32000}, {0xB500, 3816, 44100}, {0xC400, 2844, 20000}}}},
    // BNS: M16Cock0 split, M16UnLd0, M16Ld0
    {"BNS", {{{{0x4700, 10584, 24000, 1296, 4284, 0x6E56549C}, {0x4700, 10584, 24000, 6084, 4500, 0x6E56549C}, {0x9C00, 4068, 24000, 0, 4068, 0x0EA608D5}, {0x7100, 5184, 24000, 0, 5184, 0xC3C300CF}}},
            {{0x0000, 14112, 16000}, {0x3800, 3708, 16000}, {0x4700, 10584, 24000}, {0x7100, 5184, 24000}, {0x8600, 5400, 24000}, {0x9C00, 4068, 24000}, {0xAC00, 2304, 32000}, {0xB600, 10944, 20000}}}},
    // GK1: GkCock0 split, GkUnld0, Gkld0
    {"GK1", {{{{0x3400, 4320, 32000, 0, 1584, 0x7F632B14}, {0x3400, 4320, 32000, 1584, 2736, 0x7F632B14}, {0x4500, 4284, 32000, 0, 4284, 0x90B12E78}, {0x5600, 8028, 32000, 0, 8028, 0x5E49EA0B}}},
            {{0x0000, 10332, 16000}, {0x2900, 2628, 16000}, {0x3400, 4320, 32000}, {0x4500, 4284, 32000}, {0x5600, 8028, 32000}, {0x7600, 3564, 44100}}}},
    // GK5: GkCock0 split, GkUnld0, Gkld0
    {"GK5", {{{{0x3400, 4320, 32000, 0, 1584, 0x7F632B14}, {0x3400, 4320, 32000, 1584, 2736, 0x7F632B14}, {0x4500, 4284, 32000, 0, 4284, 0x90B12E78}, {0x5600, 8028, 32000, 0, 8028, 0x5E49EA0B}}},
            {{0x0000, 10332, 16000}, {0x2900, 2628, 16000}, {0x3400, 4320, 32000}, {0x4500, 4284, 32000}, {0x5600, 8028, 32000}, {0x7600, 3564, 44100}}}},
    // HV5: HvMgnCk0 / HvMgnCk1 (from 0.28 s), HvMgnULd0, HvMgnLd0 (its first 0.4 s)
    {"HV5", {{{{0xAC00, 2952, 20000, 0, 2952, 0x3070236B}, {0x9200, 6444, 20000, 3096, 3348, 0x6CDA777D}, {0x7A00, 3276, 20000, 0, 3276, 0x0A7C232F}, {0x3900, 16488, 20000, 0, 4428, 0x97A3F090}}},
            {{0x0000, 10692, 16000}, {0x2A00, 3672, 16000}, {0x3900, 16488, 20000}, {0x7A00, 3276, 20000}, {0x8700, 2664, 20000}, {0x9200, 6444, 20000}, {0xAC00, 2952, 20000}}}},
    // HVY: HvMgnCk0 / HvMgnCk1 (from 0.28 s), HvMgnULd0, HvMgnLd0 (its first 0.4 s)
    {"HVY", {{{{0xAC00, 2952, 20000, 0, 2952, 0x3070236B}, {0x9200, 6444, 20000, 3096, 3348, 0x6CDA777D}, {0x7A00, 3276, 20000, 0, 3276, 0x0A7C232F}, {0x3900, 16488, 20000, 0, 4428, 0x97A3F090}}},
            {{0x0000, 10692, 16000}, {0x2A00, 3672, 16000}, {0x3900, 16488, 20000}, {0x7A00, 3276, 20000}, {0x8700, 2664, 20000}, {0x9200, 6444, 20000}, {0xAC00, 2952, 20000}}}},
    // M16: M16Cock0 split, M16UnLd0, M16Ld0
    {"M16", {{{{0x4700, 10584, 24000, 1296, 4284, 0x6E56549C}, {0x4700, 10584, 24000, 6084, 4500, 0x6E56549C}, {0xA600, 4068, 24000, 0, 4068, 0x0EA608D5}, {0x7B00, 5184, 24000, 0, 5184, 0xC3C300CF}}},
            {{0x0000, 14112, 16000}, {0x3800, 3708, 16000}, {0x4700, 10584, 24000}, {0x7100, 2304, 32000}, {0x7B00, 5184, 24000}, {0x9000, 5400, 24000}, {0xA600, 4068, 24000}}}},
    // M45: M16Cock0 split, M16UnLd0, M16Ld0
    {"M45", {{{{0x4700, 10584, 24000, 1296, 4284, 0x6E56549C}, {0x4700, 10584, 24000, 6084, 4500, 0x6E56549C}, {0xA600, 4068, 24000, 0, 4068, 0x0EA608D5}, {0x7B00, 5184, 24000, 0, 5184, 0xC3C300CF}}},
            {{0x0000, 14112, 16000}, {0x3800, 3708, 16000}, {0x4700, 10584, 24000}, {0x7100, 2304, 32000}, {0x7B00, 5184, 24000}, {0x9000, 5400, 24000}, {0xA600, 4068, 24000}}}},
    // MC5: Cock split, UnLd0, Ld0
    {"MC5", {{{{0x0F00, 10728, 32000, 0, 3528, 0x5A3BB6DF}, {0x0F00, 10728, 32000, 4248, 6480, 0x5A3BB6DF}, {0x5D00, 7632, 32000, 0, 7632, 0x5411B702}, {0x3900, 8964, 32000, 0, 8964, 0xF2D330E7}}},
            {{0x0000, 1620, 16000}, {0x0700, 1800, 16000}, {0x0F00, 10728, 32000}, {0x3900, 8964, 32000}, {0x5D00, 7632, 32000}, {0x7B00, 2304, 32000}}}},
    // MP1: Mp1Cock0 / Mp1Tap0, Mp1Unld0, Mp1Ld0
    {"MP1", {{{{0x2E00, 2448, 24000, 0, 2448, 0xCAF94164}, {0x6500, 1548, 24000, 0, 1548, 0xE35032EB}, {0x3800, 5256, 24000, 0, 5256, 0xD5391FD9}, {0x4D00, 3456, 24000, 0, 3456, 0x29EC2E9A}}},
            {{0x0000, 9252, 16000}, {0x2500, 2124, 16000}, {0x2E00, 2448, 24000}, {0x3800, 5256, 24000}, {0x4D00, 3456, 24000}, {0x5B00, 2304, 32000}, {0x6500, 1548, 24000}}}},
    // MP5: Mp1Cock0 / Mp1Tap0, Mp1Unld0, Mp1Ld0
    {"MP5", {{{{0x2D00, 2448, 24000, 0, 2448, 0xCAF94164}, {0x6400, 1548, 24000, 0, 1548, 0xE35032EB}, {0x3700, 5256, 24000, 0, 5256, 0xD5391FD9}, {0x4C00, 3456, 24000, 0, 3456, 0x29EC2E9A}}},
            {{0x0000, 9252, 16000}, {0x2500, 1800, 16000}, {0x2D00, 2448, 24000}, {0x3700, 5256, 24000}, {0x4C00, 3456, 24000}, {0x5A00, 2304, 32000}, {0x6400, 1548, 24000}}}},
    // P90: P90Cock0 split, P90Unld0, P90Ld0
    {"P90", {{{{0x3D00, 6480, 20000, 0, 2592, 0xCD6D7398}, {0x3D00, 6480, 20000, 2592, 3888, 0xCD6D7398}, {0x2400, 6156, 24000, 0, 6156, 0x7C4C5E61}, {0x6B00, 4068, 24000, 0, 4068, 0xA1327656}}},
            {{0x0000, 7236, 16000}, {0x1D00, 1620, 16000}, {0x2400, 6156, 24000}, {0x3D00, 6480, 20000}, {0x5700, 4932, 24000}, {0x6B00, 4068, 24000}, {0x7B00, 2304, 32000}}}},
    // P95: P90Cock0 split, P90Unld0, P90Ld0
    {"P95", {{{{0x3D00, 6480, 20000, 0, 2592, 0xCD6D7398}, {0x3D00, 6480, 20000, 2592, 3888, 0xCD6D7398}, {0x2400, 6156, 24000, 0, 6156, 0x7C4C5E61}, {0x6B00, 4068, 24000, 0, 4068, 0xA1327656}}},
            {{0x0000, 7236, 16000}, {0x1D00, 1620, 16000}, {0x2400, 6156, 24000}, {0x3D00, 6480, 20000}, {0x5700, 4932, 24000}, {0x6B00, 4068, 24000}, {0x7B00, 2304, 32000}}}},
    // PS5: PstBrch0 / PstBbrch1, PstUnld0, PstLd0
    {"PS5", {{{{0x5E00, 5436, 32000, 0, 5436, 0xE746C894}, {0x4A00, 5112, 32000, 0, 5112, 0x03BF92D7}, {0x7E00, 9036, 32000, 0, 9036, 0x68CE3699}, {0xCF00, 6552, 24000, 0, 6552, 0x9A9A6C24}}},
            {{0x0000, 15264, 16000}, {0x3C00, 3348, 16000}, {0x4A00, 5112, 32000}, {0x5E00, 5436, 32000}, {0x7400, 2484, 24000}, {0x7E00, 9036, 32000}, {0xA200, 5400, 24000}, {0xB800, 5652, 32000}, {0xCF00, 6552, 24000}}}},
    // PST: PstBrch0 / PstBbrch1, PstUnld0, PstLd0
    {"PST", {{{{0x5E00, 5436, 32000, 0, 5436, 0xE746C894}, {0x4A00, 5112, 32000, 0, 5112, 0x03BF92D7}, {0x7400, 9036, 32000, 0, 9036, 0x68CE3699}, {0xC500, 6552, 24000, 0, 6552, 0x9A9A6C24}}},
            {{0x0000, 15264, 16000}, {0x3C00, 3348, 16000}, {0x4A00, 5112, 32000}, {0x5E00, 5436, 32000}, {0x7400, 9036, 32000}, {0x9800, 5400, 24000}, {0xAE00, 5652, 32000}, {0xC500, 6552, 24000}, {0xDF00, 2484, 24000}}}},
    // SM3: Cock split, UnLd0, Ld0
    {"SM3", {{{{0x1700, 10728, 32000, 0, 3528, 0x5A3BB6DF}, {0x1700, 10728, 32000, 4248, 6480, 0x5A3BB6DF}, {0x6500, 7632, 32000, 0, 7632, 0x5411B702}, {0x4100, 8964, 32000, 0, 8964, 0xF2D330E7}}},
            {{0x0000, 3672, 16000}, {0x0F00, 1800, 16000}, {0x1700, 10728, 32000}, {0x4100, 8964, 32000}, {0x6500, 7632, 32000}, {0x8300, 2304, 32000}}}},
    // SM5: Cock split, UnLd0, Ld0
    {"SM5", {{{{0x4F00, 10728, 32000, 0, 3528, 0x5A3BB6DF}, {0x4F00, 10728, 32000, 4248, 6480, 0x5A3BB6DF}, {0x0D00, 7632, 32000, 0, 7632, 0x5411B702}, {0x2B00, 8964, 32000, 0, 8964, 0xF2D330E7}}},
            {{0x0000, 1404, 16000}, {0x0600, 1620, 16000}, {0x0D00, 7632, 32000}, {0x2B00, 8964, 32000}, {0x4F00, 10728, 32000}, {0x7900, 2304, 32000}}}},
    // SMG: Cock split, UnLd0, Ld0
    {"SMG", {{{{0x6D00, 10728, 32000, 0, 3528, 0x5A3BB6DF}, {0x6D00, 10728, 32000, 4248, 6480, 0x5A3BB6DF}, {0x2B00, 7632, 32000, 0, 7632, 0x5411B702}, {0x4900, 8964, 32000, 0, 8964, 0xF2D330E7}}},
            {{0x0000, 9000, 16000}, {0x2400, 1620, 16000}, {0x2B00, 7632, 32000}, {0x4900, 8964, 32000}, {0x6D00, 10728, 32000}, {0x9700, 2304, 32000}}}},
    // RM1: Cock0 / Cock1 (pump back / forward), none, Ld0 (a shell in)
    {"RM1", {{{{0x4600, 5580, 44100, 0, 5580, 0xA9061860}, {0x5C00, 3240, 24000, 0, 3240, 0x053F8787}, {}, {0x6900, 3204, 24000, 0, 3204, 0x803CF044}}},
            {{0x0000, 14076, 16000}, {0x3700, 3672, 16000}, {0x4600, 5580, 44100}, {0x5C00, 3240, 24000}, {0x6900, 3204, 24000}, {0x7600, 3996, 24000}}}},
    // RM5: Cock0 / Cock1 (pump back / forward), none, Ld0 (a shell in)
    {"RM5", {{{{0x4600, 5580, 44100, 0, 5580, 0xA9061860}, {0x5C00, 3240, 24000, 0, 3240, 0x053F8787}, {}, {0x6900, 3204, 24000, 0, 3204, 0x803CF044}}},
            {{0x0000, 14076, 16000}, {0x3700, 3672, 16000}, {0x4600, 5580, 44100}, {0x5C00, 3240, 24000}, {0x6900, 3204, 24000}, {0x7600, 3996, 24000}}}},
    // SH5: Cock0 split (pump back / forward, at 0.23 s), none, Ld1 (a shell in)
    {"SH5", {{{{0x4F00, 7452, 24000, 0, 3024, 0xCEC65153}, {0x4F00, 7452, 24000, 3024, 4428, 0xCEC65153}, {}, {0x6D00, 7560, 20000, 0, 7560, 0x2DDC683F}}},
            {{0x0000, 16380, 16000}, {0x4000, 3672, 16000}, {0x4F00, 7452, 24000}, {0x6D00, 7560, 20000}}}},
    // SHG: Cock0 split (pump back / forward, at 0.23 s), none, Ld1 (a shell in)
    {"SHG", {{{{0x4F00, 7452, 24000, 0, 3024, 0xCEC65153}, {0x4F00, 7452, 24000, 3024, 4428, 0xCEC65153}, {}, {0x6D00, 7560, 20000, 0, 7560, 0x2DDC683F}}},
            {{0x0000, 16380, 16000}, {0x4000, 3672, 16000}, {0x4F00, 7452, 24000}, {0x6D00, 7560, 20000}}}},
    // MG5: none / MgnLd3 (the cylinder shut), MgnUnLd0 (out), MgnLd0 (the rounds in)
    {"MG5", {{{{}, {0xA300, 7740, 44100, 0, 7740, 0xCE16E017}, {0x4B00, 6228, 44100, 0, 6228, 0xB7B25D10}, {0x6400, 6660, 44100, 0, 6660, 0x8A737FA6}}},
            {{0x0000, 15264, 16000}, {0x3C00, 3672, 16000}, {0x4B00, 6228, 44100}, {0x6400, 6660, 44100}, {0x7F00, 4788, 44100}, {0x9200, 4176, 44100}, {0xA300, 7740, 44100}, {0xC200, 5508, 24000}}}},
    // MGN: none / MgnLd3 (the cylinder shut), MgnUnLd0 (out), MgnLd0 (the rounds in)
    {"MGN", {{{{}, {0xA300, 7740, 44100, 0, 7740, 0xCE16E017}, {0x4B00, 6228, 44100, 0, 6228, 0xB7B25D10}, {0x6400, 6660, 44100, 0, 6660, 0x8A737FA6}}},
            {{0x0000, 15264, 16000}, {0x3C00, 3672, 16000}, {0x4B00, 6228, 44100}, {0x6400, 6660, 44100}, {0x7F00, 4788, 44100}, {0x9200, 4176, 44100}, {0xA300, 7740, 44100}, {0xC200, 5508, 24000}}}},
    // SN5: none / Cock0, Unld0 (from 0.3 s), Ld0
    {"SN5", {{{{}, {0x7A00, 6156, 32000, 0, 6156, 0xEC278F45}, {0x5600, 5112, 20000, 3312, 1800, 0x6A6ECB74}, {0x6A00, 3996, 20000, 0, 3996, 0x03DCA205}}},
            {{0x0000, 18036, 16000}, {0x4700, 3672, 16000}, {0x5600, 5112, 20000}, {0x6A00, 3996, 20000}, {0x7A00, 6156, 32000}}}},
    // SNR: none / Cock0, Unld0 (from 0.3 s), Ld0
    {"SNR", {{{{}, {0x7A00, 6156, 32000, 0, 6156, 0xEC278F45}, {0x5600, 5112, 20000, 3312, 1800, 0x6A6ECB74}, {0x6A00, 3996, 20000, 0, 3996, 0x03DCA205}}},
            {{0x0000, 18036, 16000}, {0x4700, 3672, 16000}, {0x5600, 5112, 20000}, {0x6A00, 3996, 20000}, {0x7A00, 6156, 32000}, {0x9300, 3852, 24000}, {0xA300, 4644, 24000}, {0xB600, 4644, 24000}, {0xC900, 3204, 24000}}}},
};
constexpr float kSoundGain = 0.8f;
const char* const kSoundNames[kSounds] = {"pull", "release", "unload", "load"};

uint32_t Fnv(const uint8_t* p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

// where the gun's sound is now: the sample itself where the game played it, else where any of its samples puts its
// block; 0 if not found
uint32_t FindGunSound(const GunAudio& g, const GunSound& s) {
    auto is = [&](uint32_t at) {
        const uint8_t* p = static_cast<const uint8_t*>(HostPtr(at));
        return p && HostPtr(at + s.len - 1) && Fnv(p, s.len) == s.hash;
    };
    if (uint32_t a = sound::FindSample(s.len, s.rate); a && is(a)) return a;
    for (const WaveRef& w : g.waves)
        if (uint32_t a = sound::FindSample(w.len, w.rate)) {
            uint32_t at = a - w.off + s.off;
            if (is(at)) return at;
        }
    return 0;
}

// a gun's sound by its name in the .db's wave dictionary (entries: u32 0x0043CDC0, rate, 4, length, ..., the sample's
// offset in the .wdd at +0x40, its name at +0x60), played from the .wdd as read from the disc. Both files read once
// a gun. false: no such sound
bool PlayNamedSound(const std::string& gun, const char* name) {
    const std::string code = gun == "B203" ? "BNS" : gun;  // the M203's sounds are in the M16's files
    struct Files {
        std::vector<uint8_t> db, wdd;
    };
    static std::map<std::string, Files> files;
    auto f = files.find(code);
    if (f == files.end()) {
        Files n;
        if (!weapons::GunDb(code, n.db) || !weapons::GunWaves(code, n.wdd)) n = {};
        f = files.emplace(code, std::move(n)).first;
    }
    const std::vector<uint8_t>& db = f->second.db;
    const std::vector<uint8_t>& wdd = f->second.wdd;
    for (size_t o = 0; o + 0x61 <= db.size(); o += 4) {
        uint32_t magic;
        std::memcpy(&magic, &db[o], 4);
        if (magic != 0x0043CDC0) continue;
        // (the fields before it can hold letters: AK1's FireSelect entry has a 'd'); some carry the gun's prefix
        // ("Rpg-Ld0", "Grdlch-Cock"): the part after its dash
        const char* at = reinterpret_cast<const char*>(&db[o + 0x60]);
        std::string full(at, strnlen(at, std::min<size_t>(32, db.size() - o - 0x60)));
        size_t dash = full.rfind('-');
        if ((dash == std::string::npos ? full : full.substr(dash + 1)) != name) continue;
        uint32_t rate, len, off;
        std::memcpy(&rate, &db[o + 4], 4);
        std::memcpy(&len, &db[o + 12], 4);
        std::memcpy(&off, &db[o + 0x40], 4);
        len -= len % 36;  // whole adpcm blocks
        if (!len || uint64_t(off) + len > wdd.size()) return false;
        sound::CueAdpcm(wdd.data() + off, len, rate, kSoundGain);
        LOGR("vr reload: %s %s from the disc (%u bytes at %u Hz)", code.c_str(), name, len, rate);
        return true;
    }
    LOG_FIRST(8, "vr reload: %s has no sound %s", code.c_str(), name);
    return false;
}

void PlaySound(const std::string& code, Sound which) {
    auto it = kGunAudio.find(code);
    if (it == kGunAudio.end()) {
        // none tabled (the launchers): the usual names (the grenade launcher's Grdlch-Cock opens, -C0 shuts; the
        // rpg's Rpg-Ld0 loads)
        static const char* const kNames[kSounds][2] = {{"Cock", "Cock0"}, {"C0", "Cock"}, {"UnLd0", "Unld0"},
                                                       {"Ld0", "Attach0"}};
        for (const char* n : kNames[which])
            if (PlayNamedSound(code, n)) return;
    }
    if (it != kGunAudio.end()) {
        const GunSound& s = it->second.sound[which];
        if (!s.len) return;  // none for this part (the walther's whole rack plays on its release)
        if (uint32_t at = FindGunSound(it->second, s)) {
            sound::CueSample(at + s.from, s.part, s.rate, kSoundGain);
            LOGR("vr reload: %s %s: the game's sample at %08X", code.c_str(), kSoundNames[which], at);
            return;
        }
        // none of the gun's samples played yet (a pistol never fired, an Uzi's draw plays nothing): from its .wdd on
        // the disc, read once, the same bytes (checked by the same hash)
        static std::map<std::string, std::vector<uint8_t>> files;
        auto f = files.find(code);
        if (f == files.end()) {
            std::vector<uint8_t> d;
            if (!weapons::GunWaves(code, d)) LOGR("vr reload: %s: no .wdd on the disc", code.c_str());
            f = files.emplace(code, std::move(d)).first;
        }
        const std::vector<uint8_t>& d = f->second;
        if (uint64_t(s.off) + s.len <= d.size() && Fnv(d.data() + s.off, s.len) == s.hash) {
            sound::CueAdpcm(d.data() + s.off + s.from, s.part, s.rate, kSoundGain);
            LOGR("vr reload: %s %s: from the disc's .wdd at +%X", code.c_str(), kSoundNames[which], s.off);
            return;
        }
        LOGR("vr reload: %s %s sound not found (%u bytes at %u Hz)", code.c_str(), kSoundNames[which], s.len, s.rate);
    }
    if (which == kPull || which == kRelease) sound::Cue(which == kRelease ? 1 : 0);  // ours: a rack's clicks
}
void PlayRack(const std::string& code, bool release) { PlaySound(code, release ? kRelease : kPull); }
// seating (the user's rule): the held magazine's top within this far of the well (the line from its mouth up to
// where the top sits seated), whatever angle it's held at; then it slides home along the well in this long
constexpr float kSeatReach = 0.02f, kSeatTime = 0.1f;
// a magazine's base: what's left out of a well that takes all the rest (a pistol's)
constexpr float kMagBase = 0.015f;
// the fist's axis (little finger to thumb) against the controller's aim: tilted up from its -z toward its +y
constexpr float kRodTilt = 55 * 3.14159265f / 180;
// a shell goes in with its tip within this far of the loading port's line (the user's tight zones are for magazines)
constexpr float kShellReach = 0.03f;
constexpr float kShellBall = 0.08f, kShellAbove = 0.02f;  // the shotguns' loading ball (user: a few inches round)
// knocking the old magazine out with the fresh one: within this far of its well, the hand this fast (m/s)
constexpr float kKnockReach = 0.12f, kKnockSpeed = 0.6f;

std::atomic<uint32_t> g_ctrl{0};  // the weapon in the player's hand (its state machine's controller)
// VrMag: 0 in the gun (hand empty), 1 out (falling, then gone; hand empty), 2 in the off hand (the well empty),
// 3 a fresh one in the off hand with the old one still in the gun (to knock it out with)
std::atomic<int> g_state{0};
std::atomic<bool> g_from_hand{false};  // the magazine falling now left the hand (else it left the gun)
std::atomic<int> g_bone{-1};  // the held gun's magazine bone, -1 none (not drawn moving)
GunParts g_parts;  // the held gun's (game thread writes, render reads: copied under g_lock)
// the slide: locked back when the gun ran dry; a magazine seated then waits for a rack (its rounds pending);
// the off hand's grip near it pulls it (0 forward .. 1 back)
std::atomic<float> g_slide{0};
std::atomic<float> g_handle{0};  // the hand's pull alone (a lock_bone gun's handle: the lock leaves it home)
bool g_locked = false;
uint16_t g_pending = 0;
bool g_spent = false;  // a bolt worked by hand, fired: the chamber empty, the bolt forward (the rounds wait for a rack)
bool g_racking = false;
float g_rack_from = 0;
// each gun's slide while it's put away (a switch mustn't carry one gun's lock or pending rounds to the next)
struct SlideKeep { bool locked = false, spent = false; uint16_t pending = 0; };
std::map<std::string, SlideKeep> g_kept;
std::atomic<bool> g_manual{false};  // the held gun is reloaded by hand
// the revolver (user): the gun hand's A lets the cylinder go (the latch); it swings out on its crane with the gun
// angled down (the muzzle under kCylDown) or rolled counter-clockwise past kCylRoll; out, a speed loader from the
// chest (grip: thumb and fingers) seats its rounds, the off hand's swipe across it spins it, and a grip on it with the
// off hand, or a clockwise flick of the gun hand, shuts it. Its rounds can't fire while it's out (they wait)
constexpr float kCylDown = -45 * 3.14159265f / 180, kCylRoll = -45 * 3.14159265f / 180;
constexpr float kCylFlick = 6.0f;      // rad/s clockwise roll that shuts it
constexpr float kCylReach = 0.08f;     // the off hand this near it spins or shuts it
constexpr float kCylRadius = 0.02f;    // where a swipe turns it (m): its speed over this is the spin's
std::atomic<float> g_cyl_out{0};       // the cylinder's swing, 0 shut .. 1 out (eased: drawn by vr_gun.cpp)
std::atomic<float> g_cyl_spin{0};      // its turn about its own axis, radians
std::atomic<bool> g_cyl_rounds{false};  // rounds in it (drawn in its chambers)
bool g_cyl_open = false, g_cyl_latch = false;
float g_cyl_rate = 0;  // spin, rad/s
// reloaded by the game still: launchers (phase 5 later) and the magnum (a revolver's speed loader: the user's, not
// like the pistols)
const char* const kGameReload[] = {"none"};
std::mutex g_lock;
float g_well[3];  // the magazine's middle where it sits in the gun, C (renderer, each gun draw)
float g_axis[3] = {0, 1, 0};  // the way it goes into the gun (its up), C
float g_mag_top = 0.05f, g_mag_bottom = -0.05f;  // its top and bottom from its middle along its up (measured)
// seating: the magazine sliding home from this far out (along the well), since then
bool g_seating = false;
float g_seat_from = 0;
std::chrono::steady_clock::time_point g_seat_at;
float g_throw[3] = {};        // a thrown magazine's velocity, C
std::atomic<float> g_insert{-1};
float g_rack_at[3], g_rack_to[3];  // the rack zone's ends in C (renderer, each gun draw)
float g_rack_up[3], g_rack_fwd[3];  // the rack part's up and forward (to the muzzle) in C
bool g_rack_ok = false;
std::atomic<bool> g_near_rack{false};  // the off hand at a part or the chest (the fore-grip mustn't take that grip)
std::atomic<bool> g_rack_held{false};
bool g_rack_back_cued = false;
bool g_well_ok = false;
std::chrono::steady_clock::time_point g_out_at;

uint32_t Player() { return R32(kWorld) + kWorldPlayer; }
bool On() { return xr::Active() && Opt().vr_manual_reload; }

struct Ammo {
    uint32_t ctrl = 0, ammo = 0, reserve = 0;  // reserve: the word's address
    uint32_t size = 0;
    uint32_t clip = kAmmoClip;  // where its rounds are (the M203's: kAmmoRounds2)
};
bool ReadAmmo(Ammo& a, bool second = false) {
    if (!R32(kWorld)) return false;
    a.ctrl = weapons::HeldWeapon();
    if (!a.ctrl) return false;
    a.ammo = R32(a.ctrl + (second ? kCtrlAmmo2 : kCtrlAmmo));
    a.clip = second ? kAmmoRounds2 : kAmmoClip;
    uint32_t kind = a.ammo ? R32(a.ammo + kAmmoKind) : 0, pool = R32(a.ctrl + kCtrlReserves);
    if (!kind || !pool) return false;
    a.size = R32(kind + kKindSize);
    a.reserve = pool + R32(kind + kKindSlot) * 2;
    return a.size > 0 && a.size < 1000;
}


// the chest: a column under the headset, a little ahead of the head's middle, turned with the head's yaw (local
// space)
bool NearChest(const xr::Hand& h, const float* head) {
    float hy, hp;
    if (!xr::HeadAngles(hy, hp)) return false;
    float dx = h.pos[0] - (head[0] - std::sin(hy) * kChestAhead), dz = h.pos[2] - (head[2] - std::cos(hy) * kChestAhead);
    float down = head[1] - h.pos[1];
    return dx * dx + dz * dz < kChestRadius * kChestRadius && down > kChestTop && down < kChestBottom;
}

// over the left shoulder (a rocket: user): a ball just above it, a little behind the headset (the weapon switch's
// over the right, mirrored, a little bigger)
constexpr float kLShoulderRight = -0.15f, kLShoulderBack = 0.08f, kLShoulderUp = -0.08f, kLShoulderReach = 0.14f;
bool NearLeftShoulder(const xr::Hand& h, const float* head) {
    float hy, hp;
    if (!xr::HeadAngles(hy, hp)) return false;
    float d[3] = {h.pos[0] - head[0], h.pos[1] - head[1], h.pos[2] - head[2]};
    float right = d[0] * std::cos(hy) - d[2] * std::sin(hy), back = d[0] * std::sin(hy) + d[2] * std::cos(hy);
    float e[3] = {right - kLShoulderRight, back - kLShoulderBack, d[1] - kLShoulderUp};
    return e[0] * e[0] + e[1] * e[1] + e[2] * e[2] < kLShoulderReach * kLShoulderReach;
}
// low by the waist (grenades: user): a column under the chest's, wider
constexpr float kWaistTop = 0.45f, kWaistBottom = 0.85f, kWaistRadius = 0.18f, kWaistAhead = 0.04f;
bool NearWaist(const xr::Hand& h, const float* head) {
    float hy, hp;
    if (!xr::HeadAngles(hy, hp)) return false;
    float dx = h.pos[0] - (head[0] - std::sin(hy) * kWaistAhead), dz = h.pos[2] - (head[2] - std::cos(hy) * kWaistAhead);
    float down = head[1] - h.pos[1];
    return dx * dx + dz * dz < kWaistRadius * kWaistRadius && down > kWaistTop && down < kWaistBottom;
}

// a hand's grip position in C (the head angles the camera was built from)
void PalmInCamera(const xr::Hand& h, float yaw, float pitch, float* c) {
    float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    float U[9] = {cy, sy * sp, sy * cp, 0, cp, -sp, -sy, cy * sp, cy * cp};
    float p[3] = {h.pos[0], h.pos[1] + xr::kEyeRaise + VrViewLift(), h.pos[2]};
    for (int i = 0; i < 3; i++) c[i] = U[i] * p[0] + U[3 + i] * p[1] + U[6 + i] * p[2];
}

// the magazine held in a hand, in C: its middle, its up (the fist's axis, out past the thumb) and its front (the
// controller's forward, square to that). Upright in the fist, as a magazine is carried to a well
bool HeldMagOf(const xr::Hand& h, float yaw, float pitch, float hold, float* mid, float* up, float* front) {
    if (!h.ok || !h.aim_ok) return false;
    float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    float U[9] = {cy, sy * sp, sy * cp, 0, cp, -sp, -sy, cy * sp, cy * cp};
    const float* q = h.aim;
    float fl[3] = {-2 * (q[0] * q[2] + q[3] * q[1]), -2 * (q[1] * q[2] - q[3] * q[0]),
                   -(1 - 2 * (q[0] * q[0] + q[1] * q[1]))};
    float ul[3] = {2 * (q[0] * q[1] - q[3] * q[2]), 1 - 2 * (q[0] * q[0] + q[2] * q[2]), 2 * (q[1] * q[2] + q[3] * q[0])};
    float c = std::cos(kRodTilt), s = std::sin(kRodTilt), rl[3], frl[3];
    for (int i = 0; i < 3; i++) rl[i] = c * fl[i] + s * ul[i], frl[i] = -s * fl[i] + c * ul[i];
    // front: square to the axis, the forward side of the fist (fl rotated on by the same tilt, negated: forward-down)
    for (int i = 0; i < 3; i++) frl[i] = -frl[i];
    float p[3];
    PalmInCamera(h, yaw, pitch, p);
    for (int i = 0; i < 3; i++) {
        up[i] = U[i] * rl[0] + U[3 + i] * rl[1] + U[6 + i] * rl[2];
        front[i] = U[i] * frl[0] + U[3 + i] * frl[1] + U[6 + i] * frl[2];
    }
    for (int i = 0; i < 3; i++) mid[i] = p[i] + up[i] * hold;
    return true;
}

}  // namespace

void DevModeDump();
// render thread: how far up the fist the held gun's magazine's middle is (vr_gun.cpp draws it there)
float VrMagHold() {
    std::lock_guard lock(g_lock);
    return g_parts.hold;
}
// the fist's axis tilt (vr_gun.cpp's held magazine matches HeldMagOf)
float VrRodTilt() { return kRodTilt; }

// any thread: where the held gun's magazine is (VrPoseGun), and its bone (-1: not ours to move)
int VrMagState() { return g_state.load(std::memory_order_relaxed); }
int VrMagBone() { return g_bone.load(std::memory_order_relaxed); }
float VrMagOutSeconds() {
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - g_out_at).count();
}
// render thread: the well this frame (every gun draw, the magazine in it or not: the gun moves with the hand), and
// the magazine's top and bottom from its middle along its up
void VrMagWell(const float* c, const float* axis, float top, float bottom) {
    std::lock_guard lock(g_lock);
    std::memcpy(g_well, c, 12);
    std::memcpy(g_axis, axis, 12);
    g_mag_top = top, g_mag_bottom = bottom;
    g_well_ok = true;
}
// render thread: how far out of the well a seating magazine is (metres), -1 none seating
float VrMagInsert() { return g_insert.load(std::memory_order_relaxed); }
// render thread: the velocity a thrown magazine left the hand with, C (zero when dropped)
void VrMagThrow(float* v) {
    std::lock_guard lock(g_lock);
    std::memcpy(v, g_throw, 12);
}
// the off hand holds a magazine (its fingers closed on it, and it can't take the gun meanwhile)
bool VrMagInHand() {
    int s = g_state.load(std::memory_order_relaxed);
    return s == 2 || s == 3;
}
// the revolver (vr_gun.cpp draws it): its cylinder's swing (0 shut .. 1 out), its turn, rounds in it
bool VrRevolver() {
    std::lock_guard lock(g_lock);
    return g_parts.revolver;
}
float VrCylOut() { return g_cyl_out.load(std::memory_order_relaxed); }
float VrCylSpin() { return g_cyl_spin.load(std::memory_order_relaxed); }
bool VrCylRounds() { return g_cyl_rounds.load(std::memory_order_relaxed); }
// the held gun is fed a shell at a time (vr_gun.cpp: its shell hidden but in the hand, fingers pinching)
bool VrShellGun() {
    std::lock_guard lock(g_lock);
    return g_parts.shells;
}
// the held gun is a launcher (vr_gun.cpp): its root bone (-1 not one), and whether a round is in it (its tube's or
// the rocket on its front)
std::atomic<bool> g_launch_loaded{false};
std::atomic<bool> g_m203_loaded{false};   // the M16's M203 has its grenade in (vr_gun.cpp draws it in the rifle's)
std::atomic<bool> g_m203_trigger{false};  // the off hand's trigger kept from the game (the M203's, off the gun)
bool VrM203Loaded() { return g_m203_loaded.load(std::memory_order_relaxed); }
bool VrM203TriggerTaken() { return g_m203_trigger.load(std::memory_order_relaxed); }
int VrLauncherRoot() {
    std::lock_guard lock(g_lock);
    return g_parts.launcher ? g_parts.root : -1;
}
bool VrLauncherLoaded() { return g_launch_loaded.load(std::memory_order_relaxed); }
// the held gun's root bone where its parts give one (its round's or shell's place is the root's), -1 none
int VrPartRoot() {
    std::lock_guard lock(g_lock);
    return g_parts.root;
}
// a hint floating over the off hand (vr_totest.cpp draws it), empty none; game thread writes, render thread reads
std::mutex g_tip_lock;
std::string g_tip;
void SetTip(const char* t) {
    std::lock_guard lock(g_tip_lock);
    g_tip = t;
}
std::string VrTip() {
    std::lock_guard lock(g_tip_lock);
    return g_tip;
}
// the magazine falling now left the hand (VrPoseGun throws it from there)
bool VrMagFromHand() { return g_from_hand.load(std::memory_order_relaxed); }

// game thread, each gun frame: the hands' part of a reload
// dev_reload in the app folder (with dev_fakehands; a gun with a magazine): the off hand's reload played through
// the real code, the hands xr::SetDevHands gives: 6 s into the gun and every 16 s, A drops the magazine, the left
// grip takes one at the chest and seats it in the well, the handle is gripped, pulled back and let go, then the
// fore-grip is gripped and held; "vr reload: dev_reload" logs each step and whether the fore-grip took
void DevReloadScript(const std::string& code, int state, float yaw, float pitch, const float* head, const float* fwd) {
    static const bool on = std::filesystem::exists(app::ExeDir() / "dev_reload");
    if (!on) return;
    static std::string for_code;
    static auto since = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (for_code != code) for_code = code, since = now;
    float hy, hp;
    if (g_parts.shells || g_parts.revolver || g_parts.launcher || g_bone < 0 || !xr::HeadAngles(hy, hp)) {
        xr::SetDevHands(nullptr, nullptr);
        return;
    }
    float t = std::chrono::duration<float>(now - since).count() - 6;
    if (t < 0) return;
    t = std::fmod(t, 16.0f);
    // the head's turn (the fake hands' frame), and C to local: p = U c, less the eye's raise
    float cy = std::cos(hy), sy = std::sin(hy), cp = std::cos(hp), sp = std::sin(hp);
    const float r[9] = {cy, sy * sp, sy * cp, 0, cp, -sp, -sy, cy * sp, cy * cp};
    float ly = std::cos(yaw), lsy = std::sin(yaw), lp = std::cos(pitch), lsp = std::sin(pitch);
    const float U[9] = {ly, lsy * lsp, lsy * lp, 0, lp, -lsp, -lsy, ly * lsp, ly * lp};
    auto to_local = [&](const float* c, float* p) {
        for (int k = 0; k < 3; k++) p[k] = U[k * 3] * c[0] + U[k * 3 + 1] * c[1] + U[k * 3 + 2] * c[2];
        p[1] -= xr::kEyeRaise + VrViewLift();
    };
    auto ahead = [&](float x, float y, float z, float* p) {
        for (int i = 0; i < 3; i++) p[i] = head[i] + r[i * 3] * x + r[i * 3 + 1] * y + r[i * 3 + 2] * z;
    };
    xr::Hand h[2]{};
    for (int k = 0; k < 2; k++) h[k].ok = true;
    // the right: the fake gun hand's place, aimed as the head (Ry(yaw) Rx(pitch))
    ahead(0.03f, -0.10f, -0.45f, h[1].pos);
    h[1].aim_ok = true;
    {
        float c2 = std::cos(hy / 2), s2 = std::sin(hy / 2), cx = std::cos(hp / 2), sx = std::sin(hp / 2);
        h[1].aim[0] = c2 * sx, h[1].aim[1] = s2 * cx, h[1].aim[2] = -s2 * sx, h[1].aim[3] = c2 * cx;
    }
    // the left (no aim: a held magazine is upright at the palm)
    float c[3];
    int step;
    // dev_reload_carry: the grip held from the seated magazine straight onto the fore-grip (slid there along the gun
    // over half a second), no rack
    static const bool carry = std::filesystem::exists(app::ExeDir() / "dev_reload_carry");
    if (carry && t >= 6.5f) step = t < 7 ? 11 : t < 10 ? 10 : 0;
    else if (t < 1) step = 0;      // at rest
    else if (t < 1.2f) step = 1;   // A: the magazine out
    else if (t < 2) step = 0;
    else if (t < 2.5f) step = 2;   // at the chest
    else if (t < 3.5f) step = 3;   // gripped: one taken
    else if (t < 6.5f) step = 4;   // at the well, held: seats
    else if (t < 7) step = 5;      // let go, still there
    else if (t < 7.5f) step = 6;   // at the handle
    else if (t < 8.5f) step = 7;   // gripped, pulled back
    else if (t < 9) step = 8;      // let go: racked
    else if (t < 9.5f) step = 0;
    else if (t < 10) step = 9;     // at the fore-grip
    else if (t < 13) step = 10;    // gripped
    else step = 0;
    ahead(-0.10f, -0.14f, -0.55f, h[0].pos);
    if (step == 1) h[1].primary = true;
    if (step == 2 || step == 3) {
        h[0].pos[0] = head[0] - sy * kChestAhead, h[0].pos[2] = head[2] - cy * kChestAhead;
        h[0].pos[1] = head[1] - 0.5f * (kChestTop + kChestBottom);
        h[0].grip = step == 3 ? 1.0f : 0.0f;
    }
    if (step == 4 || step == 5) {
        std::lock_guard lock(g_lock);
        if (g_well_ok) {
            // the palm so the held magazine's top is 3 cm down the well's line (HeldMagOf without an aim: up is C's)
            for (int i = 0; i < 3; i++) c[i] = g_well[i] + g_axis[i] * (g_mag_top - 0.03f) - (i == 1 ? g_mag_top : 0);
            to_local(c, h[0].pos);
        }
        h[0].grip = step == 4 ? 1.0f : 0.0f;
    }
    if (step >= 6 && step <= 8) {
        std::lock_guard lock(g_lock);
        if (g_rack_ok) {
            float back = step == 7 ? std::min((t - 7.5f) / 0.8f, 1.0f) * (kRackPull + 0.02f) : step == 8 ? kRackPull + 0.02f : 0;
            for (int i = 0; i < 3; i++) c[i] = g_rack_at[i] - fwd[i] * back;
            to_local(c, h[0].pos);
        }
        h[0].grip = step == 7 ? 1.0f : 0.0f;
    }
    if (step == 11) {
        // from the well toward the fore-grip, the grip held
        float w[3], f[3];
        bool ok;
        {
            std::lock_guard lock(g_lock);
            ok = g_well_ok;
            for (int i = 0; i < 3; i++) w[i] = g_well[i] + g_axis[i] * (g_mag_top - 0.03f) - (i == 1 ? g_mag_top : 0);
        }
        if (ok && VrForeAt(f)) {
            float k = std::clamp((t - 6.5f) / 0.5f, 0.0f, 1.0f);
            for (int i = 0; i < 3; i++) c[i] = w[i] + (f[i] - w[i]) * k;
            to_local(c, h[0].pos);
        }
        h[0].grip = 1;
    }
    if ((step == 9 || step == 10) && VrForeAt(c)) {
        to_local(c, h[0].pos);
        h[0].grip = step == 10 ? 1.0f : 0.0f;
    }
    xr::SetDevHands(&h[0], &h[1]);
    static int was = -1;
    static bool checked = false;
    if (step != was) {
        LOGR("vr reload: dev_reload %s t %.1f step %d (state %d, on the gun %d, fore %.3f, near a zone %d, racking %d)",
             code.c_str(), t, step, state, int(VrLeftOnGun()), VrLeftFore(), int(g_near_rack.load()), int(g_racking));
        was = step;
        if (step == 10) checked = false;
    }
    if (step == 10 && t > (carry ? 9.5f : 12) && !checked) {
        checked = true;
        LOGR("vr reload: dev_reload %s: the fore-grip after the reload %s (fore %.3f, state %d, racking %d, near a "
             "zone %d)", code.c_str(), VrLeftOnGun() ? "TOOK" : "DID NOT TAKE", VrLeftFore(), state, int(g_racking),
             int(g_near_rack.load()));
        if (VrLeftOnGun()) TestEvent("dev_reload_fore");
    }
}

void VrReloadTick() {
    static std::string code;
    // the dev markers' timers and one-shots, from each gun's switch (dev_give cycles guns in one run)
    static auto since = std::chrono::steady_clock::now();
    static bool dev_done = false, fresh_done = false, rs_pull = false, rs_rel = false;
    std::string now_code = weapons::HeldCode();
    // the M16's M203: its own parts and ammo (B203) from when it's empty with grenades to load until it's loaded
    // and shut again, the rifle's the rest of the time
    static bool m203 = false;
    {
        Ammo m;
        bool bns = now_code == "BNS" && ReadAmmo(m, true);
        uint16_t aclip = bns ? G<uint16_t>(m.ammo + m.clip) : 0, ares = bns ? G<uint16_t>(m.reserve) : 0;
        if (bns && aclip > 1 && On()) {
            // one in it at a time: the game's start (3) leaves the rest in the reserve
            G<uint16_t>(m.reserve) = ares = uint16_t(std::min<uint32_t>(ares + aclip - 1, 0xFFFF));
            G<uint16_t>(m.ammo + m.clip) = aclip = 1;
        }
        g_m203_loaded = bns && aclip > 0;
        xr::Hand lh, rh;
        float hd[3];
        const bool hands = xr::Hands(lh, rh, hd);
        // A with the m203 empty and shut: the rifle's magazine drops (user 2026-11-26: it couldn't be, the m203's
        // parts had the gun till loaded); the m203's back once the rifle's reload is done
        static bool a_prev = false, rifle_first = false, rifle_busy = false;
        const bool a_press = hands && rh.primary && !a_prev;
        a_prev = hands && rh.primary;
        if (rifle_first && !m203 && g_state != 0) rifle_busy = true;
        if (rifle_first && rifle_busy && g_state == 0) rifle_first = rifle_busy = false;
        if (!bns) m203 = rifle_first = rifle_busy = false;
        else if (m203 && a_press && g_state == 0 && g_slide < 0.01f) m203 = false, rifle_first = true;
        else if (!m203 && !rifle_first && g_state == 0 && !aclip && ares > 0 && !g_racking) m203 = true;
        else if (m203 && g_state == 0 && aclip > 0 && g_slide < 0.01f) m203 = false;
        if (m203) now_code = "B203";
        // its trigger is the off hand's: it fires only with that hand gripping the gun, the grip held (user: in
        // toggle grip too, where the hand stays on the gun let go)
        bool gripped = VrLeftOnGun() && hands && lh.grip > 0.6f;
        static const bool dev_on = std::filesystem::exists(app::ExeDir() / "dev_lefton");  // dev hands: gripping
        // and never empty (a pull then is the game's own endless reload)
        g_m203_trigger = bns && (!(gripped || dev_on) || !aclip);
        // phase 6: the game's own grenade throw (the off hand's trigger) is off in the headset: grenades are taken at
        // the waist and thrown by hand (vr_grenade.cpp); on the M16+M203 that trigger is the launcher's
        if (!bns && xr::Active()) g_m203_trigger = true;
    }
    if (now_code != code) {
        // a weapon switch: its magazine is in it; the slide as that gun was left
        if (!code.empty()) g_kept[code] = {g_locked, g_spent, g_pending};
        code = now_code;
        SlideKeep k = g_kept.count(code) ? g_kept[code] : SlideKeep{};
        g_locked = k.locked, g_spent = k.spent, g_pending = k.pending;
        g_slide = g_locked ? 1.0f : 0.0f;
        g_racking = false;
        g_seating = false;
        g_insert = -1;
        g_state = 0;
        since = std::chrono::steady_clock::now();
        dev_done = fresh_done = rs_pull = rs_rel = false;
        std::lock_guard lock(g_lock);
        g_well_ok = false;
        g_rack_ok = false;
    }
    g_ctrl = weapons::HeldWeapon();
    DevModeDump();
    if (VrGunAway()) {  // phase 6: a grenade in the hand or empty: the gun isn't there to reload
        xr::TakeLeftTrigger(true);
        SetTip("");
        return;
    }
    auto it = kParts.find(code);
    // dev_dumpreload (recording the game's own reload): the game reloads, ours stands aside
    static const bool recording = std::filesystem::exists(app::ExeDir() / "dev_dumpreload");
    bool manual = On() && !code.empty() && !recording;
    for (const char* c : kGameReload)
        if (code == c) manual = false;
    g_manual = manual;
    g_bone = manual && it != kParts.end() ? it->second.mag : -1;
    {
        std::lock_guard lock(g_lock);
        g_parts = manual && it != kParts.end() ? it->second : GunParts{};
    }
    // a bolt or pump worked by hand: the game's own cocking after each shot isn't heard (ours plays with the hand)
    {
        auto au = kGunAudio.find(code);
        bool mute = manual && it != kParts.end() && it->second.bolt && au != kGunAudio.end();
        const GunSound& rel = mute ? au->second.sound[kRelease] : GunSound{};
        const GunSound& pull = mute ? au->second.sound[kPull] : GunSound{};
        sound::Mute(mute ? rel.len : 0, mute ? rel.rate : 0, 0);
        sound::Mute(mute ? pull.len : 0, mute ? pull.rate : 0, 1);
    }
    if (!manual) {
        g_state = 0;
        xr::TakeLeftTrigger(g_m203_trigger);
        SetTip("");
        return;
    }
    Ammo a;
    xr::Hand hand[2];
    float head[3], yaw, pitch;
    if (!ReadAmmo(a, code == "B203") || !xr::Hands(hand[0], hand[1], head) || !VrLookAngles(yaw, pitch)) {
        xr::TakeLeftTrigger(g_m203_trigger);
        return;
    }
    uint16_t& clip = G<uint16_t>(a.ammo + a.clip);  // (the M203's an int never below 0: its low half)
    uint16_t& reserve = G<uint16_t>(a.reserve);
    // a bolt worked by hand: a shot (the clip down since the last tick, nothing of ours) leaves the rest waiting for
    // a rack, the bolt forward (none left: it locks back below)
    static uint16_t last_clip = 0;
    static std::string last_clip_code;
    if (last_clip_code != code) last_clip_code = code, last_clip = clip;
    // the SPAS 12's modes (user): pump (the game's, its only one) and semi-auto, ours, on the fire-mode button: a
    // click and the button's buzz (vr_body.cpp). Kept per gun while it's put away
    static std::map<std::string, bool> semi;
    static int presses = VrModePresses();
    // dev_semi in the app folder: the SPAS starts in semi-auto
    static const bool dev_semi = std::filesystem::exists(app::ExeDir() / "dev_semi");
    if (dev_semi && !semi.count(code)) semi[code] = true;
    // dev_dumpkind: the held gun's ammo kind's words, and its controller's, as floats and hex, once a gun
    if (std::filesystem::exists(app::ExeDir() / "dev_dumpkind")) {
        static std::string dumped;
        if (dumped != code) {
            dumped = code;
            uint32_t kind = R32(a.ammo + kAmmoKind);
            for (int which = 0; which < 3; which++) {
                uint32_t base = which == 0 ? kind : which == 1 ? a.ctrl : a.ammo;
                uint32_t len = which == 1 ? 0x400 : 0x100;
                std::string line;
                for (uint32_t o = 0; o < len; o += 4) {
                    uint32_t w = R32(base + o);
                    float f;
                    std::memcpy(&f, &w, 4);
                    char b[40];
                    if (std::fabs(f) > 1e-3f && std::fabs(f) < 1e4f) std::snprintf(b, sizeof b, " %X=%g", o, f);
                    else std::snprintf(b, sizeof b, " %X:%X", o, w);
                    line += b;
                }
                LOGR("vr reload: dev_dumpkind %s %s %08X:%s", code.c_str(), which == 0 ? "kind" : which == 1 ? "ctrl" : "ammo",
                     base, line.c_str());
            }
        }
    }
    // the SPAS in semi-auto fires as fast as the trigger's pulled (user): the game holds the next shot off on the
    // weapon's own clock at +0xDC, set to the kind's interval at a shot (its +0x20: the SPAS 1.315 s, the
    // remington 1.67) and counting down; the trigger let go, that clock is cleared, so the next pull fires at once
    if (g_parts.shells && semi[code] && !VrFireHeld() && G<float>(a.ctrl + kCtrlShotClock) > 0) {
        LOG_FIRST(6, "vr reload: %s shot clock %.2f cleared (trigger let go)", code.c_str(), G<float>(a.ctrl + kCtrlShotClock));
        G<float>(a.ctrl + kCtrlShotClock) = 0;
    }
    if (g_parts.shells) {
        static uint32_t last_state = ~0u;
        uint32_t st = R32(a.ctrl + kCtrlState);
        // dev_shotclock: the weapon's state and clocks every 0.1 s (to see what holds the next shot)
        static const bool dev_clock = std::filesystem::exists(app::ExeDir() / "dev_shotclock");
        static int64_t clock_next = 0;
        int64_t clock_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch()).count();
        if (dev_clock && clock_ms >= clock_next) {
            clock_next = clock_ms + 100;
            LOGR("vr reload: clock state %u clip %u dc %.3f e0 %.3f e4 %.3f 1ec %.3f 1f0 %.3f 110 %u 1c %.2f", st, clip,
                 G<float>(a.ctrl + 0xDC), G<float>(a.ctrl + 0xE0), G<float>(a.ctrl + 0xE4), G<float>(a.ctrl + 0x1EC),
                 G<float>(a.ctrl + 0x1F0), R32(a.ctrl + 0x110), G<float>(a.ctrl + 0x1C));
        }
        if (st != last_state) {
            last_state = st;
        }
    }
    if (int p = VrModePresses(); p != presses) {
        presses = p;
        if (manual && (code == "SHG" || code == "SH5")) {
            bool& m = semi[code];
            m = !m;
            sound::Cue(m ? 1 : 0);
            if (m) TestEvent("spas_semi");
            LOGR("vr reload: %s mode %s", code.c_str(), m ? "semi-auto" : "pump");
        }
    }
    static int64_t shot_ms = 0;  // a shotgun's shot waiting to see if the game pumps after it
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch()).count();
    if (g_state == 0 && !g_racking && clip < last_clip) {
        // a shot (the test checklist): soon after the hand worked the gun, or the spas's semi-auto three quickly
        static int64_t last_shot = 0;
        static int quick = 0;
        if (g_worked_ms && now_ms - g_worked_ms < 3000)
            TestEvent(g_parts.shells ? "fire_after_pump" : "fire_after_rack:" + code);
        g_worked_ms = 0;
        quick = now_ms - last_shot < 800 ? quick + 1 : 1;
        last_shot = now_ms;
        if (semi[code] && quick >= 3) TestEvent("spas_rapid");
    }
    if (g_parts.bolt && g_state == 0 && !g_racking && clip < last_clip) {
        if (g_parts.launcher) {
        } else if (g_parts.shells) {
            shot_ms = semi[code] ? 0 : now_ms;  // semi-auto: the next round's in, whatever the game animates
            if (semi[code]) LOGR("vr reload: %s fired semi-auto, clip %u", code.c_str(), clip);
        } else {
            g_pending = clip;
            clip = 0;
            g_spent = true;
        }
    }
    if (shot_ms && g_parts.shells) {
        if (VrGameCycledMs() >= shot_ms) {
            // the game pumped after it (pump mode): the rest wait for the hand's pump
            g_pending = uint16_t(g_pending + clip);
            clip = 0;
            g_spent = true;
            shot_ms = 0;
            LOGR("vr reload: %s fired, pump to chamber (waiting %u)", code.c_str(), g_pending);
        } else if (now_ms - shot_ms > kCycleWait) {
            shot_ms = 0;  // no pump after it: semi-auto, the next round's in
            LOGR("vr reload: %s fired semi-auto, clip %u", code.c_str(), clip);
        }
    }
    if (!g_parts.shells || g_parts.launcher) shot_ms = 0;
    if (g_parts.launcher) {
        // one round at a time: more in it (the game's start) back to the reserve
        if (g_state == 0 && clip > 1 && !g_pending) {
            reserve = uint16_t(std::min<uint32_t>(reserve + clip - 1, 0xFFFF));
            clip = 1;
        }
        // fired: a tube is open to load only now (the user's), the rocket's front is empty. Empty is fired (the M203's
        // shot was the rifle's code's)
        if (g_state == 0 && !clip && !g_pending && g_parts.slide >= 0 && !g_spent) {
            g_spent = true;
            LOGR("vr reload: %s empty, the tube to open", code.c_str());
        }
    }
    // a kept lock with rounds in the gun is stale (a new level's gun of the same kind): a locked slide means empty
    if (g_locked && clip && !g_racking) g_locked = false, g_pending = 0, g_slide = 0;
    if (g_spent && clip && !g_racking) g_spent = false, g_pending = 0;
    // dev_eject in the app folder: A pressed once, 8 s into this gun (to see the magazine fall without controllers)
    static const bool dev_eject = std::filesystem::exists(app::ExeDir() / "dev_eject");
    if (dev_eject && !dev_done &&
        std::chrono::duration<float>(std::chrono::steady_clock::now() - since).count() > 8) {
        hand[1].primary = true;
        dev_done = true;
    }
    // dev_shells in the app folder (a shotgun's hands without the controllers): from 6 s into this gun, every 6 s
    // a shell is taken (in the hand half a second, then 6 cm out along its slot, to see it), loaded, then the pump worked back
    // (held a second) and forward; with dev_autofire the gun fires between
    static const bool dev_shells = std::filesystem::exists(app::ExeDir() / "dev_shells");
    float dev_t = std::chrono::duration<float>(std::chrono::steady_clock::now() - since).count();
    int dev_phase = dev_shells && g_parts.shells && dev_t > 6 ? int(std::fmod(dev_t - 6, 6.0f) * 2) : -1;  // 0..11
    static bool a_was = false, grab_was = false;
    // a shell is pinched (user: thumb and index finger): taken and held on the off hand's trigger, not its grip
    float take = g_parts.shells && !g_parts.launcher ? hand[0].trigger : hand[0].grip;
    bool a_edge = hand[1].primary && !a_was, grab = take > 0.6f, grab_edge = grab && !grab_was;
    if (g_parts.shells) a_edge = false;  // no magazine to drop
    bool latch_edge = g_parts.revolver && a_edge;  // the revolver's A is its latch, not an eject
    if (g_parts.revolver) a_edge = false;
    a_was = hand[1].primary;
    grab_was = grab;
    int state = g_state;
    // the off hand in C, and the barrel's forward (the gun hand's aim, its -z)
    float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    float U[9] = {cy, sy * sp, sy * cp, 0, cp, -sp, -sy, cy * sp, cy * cp};
    float hc[3], fwd[3];
    PalmInCamera(hand[0], yaw, pitch, hc);
    {
        const float* q = hand[1].aim;
        float fl[3] = {-2 * (q[0] * q[2] + q[3] * q[1]), -2 * (q[1] * q[2] - q[3] * q[0]),
                       -(1 - 2 * (q[0] * q[0] + q[1] * q[1]))};
        for (int i = 0; i < 3; i++) fwd[i] = U[i] * fl[0] + U[3 + i] * fl[1] + U[6 + i] * fl[2];
    }
    float along = hc[0] * fwd[0] + hc[1] * fwd[1] + hc[2] * fwd[2];
    // the rack: the off hand's grip pressed at the slide (or charging handle), the hand empty
    static bool grip_was = false;
    bool grip = hand[0].grip > 0.6f, grip_edge = grip && !grip_was;
    grip_was = grip;
    bool near_rack = false;
    {
        std::lock_guard lock(g_lock);
        if (g_rack_ok) {
            // the hand's distance from the zone's line (a point when its ends meet)
            float a[3], v[3], aa = 0, av = 0, d2 = 0;
            for (int i = 0; i < 3; i++) a[i] = g_rack_to[i] - g_rack_at[i], v[i] = hc[i] - g_rack_at[i];
            for (int i = 0; i < 3; i++) aa += a[i] * a[i], av += a[i] * v[i];
            float t = aa > 1e-8f ? std::clamp(av / aa, 0.0f, 1.0f) : 0.0f;
            for (int i = 0; i < 3; i++) d2 += (v[i] - a[i] * t) * (v[i] - a[i] * t);
            float reach = g_parts.rack_reach > 0 ? g_parts.rack_reach : kRackReach;
            near_rack = d2 < reach * reach;
            // diagnostic: how close the off hand is twice a second while it's within 12 cm and not racking, and
            // where from the rack point (cm: out along the zone, up, toward the muzzle), to fit the zone by
            static auto told = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            if (!g_racking && d2 < 0.12f * 0.12f && now - told > std::chrono::milliseconds(500)) {
                told = now;
                float al = std::sqrt(aa), o = 0, u = 0, f = 0;
                for (int i = 0; i < 3; i++) {
                    o += v[i] * (al > 1e-4f ? a[i] / al : 0.0f), u += v[i] * g_rack_up[i], f += v[i] * g_rack_fwd[i];
                }
                LOGR("vr reload: %s off hand %.3f from the rack zone (out %.1f up %.1f fwd %.1f cm; state %d, on the "
                     "gun %d)", code.c_str(), std::sqrt(d2), o * 100, u * 100, f * 100, state, int(VrLeftOnGun()));
            }
        }
    }
    near_rack = near_rack && (state == 0 || state == 1) && !VrLeftOnGun() && !g_parts.shells;
    // at the magazine in the gun (its body, not the gun round it)
    bool near_mag = false;
    // (not a shotgun's: there's nothing to pull; not within 5 cm of the fore-grip: that's the grip's, user)
    if (state == 0 && g_bone >= 0 && !VrLeftOnGun() && !g_parts.shells && VrLeftFore() > 0.05f) {
        std::lock_guard lock(g_lock);
        if (g_well_ok) {
            float v[3] = {hc[0] - g_well[0], hc[1] - g_well[1], hc[2] - g_well[2]};
            float t = std::clamp(v[0] * g_axis[0] + v[1] * g_axis[1] + v[2] * g_axis[2], -kPullDown, kPullUp);
            float d2 = 0;
            for (int i = 0; i < 3; i++) d2 += (v[i] - g_axis[i] * t) * (v[i] - g_axis[i] * t);
            near_mag = d2 < kPullReach * kPullReach;
        }
    }
    {
        // the off hand reaching the slide or the magazine: a faint tick, as fingers finding it (user: touch)
        static bool rack_was = false, mag_was = false;
        if (near_rack && !rack_was) xr::Pulse(0, 0.12f, 0.015f);
        if (near_mag && !mag_was) xr::Pulse(0, 0.12f, 0.015f);
        rack_was = near_rack, mag_was = near_mag;
    }
    if (!g_racking && near_rack && grip_edge) {
        g_racking = true;
        g_rack_from = along;
        g_rack_back_cued = false;
        xr::Pulse(0, 0.4f, 0.03f);
    }
    // a shotgun's pump (the off hand on the fore-end, aiming with it): pump mode, the hand locked on the pump, after
    // a shot that needs it (from where the hand is) or on a clear rock back at any time (to open it). Worked back,
    // the action is open: let go there and it stays open (user: to load shells, which then wait in the tube);
    // taken again it's locked on at the back, and forward chambers. Not worked back yet, letting go puts it forward
    static bool pump_mode = false, pump_counted = false, pump_open = false;
    static std::string pump_code;
    if (pump_code != code) pump_code = code, pump_mode = pump_counted = pump_open = false;
    bool pump_need = g_parts.shells && (g_spent || (!clip && g_pending)) && state == 0;
    // a launcher's tube: forward to open (dir -1), farther; a pump: back
    const bool tube = g_parts.launcher && g_parts.slide >= 0;
    const float dir = tube ? -1.0f : 1.0f, stroke = tube ? -g_parts.travel : kPumpStroke;  // a tube: its own travel
    if (g_parts.launcher && g_parts.slide < 0) pump_need = false;  // the rpg has nothing to work
    static const bool dev_lefton = std::filesystem::exists(app::ExeDir() / "dev_lefton");
    bool on_fore = VrLeftOnGun() || dev_lefton;
    static bool fore_was = false;
    if (VrLeftOnGun() && !fore_was) {
        xr::Pulse(0, 0.22f, 0.02f);  // the hand on the fore-end (user: touch)
        TestEvent("fore_grip:" + code);
        if (g_parts.shells) TestEvent("fore_grip_shotgun");
    }
    fore_was = VrLeftOnGun();
    float pump_speed = 0;  // the off hand's speed back along the barrel (toward the player), m/s
    {
        float v[3];
        for (int i = 0; i < 3; i++) v[i] = U[i] * hand[0].vel[0] + U[3 + i] * hand[0].vel[1] + U[6 + i] * hand[0].vel[2];
        for (int i = 0; i < 3; i++) pump_speed -= v[i] * fwd[i];
    }
    if (g_parts.shells && on_fore && !pump_mode) {
        if (pump_open) {
            // taken again open: locked on at the back
            pump_mode = pump_counted = true, pump_open = false;
            g_rack_from = along + dir * stroke;
            xr::Pulse(0, 0.25f, 0.02f);
        } else if (pump_need || (!g_parts.launcher && pump_speed >= kPumpSpeed)) {
            pump_mode = true, pump_counted = false;
            g_rack_from = along + (pump_need ? 0.0f : 0.01f) * dir;
            xr::Pulse(0, 0.25f, 0.02f);
        }
    }
    if (pump_mode && (!on_fore || !g_parts.shells)) {
        pump_mode = false;
        if (pump_counted && g_parts.shells) {
            pump_open = true;  // let go of it back: it stays open
            xr::Pulse(0, 0.2f, 0.02f);
            TestEvent("pump_left_open");
        } else {
            g_slide = 0;  // not worked back: forward it goes, nothing done
        }
    }
    if (pump_open) g_slide = 1;
    g_rack_held = g_racking;
    // the off hand on the gun's fore-grip: its grip is the fore-grip's, nothing of the reload's
    if (VrLeftOnGun() || g_racking) grab_edge = false;
    // entering the chest with a magazine to take: a light buzz
    static bool chest_was = false;
    bool room = !g_parts.shells || uint32_t(clip) + g_pending < a.size;  // a shell: only while the gun has room
    if (g_parts.revolver) room = g_cyl_open;  // a speed loader: into an open cylinder
    const bool tube_open = pump_open || (pump_mode && pump_counted);
    if (g_parts.launcher) room = !clip && !g_pending && (!tube || tube_open);  // one round, a tube open for it
    bool zone = g_parts.take == kTakeShoulder ? NearLeftShoulder(hand[0], head)
                : g_parts.take == kTakeWaist  ? NearWaist(hand[0], head)
                                              : NearChest(hand[0], head);
    bool chest = (state == 0 || state == 1) && reserve > 0 && room && !VrLeftOnGun() && !g_racking && zone &&
                 VrLeftFore() > 0.08f;  // (the fore-grip's reach is the grip's)
    // the launchers' hints over the off hand (user)
    // (user: only until the first round ever goes into a launcher, then never again)
    if (!g_parts.launcher || reserve == 0 || state == 2 || Opt().vr_launcher_tips_done) SetTip("");
    else if (tube && g_spent && !tube_open && !clip && !g_pending) SetTip("Push the tube forward to open it");
    else if (room) SetTip(g_parts.take == kTakeShoulder ? "Grip over your left shoulder for a rocket"
                                                        : "Grip low by your waist for a grenade");
    else SetTip("");
    if (chest && !chest_was) xr::Pulse(0, 0.2f, 0.025f);
    chest_was = chest;
    g_near_rack = near_rack || g_racking || near_mag || chest;
    // dev_magfresh: 8 s into this gun a fresh magazine is in the off hand, the old one still in (to see both)
    if (!fresh_done && state == 0 && std::filesystem::exists(app::ExeDir() / "dev_magfresh") &&
        std::chrono::duration<float>(std::chrono::steady_clock::now() - since).count() > 8)
        fresh_done = true, state = 3, grab_was = true;
    // dev_maghand: 2 s after an eject a magazine is in the off hand (to see it held, and the dropped one land)
    if (state == 1 && VrMagOutSeconds() > 2 && std::filesystem::exists(app::ExeDir() / "dev_maghand")) state = 2;
    if (state == 2 && std::filesystem::exists(app::ExeDir() / "dev_maghand")) grab = true, grab_was = true;
    // dev_racksound: 10 s into this gun its rack's sounds, pull then release half a second later (to hear them)
    if (std::filesystem::exists(app::ExeDir() / "dev_racksound")) {
        float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - since).count();
        if (!rs_pull && t > 10) rs_pull = true, PlayRack(code, false);
        if (!rs_rel && t > 10.5f) rs_rel = true, PlayRack(code, true);
    }
    // dev_magslide: 2 s after an eject the magazine comes back on the well's line (below)
    if (state == 1 && VrMagOutSeconds() > 2 && std::filesystem::exists(app::ExeDir() / "dev_magslide")) state = 2;
    // no magazine in the gun: no rounds in it either (anything the game refilled goes back to the reserve)
    if (state != 0 && clip && !g_parts.shells && !g_parts.revolver) {
        reserve = uint16_t(std::min<uint32_t>(reserve + clip, 0xFFFF));
        clip = 0;
        LOG_FIRST(8, "vr reload: the game refilled the gun with its magazine out; put back");
    }
    // diagnostic: the ammo and the weapon's state every half second for 12 s after an eject
    static int samples = 0;
    static float next = 0;
    if (samples > 0 && VrMagOutSeconds() > next) {
        samples--, next += 0.5f;
        LOGR("vr reload: +%.1f s: clip %u, reserve %u, weapon state %u, ours %d", VrMagOutSeconds(), clip, reserve,
             R32(a.ctrl + kCtrlState), state);
    }
    bool rev_dev_seat = false;
    if (g_parts.revolver) {
        // the gun hand's angles: its pitch (the muzzle over level) and roll (counter-clockwise negative, looking down
        // the barrel), and the roll's rate
        const float* q = hand[1].aim;
        float fl[3] = {-2 * (q[0] * q[2] + q[3] * q[1]), -2 * (q[1] * q[2] - q[3] * q[0]),
                       -(1 - 2 * (q[0] * q[0] + q[1] * q[1]))};
        float ul[3] = {2 * (q[0] * q[1] - q[3] * q[2]), 1 - 2 * (q[0] * q[0] + q[2] * q[2]), 2 * (q[1] * q[2] + q[3] * q[0])};
        float gun_pitch = std::asin(std::clamp(fl[1], -1.0f, 1.0f));
        float upr[3] = {-fl[1] * fl[0], 1 - fl[1] * fl[1], -fl[1] * fl[2]};  // world up square to the barrel
        float ul2 = std::sqrt(upr[0] * upr[0] + upr[1] * upr[1] + upr[2] * upr[2]);
        float roll = 0;
        if (ul2 > 0.1f) {
            for (float& v : upr) v /= ul2;
            float rt[3] = {fl[1] * upr[2] - fl[2] * upr[1], fl[2] * upr[0] - fl[0] * upr[2], fl[0] * upr[1] - fl[1] * upr[0]};
            roll = std::atan2(ul[0] * rt[0] + ul[1] * rt[1] + ul[2] * rt[2], ul[0] * upr[0] + ul[1] * upr[1] + ul[2] * upr[2]);
        }
        // dev_fakegun (vr_gun.cpp's test hands turned: "pitch roll" degrees): the same angles here, and the swing
        // out waits for them (not forced)
        static float fk[2] = {0, 0};
        static const bool fake_gun = [] {
            FILE* f = std::fopen((app::ExeDir() / "dev_fakegun").string().c_str(), "r");
            if (!f) return false;
            bool ok = std::fscanf(f, "%f %f", &fk[0], &fk[1]) == 2;
            std::fclose(f);
            return ok;
        }();
        if (fake_gun) gun_pitch = fk[0] * 3.14159265f / 180, roll = fk[1] * 3.14159265f / 180;
        static float last_roll = 0;
        static auto last_t = std::chrono::steady_clock::now();
        auto tnow = std::chrono::steady_clock::now();
        float dt = std::clamp(std::chrono::duration<float>(tnow - last_t).count(), 1e-3f, 0.1f);
        float d = roll - last_roll;
        if (d > 3.14159265f) d -= 2 * 3.14159265f;
        if (d < -3.14159265f) d += 2 * 3.14159265f;
        float roll_rate = d / dt;
        last_roll = roll, last_t = tnow;
        // dev_revolver: from 6 s into the gun, every 10 s: the latch, the swing out, a loader in the hand (1.5 s),
        // seated, a spin, then shut
        static const bool dev_rev = std::filesystem::exists(app::ExeDir() / "dev_revolver");
        float rt_dev = std::chrono::duration<float>(tnow - since).count();
        int rph = dev_rev && rt_dev > 6 ? int(std::fmod(rt_dev - 6, 10.0f) * 2) : -1;  // 0..19
        static int rph_last = -1;
        bool dev_open = false, dev_spin = false, dev_shut = false;
        if (rph != rph_last && rph >= 0) {
            rph_last = rph;
            LOGR("vr reload: dev_revolver phase %d (open %d, state %d, clip %u, waiting %u)", rph, int(g_cyl_open), state,
                 clip, g_pending);
            if (rph == 0) latch_edge = true;
            if (rph == 1 && !fake_gun) dev_open = true;
            if (rph == 4 && g_cyl_open && state == 0 && reserve > 0) state = 2, grab_was = true;
            if (rph == 9) dev_spin = true;
            if (rph == 15) dev_shut = true;
        }
        if (rph >= 4 && rph < 8 && state == 2) grab = true;
        if (latch_edge && !g_cyl_open) {
            g_cyl_latch = true;
            xr::Pulse(1, 0.3f, 0.03f);
            TestEvent("cyl_latch");
            LOGR("vr reload: %s cylinder latch let go", code.c_str());
        }
        // (a big fast roll can pass -180 degrees and wrap before it's seen past kCylRoll: a fast counter-clockwise
        // flick swings it out too, user: small and big motions both)
        static auto opened_at = tnow;
        if (g_cyl_latch && !g_cyl_open &&
            (gun_pitch < kCylDown || roll < kCylRoll || roll_rate < -kCylFlick || dev_open)) {
            // out it swings: its rounds wait (none fire with it out)
            g_cyl_open = true, g_cyl_latch = false;
            opened_at = tnow;
            g_pending = uint16_t(g_pending + clip);
            clip = 0;
            PlaySound(code, kUnload);
            xr::Pulse(1, 0.5f, 0.04f);
            TestEvent(gun_pitch < kCylDown ? "cyl_out_down" : roll < kCylRoll ? "cyl_out_roll" : "cyl_out_dev");
            LOGR("vr reload: %s cylinder out (pitch %.0f, roll %.0f deg), %u in it", code.c_str(), gun_pitch * 57.3f,
                 roll * 57.3f, g_pending);
        }
        if (g_cyl_open) {
            // the off hand near it: a swipe spins it (its speed across, about the cylinder's axis), a grip shuts it
            float c[3], ax[3], dist = 1;
            bool known = false;
            {
                std::lock_guard lock(g_lock);
                if (g_well_ok) {
                    known = true;
                    std::memcpy(ax, g_axis, 12);
                    for (int i = 0; i < 3; i++) c[i] = g_well[i] + g_axis[i] * 0.03f;  // the cylinder's middle
                }
            }
            if (known) {
                float r[3], v[3], rv[3];
                for (int i = 0; i < 3; i++) r[i] = hc[i] - c[i];
                for (int i = 0; i < 3; i++) v[i] = U[i] * hand[0].vel[0] + U[3 + i] * hand[0].vel[1] + U[6 + i] * hand[0].vel[2];
                dist = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
                if (state == 0 && dist < kCylReach) {
                    rv[0] = r[1] * v[2] - r[2] * v[1], rv[1] = r[2] * v[0] - r[0] * v[2], rv[2] = r[0] * v[1] - r[1] * v[0];
                    float w = (rv[0] * ax[0] + rv[1] * ax[1] + rv[2] * ax[2]) / std::max(dist * dist, kCylRadius * kCylRadius);
                    if (std::fabs(w) > std::fabs(g_cyl_rate) && std::fabs(w) > 3) g_cyl_rate = std::clamp(w, -40.0f, 40.0f);
                    if (std::fabs(w) > 6) TestEvent("cyl_spin");
                }
            }
            if (dev_spin) g_cyl_rate = 25;
            g_cyl_spin = std::fmod(g_cyl_spin.load() + g_cyl_rate * dt, 2 * 3.14159265f);
            g_cyl_rate *= std::exp(-1.2f * dt);
            if (std::fabs(g_cyl_rate) < 0.5f) {
                // coasting to a stop: it settles on a chamber (sixths)
                float sp = g_cyl_spin, step = 3.14159265f / 3;
                g_cyl_rate = 0;
                g_cyl_spin = sp + (std::round(sp / step) * step - sp) * std::min(dt * 10, 1.0f);
            }
            bool push = state == 0 && grab_edge && dist < kCylReach;
            // (the flick that shuts it not the hand coming back from the roll that swung it out: 0.6 s on)
            bool flick = roll_rate > kCylFlick && std::chrono::duration<float>(tnow - opened_at).count() > 0.6f;
            if (push || flick || dev_shut) {
                g_cyl_open = false;
                clip = uint16_t(clip + g_pending), g_pending = 0;
                g_cyl_rate = 0;
                PlaySound(code, kRelease);
                xr::Pulse(1, 0.6f, 0.05f);
                xr::Pulse(0, 0.3f, 0.03f);
                TestEvent(push ? "cyl_shut_push" : "cyl_shut_flick");  // (dev_shut counted as a flick)
                LOGR("vr reload: %s cylinder shut (%s), clip %u", code.c_str(), push ? "pushed" : "flicked", clip);
                grab_edge = false;
            }
        }
        float out = g_cyl_out, want = g_cyl_open ? 1.0f : 0.0f;
        g_cyl_out = out + (want - out) * std::min(dt * 12, 1.0f);
        g_cyl_rounds = clip + g_pending > 0;
        if (rph >= 6 && rph < 8 && state == 2) rev_dev_seat = true;
    } else {
        g_cyl_open = g_cyl_latch = false, g_cyl_out = 0, g_cyl_rate = 0;
    }
    if (dev_phase >= 0) {
        static int dev_last = -1;
        if (dev_phase != dev_last) {
            dev_last = dev_phase;
            LOGR("vr reload: dev_shells phase %d (state %d, clip %u, waiting %u)", dev_phase, state, clip, g_pending);
            // taken (a launcher's tube: after it's opened, phases 0-2)
            if (dev_phase == (tube ? 3 : 0) && state == 0 && reserve > 0 && room) state = 2, grab = grab_was = true;
            if (dev_phase == 4 && !g_racking && !g_parts.shells) {  // the handle taken, then worked back
                g_racking = true, g_rack_from = along, g_rack_back_cued = false;
            }
        }
        if (state == 2) grab = true;
        if (g_racking) grip = dev_phase >= 4 && dev_phase < 7;
        if (tube) {
            if (dev_phase < 6) along = g_rack_from - dir * stroke;  // the tube forward, open, until loaded
        } else if (dev_phase >= 4 && dev_phase < 7) {
            along = g_rack_from - (g_parts.shells ? kPumpStroke : kRackPull);  // back
        }
    }
    if (state == 0 && a_edge) {
        xr::Pulse(1, 0.35f, 0.03f);  // the release button giving, the magazine dropping free
        samples = 24, next = 0;
        // eject: the rounds left go back to the reserve (none lost with the magazine, nor any waiting for a rack in
        // it), the clip is empty
        reserve = uint16_t(std::min<uint32_t>(reserve + clip + g_pending, 0xFFFF));
        clip = 0, g_pending = 0;
        state = 1;
        g_from_hand = false;
        {
            std::lock_guard lock(g_lock);
            for (float& x : g_throw) x = 0;  // dropped
        }
        g_out_at = std::chrono::steady_clock::now();
        xr::Pulse(1, 0.4f, 0.05f);
        LOGR("vr reload: %s magazine out, reserve %u (ctrl %08X, magazine %08X)", code.c_str(), reserve, a.ctrl, a.ammo);
        PlaySound(code, kUnload);
    } else if ((state == 1 || state == 0) && grab_edge && chest) {
        // a fresh one from the chest: into an empty well, or to knock the old one out with (a shell: into the hand)
        state = state == 1 || g_parts.shells || g_parts.revolver ? 2 : 3;
        xr::Pulse(0, 0.5f, 0.04f);
        if (g_parts.launcher) TestEvent(g_parts.take == kTakeShoulder ? "rocket_taken" : "grenade_taken:" + code);
        else if (g_parts.shells) TestEvent("shell_taken");
        if (g_parts.revolver) TestEvent("loader_taken");
    } else if (state == 3 && !grab && !std::filesystem::exists(app::ExeDir() / "dev_magfresh")) {
        // let go of the fresh one with the old still in: it goes back (nothing taken)
        state = 0;
    } else if (state == 3) {
        // the fresh magazine struck at the old one (near its well, moving fast): the old one is knocked out, ahead
        // of the strike, its rounds back to the reserve; the fresh one is ready to go in
        float pc[3], pu[3], pf[3], d2 = 1, v[3];
        if (!HeldMagOf(hand[0], yaw, pitch, g_parts.hold, pc, pu, pf)) PalmInCamera(hand[0], yaw, pitch, pc);
        for (int i = 0; i < 3; i++) v[i] = U[i] * hand[0].vel[0] + U[3 + i] * hand[0].vel[1] + U[6 + i] * hand[0].vel[2];
        float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        {
            std::lock_guard lock(g_lock);
            if (g_well_ok) {
                d2 = 0;
                for (int i = 0; i < 3; i++) d2 += (pc[i] - g_well[i]) * (pc[i] - g_well[i]);
            }
            if (d2 < kKnockReach * kKnockReach && speed > kKnockSpeed)
                for (int i = 0; i < 3; i++) g_throw[i] = v[i] * 1.2f;
        }
        if (d2 < kKnockReach * kKnockReach && speed > kKnockSpeed) {
            reserve = uint16_t(std::min<uint32_t>(reserve + clip + g_pending, 0xFFFF));
            clip = 0, g_pending = 0;
            state = 2;
            g_from_hand = false;
            g_out_at = std::chrono::steady_clock::now();
            xr::Pulse(0, 0.7f, 0.05f);
            xr::Pulse(1, 0.5f, 0.05f);
            LOGR("vr reload: %s magazine knocked out with the fresh one, reserve %u", code.c_str(), reserve);
            PlaySound(code, kUnload);
        }
    } else if (state == 0 && grab_edge && g_bone >= 0 && !g_parts.shells && !g_parts.revolver) {
        // the off hand's grip at the magazine in the gun (its body, not the gun round it): out it comes into the
        // hand (its rounds back to the reserve, as an eject)
        if (near_mag) {
            reserve = uint16_t(std::min<uint32_t>(reserve + clip + g_pending, 0xFFFF));
            clip = 0, g_pending = 0;
            state = 2;
            g_out_at = std::chrono::steady_clock::now() - std::chrono::seconds(1000);  // it's the hand's now
            xr::Pulse(0, 0.5f, 0.04f);
            LOGR("vr reload: %s magazine pulled out by hand, reserve %u", code.c_str(), reserve);
            PlaySound(code, kUnload);
        }
    } else if (state == 2 && dev_phase == 0) {
        g_insert = -1;  // dev_shells: in the hand
    } else if (state == 2 && dev_phase == 1) {
        g_insert = 0.06f;  // dev_shells: held out along its slot
    } else if (state == 2 && std::filesystem::exists(app::ExeDir() / "dev_maghand")) {
        g_insert = -1;  // dev_maghand: held, never lined up (the untracked controllers would line it up)
    } else if (state == 2 && std::filesystem::exists(app::ExeDir() / "dev_magslide")) {
        g_insert = 0.06f;  // dev_magslide: held 6 cm out along the well's line (to see the line without hands)
    } else if (state == 2) {
        // the held magazine's top at the well: within kSeatReach of the line from the well's mouth up to where the
        // top sits seated, it slides home along the well and seats (the hand's angle doesn't matter)
        float pc[3], pu[3] = {0, 1, 0}, pf[3];
        if (!HeldMagOf(hand[0], yaw, pitch, g_parts.hold, pc, pu, pf)) PalmInCamera(hand[0], yaw, pitch, pc);
        float dist = 1, below = 0;
        bool known = false;
        {
            std::lock_guard lock(g_lock);
            if (g_well_ok && g_bone >= 0) {
                known = true;
                float depth = g_parts.depth > 0 ? g_parts.depth : g_mag_top - g_mag_bottom - kMagBase;
                float v[3];
                for (int i = 0; i < 3; i++) v[i] = (pc[i] + pu[i] * g_mag_top) - (g_well[i] + g_axis[i] * g_mag_top);
                float s = v[0] * g_axis[0] + v[1] * g_axis[1] + v[2] * g_axis[2];  // < 0: below its seat
                float t = std::clamp(s, -depth, 0.0f), d2 = 0;
                for (int i = 0; i < 3; i++) d2 += (v[i] - g_axis[i] * t) * (v[i] - g_axis[i] * t);
                dist = std::sqrt(d2);
                below = std::max(-s, 0.0f);
                if (g_parts.shells && !g_parts.launcher) {
                    // a shell (user 2026-11-26): anywhere in a ball round the loading port, kShellBall about it, down
                    // and to the sides as well, but not over the gun's top (kShellAbove up the gun from the port)
                    float e[3] = {pc[0] - g_well[0], pc[1] - g_well[1], pc[2] - g_well[2]};
                    float r = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
                    float up = e[0] * g_rack_up[0] + e[1] * g_rack_up[1] + e[2] * g_rack_up[2];
                    dist = r < kShellBall && up < kShellAbove ? 0.0f : 1.0f;
                }
            }
        }
        // diagnostic: the held top's distance from the well twice a second while it's within 15 cm
        static auto told = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (known && dist < 0.15f && now - told > std::chrono::milliseconds(500)) {
            told = now;
            LOGR("vr reload: %s held top %.3f from the well, %.3f below its seat", code.c_str(), dist, below);
        }
        if (dev_phase >= (tube ? 4 : 2) && dev_phase < (tube ? 6 : 4)) dist = 0;  // dev_shells: at the port
        if (rev_dev_seat) dist = 0;  // dev_revolver: at the cylinder
        if (!g_seating && known &&
            dist < (g_parts.shells ? kShellReach : g_parts.seat_reach > 0 ? g_parts.seat_reach : kSeatReach)) {
            g_seating = true;
            g_seat_from = std::min(below, 0.1f);
            g_seat_at = now;
            xr::Pulse(0, 0.4f, 0.03f);
        }
        bool home = false;
        if (known) {
            float k = std::chrono::duration<float>(now - g_seat_at).count() / kSeatTime;
            g_insert = g_seating ? g_seat_from * std::max(1 - k, 0.0f) : -1.0f;
            home = g_seating && k >= 1;
        } else {
            // no magazine bone known for this gun yet: home is the gun hand's grip (a pistol's)
            float gc[3], d2 = 0;
            PalmInCamera(hand[1], yaw, pitch, gc);
            for (int i = 0; i < 3; i++) d2 += (pc[i] - gc[i]) * (pc[i] - gc[i]);
            home = d2 < 0.12f * 0.12f;
            g_insert = -1;
        }
        if (home && g_parts.revolver) {
            // the speed loader's rounds into the chambers: what was in them back to the reserve (fired cases out),
            // six (or the reserve's) in, waiting for the cylinder to shut
            reserve = uint16_t(std::min<uint32_t>(reserve + g_pending, 0xFFFF));
            uint32_t n = std::min<uint32_t>(a.size, reserve);
            g_pending = uint16_t(n);
            reserve = uint16_t(reserve - n);
            state = 0;
            g_insert = -1;
            g_seating = false;
            xr::Pulse(0, 0.7f, 0.05f);
            xr::Pulse(1, 0.5f, 0.04f);
            TestEvent("loader_in");
            LOGR("vr reload: %s speed loader in, %u rounds, reserve %u", code.c_str(), n, reserve);
            PlaySound(code, kLoad);
        } else if (home && g_parts.shells) {
            // a shell in: chambered if the chamber's live, else into the tube to wait for the pump
            if (g_locked || g_spent) g_pending++;
            else clip++;
            reserve--;
            state = 0;
            g_insert = -1;
            g_seating = false;
            xr::Pulse(0, 0.6f, 0.04f);
            xr::Pulse(1, 0.4f, 0.03f);
            if (g_parts.launcher) TestEvent(g_parts.take == kTakeShoulder ? "rocket_in" : "grenade_in:" + code);
            if (g_parts.launcher && !Opt().vr_launcher_tips_done) {
                Opt().vr_launcher_tips_done = true;
                app::Save();
                SetTip("");
            }
            else TestEvent("shell_in");
            if (pump_open && !g_parts.launcher) TestEvent("shell_in_open");
            LOGR("vr reload: %s shell in, clip %u, waiting %u, reserve %u", code.c_str(), clip, g_pending, reserve);
            PlaySound(code, kLoad);
        } else if (home) {
            uint32_t n = std::min<uint32_t>(a.size, reserve);
            // a locked slide (or a fired bolt's empty chamber): the rounds wait in the magazine until it's racked
            if (g_locked || g_spent) g_pending = uint16_t(n);
            else clip = uint16_t(n);
            reserve = uint16_t(reserve - n);
            state = 0;
            g_insert = -1;
            g_seating = false;
            xr::Pulse(0, 0.8f, 0.06f);
            xr::Pulse(1, 0.8f, 0.06f);
            TestEvent("mag_in:" + code);
            LOGR("vr reload: %s magazine in, clip %u, reserve %u", code.c_str(), n, reserve);
            PlaySound(code, kLoad);
        } else if (!grab && !g_seating && (g_parts.shells || g_parts.revolver)) {
            state = 0;  // a shell let go of away from the gun: back on the belt (nothing taken)
            g_insert = -1;
        } else if (!grab && !g_seating && reserve >= a.size && HandOver()) {
            // co-op (user): let go of at a partner's chest, it's theirs: one clip off the reserve (its rounds were
            // still counted there), and nothing drops
            reserve = uint16_t(reserve - a.size);
            state = 0;
            g_insert = -1;
            xr::Pulse(0, 0.6f, 0.08f);
            TestEvent("mag_given");
            LOGR("vr reload: %s magazine handed over, reserve %u", code.c_str(), reserve);
        } else if (!grab && !g_seating) {
            // let go of it away from the gun: thrown with the hand's speed (its rounds were never taken)
            {
                std::lock_guard lock(g_lock);
                for (int i = 0; i < 3; i++)
                    g_throw[i] = U[i] * hand[0].vel[0] + U[3 + i] * hand[0].vel[1] + U[6 + i] * hand[0].vel[2];
            }
            state = 1;
            g_insert = -1;
            g_from_hand = true;
            g_out_at = std::chrono::steady_clock::now();
        }
    }
    if (pump_mode) {
        float back_speed = pump_speed;
        static float dev_last_along = 0;
        if (dev_phase >= (tube ? 0 : 4) && dev_phase < (tube ? 3 : 7) && along != dev_last_along)
            back_speed = dir;  // dev: the jump back (a tube: forward)
        dev_last_along = along;
        // locked where it's most forward (a tube: most back)
        if (!pump_counted) g_rack_from = tube ? std::min(g_rack_from, along) : std::max(g_rack_from, along);
        float pull = std::clamp(dir * (g_rack_from - along) / stroke, 0.0f, 1.0f);
        g_slide = pull;
        // a tube opens with a pump's push (user 2026-11-26: the m203 took a 13 cm shove): kPumpStroke at kPumpSpeed,
        // then it runs open the rest of its travel
        const bool far_enough = tube ? dir * (g_rack_from - along) >= kPumpStroke : pull >= 1.0f;
        if (!pump_counted && far_enough && dir * back_speed >= kPumpSpeed) {
            // open: a live round in the chamber is thrown out, the rest wait in the tube for it to close
            pump_counted = true;
            if (tube) g_rack_from = along + dir * stroke, g_slide = 1;  // the hand's place now its open end
            if (tube && code == "B203") VrM203CaseOut();  // the fired round's case drops out of the tube's back
            if (!g_spent && clip > 0) {
                clip--;
                TestEvent("pump_live_out");
                LOGR("vr reload: %s pumped on a live round: thrown out", code.c_str());
            }
            g_pending = uint16_t(g_pending + clip);
            clip = 0;
            g_spent = true;
            PlayRack(code, false);
            xr::Pulse(0, 0.35f, 0.03f);
            TestEvent(tube ? "tube_open:" + code : "pump_open");
            LOGR("vr reload: %s pump open (waiting %u)", code.c_str(), g_pending);
        }
        if (pump_counted && pull < 0.25f) {
            // forward again: chambered
            if (g_pending) clip = uint16_t(clip + g_pending), g_pending = 0;
            g_spent = clip == 0;
            pump_mode = false;
            g_slide = 0;
            G<float>(a.ctrl + kCtrlShotClock) = 0;  // pumped: it fires at once (not after the game's own pump time)
            PlayRack(code, true);
            xr::Pulse(1, 0.5f, 0.04f);
            xr::Pulse(0, 0.4f, 0.03f);
            TestEvent(tube ? "tube_shut:" + code : "pumped");
            g_worked_ms = NowMsR();
            LOGR("vr reload: %s pumped, clip %u", code.c_str(), clip);
        }
    }
    g_rack_held = g_racking || pump_mode;
    // the off hand's trigger is the pinch while it takes or holds a shell (not the game's grenade)
    xr::TakeLeftTrigger((g_parts.shells && !g_parts.launcher && (chest || state == 2)) || g_m203_trigger);
    g_launch_loaded = g_parts.launcher && (clip > 0 || g_pending > 0);
    // the slide: locks back as the last round goes; the off hand's grip held at the slide (or handle) pulls it back
    // along the barrel (taken above); let go and it runs forward, chambering a round if it was locked or pulled
    // most of the way
    if (state == 0 && clip == 0 && !g_pending && !g_locked && !g_parts.shells && !g_parts.revolver) {
        if (g_parts.hold_open) {
            g_locked = true;
            g_slide = 1;
            LOGR("vr reload: %s empty: its slide (or bolt) locked back", code.c_str());
        } else if (!g_spent) {
            g_spent = true;  // closed on an empty chamber: a magazine's rounds wait for a rack
            LOGR("vr reload: %s empty: its bolt forward on an empty chamber", code.c_str());
        }
    }
    if (g_racking) {
        {
            float pull = std::clamp((g_rack_from - along) / kRackPull, 0.0f, 1.0f);
            g_slide = std::max(pull, g_locked ? 1.0f : 0.0f);  // a locked slide is back already
            g_handle = pull;
            // the pull's sound once it's well on its way back (a locked slide: on the grab); its release waits for
            // the hand to let go (syncing either to the motion: TODO)
            if (!g_rack_back_cued && (pull > 0.4f || g_locked)) {
                g_rack_back_cued = true;
                PlayRack(code, false);
                xr::Pulse(0, 0.3f, 0.03f);
            }
            if (!grip) {
                g_racking = false;
                if (pull > 0.7f || g_locked) {
                    // forward it goes, a round chambered: from a magazine seated on the locked slide, or, a round
                    // already in the chamber, that one thrown out (one fewer, as a real gun)
                    if (g_pending) {
                        clip = g_pending, g_pending = 0;
                    } else if (!g_locked && clip > 0) {
                        clip--;
                        LOGR("vr reload: %s racked loaded: a round out, clip %u", code.c_str(), clip);
                    }
                    if (clip) g_locked = false;
                    g_spent = g_parts.shells && clip == 0;  // pumped on nothing: the chamber's empty
                    if (g_parts.bolt) G<float>(a.ctrl + kCtrlShotClock) = 0;  // worked: it fires at once
                    PlayRack(code, true);
                    xr::Pulse(1, 0.7f, 0.05f);
                    TestEvent("rack:" + code);
                    g_worked_ms = NowMsR();
                    LOGR("vr reload: %s racked, clip %u", code.c_str(), clip);
                }
                g_slide = g_locked ? 1.0f : 0.0f;
            }
        }
    }
    if (!g_racking) g_handle = 0;
    if (state != 2) g_seating = false;
    g_state = state;
    last_clip = clip;
    DevReloadScript(code, state, yaw, pitch, head, fwd);
}

// dev_modeflip in the app folder: the fire-mode button pressed for 0.2 s every 6 s, and the held weapon's words
// that differ from 1 s before the press to 1 s after it logged (the controller's first 0x400 bytes, its ammo
// object's 0x100): to find where the mode lives
bool VrDevModePress() {
    static const bool on = std::filesystem::exists(app::ExeDir() / "dev_modeflip");
    if (!on) return false;
    int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now().time_since_epoch()).count() % 6000;
    return ms < 200;
}
void DevModeDump() {
    // dev_modelog: the post effect's mode (the blurs: 1 the reload's) and the weapon's state as they change, with the
    // time since the fire-mode button's last press
    static const bool dev_log = std::filesystem::exists(app::ExeDir() / "dev_modelog");
    if (dev_log && R32(kWorld)) {
        static uint32_t last_post = ~0u, last_state = ~0u;
        static int last_presses = 0;
        static auto pressed = std::chrono::steady_clock::now();
        if (int p = VrModePresses(); p != last_presses) last_presses = p, pressed = std::chrono::steady_clock::now();
        uint32_t post = R32(0x2D1990) ? R32(R32(0x2D1990) + 0x830D4) : 0;
        uint32_t ctrl = weapons::HeldWeapon(), st = ctrl ? R32(ctrl + 0xD8) : 0;
        // the second ammo (the M203's at +0xF8): its clip, its kind's size and reserve word, as they change
        uint32_t alt = ctrl ? R32(ctrl + 0xF8) : 0, akind = alt ? R32(alt + kAmmoKind) : 0;
        uint32_t pool = ctrl ? R32(ctrl + kCtrlReserves) : 0;
        // (its shots count down +0x14, refilled from the kind's +0x98 in state 3: 0x1EFDBE..0x1EFEED)
        uint32_t aclip = alt ? R32(alt + 0x14) * 1000 + G<uint16_t>(alt + kAmmoClip) : 0;
        uint32_t asize = akind ? R32(akind + 0x98) * 1000 + R32(akind + kKindSize) : 0;
        uint32_t ares = akind && pool ? G<uint16_t>(pool + R32(akind + kKindSlot) * 2) : 0;
        static uint32_t last_alt[3] = {~0u, ~0u, ~0u};
        // the player's hand grenades (character +0x286), as they change
        static uint32_t last_nades = ~0u;
        uint32_t me = ctrl ? R32(ctrl + kCtrlOwner) : 0, nades = me ? G<uint16_t>(me + 0x286) : 0;
        if (nades != last_nades) LOGR("dev_modelog: hand grenades %u", nades), last_nades = nades;
        if (aclip != last_alt[0] || asize != last_alt[1] || ares != last_alt[2]) {
            LOGR("dev_modelog: %s second ammo %08X kind %08X +14/clip %u +98/size %u reserve %u (slot %u), state %u",
                 weapons::HeldCode().c_str(), alt, akind, aclip, asize, ares, akind ? R32(akind + kKindSlot) : 0, st);
            last_alt[0] = aclip, last_alt[1] = asize, last_alt[2] = ares;
        }
        if (post != last_post || st != last_state) {
            LOGR("dev_modelog: %s post %u state %u, %.2f s after the mode press", weapons::HeldCode().c_str(), post, st,
                 std::chrono::duration<float>(std::chrono::steady_clock::now() - pressed).count());
            last_post = post, last_state = st;
        }
    }
    static const bool on = std::filesystem::exists(app::ExeDir() / "dev_modeflip");
    if (!on || !R32(kWorld)) return;
    uint32_t ctrl = weapons::HeldWeapon();
    if (!ctrl) return;
    int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now().time_since_epoch()).count() % 6000;
    static std::vector<uint32_t> before;
    static uint32_t before_ctrl = 0;
    static bool took = false, told = false;
    uint32_t ammo = R32(ctrl + kCtrlAmmo);
    auto snap = [&] {
        std::vector<uint32_t> v;
        for (uint32_t o = 0; o < 0x400; o += 4) v.push_back(R32(ctrl + o));
        for (uint32_t o = 0; o < 0x100; o += 4) v.push_back(ammo ? R32(ammo + o) : 0);
        return v;
    };
    if (ms >= 5000 && ms < 5200 && !took) before = snap(), before_ctrl = ctrl, took = true, told = false;
    if (ms >= 1000 && ms < 1200 && took && !told) {
        told = true, took = false;
        if (before_ctrl != ctrl) return;
        std::vector<uint32_t> now = snap();
        std::string line;
        for (size_t i = 0; i < now.size(); i++)
            if (now[i] != before[i]) {
                char b[48];
                std::snprintf(b, sizeof b, " %s+%03zX %X>%X", i < 0x100 ? "c" : "a", (i < 0x100 ? i : i - 0x100) * 4,
                              before[i], now[i]);
                line += b;
            }
        LOGR("vr reload: dev_modeflip %s:%s", weapons::HeldCode().c_str(), line.c_str());
    }
}

// any thread: the slide's pull, 0 forward .. 1 back. dev_rackheld in the app folder: held back (to see it move)
float VrSlide() {
    static const bool held = std::filesystem::exists(app::ExeDir() / "dev_rackheld");
    return held ? 1.0f : g_slide.load(std::memory_order_relaxed);
}
// render thread: the part an empty lock holds back if not the slide (-1), and the handle's pull by hand alone
int VrLockBone() {
    std::lock_guard lock(g_lock);
    return g_parts.lock_bone;
}
float VrHandle() {
    static const bool held = std::filesystem::exists(app::ExeDir() / "dev_rackheld");
    return held ? 1.0f : g_handle.load(std::memory_order_relaxed);
}
// render thread: the held gun's parts (the magazine's top and bottom: the table's, used when not measured)
bool VrGunParts(int& mag, int& slide, float& travel, float* mid, float* up, bool& measure, float& top, float& bottom,
                int& bullets) {
    std::lock_guard lock(g_lock);
    mag = g_parts.mag, slide = g_parts.slide, travel = g_parts.travel;
    std::memcpy(mid, g_parts.mid, 12);
    std::memcpy(up, g_parts.up, 12);
    measure = g_parts.measured;
    top = g_parts.top, bottom = g_parts.bottom, bullets = g_parts.bullets;
    return mag >= 0;
}
// the off hand is racking, or at a part or the chest (its grip is the reload's, not the fore-grip's)
bool VrRacking() { return g_near_rack.load(std::memory_order_relaxed); }
// the off hand holds the slide or handle (its fingers close on it)
bool VrRackHeld() { return g_rack_held.load(std::memory_order_relaxed); }
// game thread: the slide or handle in the off hand (not a pump or tube: those are worked from the fore-grip)
bool VrSlideHeld() { return g_racking; }
// render thread: the held gun's rack point (a bone and a point in the mesh), and where it is this frame (C)
bool VrRackPart(int& bone, float* pt, float* to) {
    std::lock_guard lock(g_lock);
    bone = g_parts.rack_bone;
    std::memcpy(pt, g_parts.rack, 12);
    for (int i = 0; i < 3; i++) to[i] = g_parts.rack[i] + g_parts.rack_out[i] * g_parts.rack_len;
    return bone >= 0;
}
void VrRackAt(const float* c, const float* to, const float* up, const float* fwd) {
    std::lock_guard lock(g_lock);
    std::memcpy(g_rack_at, c, 12);
    std::memcpy(g_rack_to, to, 12);
    std::memcpy(g_rack_up, up, 12);
    std::memcpy(g_rack_fwd, fwd, 12);
    g_rack_ok = true;
}

// the M203's reserve word too (BNS), 0 none
uint32_t VrHideReserve2(uint32_t ctrl) {
    if (!g_manual.load(std::memory_order_relaxed) || !R32(kWorld) || !ctrl) return 0;
    if (R32(ctrl + kCtrlOwner) != Player() || ctrl != g_ctrl.load(std::memory_order_relaxed)) return 0;
    if (weapons::HeldCode() != "BNS") return 0;
    static const bool dev_off = std::filesystem::exists(app::ExeDir() / "dev_nohide2");  // to see the game's own
    if (dev_off) return 0;
    Ammo a;
    return ReadAmmo(a, true) ? a.reserve : 0;
}
// the weapon state machine's wrapper (hooks.cpp): the reserve word to show the game as empty while it runs, the
// player's gun in hand being reloaded by hand; 0 for any other
uint32_t VrHideReserve(uint32_t ctrl) {
    if (!g_manual.load(std::memory_order_relaxed) || !R32(kWorld) || !ctrl) return 0;
    if (R32(ctrl + kCtrlOwner) != Player() || ctrl != g_ctrl.load(std::memory_order_relaxed)) return 0;
    if (std::filesystem::exists(app::ExeDir() / "dev_dumpreload")) return 0;  // recording the game's own reload
    Ammo a;
    if (!ReadAmmo(a)) return 0;  // the rifle's (VrHideReserve2: the M203's)
    static bool told = false;
    if (!told) told = true, LOGR("vr reload: the game's reloads held off for the player's gun");
    return a.reserve;
}

}  // namespace game

// the fire mode's change (config/game.toml at 0x1BB3DB, in FUN_001BB3C0, esi the weapon's controller): the mode has
// advanced already (FUN_001BAE70: the ammo's +0x20, 0..2); next the game would set state 30, which asks for the
// mode animation (FP_<gun>_S_105, request 8) and holds the gun in state 31 for its 2 s, the gun pulled up to the face.
// In the headset (user) none: the player's gun stays idle and ready, the gun's own FireSelect click is played here
// (the animation's trigger played it). false skips the state
bool FireModeAnim(X86Context& ctx, uint8_t*) {
    if (!xr::Active() || ctx.esi != weapons::HeldWeapon()) return true;
    game::PlayNamedSound(weapons::HeldCode(), "FireSelect");
    game::TestEvent("mode_no_anim");
    return false;
}
