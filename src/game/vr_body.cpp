// blackquest: the player's real height. The view follows the headset up and down (vulkan.cpp, local space); a real
// crouch also crouches the game's player (cover, hit box, speed) by holding its crouch button (left stick click),
// and while that crouch is the headset's, the game's own lowering of the camera is given back so the view drops only
// as far as the player did. The button still crouches the game's way.
#include <algorithm>
#include <atomic>
#include <chrono>

#include "core/log.h"
#include "core/input.h"
#include "game/layout.h"
#include "xdk/hle.h"

namespace xr {
bool HeadHeight(float& y);
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
    int64_t retry_at = 0;   // released standing up: if the game stays down (toggle crouch), one press then
} g;

}  // namespace

float VrViewLift() { return g_lift.load(std::memory_order_relaxed); }

// game thread, each pad poll of port 0
void VrPad(input::Pad& pad) {
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
