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
void Mute(uint32_t len, uint32_t rate);  // a game sample not played (mixer.cpp)
}
namespace weapons {
std::string HeldCode();  // weapons.cpp
bool GunWaves(const std::string& code, std::vector<uint8_t>& out);  // the gun's .wdd from the disc
uint32_t HeldWeapon();
}

namespace game {
float VrViewLift();                           // vr_body.cpp
bool VrLeftOnGun();                           // vr_gun.cpp
bool VrLookAngles(float& yaw, float& pitch);  // vr_body.cpp

namespace {

// the weapon controller and its ammo (NOTES, Reload): the ammo object at +0xF4 has the clip (word
// +0x18) and its kind at +0x08 (the clip's size +0x60, the reserve's slot +0x64); the reserves are words at
// [controller +0xFC] by slot
constexpr uint32_t kCtrlAmmo = 0xF4, kCtrlReserves = 0xFC, kCtrlState = 0xD8, kCtrlOwner = 0xF0;
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
    // the rack zone runs from the rack point this far along this (mesh) direction, an invisible extension of a knob
    // (the user's: a knob is too small to close a fist on), taken within rack_reach of it (0: kRackReach); a slide
    // taken palm over is the point alone
    float rack_out[3] = {};
    float rack_len = 0, rack_reach = 0;
    // the bolt is worked by hand after every shot (the walther: the user's): a shot's next round waits for a rack
    bool bolt = false;
};
// a rifle's handle: the zone 3 cm on from the knob's tip, 3.5 cm round it (user tests 6 and 7). Knobs from each
// slide bone's outermost vertices; mesh +x is the player's side (the game's guns eject to the left). Travels are
// guesses (the game's own reloads would show them); the pistols' frame parts move 3.7 cm against the slide
constexpr float kKnobLen = 0.03f, kKnobReach = 0.035f;
// AK-47: magazine 81 (measured on the headset: up 0.94 -0.33), its rounds 87, the bolt carrier 84 with the handle's
// knob at its front on the port side (the vertex dumps that had it painted on held only part of the gun's draw)
const GunParts kAk = {.mag = 81, .slide = 84, .travel = 0.07f, .mid = {0, -0.054f, 0.127f}, .up = {0, 0.94f, -0.33f},
                      .top = 0.100f, .bottom = -0.103f, .measured = true, .bullets = 87, .rack_bone = 84,
                      .rack = {0.031f, 0.065f, 0.132f}, .hold = 0.05f, .depth = 0.04f, .rack_out = {1, 0, 0},
                      .rack_len = kKnobLen, .rack_reach = kKnobReach};
// the pistols' slides are taken anywhere over the top of their back half, or its sides (user): the zone a line 1.4 cm
// over the slide's top from its rear end to its middle, 4 cm round it
constexpr float kSlideReach = 0.04f;
// pistol: magazine 114 in the grip, slide 117 (measured on the headset; z -0.094..0.129, its top y 0.066)
const GunParts kPistol = {.mag = 114, .slide = 117, .travel = 0.037f, .mid = {0, -0.022f, -0.057f},
                          .up = {0, 0.98f, 0.19f}, .top = 0.044f, .bottom = -0.051f, .measured = true, .rack_bone = 117,
                          .rack = {0, 0.080f, -0.094f}, .hold = 0.025f, .rack_out = {0, 0, 1}, .rack_len = 0.111f,
                          .rack_reach = kSlideReach};
// glock: magazine 117 in the grip, its rounds 114, slide 120 (z -0.090..0.101, its top y 0.060)
const GunParts kGlock = {.mag = 117, .slide = 120, .travel = 0.035f, .mid = {0, -0.012f, -0.047f},
                         .up = {0, 0.95f, 0.31f}, .top = 0.050f, .bottom = -0.078f, .measured = true, .bullets = 114,
                         .rack_bone = 120, .rack = {0, 0.074f, -0.090f}, .hold = 0.03f, .rack_out = {0, 0, 1},
                         .rack_len = 0.096f, .rack_reach = kSlideReach};
// g36c: magazine 78, the handle on 81
const GunParts kG36 = {.mag = 78, .slide = 81, .travel = 0.07f, .mid = {0, -0.055f, 0.075f}, .up = {0, 0.99f, -0.14f},
                       .top = 0.069f, .bottom = -0.069f, .rack_bone = 81, .rack = {0.025f, 0.076f, 0.100f},
                       .hold = 0.04f, .depth = 0.03f, .rack_out = {1, 0, 0}, .rack_len = kKnobLen,
                       .rack_reach = kKnobReach};
// m16: magazine 78, its rounds 81, the T handle 84 at the receiver's rear: its zone runs back from the T
const GunParts kM16 = {.mag = 78, .slide = 84, .travel = 0.07f, .mid = {0, -0.010f, 0.077f}, .up = {0, 1, 0},
                       .top = 0.056f, .bottom = -0.085f, .bullets = 81, .rack_bone = 84, .rack = {0, 0.095f, -0.100f},
                       .hold = 0.035f, .depth = 0.05f, .rack_out = {0, 0, -1}, .rack_len = kKnobLen,
                       .rack_reach = kKnobReach};
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
                          .hold = 0.02f, .depth = 0.03f, .rack_out = {1, 0, 0}, .rack_len = kKnobLen,
                          .rack_reach = kKnobReach, .bolt = true};
