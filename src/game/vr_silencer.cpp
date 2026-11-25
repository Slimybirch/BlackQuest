// The silencer by hand (user, 2026-11-24). A gun whose silencer the player has (on it when it came to hand, or put
// on since) keeps it in a holster on the left hip while it's off. The off hand's grip there takes it; brought to the
// muzzle, within a few centimetres and roughly along the barrel, it lines up; one twist (clockwise seen from
// behind the gun) locks it on: the gun's suppressor flag goes on (weapons.cpp). On the gun, the off hand's grip at
// the muzzle and the twist the other way takes it off into the hand. Let go of anywhere but locked on, it goes back
// to the holster. The game's suppressor button stays as it was.
// Still to do with the headset: the silencer drawn in the hand (the gun model's own part, once found in a dump), and
// the tail of the game's suppressor animation played as it locks
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "app/app.h"
#include "game/layout.h"
#include "xdk/hle.h"

#include "core/log.h"
#include "xr.h"

namespace game {
bool VrGunLocal(float* A, float* muzzle);  // vr_gun.cpp: the gun's axes and its muzzle in local space, as drawn
bool HudShown();  // hud.cpp: the player's hud is up (in play)
void VrTouch(int hand, float amp);  // vr_gun.cpp: a light touch
bool VrWorldToLocal(const float* w, float* local);  // vr_gun.cpp
bool VrPlayerWorld(float* eye, float* fwd, float* floor_y);  // vr_gun.cpp
void TestEvent(const std::string& name);   // vr_totest.cpp
}  // namespace game
namespace weapons {
int SuppressorHeld(uint32_t& weapon);  // -1 the held gun takes none, 0 off, 1 on; weapon: the held one
void SetSuppressorHeld(bool on);
bool RequestSuppressorAnim(bool attach);  // the game's own attach (true) or detach animation
bool CancelSuppressorAttach();             // the game's attach on the gun in hand called off, its flag off
uint32_t HeldAnim();                       // the held gun's first person animation id
void PickupMessage(int kind);              // the game's pickup message (its sound and line), 4 the suppressors
}  // namespace weapons

namespace game {
namespace {

enum class S { kNone, kOnGun, kHolster, kHand, kLined, kUnscrew };
S g_state = S::kNone;
uint32_t g_weapon = 0;            // the gun the state is for
std::set<uint32_t> g_owned;       // guns whose silencer the player has (seen on, or put on)
float g_roll0 = 0;                // the hand's roll about the barrel where the twist started
bool g_grip_was = false;
std::atomic<int> g_shown{0};      // for the renderer and the checklist: the state, as an int
int64_t g_wait_until = 0;         // the game's own attach or detach runs till then: its flag not read
// the inventory (user): one silencer, picked up from the ground or put back on the hip; on a gun it's that gun's
std::atomic<bool> g_carried{false};
// a suppressor pickup of the level's, taken (game::sub_001BED90): the attach the game starts with it is called off
// till then, so it stays in the inventory
int64_t g_block_until = 0;
// a silencer on the ground (the settings' test button lays one 1.2 m ahead, across the way), world space; walked over
// (within 0.7 m across the floor): into the inventory with the game's pickup message
std::atomic<bool> g_lay{false};
std::mutex g_ground_lock;
bool g_ground = false;
float g_ground_at[3], g_ground_dir[3];

constexpr float kGripOn = 0.6f, kGripOff = 0.35f;
constexpr float kLineUp = 0.04f;       // within this of the muzzle it lines up
constexpr float kLineAngle = 0.82f;    // and its axis within ~35 degrees of the barrel's (cos)
constexpr float kLeave = 0.07f;        // farther than this, lined up no more
constexpr float kTwist = 1.05f;        // one twist: ~60 degrees
constexpr float kMuzzleReach = 0.06f;  // the off hand at the muzzle, to unscrew

// the left hip: a ball to the left of the waist and a little behind, turned with the head's yaw (local space)
constexpr float kHipRight = -0.20f, kHipBack = 0.04f, kHipDown = 0.80f, kHipReach = 0.16f;
bool NearLeftHip(const xr::Hand& h, const float* head) {
    float hy, hp;
    if (!xr::HeadAngles(hy, hp)) return false;
    float d[3] = {h.pos[0] - head[0], h.pos[1] - head[1], h.pos[2] - head[2]};
    float right = d[0] * std::cos(hy) - d[2] * std::sin(hy), back = d[0] * std::sin(hy) + d[2] * std::cos(hy);
    float e[3] = {right - kHipRight, back - kHipBack, d[1] + kHipDown};
    return e[0] * e[0] + e[1] * e[1] + e[2] * e[2] < kHipReach * kHipReach;
}

// a quaternion's axes (columns: x, y, z)
void Axes(const float* q, float* R) {
    float xx = q[0] * q[0], yy = q[1] * q[1], zz = q[2] * q[2];
    float xy = q[0] * q[1], xz = q[0] * q[2], yz = q[1] * q[2], wx = q[3] * q[0], wy = q[3] * q[1], wz = q[3] * q[2];
    float r[9] = {1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy), 2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx),
                  2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy)};
    for (int i = 0; i < 9; i++) R[i] = r[i];
}

