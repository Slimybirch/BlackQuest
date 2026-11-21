// blackquest: the player's real height. The view follows the headset up and down (vulkan.cpp, local space); a real
// crouch also crouches the game's player (cover, hit box, speed) by holding its crouch button (left stick click),
// and while that crouch is the headset's, the game's own lowering of the camera is given back so the view drops only
// as far as the player did. The button still crouches the game's way.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

#include "core/log.h"
#include "core/input.h"
#include "game/layout.h"
#include "xdk/hle.h"

#include "common/options.h"

#include "xr.h"
namespace d3d {
void VrLookUsed(float yaw, float pitch);
}

using namespace kernel;
using namespace layout;

namespace game {

namespace {

constexpr uint32_t kStance = 0x2E0;    // player: eye height over its position, lower crouched (eye = y + this - 0.2)
constexpr uint16_t kThumbL = 1 << 6;   // pad button: the crouch action's default
constexpr float kCrouchDrop = 0.35f;   // metres under standing height that count as crouching
constexpr float kStandDrop = 0.25f;    // and back over, standing again
constexpr float kStanceSlack = 0.05f;  // the game's stance this near its standing value is standing
constexpr int64_t kTogglePress = 150;  // ms a press is held when the crouch toggles

std::atomic<float> g_lift{0};  // metres the view is raised back, read by the renderer

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Body {
    float top = -1e9f;      // standing head height: the highest seen (local space starts where the session did)
    float stand = 0;        // the game's standing stance, the highest seen
    bool real = false;      // the headset is crouched
    bool ours = false;      // the game's crouch came from the headset
    int64_t press_until = 0;
    // phase 3 look: the game's yaw = body + the head's; the right stick turns the body, the head does the rest
    bool look = false;
    float body = 0;          // degrees
    float written = 0;       // the yaw last written, to find what the game turned since (smooth turning)
    float pitch_sign = 1;    // the game's pitch against the head's, checked on its built view
    float last_pitch = 0;    // the head's pitch last written, radians
    int64_t play_ms = 0;     // the player's view was last built (play, not menus or films)
    bool flicked = false;
    // vr controls in play (user's scheme)
    bool grip_was = false;
    bool grip_switch = false;      // this grip began over the shoulder: a weapon switch, not X
    int64_t switch_until = 0;
    int64_t melee_until = 0, melee_ready = 0;
    bool b_was = false;
    int64_t melee_hide_until = 0;  // the game's melee swing of the gun isn't drawn till then
    int64_t retry_at = 0;   // released standing up: if the game stays down (toggle crouch), one press then
} g;

}  // namespace

bool VrMeleeHidden() { return NowMs() < g.melee_hide_until; }

float VrViewLift() { return g_lift.load(std::memory_order_relaxed); }

// The user's touch scheme in play: the right stick up/down is the d-pad's (firing mode / health pack), the right grip
// is X (pick up, use), the right grip closed with the hand reached back over the right shoulder switches weapon (the
// d-pad's right), and a hit with either hand (fast, away from the head) is melee (B)
constexpr int16_t kStickPress = 20000;
constexpr uint8_t kGripPress = 128;
constexpr float kMeleeSpeed = 2.5f;    // m/s, away from the head
constexpr int64_t kPulse = 120;        // ms a gesture's button is held
constexpr int64_t kMeleeRearm = 600;
constexpr int64_t kMeleeHide = 650;    // ms: about the game's melee swing
// over the shoulder: the hand behind the face's plane, right of the head's middle, higher than the chest (metres, in
// the head's yaw frame)
constexpr float kShoulderBack = -0.05f, kShoulderRight = 0.0f, kShoulderDown = -0.35f;

void VrControls(input::Pad& pad) {
    int64_t now = NowMs();
    if (pad.ry > kStickPress) pad.buttons |= 1 << 0;
    if (pad.ry < -kStickPress) pad.buttons |= 1 << 1;
    pad.ry = 0;
    xr::Hand hand[2];
    float head[3];
    float yaw, pitch;
    bool poses = xr::Hands(hand[0], hand[1], head) && xr::HeadAngles(yaw, pitch);
    // right grip: the touch mapping put it on the black button (analog 4), unused by the game's controls
    bool grip = pad.analog[4] >= kGripPress;
    pad.analog[4] = 0;
    if (grip && !g.grip_was) {
        g.grip_switch = false;
        if (poses && hand[1].ok) {
            float d[3] = {hand[1].pos[0] - head[0], hand[1].pos[1] - head[1], hand[1].pos[2] - head[2]};
            float right = d[0] * std::cos(yaw) - d[2] * std::sin(yaw);
            float back = d[0] * std::sin(yaw) + d[2] * std::cos(yaw);
            g.grip_switch = back > kShoulderBack && right > kShoulderRight && d[1] > kShoulderDown;
            if (g.grip_switch) g.switch_until = now + kPulse;
        }
    }
    g.grip_was = grip;
    if (grip && !g.grip_switch) pad.analog[2] = 255;
    if (now < g.switch_until) pad.buttons |= 1 << 3;
    // melee: a hand's speed away from the head
    for (int h = 0; h < 2 && poses && now >= g.melee_ready; h++) {
        if (!hand[h].ok) continue;
        const float* v = hand[h].vel;
        float d[3] = {hand[h].pos[0] - head[0], hand[h].pos[1] - head[1], hand[h].pos[2] - head[2]};
        float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (dl < 0.05f) continue;
        float away = (v[0] * d[0] + v[1] * d[1] + v[2] * d[2]) / dl;
        if (away > kMeleeSpeed) {
            g.melee_until = now + kPulse;
            g.melee_ready = now + kMeleeRearm;
        }
    }
    if (now < g.melee_until) pad.analog[1] = 255;
    // any melee press (the gesture's or B): the gun and arms hide for the game's swing
    bool b = pad.analog[1] >= kGripPress;
    if (b && !g.b_was) g.melee_hide_until = now + kMeleeHide;
    g.b_was = b;
}

// game thread, each pad poll of port 0
void VrPad(input::Pad& pad) {
    // in play the head looks up and down; the right stick turns the body, a flick a step (or smoothly: the game's)
    if (NowMs() - g.play_ms < 250) {
        uint32_t snap = Opt().vr_snap_turn;
        if (snap) {
            int x = pad.rx;
            if (!g.flicked && (x > 22000 || x < -22000)) {
                g.flicked = true;
                g.body += x > 0 ? -float(snap) : float(snap);  // yaw grows turning left
            } else if (g.flicked && x < 10000 && x > -10000) {
                g.flicked = false;
            }
            pad.rx = 0;
        }
    }
    if (NowMs() - g.play_ms < 250) VrControls(pad);
    float head;
    uint32_t world = R32(kWorld);
    if (!world || !xr::HeadHeight(head)) {
        g_lift.store(0, std::memory_order_relaxed);
        return;
    }
    float stance = G<float>(world + kWorldPlayer + kStance);
    if (!(stance > 0.3f && stance < 3.0f)) {
        g_lift.store(0, std::memory_order_relaxed);
        return;
    }
    int64_t now = NowMs();
    g.top = std::max(g.top, head);
    g.stand = std::max(g.stand, stance);
    bool game_down = stance < g.stand - kStanceSlack;
    bool was = g.real;
    g.real = head < g.top - (g.real ? kStandDrop : kCrouchDrop);
    if (g.real && !was && !game_down) {
        g.ours = true;
        g.retry_at = 0;
    }
    if (!g.real && was && g.ours) g.retry_at = now + 600;
    // held while really crouched: a hold-to-crouch game stays down, a toggle one went down on the press
    bool hold = g.real && g.ours;
    if (g.retry_at && now >= g.retry_at) {
        g.retry_at = 0;
        if (game_down) g.press_until = now + kTogglePress;
    }
    if (hold || now < g.press_until) pad.buttons |= kThumbL;
    if (!g.real && !game_down && now >= g.press_until) g.ours = false;
    g_lift.store(g.ours ? std::max(0.0f, g.stand - stance) : 0.0f, std::memory_order_relaxed);
    static int64_t logged = 0;
    if (now - logged > 5000) {
        logged = now;
        LOGR("vr body: head %.2f top %.2f, stance %.2f stand %.2f, crouch real %d ours %d, lift %.2f", head, g.top,
             stance, g.stand, g.real, g.ours, g_lift.load());
    }
}

}  // namespace game

