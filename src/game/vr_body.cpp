// blackquest: the player's real height. The view follows the headset up and down (vulkan.cpp, local space); a real
// crouch also crouches the game's player (cover, hit box, speed) by holding its crouch button (left stick click),
// and while that crouch is the headset's, the game's own lowering of the camera is given back so the view drops only
// as far as the player did. The button still crouches the game's way.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include "app/app.h"
#include "common/menu.h"
#include "game/flash.h"
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
bool VrDevModePress();  // vr_reload.cpp: dev_modeflip's press of the fire-mode button now
void TestEvent(const std::string& name);  // vr_totest.cpp: the test checklist
bool VrM203TriggerTaken();  // vr_reload.cpp: the M203's trigger (the off hand's) kept from the game
bool VrGrenadeGrip(bool grip, bool grip_was, const xr::Hand* right, const float* head, float yaw, bool crouched,
                   input::Pad& pad);  // vr_grenade.cpp
std::atomic<int> g_mode_presses{0};  // the fire-mode button's presses (vr_reload.cpp: the SPAS's own modes)
int VrModePresses() { return g_mode_presses.load(std::memory_order_relaxed); }
std::atomic<bool> g_fire_held{false};  // the game's fire trigger as the pad last gave it (vr_reload.cpp: semi-auto)
bool VrFireHeld() { return g_fire_held.load(std::memory_order_relaxed); }

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
    float last_yaw = 0;
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
void VrToggleGunFingers();  // vr_gun.cpp

// the head angles (radians) the player's camera was last built from, while in play
bool VrLookAngles(float& yaw, float& pitch) {
    if (!g.look || NowMs() - g.play_ms > 250) return false;
    yaw = g.last_yaw;
    pitch = g.last_pitch;
    return true;
}

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
// over the shoulder: the hand in a ball just above the right shoulder, a little behind and right of the headset
// (metres from the head's middle, in the head's yaw frame: right, back, up). Small, so the gun hand's own grip
// presses near the face aren't taken for it
constexpr float kShoulderRight = 0.15f, kShoulderBack = 0.08f, kShoulderUp = -0.08f, kShoulderReach = 0.12f;

// the game paused over play (its pause menu): the weapons' logic, which runs every tick of play, hasn't run for a
// while (the title card and the hud's pages don't stop it). The renderer fixes the hud plane in the room then
// (vulkan.cpp HudMatrix)
std::atomic<int64_t> g_logic_ms{0};
void VrLogicTick() { g_logic_ms.store(NowMs(), std::memory_order_relaxed); }
bool VrMenuOverPlay() {
    int64_t t = g_logic_ms.load(std::memory_order_relaxed);
    return t && NowMs() - t > 300;
}