// the hand's roll about the barrel: its up (+y) in the plane across the barrel, against the gun's own up and right
float Roll(const float* hq, const float* A) {
    float H[9];
    Axes(hq, H);
    float up[3] = {H[1], H[4], H[7]};
    float gx = up[0] * A[0] + up[1] * A[3] + up[2] * A[6];  // along the gun's right
    float gy = up[0] * A[1] + up[1] * A[4] + up[2] * A[7];  // along the gun's up
    return std::atan2(gx, gy);
}

float Wrap(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

void Set(S s, const char* why) {
    if (s == g_state) return;
    g_state = s;
    g_shown.store(int(s), std::memory_order_relaxed);
    LOGR("vr silencer: %s", why);
}

}  // namespace

namespace {
void GroundTick() {
    float eye[3], fwd[3], floor_y;
    if (!HudShown() || !VrPlayerWorld(eye, fwd, &floor_y)) return;
    {
        // dev_spawn_silencer: the test button pressed once, 8 s into play
        static const bool dev = std::filesystem::exists(app::ExeDir() / "dev_spawn_silencer");
        static int64_t first = 0;
        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (dev && !first) first = now;
        if (dev && first > 0 && now - first > 8000) {
            first = -1;
            g_lay = true;
        }
    }
    std::lock_guard lock(g_ground_lock);
    if (g_lay.exchange(false)) {
        float h = std::sqrt(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
        if (h > 1e-3f) {
            float f[3] = {fwd[0] / h, 0, fwd[2] / h};
            g_ground_at[0] = eye[0] + f[0] * 1.2f, g_ground_at[1] = floor_y + 0.017f, g_ground_at[2] = eye[2] + f[2] * 1.2f;
            g_ground_dir[0] = -f[2], g_ground_dir[1] = 0, g_ground_dir[2] = f[0];
            g_ground = true;
            LOGR("vr silencer: one laid on the ground ahead (%.1f %.1f %.1f)", g_ground_at[0], g_ground_at[1], g_ground_at[2]);
        }
        return;
    }
    if (!g_ground) return;
    float dx = eye[0] - g_ground_at[0], dz = eye[2] - g_ground_at[2], dy = eye[1] - g_ground_at[1];
    if (dx * dx + dz * dz < 0.7f * 0.7f && dy > -0.5f && dy < 2.4f) {
        g_ground = false;
        g_carried = true;
        weapons::PickupMessage(4);
        LOGR("vr silencer: picked up from the ground into the inventory");
        TestEvent("silencer_picked");
    }
}
}  // namespace

// game thread, each tick (weapons.cpp WeaponsTick)
void VrSilencerTick() {
    GroundTick();
    uint32_t weapon = 0;
    int on = weapons::SuppressorHeld(weapon);
    if (on < 0 || !weapon) {
        g_weapon = 0;
        Set(S::kNone, "no gun that takes one in hand");
        return;
    }
    if (on == 1) g_owned.insert(weapon);
    if (weapon != g_weapon) {
        // another gun in hand: its silencer on it, or in the holster if it has one, or none
        g_weapon = weapon;
        Set(on ? S::kOnGun : g_carried ? S::kHolster : S::kNone, "a gun came to hand");
    }
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    // the game's own attach, not asked for by the twist (a level's pickup, or a level that fits one at its start, as
    // mission 2): called off before the flag goes on, the silencer into the inventory instead (user: picked up it's
    // carried, fitted by hand)
    if (now_ms >= g_wait_until) {
        uint32_t anim = weapons::HeldAnim();
        if ((anim == 0x17 || anim == 0x19 || now_ms < g_block_until) && weapons::CancelSuppressorAttach()) {
            on = 0;
            // (mission 2's start puts the flag on by itself ~7 s after its attach began: watched 12 s more)
            if (now_ms >= g_block_until) {
                g_carried = true;
                LOGR("vr silencer: the game's attach called off (anim 0x%X): into the inventory", anim);
                TestEvent("silencer_picked");
            } else {
                LOGR("vr silencer: the game's flag put on again: off (in the inventory)");
            }
            g_block_until = std::max(g_block_until, now_ms + 12000);
        }
    }
    if (g_state == S::kNone && on == 1) Set(S::kOnGun, "the gun has one now");
    if (g_state == S::kNone && g_carried) Set(S::kHolster, "one in the inventory");
    if (g_state == S::kNone) return;
    {
        // dev_silencer "hip" or "hand": an owned silencer on the gun taken off once, 8 s after it's seen, through the
        // game's detach (tests with test hands: the holster and the icon, or the model in the hand); "cycle": off to
        // the hip, then 6 s later on again through the game's attach
        static const std::string want = [] {
            std::ifstream f(app::ExeDir() / "dev_silencer");
            std::string w;
            f >> w;
            return w;
        }();
        static int64_t seen = 0;
        static bool done = false;
        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        static int64_t off_at = 0;
        if (!want.empty() && !done && g_state == S::kOnGun) {
            if (!seen) seen = now;
            if (now - seen > 8000 && weapons::RequestSuppressorAnim(false)) {
                done = true;
                off_at = now;
                g_wait_until = now_ms + 4000;
                g_carried = true;
                Set(want == "hand" ? S::kHand : S::kHolster, "dev_silencer: taken off (the game's detach)");
            }
        }
        if (want == "cycle" && done && off_at && now - off_at > 6000 && g_state == S::kHolster &&
            weapons::RequestSuppressorAnim(true)) {
            off_at = 0;
            g_wait_until = now_ms + 4000;
            g_carried = false;
            Set(S::kOnGun, "dev_silencer cycle: on again (the game's attach)");
        }
    }
    // (while the game's own attach or detach runs, the flag lags our state: it isn't read then)
    if (now_ms >= g_wait_until) {
        if (on == 1 && g_state != S::kOnGun && g_state != S::kUnscrew) Set(S::kOnGun, "put on another way (the game's button)");
        if (on == 0 && (g_state == S::kOnGun || g_state == S::kUnscrew)) {
            g_carried = true;
            Set(S::kHolster, "taken off another way");
        }
    }

    xr::Hand hand[2];
    float head[3], A[9], muzzle[3];
    if (!xr::Hands(hand[0], hand[1], head) || !hand[0].ok || !hand[0].aim_ok || !VrGunLocal(A, muzzle)) return;
    const xr::Hand& h = hand[0];
    bool grip = g_grip_was ? h.grip > kGripOff : h.grip > kGripOn;
    bool pressed = grip && !g_grip_was, released = !grip && g_grip_was;
    g_grip_was = grip;
    float d[3] = {h.pos[0] - muzzle[0], h.pos[1] - muzzle[1], h.pos[2] - muzzle[2]};
    float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    float H[9];
    Axes(h.aim, H);
    // the silencer held in the fist points where the controller does (-z); the barrel's forward is the gun's -z
    float along = H[2] * A[2] + H[5] * A[5] + H[8] * A[8];

    switch (g_state) {
        case S::kHolster:
            if (pressed && NearLeftHip(h, head)) {
                Set(S::kHand, "taken from the hip");
                xr::Pulse(0, 0.5f, 0.05f);
            }
            break;
        case S::kHand:
            if (released) {
                Set(S::kHolster, "let go: back in the holster");
                break;
            }
            if (dist < kLineUp && along > kLineAngle) {
                g_roll0 = Roll(h.aim, A);
                Set(S::kLined, "lined up at the muzzle");
                xr::Pulse(0, 0.3f, 0.03f);
            }
            break;
        case S::kLined: {
            if (released) {
                Set(S::kHolster, "let go at the muzzle, not locked: back in the holster");
                break;
            }
            if (dist > kLeave) {
                Set(S::kHand, "away from the muzzle");
                break;
            }
            // clockwise seen from behind: the hand's up turns toward the gun's right
            float twist = Wrap(Roll(h.aim, A) - g_roll0);
            if (twist > kTwist) {
                // the game's own attach: its animation, the flag and the sound (NOTES, 2026-11-25); the flag written
                // ourselves only if it won't take the request now
                if (weapons::RequestSuppressorAnim(true)) g_wait_until = now_ms + 4000;
                else weapons::SetSuppressorHeld(true);
                g_owned.insert(weapon);
                g_carried = false;  // (out of the inventory: on the gun)
                Set(S::kOnGun, "twisted on: locked");
                xr::Pulse(0, 0.9f, 0.08f);
                xr::Pulse(1, 0.5f, 0.05f);
                TestEvent("silencer_on");
            }
            break;
        }
        case S::kOnGun:
            if (pressed && dist < kMuzzleReach) {
                g_roll0 = Roll(h.aim, A);
                Set(S::kUnscrew, "gripped on the gun");
            }
            break;
        case S::kUnscrew: {
            if (released || dist > kLeave) {
                Set(S::kOnGun, "let go before it came off");
                break;
            }
            float twist = Wrap(Roll(h.aim, A) - g_roll0);
            if (twist < -kTwist) {
                if (weapons::RequestSuppressorAnim(false)) g_wait_until = now_ms + 4000;
                else weapons::SetSuppressorHeld(false);
                g_carried = true;
                Set(S::kHand, "twisted off: in the hand");
                xr::Pulse(0, 0.9f, 0.08f);
                xr::Pulse(1, 0.5f, 0.05f);
                TestEvent("silencer_off");
            }
            break;
        }
        case S::kNone:
            break;
    }
}

// any thread: the silencer in the off hand (taken, lined up or being unscrewed), for drawing it there
bool VrSilencerInHand() {
    int s = g_shown.load(std::memory_order_relaxed);
    return s == int(S::kHand) || s == int(S::kLined);
}

// render thread: our silencer's model where it is now (local space): its back end (the thread collar's), its front,
// its radius. Modelled on the user's photo: a matte black tube 16 cm long, 3.2 cm across, a narrower threaded collar
// at the back, two grooves, a vent hole near the front (the filter pass draws it: native/vulkan.cpp Overlay)
constexpr float kSilLength = 0.16f, kSilRadius = 0.016f;
bool VrSilencerPose(float* rear, float* front, float* radius) {
    S s = S(g_shown.load(std::memory_order_relaxed));
    *radius = kSilRadius;
    if (s == S::kNone || (s == S::kHolster && !g_carried)) {
        // on the ground, if one's lying there
        float at[3], dir[3], wr[3], wf[3];
        {
            std::lock_guard lock(g_ground_lock);
            if (!g_ground) return false;
            std::memcpy(at, g_ground_at, 12);
            std::memcpy(dir, g_ground_dir, 12);
        }
        for (int i = 0; i < 3; i++) wr[i] = at[i] - dir[i] * kSilLength * 0.5f, wf[i] = at[i] + dir[i] * kSilLength * 0.5f;
        return VrWorldToLocal(wr, rear) && VrWorldToLocal(wf, front);
    }
    float fwd[3], at[3];
    if (s == S::kHolster) {
        // on the left hip, upright, the collar up
        float hy, hp;
        xr::Hand hand[2];
        float head[3];
        if (!xr::HeadAngles(hy, hp) || !xr::Hands(hand[0], hand[1], head)) return false;
        float right[3] = {std::cos(hy), 0, -std::sin(hy)}, back[3] = {std::sin(hy), 0, std::cos(hy)};
        for (int i = 0; i < 3; i++) at[i] = head[i] + right[i] * kHipRight + back[i] * kHipBack;
        at[1] -= kHipDown - kSilLength * 0.5f;
        fwd[0] = 0, fwd[1] = -1, fwd[2] = 0;
    } else if (s == S::kHand) {
        // in the off hand's fist, along where the controller points, the collar 5 cm behind the grip
        xr::Hand hand[2];
        float head[3], H[9];
        if (!xr::Hands(hand[0], hand[1], head) || !hand[0].ok || !hand[0].aim_ok) return false;
        Axes(hand[0].aim, H);
        for (int i = 0; i < 3; i++) fwd[i] = -H[i * 3 + 2], at[i] = hand[0].pos[i] - fwd[i] * 0.05f;
    } else {
        // lined up, on the gun or being unscrewed: on the muzzle along the barrel (screwed on: the collar inside)
        float A[9], muzzle[3];
        if (!VrGunLocal(A, muzzle)) return false;
        float in = s == S::kLined ? 0.0f : 0.012f;
        for (int i = 0; i < 3; i++) fwd[i] = -A[i * 3 + 2], at[i] = muzzle[i] - fwd[i] * in;
    }
    for (int i = 0; i < 3; i++) rear[i] = at[i], front[i] = at[i] + fwd[i] * kSilLength;
    return true;
}

// any thread: a silencer laid on the ground 1.2 m ahead (VR SETTINGS, TESTING, SPAWN A SILENCER: user, for tests):
// walked over, it's picked up as a level's; false if one is carried or lying there already
bool VrSilencerGive() {
    {
        std::lock_guard lock(g_ground_lock);
        if (g_carried || g_ground) return false;
    }
    g_lay = true;
    return true;
}
// 0 none, 1 on the ground, 2 in the inventory (the button's text)
int VrSilencerWhere() {
    if (g_carried) return 2;
    std::lock_guard lock(g_ground_lock);
    return g_ground ? 1 : 0;
}

// any thread: the inventory's icon under the ammunition: -1 hidden (out of play), 0 dark (no silencer in the
// inventory), 1 bright with a glow (one carried: on the hip, in the hand or lined up)
int VrSilencerIcon() {
    if (!HudShown()) return -1;
    return g_carried ? 1 : 0;
}

}  // namespace game

// The game's pickup taken (0x1BED90; esi the pickup, its kind at +0x140, 4 a suppressor: NOTES "suppressor pickup
// hunt"): its slot freed, its message and sound, as ever. A suppressor goes into the inventory (user: where the levels
// place them; picked up, it's carried, not fitted): the attach the game starts with it is called off for 3 s
X86_EXTERN_FUNC(__imp__sub_001BED90);
X86_FUNC(sub_001BED90) {
    uint32_t pickup = ctx.esi;
    uint32_t kind = pickup ? kernel::R32(pickup + 0x140) : 0;
    __imp__sub_001BED90(ctx, base);
    // a touch in both hands for anything picked up (user: pickups felt)
    if (pickup) {
        game::VrTouch(0, 0.3f);
        game::VrTouch(1, 0.3f);
        game::TestEvent("pickup_felt");
    }
    if (kind != 4) return;
    game::g_carried = true;
    game::g_block_until = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch()).count() + 3000;
    LOGR("vr silencer: a suppressor pickup taken: into the inventory");
    game::TestEvent("silencer_picked");
}