// the player's look before the game builds its view from it (0x1E5FE1 in FUN_001E5FA0; ebx the character, its
// look angles at [+0x324] +0x18 yaw / +0x1C pitch, degrees). The view, the aim and the walking direction follow
// the head; the renderer turns each eye by what the head did since (vulkan.cpp EyeMatrix)
void VrLook(X86Context& ctx, uint8_t*) {
    using namespace game;
    float yaw, pitch;
    uint32_t world = R32(kWorld);
    if (!world || ctx.ebx != world + kWorldPlayer || !xr::HeadAngles(yaw, pitch)) {
        g.look = false;
        return;
    }
    constexpr float kDeg = 57.2957795f;
    constexpr uint32_t kLook = 0x324, kYaw = 0x18, kPitch = 0x1C, kForward = 0xF0;
    uint32_t look = R32(ctx.ebx + kLook);
    if (!look) return;
    float& game_yaw = G<float>(look + kYaw);
    float& game_pitch = G<float>(look + kPitch);
    if (!g.look) {
        g.body = game_yaw - yaw * kDeg;
        g.look = true;
    } else {
        // the game's own turn since the last write (smooth turning, scripted turns), wrapped
        float d = game_yaw - g.written;
        d -= 360.0f * std::floor((d + 180.0f) / 360.0f);
        g.body += d;
    }
    // the sign of the game's pitch, from the view it built last time: its forward row's height against the head's
    float fy = G<float>(ctx.ebx + kForward + 4);
    if (std::fabs(g.last_pitch) > 0.25f && std::fabs(fy) > 0.15f && (fy > 0) != (g.last_pitch > 0)) {
        g.pitch_sign = -g.pitch_sign;
        LOGR("vr look: the game's pitch runs the other way, sign %.0f", g.pitch_sign);
    }
    game_yaw = g.body + yaw * kDeg;
    game_pitch = g.pitch_sign * pitch * kDeg;
    g.written = game_yaw;
    g.last_pitch = pitch;
    g.play_ms = NowMs();
    d3d::VrLookUsed(yaw, pitch);
}
