// blackquest phase 5: reloading by hand. In vr with manual reload on (VR SETTINGS), for a gun whose magazine is
// known, the game's own reload never starts (the weapon state machine's reload request is cleared: hooks.cpp); the
// player ejects the magazine with A on the gun hand (or pulls it out with the off hand's grip), takes a fresh one at
// the chest with the off hand's grip and seats it at the magazine well, then racks with the grip at the slide or
// charging handle (the user keeps the off hand's trigger for grenades and the m16's launcher). Seating fills the
// clip from the reserve. The magazine is one of the gun's bones
// (the AK's 81, the pistol's 114: NOTES 2026-11-21); the renderer moves or hides it
// (vr_gun.cpp VrPoseGun) as this says
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>

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
}
namespace weapons {
std::string HeldCode();  // weapons.cpp
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

// the guns' moving parts (their mesh's own bones, found by recording the game's reload: NOTES 2026-11-21): the
// magazine, and the slide where it is a bone of its own, which goes back along the mesh's -z by its travel (the
// pistol's 117 is the slide and its top; the frame's parts move against it). The magazine's middle in the mesh
// measured: the magazine's middle and up from its bone's vertices; else the table's (the AK's bone 87 vertices
// measure as a 72 vertex piece at the receiver, not the curved magazine below it)
// retag: a part modelled into the gun's body (the AK's charging handle): the body's vertices in this box (the
// mesh's x, y, z ranges) are moved onto a spare bone (the slide), which then follows the body unless a rack moves it
struct GunParts {
    int mag, slide;
    float travel;
    float mid[3], up[3];
    bool measured;
    int retag_from;
    float box[6];
    // where the off hand's palm takes the slide (or charging handle): a point in the mesh, on this bone
    int rack_bone;
    float rack[3];
    // held in the fist (upright, its top out past the thumb): its middle this far from the grip point along the fist
    float hold;
};
constexpr int kSpareBone = 3;  // c-33..-31: an identity no mesh of the gun list uses
const std::map<std::string, GunParts> kParts = {
    // the AK's magazine is bone 81 (found by collapsing bones in halves, side on: NOTES); measured from its vertices
    // rack, from the meshes' vertices (NOTES): the AK's charging handle is a block on the receiver's +x side (the
    // game's inside) at z 0.172, y 0.043..0.057, the palm 2 cm out from it; the pistol's slide (117) ends at z
    // -0.094, its top at y 0.071: the palm over its rear
    // the handle (12 vertices, the only part of the receiver not mirrored in x) moves onto the spare bone, the
    // AK's slide, 7 cm back at full pull
    {"AK1", {81, kSpareBone, 0.07f, {0, -0.03f, 0.12f}, {0, 0.97f, 0.24f}, true, 75,
             {0.0155f, 0.03f, 0.040f, 0.059f, 0.169f, 0.175f}, kSpareBone, {0.037f, 0.05f, 0.172f}, 0.05f}},
    {"AK5", {81, kSpareBone, 0.07f, {0, -0.03f, 0.12f}, {0, 0.97f, 0.24f}, true, 75,
             {0.0155f, 0.03f, 0.040f, 0.059f, 0.169f, 0.175f}, kSpareBone, {0.037f, 0.05f, 0.172f}, 0.05f}},
    {"PST", {114, 117, 0.037f, {0, -0.046f, -0.062f}, {0, 0.96f, 0.28f}, true, -1, {}, 117, {0, 0.085f, -0.078f}, 0.025f}},
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
// the rack's sounds (sound::Cue, mixer.cpp): the handle pulled back, and run forward
constexpr int kCueBack = 0, kCueForward = 1;
// lining up: the held magazine's middle within this far of the well's axis, this far below it at most, and
// turned within about 35 degrees of it; home when this close
constexpr float kAlignSide = 0.035f, kAlignOut = 0.05f, kHome = 0.015f, kAlignTurn = 0.82f;
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
GunParts g_parts{-1, -1, 0, {}, {0, 1, 0}, false, -1, {}, -1, {}, 0};  // the held gun's (game thread writes, render reads: copied under g_lock)
// the slide: locked back when the gun ran dry; a magazine seated then waits for a rack (its rounds pending);
// the off hand's grip near it pulls it (0 forward .. 1 back)
std::atomic<float> g_slide{0};
bool g_locked = false;
uint16_t g_pending = 0;
bool g_racking = false;
float g_rack_from = 0;
std::atomic<bool> g_manual{false};  // the held gun is reloaded by hand
// reloaded by the game still: shells and launchers (phase 5 later)
const char* const kGameReload[] = {"SHG", "SH5", "RPG", "GRL"};
std::mutex g_lock;
float g_well[3];  // the magazine's middle where it sits in the gun, C (renderer, each gun draw)
float g_axis[3] = {0, 1, 0};  // the way it goes into the gun (its up), C
float g_throw[3] = {};        // a thrown magazine's velocity, C
std::atomic<float> g_insert{-1};
float g_rack_at[3];            // the rack point in C (renderer, each gun draw)
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
// render thread: the well this frame
void VrMagWell(const float* c, const float* axis) {
    std::lock_guard lock(g_lock);
    std::memcpy(g_well, c, 12);
    std::memcpy(g_axis, axis, 12);
    g_well_ok = true;
}
// render thread: how far out of the well a lined-up magazine is (metres), -1 not lined up
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
    std::string now_code = weapons::HeldCode();
    if (now_code != code) {
        // a weapon switch: its magazine is in it
        code = now_code;
        g_state = 0;
        std::lock_guard lock(g_lock);
        g_well_ok = false;
        g_rack_ok = false;
    }
    g_ctrl = weapons::HeldWeapon();
    auto it = kParts.find(code);
    bool manual = On() && !code.empty();
    for (const char* c : kGameReload)
        if (code == c) manual = false;
    g_manual = manual;
    g_bone = manual && it != kParts.end() ? it->second.mag : -1;
    {
        std::lock_guard lock(g_lock);
        g_parts = manual && it != kParts.end() ? it->second : GunParts{-1, -1, 0, {}, {0, 1, 0}, false, -1, {}, -1, {}, 0};
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
    // dev_eject in the app folder: A pressed once, 8 s into this gun (to see the magazine fall without controllers)
    static const bool dev_eject = std::filesystem::exists(app::ExeDir() / "dev_eject");
    static auto since = std::chrono::steady_clock::now();
    static bool dev_done = false;
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
            float d2 = 0;
            for (int i = 0; i < 3; i++) d2 += (hc[i] - g_rack_at[i]) * (hc[i] - g_rack_at[i]);
            near_rack = d2 < kRackReach * kRackReach;
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
    static bool fresh_done = false;
    if (!fresh_done && state == 0 && std::filesystem::exists(app::ExeDir() / "dev_magfresh") &&
        std::chrono::duration<float>(std::chrono::steady_clock::now() - since).count() > 8)
        fresh_done = true, state = 3, grab_was = true;
    // dev_maghand: 2 s after an eject a magazine is in the off hand (to see it held, and the dropped one land)
    if (state == 1 && VrMagOutSeconds() > 2 && std::filesystem::exists(app::ExeDir() / "dev_maghand")) state = 2;
    if (state == 2 && std::filesystem::exists(app::ExeDir() / "dev_maghand")) grab = true, grab_was = true;
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
        // eject: the rounds left go back to the reserve (none lost with the magazine), the clip is empty
        reserve = uint16_t(std::min<uint32_t>(reserve + clip, 0xFFFF));
        clip = 0;
        state = 1;
        g_from_hand = false;
        {
            std::lock_guard lock(g_lock);
            for (float& x : g_throw) x = 0;  // dropped
        }
        g_out_at = std::chrono::steady_clock::now();
        xr::Pulse(1, 0.4f, 0.05f);
        LOGR("vr reload: %s magazine out, reserve %u (ctrl %08X, magazine %08X)", code.c_str(), reserve, a.ctrl, a.ammo);
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
            reserve = uint16_t(std::min<uint32_t>(reserve + clip, 0xFFFF));
            clip = 0;
            state = 2;
            g_from_hand = false;
            g_out_at = std::chrono::steady_clock::now();
            xr::Pulse(0, 0.7f, 0.05f);
            xr::Pulse(1, 0.5f, 0.05f);
            LOGR("vr reload: %s magazine knocked out with the fresh one, reserve %u", code.c_str(), reserve);
        }
    } else if (state == 0 && grab_edge && g_bone >= 0) {
        // the off hand's grip at the magazine in the gun (its body, not the gun round it): out it comes into the
        // hand (its rounds back to the reserve, as an eject)
        if (near_mag) {
            reserve = uint16_t(std::min<uint32_t>(reserve + clip, 0xFFFF));
            clip = 0;
            state = 2;
            g_out_at = std::chrono::steady_clock::now() - std::chrono::seconds(1000);  // it's the hand's now
            xr::Pulse(0, 0.5f, 0.04f);
            LOGR("vr reload: %s magazine pulled out by hand, reserve %u", code.c_str(), reserve);
        }
    } else if (state == 2 && std::filesystem::exists(app::ExeDir() / "dev_maghand")) {
        g_insert = -1;  // dev_maghand: held, never lined up (the untracked controllers would line it up)
    } else if (state == 2 && std::filesystem::exists(app::ExeDir() / "dev_magslide")) {
        g_insert = 0.06f;  // dev_magslide: held 6 cm out along the well's line (to see the line without hands)
    } else if (state == 2) {
        // the off hand's palm against the well: lined up with it (near its axis, below it), the magazine slides
        // along the axis with the hand and seats when it's pushed home
        float pc[3], pu[3] = {0, 1, 0}, pf[3];
        if (!HeldMagOf(hand[0], yaw, pitch, g_parts.hold, pc, pu, pf)) PalmInCamera(hand[0], yaw, pitch, pc);
        float out = -1, lateral = 1, turn = 0;
        bool known = false;
        {
            std::lock_guard lock(g_lock);
            if (g_well_ok && g_bone >= 0) {
                known = true;
                turn = pu[0] * g_axis[0] + pu[1] * g_axis[1] + pu[2] * g_axis[2];
                float v[3] = {pc[0] - g_well[0], pc[1] - g_well[1], pc[2] - g_well[2]};
                float into = v[0] * g_axis[0] + v[1] * g_axis[1] + v[2] * g_axis[2];
                out = -into;
                float l2 = 0;
                for (int i = 0; i < 3; i++) l2 += (v[i] - g_axis[i] * into) * (v[i] - g_axis[i] * into);
                lateral = std::sqrt(l2);
            }
        }
        bool lined = false, home = false;
        if (known) {
            lined = lateral < kAlignSide && out < kAlignOut && out > -0.05f && turn > kAlignTurn;
            home = lined && out < kHome;
            g_insert = lined ? std::max(out, 0.0f) : -1.0f;
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
            // a locked slide: the rounds wait in the magazine until it's racked
            if (g_locked) g_pending = uint16_t(n);
            else clip = uint16_t(n);
            reserve = uint16_t(reserve - n);
            state = 0;
            g_insert = -1;
            xr::Pulse(0, 0.8f, 0.06f);
            xr::Pulse(1, 0.8f, 0.06f);
            LOGR("vr reload: %s magazine in, clip %u, reserve %u", code.c_str(), n, reserve);
        } else if (!grab && !lined) {
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
            // back against its stop: the pull's click (a locked slide is there already: its click on the grab)
            if (!g_rack_back_cued && (pull > 0.9f || g_locked)) {
                g_rack_back_cued = true;
                sound::Cue(kCueBack);
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
                    sound::Cue(kCueForward);
                    xr::Pulse(1, 0.7f, 0.05f);
                    LOGR("vr reload: %s racked, clip %u", code.c_str(), clip);
                }
                g_slide = g_locked ? 1.0f : 0.0f;
            }
        }
    }
    g_state = state;
}

