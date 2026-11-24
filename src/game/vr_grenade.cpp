// phase 6: grenades thrown by hand (user's design: taken by a grip low across the waist, the gun leaving the hand;
// let go while throwing and it flies from the hand with the hand's speed, the game's fuse starting then). Here: a
// throw asked for (the release) and taken by the weapon state machine's wrapper (hooks.cpp), which makes the
// grenade itself through the game's own maker (FUN_001DD000) with the hand's place and speed in the world, the
// count taken as the game's throw takes it. The game's throw animation never runs
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "game/layout.h"
#include "xdk/hle.h"
#include "xr.h"

using namespace kernel;
using namespace layout;

namespace sound {
void CuePcm(const int16_t* p, uint32_t frames, uint32_t rate, float gain);  // mixer.cpp
}
namespace weapons {
bool DiscFile(const std::string& path, std::vector<uint8_t>& out);  // weapons.cpp: a file from the disc
}

namespace game {
bool VrHandWorld(int hand, float* pos, float* vel);  // vr_gun.cpp: a hand's grip in the game's world, metres

namespace {
// the hand's speed as thrown (1: as it was; the game's own throws are a little faster than an arm's)
constexpr float kThrowGain = 1.5f;
// the throw's speed: the fastest the hand went in this long before it let go (at the release the arm has already
// slowed: user, the throws all went the same way short)
constexpr int64_t kPeakMs = 100;
struct Sample {
    int64_t ms;
    float vel[3];
};
std::mutex g_samples_lock;
std::vector<Sample> g_samples;  // while a grenade's held, the hand's world velocity each poll
std::atomic<int> g_throw_hand{-1};  // a throw asked for, from this hand; -1 none
std::atomic<int64_t> g_thrown_ms{0};
int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace
// steady ms of the last grenade made by hand (vulkan.cpp's dev_drawdiff)
int64_t VrThrownMs() { return g_thrown_ms.load(); }

namespace {
// the throw's sound (user: as the grenade leaves the hand, only then): the game's own, /sound/pcm/GrdPin.dav (a RIFF
// WAVE, 16-bit mono PCM, 48 kHz: the only grenade sound on the disc, which its throw animation played), read once
void PlayThrowSound() {
    static std::vector<uint8_t> wav;
    static bool tried = false;
    if (!tried) {
        tried = true;
        if (!weapons::DiscFile("sound\\pcm\\GrdPin.dav", wav)) LOGR("vr grenade: no GrdPin.dav on the disc");
    }
    // its chunks: fmt (channels, rate, bits), data
    uint32_t rate = 0, channels = 0, bits = 0;
    for (size_t o = 12; o + 8 <= wav.size();) {
        uint32_t len;
        std::memcpy(&len, &wav[o + 4], 4);
        if (!std::memcmp(&wav[o], "fmt ", 4) && o + 24 <= wav.size()) {
            channels = wav[o + 10] | wav[o + 11] << 8;
            std::memcpy(&rate, &wav[o + 12], 4);
            bits = wav[o + 22] | wav[o + 23] << 8;
        } else if (!std::memcmp(&wav[o], "data", 4) && channels == 1 && bits == 16) {
            uint32_t n = uint32_t(std::min<size_t>(len, wav.size() - o - 8) / 2);
            sound::CuePcm(reinterpret_cast<const int16_t*>(&wav[o + 8]), n, rate, 1.0f);
            return;
        }
        o += 8 + len + (len & 1);
    }
}
}  // namespace

// any thread: throw now from this hand (its place and speed when the game thread takes it, within the frame)
void VrRequestThrow(int hand) { g_throw_hand.store(hand); }

// game thread (the player's weapon each frame): a throw asked for, its place and velocity in the world (metres,
// m/s); false: none
bool VrTakeThrow(float* pos, float* vel) {
    int hand = g_throw_hand.exchange(-1);
    if (hand < 0) return false;
    if (!VrHandWorld(hand, pos, vel)) {
        LOGR("vr grenade: a throw asked for, the hand not known: none");
        return false;
    }
    {
        // the peak of the swing's last kPeakMs, if faster than now
        std::lock_guard lock(g_samples_lock);
        int64_t now = NowMs();
        float best = vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2];
        for (const Sample& s : g_samples) {
            float sp = s.vel[0] * s.vel[0] + s.vel[1] * s.vel[1] + s.vel[2] * s.vel[2];
            if (now - s.ms <= kPeakMs + 50 && sp > best) best = sp, std::memcpy(vel, s.vel, 12);
        }
        g_samples.clear();
    }
    for (int i = 0; i < 3; i++) vel[i] *= kThrowGain;
    g_thrown_ms = NowMs();
    PlayThrowSound();
    LOGR("vr grenade: thrown from %.2f %.2f %.2f at %.2f %.2f %.2f m/s", pos[0], pos[1], pos[2], vel[0], vel[1], vel[2]);
    return true;
}

// the right hand: the gun, a grenade (the gun away), or empty after a throw till a grip over the right shoulder
enum Hold { kHoldGun, kHoldGrenade, kHoldEmpty };
namespace {
std::atomic<int> g_hold{kHoldGun};
// where the right grip takes a grenade (user): the right hip, and standing also a band across the front of the
// waist under the headset; metres from the head in its yaw frame (right, forward, down). Crouched the hip is nearer
// the head, and the band is off (no grenade taken by a crouch's hand at the knees)
constexpr float kHipRight0 = 0.12f, kHipRight1 = 0.32f, kHipFwd0 = -0.15f, kHipFwd1 = 0.15f;
constexpr float kHipDown0 = 0.55f, kHipDown1 = 0.85f, kHipDownCrouch0 = 0.40f, kHipDownCrouch1 = 0.70f;
constexpr float kBandRight0 = -0.20f, kBandRight1 = 0.20f, kBandFwd0 = -0.05f, kBandFwd1 = 0.25f;
constexpr float kBandDown0 = 0.48f, kBandDown1 = 0.75f;
// over the right shoulder (the weapon switch's ball, vr_body.cpp): the gun back
constexpr float kShoulderRight = 0.15f, kShoulderBack = 0.08f, kShoulderUp = -0.08f, kShoulderReach = 0.12f;

bool AtWaist(const float* d, float yaw, bool crouched) {
    float right = d[0] * std::cos(yaw) - d[2] * std::sin(yaw), fwd = -(d[0] * std::sin(yaw) + d[2] * std::cos(yaw));
    float down = -d[1];
    bool hip = right > kHipRight0 && right < kHipRight1 && fwd > kHipFwd0 && fwd < kHipFwd1 &&
               down > (crouched ? kHipDownCrouch0 : kHipDown0) && down < (crouched ? kHipDownCrouch1 : kHipDown1);
    bool band = !crouched && right > kBandRight0 && right < kBandRight1 && fwd > kBandFwd0 && fwd < kBandFwd1 &&
                down > kBandDown0 && down < kBandDown1;
    return hip || band;
}
bool AtShoulder(const float* d, float yaw) {
    float right = d[0] * std::cos(yaw) - d[2] * std::sin(yaw), back = d[0] * std::sin(yaw) + d[2] * std::cos(yaw);
    float e[3] = {right - kShoulderRight, back - kShoulderBack, d[1] - kShoulderUp};
    return e[0] * e[0] + e[1] * e[1] + e[2] * e[2] < kShoulderReach * kShoulderReach;
}
}  // namespace

// the gun isn't in the hand (a grenade, or empty): not drawn, not fired, nothing of its reload (hooks.cpp,
// vr_reload.cpp)
bool VrGunAway() { return xr::Active() && g_hold.load(std::memory_order_relaxed) != kHoldGun; }
bool VrGrenadeInHand() { return g_hold.load(std::memory_order_relaxed) == kHoldGrenade; }

// game thread, each pad poll (vr_body.cpp, before the right grip's own uses): the right grip at the waist takes a
// grenade (the game has one: character +0x286), held while the grip is; let go, thrown; empty, a grip over the right
// shoulder brings the gun back. true: the grip was ours this poll (its other uses skipped)
bool VrGrenadeGrip(bool grip, bool grip_was, const xr::Hand* right, const float* head, float yaw, bool crouched,
                   input::Pad& pad) {
    bool press = grip && !grip_was;
    float d[3] = {};
    if (right && right->ok)
        for (int i = 0; i < 3; i++) d[i] = right->pos[i] - head[i];
    int hold = g_hold.load();
    bool ours = false;
    if (hold == kHoldGun && press && right && right->ok && AtWaist(d, yaw, crouched)) {
        uint32_t me = R32(kWorld) ? R32(kWorld) + kWorldPlayer : 0;  // the player
        if (me && G<uint16_t>(me + 0x286) > 0) {
            g_hold = hold = kHoldGrenade;
            xr::Pulse(1, 0.5f, 0.04f);
            LOGR("vr grenade: taken at the waist (%u left), the gun away", G<uint16_t>(me + 0x286));
            ours = true;
        }
    } else if (hold == kHoldGrenade) {
        ours = true;
        float p[3], v[3];
        if (VrHandWorld(1, p, v)) {
            std::lock_guard lock(g_samples_lock);
            int64_t now = NowMs();
            g_samples.push_back({now, {v[0], v[1], v[2]}});
            while (!g_samples.empty() && now - g_samples.front().ms > kPeakMs) g_samples.erase(g_samples.begin());
        }
        if (!grip) {
            // thrown, and the gun's back in the hand at once (user: no second grenade can be thrown with it)
            VrRequestThrow(1);
            g_hold = hold = kHoldGun;
        }
    } else if (hold == kHoldEmpty) {
        ours = true;
        if (press && right && right->ok && AtShoulder(d, yaw)) {
            g_hold = hold = kHoldGun;
            xr::Pulse(1, 0.4f, 0.04f);
            LOGR("vr grenade: the gun back from over the shoulder");
        }
    }
    // dev_throw: a throw every 6 s in play, from the right hand (no grip: to see grenades made without controllers)
    static const bool dev_throw = std::filesystem::exists(app::ExeDir() / "dev_throw");
    static int64_t dev_next = 0;
    if (dev_throw && NowMs() > dev_next) {
        uint32_t me = R32(kWorld) ? R32(kWorld) + kWorldPlayer : 0;
        if (me && !G<uint16_t>(me + 0x286)) G<uint16_t>(me + 0x286) = 3;  // (the first level gives none)
        if (dev_next) VrRequestThrow(1);
        dev_next = NowMs() + 6000;
    }
    if (hold != kHoldGun) pad.analog[7] = 0;  // no gun to fire
    return ours;
}

}  // namespace game