// m249: the box 99 at the side under the feed (its belt, bones 102-111, stays in the gun), the handle 114
const GunParts kM249 = {.mag = 99, .slide = 114, .travel = 0.07f, .mid = {0.025f, 0.035f, 0.128f}, .up = {0, 1, 0},
                        .top = 0.031f, .bottom = -0.045f, .rack_bone = 114, .rack = {0.035f, 0.077f, 0.082f},
                        .hold = 0.02f, .depth = 0.03f, .rack_out = {1, 0, 0}, .rack_len = kKnobLen,
                        .rack_reach = kKnobReach};
// the silver and silenced ones are the same models (their bones checked on the disc)
const std::map<std::string, GunParts> kParts = {
    {"AK1", kAk},     {"AK5", kAk},       {"PST", kPistol}, {"PS5", kPistol}, {"GK1", kGlock}, {"GK5", kGlock},
    {"ASR", kG36},    {"AS5", kG36},      {"M16", kM16},    {"M45", kM16},    {"BNS", M203()}, {"MP1", kMp5},
    {"MP5", kMp5},    {"P90", kP90},      {"P95", kP90},    {"SMG", kUzi},    {"SM5", kUzi},   {"SM3", kMac10},
    {"MC5", kMac10},  {"SNR", kWa2000},   {"SN5", kWa2000}, {"HVY", kM249},   {"HV5", kM249},
};
// the user's zones are tight (1-2 cm past where the hand closes on the part), so the fore-grip and the magazine
// don't overlap them; reaches are from the controller's grip point
constexpr float kRackReach = 0.025f;  // metres from the rack point the off hand's grip takes the slide (or handle)
constexpr float kRackPull = 0.06f;    // metres back the hand pulls for a full rack

// the chest: a column under the headset, its top this far below the head's middle, its bottom, its radius, and
// its middle this far ahead of the head's middle
constexpr float kChestTop = 0.15f, kChestBottom = 0.50f, kChestRadius = 0.09f, kChestAhead = 0.04f;
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