// any thread: the slide's pull, 0 forward .. 1 back
float VrSlide() { return g_slide.load(std::memory_order_relaxed); }
// render thread: the held gun's parts
bool VrGunParts(int& mag, int& slide, float& travel, float* mid, float* up, bool& measure) {
    std::lock_guard lock(g_lock);
    mag = g_parts.mag, slide = g_parts.slide, travel = g_parts.travel;
    std::memcpy(mid, g_parts.mid, 12);
    std::memcpy(up, g_parts.up, 12);
    measure = g_parts.measured;
    return mag >= 0;
}
// render thread: the held gun's magazine to move onto its spare bone: the body bone and the box (false: none)
bool VrMagRetag(int& from, float* box) {
    std::lock_guard lock(g_lock);
    from = g_parts.retag_from;
    std::memcpy(box, g_parts.box, sizeof(g_parts.box));
    return from >= 0;
}
// the off hand is racking, or at a part or the chest (its grip is the reload's, not the fore-grip's)
bool VrRacking() { return g_near_rack.load(std::memory_order_relaxed); }
// the off hand holds the slide or handle (its fingers close on it)
bool VrRackHeld() { return g_rack_held.load(std::memory_order_relaxed); }
// render thread: the held gun's rack point (a bone and a point in the mesh), and where it is this frame (C)
bool VrRackPart(int& bone, float* pt) {
    std::lock_guard lock(g_lock);
    bone = g_parts.rack_bone;
    std::memcpy(pt, g_parts.rack, 12);
    return bone >= 0;
}
void VrRackAt(const float* c) {
    std::lock_guard lock(g_lock);
    std::memcpy(g_rack_at, c, 12);
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
