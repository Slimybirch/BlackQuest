// The silencer by hand (user, 2026-11-24). A gun whose silencer the player has (on it when it came to hand, or put
// on since) keeps it in a holster on the left hip while it's off. The off hand's grip there takes it; brought to the
// muzzle, within a few centimetres and roughly along the barrel, it lines up; one twist (clockwise seen from
// behind the gun) locks it on: the gun's suppressor flag goes on (weapons.cpp). On the gun, the off hand's grip at
// the muzzle and the twist the other way takes it off into the hand. Let go of anywhere but locked on, it goes back
// to the holster. The game's suppressor button stays as it was.
// Still to do with the headset: the silencer drawn in the hand (the gun model's own part, once found in a dump), and
// the tail of the game's suppressor animation played as it locks
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>

#include "core/log.h"
#include "xr.h"

namespace game {
bool VrGunLocal(float* A, float* muzzle);  // vr_gun.cpp: the gun's axes and its muzzle in local space, as drawn
void TestEvent(const std::string& name);   // vr_totest.cpp
}  // namespace game
namespace weapons {
int SuppressorHeld(uint32_t& weapon);  // -1 the held gun takes none, 0 off, 1 on; weapon: the held one
void SetSuppressorHeld(bool on);
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

// game thread, each tick (weapons.cpp WeaponsTick)
void VrSilencerTick() {
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
        Set(on ? S::kOnGun : g_owned.count(weapon) ? S::kHolster : S::kNone, "a gun came to hand");
    }
    if (g_state == S::kNone) return;
    if (on == 1 && g_state != S::kOnGun && g_state != S::kUnscrew) Set(S::kOnGun, "put on another way (the game's button)");
    if (on == 0 && (g_state == S::kOnGun || g_state == S::kUnscrew)) Set(S::kHolster, "taken off another way");

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
                weapons::SetSuppressorHeld(true);
                g_owned.insert(weapon);
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
                weapons::SetSuppressorHeld(false);
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

}  // namespace game