void PlaySound(const std::string& code, Sound which) {
    auto it = kGunAudio.find(code);
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
bool g_locked = false;
uint16_t g_pending = 0;
bool g_spent = false;  // a bolt worked by hand, fired: the chamber empty, the bolt forward (the rounds wait for a rack)
bool g_racking = false;
float g_rack_from = 0;
// each gun's slide while it's put away (a switch mustn't carry one gun's lock or pending rounds to the next)
struct SlideKeep { bool locked = false, spent = false; uint16_t pending = 0; };
std::map<std::string, SlideKeep> g_kept;
std::atomic<bool> g_manual{false};  // the held gun is reloaded by hand
// reloaded by the game still: shells, launchers (phase 5 later) and the magnum (a revolver's speed loader: the
// user's, not like the pistols)
const char* const kGameReload[] = {"SHG", "SH5", "RM1", "RM5", "RPG", "GRL", "MGN", "MG5"};
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
};
bool ReadAmmo(Ammo& a) {
    if (!R32(kWorld)) return false;
    a.ctrl = weapons::HeldWeapon();
    if (!a.ctrl) return false;
    a.ammo = R32(a.ctrl + kCtrlAmmo);
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
// the magazine falling now left the hand (VrPoseGun throws it from there)
bool VrMagFromHand() { return g_from_hand.load(std::memory_order_relaxed); }

// game thread, each gun frame: the hands' part of a reload
void VrReloadTick() {
    static std::string code;
    // the dev markers' timers and one-shots, from each gun's switch (dev_give cycles guns in one run)
    static auto since = std::chrono::steady_clock::now();
    static bool dev_done = false, fresh_done = false, rs_pull = false, rs_rel = false;
    std::string now_code = weapons::HeldCode();
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
    // a bolt worked by hand: the game's own cocking after each shot isn't heard (ours plays at the release)
    {
        auto au = kGunAudio.find(code);
        bool mute = manual && it != kParts.end() && it->second.bolt && au != kGunAudio.end();
        const GunSound& rel = mute ? au->second.sound[kRelease] : GunSound{};
        sound::Mute(mute ? rel.len : 0, mute ? rel.rate : 0);
    }
    if (!manual) {
        g_state = 0;
        return;
    }
    Ammo a;
    xr::Hand hand[2];
    float head[3], yaw, pitch;
    if (!ReadAmmo(a) || !xr::Hands(hand[0], hand[1], head) || !VrLookAngles(yaw, pitch)) return;
    uint16_t& clip = G<uint16_t>(a.ammo + kAmmoClip);
    uint16_t& reserve = G<uint16_t>(a.reserve);
    // a bolt worked by hand: a shot (the clip down since the last tick, nothing of ours) leaves the rest waiting for
    // a rack, the bolt forward (none left: it locks back below)
    static uint16_t last_clip = 0;
    static std::string last_clip_code;
    if (last_clip_code != code) last_clip_code = code, last_clip = clip;
    if (g_parts.bolt && g_state == 0 && !g_racking && clip < last_clip) {
        g_pending = clip;
        clip = 0;
        g_spent = true;
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
    static bool a_was = false, grab_was = false;
    bool a_edge = hand[1].primary && !a_was, grab = hand[0].grip > 0.6f, grab_edge = grab && !grab_was;
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
    near_rack = near_rack && (state == 0 || state == 1) && !VrLeftOnGun();
    // at the magazine in the gun (its body, not the gun round it)
    bool near_mag = false;
    if (state == 0 && g_bone >= 0 && !VrLeftOnGun()) {
        std::lock_guard lock(g_lock);
        if (g_well_ok) {
            float v[3] = {hc[0] - g_well[0], hc[1] - g_well[1], hc[2] - g_well[2]};
            float t = std::clamp(v[0] * g_axis[0] + v[1] * g_axis[1] + v[2] * g_axis[2], -kPullDown, kPullUp);
            float d2 = 0;
            for (int i = 0; i < 3; i++) d2 += (v[i] - g_axis[i] * t) * (v[i] - g_axis[i] * t);
            near_mag = d2 < kPullReach * kPullReach;
        }
    }
    if (!g_racking && near_rack && grip_edge) {
        g_racking = true;
        g_rack_from = along;
        g_rack_back_cued = false;
        xr::Pulse(0, 0.4f, 0.03f);
    }
    g_rack_held = g_racking;
    // the off hand on the gun's fore-grip: its grip is the fore-grip's, nothing of the reload's
    if (VrLeftOnGun() || g_racking) grab_edge = false;
    // entering the chest with a magazine to take: a light buzz
    static bool chest_was = false;
    bool chest = (state == 0 || state == 1) && reserve > 0 && !VrLeftOnGun() && !g_racking && NearChest(hand[0], head);
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
    if (state != 0 && clip) {
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
    if (state == 0 && a_edge) {
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
        // a fresh one from the chest: into an empty well, or to knock the old one out with
        state = state == 1 ? 2 : 3;
        xr::Pulse(0, 0.5f, 0.04f);
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
    } else if (state == 0 && grab_edge && g_bone >= 0) {
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
            }
        }
        // diagnostic: the held top's distance from the well twice a second while it's within 15 cm
        static auto told = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (known && dist < 0.15f && now - told > std::chrono::milliseconds(500)) {
            told = now;
            LOGR("vr reload: %s held top %.3f from the well, %.3f below its seat", code.c_str(), dist, below);
        }
        if (!g_seating && known && dist < kSeatReach) {
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
        if (home) {
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
            LOGR("vr reload: %s magazine in, clip %u, reserve %u", code.c_str(), n, reserve);
            PlaySound(code, kLoad);
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
    // the slide: locks back as the last round goes; the off hand's grip held at the slide (or handle) pulls it back
    // along the barrel (taken above); let go and it runs forward, chambering a round if it was locked or pulled
    // most of the way
    if (state == 0 && clip == 0 && !g_pending && !g_locked) {
        g_locked = true;
        g_slide = 1;
    }
    if (g_racking) {
        {
            float pull = std::clamp((g_rack_from - along) / kRackPull, 0.0f, 1.0f);
            g_slide = std::max(pull, g_locked ? 1.0f : 0.0f);  // a locked slide is back already
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
                    g_spent = false;
                    PlayRack(code, true);
                    xr::Pulse(1, 0.7f, 0.05f);
                    LOGR("vr reload: %s racked, clip %u", code.c_str(), clip);
                }
                g_slide = g_locked ? 1.0f : 0.0f;
            }
        }
    }
    if (state != 2) g_seating = false;
    g_state = state;
    last_clip = clip;
}

// any thread: the slide's pull, 0 forward .. 1 back. dev_rackheld in the app folder: held back (to see it move)
float VrSlide() {
    static const bool held = std::filesystem::exists(app::ExeDir() / "dev_rackheld");
    return held ? 1.0f : g_slide.load(std::memory_order_relaxed);
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

// the weapon state machine's wrapper (hooks.cpp): the reserve word to show the game as empty while it runs, the
// player's gun in hand being reloaded by hand; 0 for any other
uint32_t VrHideReserve(uint32_t ctrl) {
    if (!g_manual.load(std::memory_order_relaxed) || !R32(kWorld) || !ctrl) return 0;
    if (R32(ctrl + kCtrlOwner) != Player() || ctrl != g_ctrl.load(std::memory_order_relaxed)) return 0;
    if (std::filesystem::exists(app::ExeDir() / "dev_dumpreload")) return 0;  // recording the game's own reload
    Ammo a;
    if (!ReadAmmo(a)) return 0;
    static bool told = false;
    if (!told) told = true, LOGR("vr reload: the game's reloads held off for the player's gun");
    return a.reserve;
}

}  // namespace game