void VrControls(input::Pad& pad) {
    int64_t now = NowMs();
    if (pad.ry > kStickPress) pad.buttons |= 1 << 0;
    if (pad.ry < -kStickPress) pad.buttons |= 1 << 1;
    // the fire mode's press (the stick up): a very small buzz in the gun hand (user; the game plays the gun's own
    // sound for it)
    static bool mode_was = false;
    bool mode = pad.ry > kStickPress;
    if (mode && !mode_was) xr::Pulse(1, 0.12f, 0.02f), g_mode_presses++, TestEvent("mode_press");
    mode_was = mode;
    pad.ry = 0;
    xr::Hand hand[2];
    float head[3];
    float yaw, pitch;
    bool poses = xr::Hands(hand[0], hand[1], head) && xr::HeadAngles(yaw, pitch);
    // right grip: the touch mapping put it on the black button (analog 4), unused by the game's controls
    bool grip = pad.analog[4] >= kGripPress;
    pad.analog[4] = 0;
    // phase 6: grenades at the waist, the gun away, back over the shoulder (vr_grenade.cpp): its grip, when it's ours
    if (VrGrenadeGrip(grip, g.grip_was, poses ? &hand[1] : nullptr, head, yaw, g.real, pad)) {
        g.grip_was = grip;
        g.grip_switch = false;
        grip = false;
    }
    if (grip && !g.grip_was) {
        g.grip_switch = false;
        if (poses && hand[1].ok) {
            float d[3] = {hand[1].pos[0] - head[0], hand[1].pos[1] - head[1], hand[1].pos[2] - head[2]};
            float right = d[0] * std::cos(yaw) - d[2] * std::sin(yaw);
            float back = d[0] * std::sin(yaw) + d[2] * std::cos(yaw);
            float e[3] = {right - kShoulderRight, back - kShoulderBack, d[1] - kShoulderUp};
            g.grip_switch = e[0] * e[0] + e[1] * e[1] + e[2] * e[2] < kShoulderReach * kShoulderReach;
            if (g.grip_switch) g.switch_until = now + kPulse;
        }
        if (!g.grip_switch) VrToggleGunFingers();  // own hands: the grip opens or closes the gun hand
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

// dev testing without the controllers in hand (files in the app's folder): dev_autostart presses A through the
// menus till play; dev_autofire pulls the trigger now and then in play, dev_automelee hits B
std::atomic<int64_t> g_record_until{0};  // dev_dumpreload
void DevPad(input::Pad& pad) {
    static const bool start = std::filesystem::exists(app::ExeDir() / "dev_autostart");
    static const bool fire = std::filesystem::exists(app::ExeDir() / "dev_autofire");
    int64_t now = NowMs();
    bool play = now - g.play_ms < 2000;
    if (start && !play && now % 1500 < 150) pad.analog[0] = 255;
    if (fire && play && now % 3000 < 400) pad.analog[7] = 255;
    // dev_altfire: the off hand's trigger (the M203's) pulled now and then in play, between dev_autofire's, held as
    // many ms as the file says (300 if none), every 6 s
    static const int alt_ms = [] {
        std::ifstream f(app::ExeDir() / "dev_altfire");
        int ms = 0;
        if (!f) return -1;
        f >> ms;
        return ms > 0 ? ms : 300;
    }();
    if (alt_ms > 0 && play && now % 6000 >= 2000 && now % 6000 < 2000 + alt_ms && !VrM203TriggerTaken())
        pad.analog[6] = 255;  // (not while the M203's trigger is held back, as a real one's)
    // dev_firehold: the trigger held down in play (automatic fire, for catching the flash in a screenshot)
    static const bool hold = std::filesystem::exists(app::ExeDir() / "dev_firehold");
    if (hold && play) pad.analog[7] = 255;
    // dev_rapid: the trigger pulled for 0.12 s every 0.3 s in play, and let go between (fast semi-auto)
    static const bool rapid = std::filesystem::exists(app::ExeDir() / "dev_rapid");
    if (rapid && play) pad.analog[7] = now % 300 < 120 ? 255 : 0;  // over the controller's (it may lie pressed)
    static bool dev_mode_was = false;
    bool dev_mode = play && VrDevModePress();
    if (dev_mode) pad.buttons |= 1;  // dev_modeflip: the d-pad's up (the fire mode)
    if (dev_mode && !dev_mode_was) g_mode_presses++;
    dev_mode_was = dev_mode;
    {
        // the page's name, as it changes (the menus' log)
        uint32_t ui = R32(layout::kUi);
        static std::string last_page;
        std::string page;
        if (ui)
            for (int i = 0; i < 32; i++) {
                char ch = char(G<uint8_t>(ui + layout::kPageName + i));
                if (!ch) break;
                page += ch;
            }
        if (page != last_page) LOGR("vr body: page \"%s\" (play %d, input %d)", page.c_str(), play, ui && layout::InputTaken(ui)), last_page = page;
    }
    // dev_menu: the settings panel opens once, 5 s into play (or 20 s after boot without autostart: the title)
    static const bool menu = std::filesystem::exists(app::ExeDir() / "dev_menu");
    static int64_t boot = now, first_play = 0;
    static bool opened = false;
    if (play && !first_play) first_play = now;
    if (menu && !opened && ((first_play && now - first_play > 5000) || (!start && now - boot > 20000))) {
        opened = true;
        if (!app::MenuOpen()) app::ToggleMenu();
    }
    static const bool melee = std::filesystem::exists(app::ExeDir() / "dev_automelee");
    if (melee && play && now % 2000 < 150) pad.analog[1] = 255;
    // dev_dumpreload: A (reload) tapped every 6 s in play, the renderer recording the gun list's bones for 2.5 s
    // after each tap (phase 5: which bones a reload moves)
    static const bool reload = std::filesystem::exists(app::ExeDir() / "dev_dumpreload");
    // (between dev_autofire's bursts: a firing gun doesn't take the reload)
    int64_t phase = now % 6000;
    if (reload && play && first_play && now - first_play > 6000 && phase >= 1500 && phase < 1650) {
        pad.analog[0] = 255;
        g_record_until.store(now + 3000);
    }
}
bool VrRecording() { return NowMs() < g_record_until.load(); }

// game thread, each pad poll of port 0
void VrPad(input::Pad& pad) {
    DevPad(pad);
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
    {
        bool held = pad.analog[7] > 64;
        static int flips = 0;
        if (held != g_fire_held.load(std::memory_order_relaxed) && flips < 12)
            flips++, LOGR("vr body: fire trigger %s (pad poll)", held ? "down" : "up");
        g_fire_held.store(held, std::memory_order_relaxed);
    }
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
    g.last_yaw = yaw;
    g.play_ms = NowMs();
    d3d::VrLookUsed(yaw, pitch);
}
