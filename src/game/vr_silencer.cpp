// The silencer as the game does it (user, 2026-11-26: no inventory, icon or hip): a level's suppressor is fitted by
// the game when picked up, with its own animation. By hand: the off hand's grip on the silencer takes it off, its
// grip at the end of the barrel puts it back on, each through the game's own detach or attach (its animation, flag
// and sound: weapons::RequestSuppressorAnim). Only a gun that has had its silencer on can have it put back. Our model
// of it is drawn on the muzzle while it's on (the game's gun models have no silencer part)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <filesystem>
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
bool RequestSuppressorAnim(bool attach);  // the game's own attach (true) or detach animation
void PickupMessage(int kind);              // the game's pickup message (its sound and line), 4 the suppressors
}  // namespace weapons

namespace game {
namespace {

std::set<uint32_t> g_owned;       // guns whose silencer the player has (seen on): it can go back on
bool g_grip_was = false;
std::atomic<bool> g_on{false};    // the held gun's silencer is on (for the renderer)
int64_t g_wait_until = 0;         // the game's own attach or detach runs till then
// a silencer on the ground (the settings' test button lays one 1.2 m ahead, across the way), world space; walked over
// (within 0.7 m across the floor): picked up as a level's
std::atomic<bool> g_lay{false};
std::mutex g_ground_lock;
bool g_ground = false;
float g_ground_at[3], g_ground_dir[3];

constexpr float kGripOn = 0.6f, kGripOff = 0.35f;
constexpr float kSilLength = 0.16f, kSilRadius = 0.016f;
constexpr float kOnReach = 0.07f;      // the off hand this near the silencer's tube grips it (to take it off)
constexpr float kMuzzleReach = 0.07f;  // this near the end of the barrel (to put it on)

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// the distance from p to the segment a..b
float SegDist(const float* p, const float* a, const float* b) {
    float ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, ap[3] = {p[0] - a[0], p[1] - a[1], p[2] - a[2]};
    float l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    float t = l2 > 0 ? std::clamp((ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / l2, 0.0f, 1.0f) : 0;
    float d[3] = {ap[0] - ab[0] * t, ap[1] - ab[1] * t, ap[2] - ab[2] * t};
    return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

void GroundTick() {
    float eye[3], fwd[3], floor_y;
    if (!HudShown() || !VrPlayerWorld(eye, fwd, &floor_y)) return;
    {
        // dev_spawn_silencer: the test button pressed once, 8 s into play
        static const bool dev = std::filesystem::exists(app::ExeDir() / "dev_spawn_silencer");
        static int64_t first = 0;
        int64_t now = NowMs();
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
        weapons::PickupMessage(4);
        // as a level's: the game's attach on the gun in hand, if it takes one
        uint32_t weapon = 0;
        if (weapons::SuppressorHeld(weapon) == 0 && weapons::RequestSuppressorAnim(true)) g_owned.insert(weapon);
        LOGR("vr silencer: picked up from the ground");
        TestEvent("silencer_picked");
    }
}

}  // namespace

// game thread, each tick (weapons.cpp WeaponsTick)
void VrSilencerTick() {
    GroundTick();
    uint32_t weapon = 0;
    int on = weapons::SuppressorHeld(weapon);
    g_on.store(on == 1, std::memory_order_relaxed);
    if (on < 0 || !weapon) return;
    if (on == 1) g_owned.insert(weapon);
    xr::Hand hand[2];
    float head[3], A[9], muzzle[3];
    if (!xr::Hands(hand[0], hand[1], head) || !hand[0].ok || !VrGunLocal(A, muzzle)) return;
    const xr::Hand& h = hand[0];
    bool grip = g_grip_was ? h.grip > kGripOff : h.grip > kGripOn;
    bool pressed = grip && !g_grip_was;
    g_grip_was = grip;
    int64_t now_ms = NowMs();
    if (!pressed || now_ms < g_wait_until) return;
    float front[3];
    for (int i = 0; i < 3; i++) front[i] = muzzle[i] - A[i * 3 + 2] * kSilLength;  // (the barrel's forward: -z)
    if (on == 1 && SegDist(h.pos, muzzle, front) < kOnReach) {
        if (!weapons::RequestSuppressorAnim(false)) return;
        g_wait_until = now_ms + 3500;
        xr::Pulse(0, 0.7f, 0.06f);
        LOGR("vr silencer: gripped: the game's detach");
        TestEvent("silencer_off");
    } else if (on == 0 && g_owned.count(weapon) && SegDist(h.pos, muzzle, muzzle) < kMuzzleReach) {
        if (!weapons::RequestSuppressorAnim(true)) return;
        g_wait_until = now_ms + 3500;
        xr::Pulse(0, 0.7f, 0.06f);
        LOGR("vr silencer: the end of the barrel gripped: the game's attach");
        TestEvent("silencer_on");
    }
}

// any thread: the silencer in the off hand (none now: the game's animation carries it)
bool VrSilencerInHand() { return false; }

// render thread: our silencer's model where it is now (local space): its back end (the thread collar's), its front,
// its radius. Modelled on the user's photo: a matte black tube 16 cm long, 3.2 cm across, a narrower threaded collar
// at the back, two grooves, a vent hole near the front (the filter pass draws it: native/vulkan.cpp Overlay). On the
// muzzle while the held gun's is on, else on the ground if the test laid one
bool VrSilencerPose(float* rear, float* front, float* radius) {
    *radius = kSilRadius;
    if (!g_on.load(std::memory_order_relaxed)) {
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
    float A[9], muzzle[3];
    if (!VrGunLocal(A, muzzle)) return false;
    for (int i = 0; i < 3; i++) {
        float fwd = -A[i * 3 + 2];
        rear[i] = muzzle[i] - fwd * 0.012f;  // (screwed on: the collar inside)
        front[i] = rear[i] + fwd * kSilLength;
    }
    return true;
}

// any thread: a silencer laid on the ground 1.2 m ahead (VR SETTINGS, TESTING, SPAWN A SILENCER: user, for tests):
// walked over, it's picked up as a level's; false if one is lying there already
bool VrSilencerGive() {
    {
        std::lock_guard lock(g_ground_lock);
        if (g_ground) return false;
    }
    g_lay = true;
    return true;
}
// 0 none, 1 on the ground (the button's text)
int VrSilencerWhere() {
    std::lock_guard lock(g_ground_lock);
    return g_ground ? 1 : 0;
}

// the inventory's icon: none now (user)
int VrSilencerIcon() { return -1; }

}  // namespace game

// The game's pickup taken (0x1BED90; esi the pickup, its kind at +0x140, 4 a suppressor: NOTES "suppressor pickup
// hunt"): as the game does it (a suppressor fitted with its animation); a light touch in both hands for anything
X86_EXTERN_FUNC(__imp__sub_001BED90);
X86_FUNC(sub_001BED90) {
    uint32_t pickup = ctx.esi;
    __imp__sub_001BED90(ctx, base);
    if (pickup) {
        game::VrTouch(0, 0.3f);
        game::VrTouch(1, 0.3f);
        game::TestEvent("pickup_felt");
    }
}
