// blackquest phase 4: the gun in the hand. The game still poses its first person list (gun, arms) ahead of its
// camera; the renderer moves it by a rigid transform K from the gun's own frame to the right controller
// (vulkan.cpp EyeMatrix). The gun's frame is its muzzle locator's, so whatever the game's animation does to the gun
// (kick, sway, the melee swing) is taken back out and the gun stays put in the hand. The arms are one mesh: split by
// a plane between the hands, the left arm goes to the left controller. The player's bullets, the flash sprite and
// the gun's effects go through the same K.
// frames: C is the game camera's in openxr axes (x right, y up, z back), metres. A local-space point q is
// U^T (q + raise) in C, U = Ry(yaw) Rx(pitch) of the head angles the camera was built from (vr_body.cpp VrLook).
// The game's camera rows (player +0xD0) are renderware's: right row = left on screen, up, at (det +1, user's view)
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "common/options.h"
#include "core/log.h"
#include "d3d/stereo.h"
#include "game/layout.h"
#include "game/vr_stocks.h"
#include "xdk/hle.h"

#include "xr.h"

using namespace kernel;
using namespace layout;

namespace d3d {
void VrGunUsed(const float* frame);  // d3d8.cpp: transform slot 4 [5..15]
}
namespace weapons {
std::string HeldCode();  // weapons.cpp
}

namespace game {
void VrScopeHeld(const std::string& code);  // (below: scopes)
float VrViewLift();                           // vr_body.cpp
bool VrLookAngles(float& yaw, float& pitch);  // vr_body.cpp
bool VrMeleeHidden();                          // vr_body.cpp
// vr_reload.cpp: the magazine
int VrMagState();
int VrMagBone();
float VrSlide();
int VrLockBone();
float VrHandle();
bool VrGunParts(int& mag, int& slide, float& travel, float* mid, float* up, bool& measure, float& top, float& bottom,
                int& bullets);
bool VrRacking();
bool VrRackHeld();
bool VrSlideHeld();
float VrMagOutSeconds();
void VrMagWell(const float* c, const float* axis, float top, float bottom);
bool VrRackPart(int& bone, float* pt, float* to);
void VrRackAt(const float* c, const float* to, const float* up, const float* fwd);
float VrMagInsert();
void VrMagThrow(float* v);
bool VrMagInHand();
bool VrShellGun();
int VrLauncherRoot();      // vr_reload.cpp: the held launcher's root bone (-1 none)
bool VrM203Loaded();       // the M16's M203 has its grenade in
bool VrGunAway();          // vr_grenade.cpp: a grenade in the hand, or empty
bool VrGrenadeInHand();
bool VrLauncherLoaded();   // a round in it
int VrPartRoot();          // vr_reload.cpp: the held gun's root where its parts place by it (-1 none)
bool VrRevolver();
float VrCylOut();
float VrCylSpin();
bool VrCylRounds();
void VrReloadTick();
bool VrMagFromHand();
float VrMagHold();
float VrRodTilt();
void TestEvent(const std::string& name);  // vr_totest.cpp: the test checklist

namespace {

constexpr uint32_t kMuzzle = 0x1D2860;    // thiscall, ecx the player: its muzzle matrix to [esp+4]
constexpr uint32_t kPlayerModel = 0x328;  // the muzzle's model; its locators at +0x44, none before it exists
constexpr uint32_t kRows = 0xD0;          // player: camera rows left, up, at, then the eye
constexpr uint32_t kFromPlayerShot = 0x1DC0D6;  // return address of the player's one bullet spawn (0x1DC0D1)
constexpr uint32_t kRecoilKick = 0x4C4;  // player: the view's recoil kick (degrees), each gun's own, dying away
// the kick on the gun in the hand: the barrel turns up about the grip by this many times the game's view kick, and
// comes back toward the shoulder by so much a degree
constexpr float kKickTurn = 1.5f, kKickBack = 0.006f, kKickMax = 20.0f;

// where the hands hold each gun, from its muzzle along the barrel (metres, the viewmodel's size): back to the handle
// and how far under the barrel's line, and back to the fore-grip for the off hand. two: held with both hands, the
// arms split (pistols cup the gun hand: one piece). measured by eye on real guns; tune in the headset
struct Grip {
    float back, down, fore;
    bool two;
};
const std::map<std::string, Grip> kGrips = {
    {"PST", {0.13f, 0.09f, 0.13f, true}}, {"PS5", {0.20f, 0.09f, 0.20f, true}},
    {"GK1", {0.13f, 0.09f, 0.13f, true}}, {"GK5", {0.20f, 0.09f, 0.20f, true}},
    {"MGN", {0.20f, 0.09f, 0.20f, true}}, {"MG5", {0.20f, 0.09f, 0.20f, true}},
    // (the uzi: one-handed, as the game holds it: user, 2026-11-25)
    {"SMG", {0.17f, 0.10f, 0.08f, false}}, {"SM5", {0.17f, 0.10f, 0.08f, false}},
    {"SM3", {0.14f, 0.09f, 0.06f, true}},  {"MC5", {0.14f, 0.09f, 0.06f, true}},
    {"MP1", {0.30f, 0.08f, 0.15f, true}},  {"MP5", {0.36f, 0.08f, 0.20f, true}},
    {"P90", {0.25f, 0.08f, 0.08f, true}},  {"P95", {0.25f, 0.08f, 0.08f, true}},
    {"AK1", {0.68f, 0.08f, 0.25f, true}},  {"AK5", {0.68f, 0.08f, 0.25f, true}},
    {"ASR", {0.38f, 0.08f, 0.20f, true}},  {"AS5", {0.38f, 0.08f, 0.20f, true}},
    {"M16", {0.50f, 0.08f, 0.28f, true}},  {"M45", {0.50f, 0.08f, 0.28f, true}},
    {"BNS", {0.50f, 0.08f, 0.28f, true}},  {"SHG", {0.55f, 0.08f, 0.30f, true}},
    {"SH5", {0.55f, 0.08f, 0.30f, true}},  {"RM1", {0.58f, 0.08f, 0.30f, true}},
    {"RM5", {0.58f, 0.08f, 0.30f, true}},  {"SNR", {0.45f, 0.08f, 0.26f, true}},
    {"SN5", {0.50f, 0.08f, 0.25f, true}},  {"HVY", {0.55f, 0.08f, 0.30f, true}},
    {"HV5", {0.55f, 0.08f, 0.30f, true}},  {"RPG", {0.45f, 0.10f, 0.20f, true}},
    {"GRL", {0.30f, 0.10f, 0.04f, true}},  // (user: the walther's off hand nearer the trigger, 6 cm; the GRL's 8 cm out:
                                           // fore is back from the muzzle, so the walther's up, the GRL's down)
};
constexpr Grip kDefaultGrip = {0.35f, 0.08f, 0.18f, true};

// the gun's frame this frame, in C: the handle, the barrel's forward and up, the fore-grip's distance ahead of the
// handle, flags (1 valid, 2 two hands). the layout of transform slot 4 [5..15]
struct Frame {
    float grip[3], fwd[3], up[3], fore, flags;
};
static_assert(sizeof(Frame) == 11 * 4);

std::mutex g_lock;
Frame g_frame{};      // the latest, for the game thread's mapping
// metres the gun is pulled back along its barrel from a wall ahead (VrWallTick), the gun hand's grip
std::atomic<float> g_wall_pull{0};
std::atomic<float> g_kick{0};  // degrees, the game's latest (game thread), read by the render thread too
std::atomic<bool> g_kick_no_turn{false};  // the held gun's kick only pushes back, no tip up (the walther)
std::atomic<int64_t> g_shot_ms{0};  // steady ms of the player's last shot (the held frame, dev_rocklog)
// the left hand on the gun: gripped at the fore-grip (or the gun hand, on a pistol), the left arm drawn where the
// game holds it; off it, the left arm follows the left controller
std::atomic<bool> g_left_on{false};
std::atomic<float> g_left_fore{1e9f};  // the off hand's distance from the fore-grip (m), for the reload's zones
constexpr float kReach = 0.08f;  // metres from the fore-grip a grip press takes hold (tight: the user's ask)
std::string g_code = "?";  // the weapon it was made for

uint32_t Player() { return R32(kWorld) + kWorldPlayer; }
const float* Row(int i) { return &G<float>(Player() + kRows + 16 * i); }

// game world → C, a point (with the eye) or a direction; the rows are left, up, at
void ToCamera(const float* p, bool point, float* c) {
    const float* eye = Row(3);
    float d[3];
    for (int i = 0; i < 3; i++) d[i] = p[i] - (point ? eye[i] : 0);
    float v[3];
    for (int r = 0; r < 3; r++) v[r] = d[0] * Row(r)[0] + d[1] * Row(r)[1] + d[2] * Row(r)[2];
    c[0] = -v[0], c[1] = v[1], c[2] = -v[2];
}
void ToWorld(const float* c, bool point, float* p) {
    const float* eye = Row(3);
    for (int i = 0; i < 3; i++)
        p[i] = (point ? eye[i] : 0) - Row(0)[i] * c[0] + Row(1)[i] * c[1] - Row(2)[i] * c[2];
}

void Normalize(float* v) {
    float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f)
        for (int i = 0; i < 3; i++) v[i] /= l;
}
void Cross(const float* a, const float* b, float* o) {
    float r[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    std::memcpy(o, r, sizeof(r));
}

// the player's muzzle matrix in the game world (renderware rows: left, up, along the barrel, position)
bool Muzzle(X86Context& ctx, float* m) {
    uint32_t model = R32(Player() + kPlayerModel);
    if (!model || !R32(model + 0x50)) return false;
    static uint32_t out = mem::AllocSystem(64, 16);
    uint32_t ecx = ctx.ecx;
    ctx.ecx = Player();
    CallGuest(ctx, kMuzzle, {out});
    ctx.ecx = ecx;
    std::memcpy(m, HostPtr(out), 64);
    for (int i = 0; i < 16; i++)
        if (!std::isfinite(m[i])) return false;
    return true;
}

bool Active() { return stereo::Eyes() == 2 && R32(kWorld); }

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// a hand's aim orientation (3x3, columns its axes in local space) and grip position. dev_fakehands in the app's
// folder holds both hands still ahead of the head, to look at the gun without wearing the controllers
bool HandPose(int h, float* A, float* pos) {
    xr::Hand hand[2];
    float head[3];
    if (!xr::Hands(hand[0], hand[1], head)) return false;
    static const bool fake = std::filesystem::exists(app::ExeDir() / "dev_fakehands");
    if (fake) {
        float yaw, pitch;
        if (!xr::HeadAngles(yaw, pitch)) return false;
        float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
        float r[9] = {cy, sy * sp, sy * cp, 0, cp, -sp, -sy, cy * sp, cy * cp};  // Ry(yaw) Rx(pitch): in view
        std::memcpy(A, r, sizeof(r));
        // dev_fakehands_side: the gun hand turned to point left across the view, the gun seen side on (its left);
        // "r" in it: pointing right, its right side seen
        static const int side = [] {
            std::ifstream f(app::ExeDir() / "dev_fakehands_side");
            if (!f) return 0;
            std::string w;
            f >> w;
            return w == "r" ? -1 : 1;
        }();
        if (side && h == 1) {
            // A · Ry(±90°): x' = -z, z' = x (left); x' = z, z' = -x (right)
            for (int i = 0; i < 3; i++) {
                float x = r[i * 3], z = r[i * 3 + 2];
                A[i * 3] = -z * side, A[i * 3 + 2] = x * side;
            }
        }
        // dev_fakegun in the app folder: "pitch roll" degrees for the gun hand (its muzzle down, rolled: negative
        // counter-clockwise looking down the barrel), to see a gun turned without the controllers
        static float fk_pitch = 0, fk_roll = 0;
        static bool fk_read = false;
        if (!fk_read) {
            fk_read = true;
            if (FILE* fg = std::fopen((app::ExeDir() / "dev_fakegun").string().c_str(), "r")) {
                if (std::fscanf(fg, "%f %f", &fk_pitch, &fk_roll) != 2) fk_pitch = fk_roll = 0;
                std::fclose(fg);
            }
        }
        if (h == 1 && (fk_pitch != 0 || fk_roll != 0)) {
            // A · Rx(pitch) · Rz(-roll) (the aim's -z forward: a counter-clockwise roll seen down it is +z turn)
            float px = fk_pitch * 3.14159265f / 180, rz = -fk_roll * 3.14159265f / 180;
            float cx = std::cos(px), sx = std::sin(px), cz = std::cos(rz), sz = std::sin(rz);
            float Rx[9] = {1, 0, 0, 0, cx, -sx, 0, sx, cx}, Rz[9] = {cz, -sz, 0, sz, cz, 0, 0, 0, 1}, M[9], O[9];
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) {
                    M[i * 3 + j] = 0;
                    for (int k = 0; k < 3; k++) M[i * 3 + j] += Rx[i * 3 + k] * Rz[k * 3 + j];
                }
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) {
                    O[i * 3 + j] = 0;
                    for (int k = 0; k < 3; k++) O[i * 3 + j] += A[i * 3 + k] * M[k * 3 + j];
                }
            std::memcpy(A, O, sizeof(O));
        }
        // dev_fakeleft_<roll>_<yaw> in the app folder (its name: the app can't read files adb's shell writes there),
        // degrees for the left controller: rolled clockwise as seen down it (from behind), then turned to its right
        // by yaw (90: pointing right across the view, seen side on): the free off hand's pose without the controllers
        static float fl_roll = 0, fl_yaw = 0;
        static bool fl_read = false;
        if (!fl_read) {
            fl_read = true;
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(app::ExeDir(), ec)) {
                const std::string n = e.path().filename().string();
                if (n.rfind("dev_fakeleft_", 0) != 0 || n == "dev_fakeleft_low") continue;
                if (std::sscanf(n.c_str() + 13, "%f_%f", &fl_roll, &fl_yaw) < 1) fl_roll = fl_yaw = 0;
                LOGR("vr gun: %s: roll %.0f yaw %.0f", n.c_str(), fl_roll, fl_yaw);
            }
        }
        if (h == 0 && fl_yaw != 0) {
            // A · Ry(-yaw): a turn to the right is negative about +y
            float ry = -fl_yaw * 3.14159265f / 180, cy = std::cos(ry), sy = std::sin(ry);
            for (int i = 0; i < 3; i++) {
                float x = A[i * 3], z = A[i * 3 + 2];
                A[i * 3] = x * cy - z * sy, A[i * 3 + 2] = x * sy + z * cy;
            }
        }
        if (h == 0 && fl_roll != 0) {
            // A · Rz(-roll): clockwise down the aim's -z is a negative turn about +z
            float rz = -fl_roll * 3.14159265f / 180, cz = std::cos(rz), sz = std::sin(rz);
            for (int i = 0; i < 3; i++) {
                float x = A[i * 3], y = A[i * 3 + 1];
                A[i * 3] = x * cz + y * sz, A[i * 3 + 1] = -x * sz + y * cz;
            }
        }
        // dev_fakeleft_low: the left hand down by the hip, off the gun
        static const bool low = std::filesystem::exists(app::ExeDir() / "dev_fakeleft_low");
        const float off[2][3] = {{low ? -0.30f : -0.10f, low ? -0.50f : -0.14f, low ? -0.20f : -0.55f},
                                 {side ? 0.10f : 0.03f, -0.10f, -0.45f}};
        for (int i = 0; i < 3; i++) pos[i] = head[i] + r[i * 3] * off[h][0] + r[i * 3 + 1] * off[h][1] + r[i * 3 + 2] * off[h][2];
        // dev_reload: the left hand where the script puts it (xr::SetDevHands, vr_reload.cpp)
        static const bool script = std::filesystem::exists(app::ExeDir() / "dev_reload");
        if (script && h == 0 && hand[0].ok) std::memcpy(pos, hand[0].pos, sizeof(hand[0].pos));
        return true;
    }
    const xr::Hand& o = hand[h];
    if (!o.ok || !o.aim_ok) return false;
    const float* q = o.aim;
    float xx = q[0] * q[0], yy = q[1] * q[1], zz = q[2] * q[2];
    float xy = q[0] * q[1], xz = q[0] * q[2], yz = q[1] * q[2], wx = q[3] * q[0], wy = q[3] * q[1], wz = q[3] * q[2];
    float r[9] = {1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy),
                  2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx),
                  2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy)};
    std::memcpy(A, r, sizeof(r));
    std::memcpy(pos, o.pos, sizeof(o.pos));
    return true;
}

// the gun's axes in C as columns: right, up, back
void GunAxes(const Frame& f, float* R) {
    float back[3] = {-f.fwd[0], -f.fwd[1], -f.fwd[2]}, right[3];
    Cross(f.up, back, right);
    for (int i = 0; i < 3; i++) R[i * 3 + 0] = right[i], R[i * 3 + 1] = f.up[i], R[i * 3 + 2] = back[i];
}

// the hands' rests on the gun in C before K (the game's own hand bones on it, VrPoseArms): where the gun hand's
// palm and the off hand's palm are as the game holds the gun. under g_lock
bool g_rest_ok = false;
float g_rest[2][3];
std::string g_rest_code;
// the off hand aims a long gun along the line from the gun hand to it (set on the game thread)
std::atomic<bool> g_two_aim{false};
// a long gun (a fore-grip to hold) fired with the off hand off it (user 2026-11-26): more kick, the shots wander
std::atomic<bool> g_long_gun{false};
constexpr float kOneHandKick = 2.0f, kOneHandSpread = 4.0f;  // the kick times this; the shots within this (degrees)

// the minimal rotation taking unit a to unit b, row-major
void RotateOnto(const float* a, const float* b, float* R) {
    float v[3];
    Cross(a, b, v);
    float c = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    if (c < -0.9999f) {  // opposite: half a turn about any axis across a
        float t[3] = {std::fabs(a[0]) < 0.9f ? 1.0f : 0.0f, std::fabs(a[0]) < 0.9f ? 0.0f : 1.0f, 0}, ax[3];
        Cross(a, t, ax);
        Normalize(ax);
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) R[i * 3 + j] = 2 * ax[i] * ax[j] - (i == j ? 1 : 0);
        return;
    }
    float k = 1 / (1 + c);
    float vx[9] = {0, -v[2], v[1], v[2], 0, -v[0], -v[1], v[0], 0};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            float vv = 0;
            for (int m = 0; m < 3; m++) vv += vx[i * 3 + m] * vx[m * 3 + j];
            R[i * 3 + j] = (i == j ? 1 : 0) + vx[i * 3 + j] + vv * k;
        }
}

// the gun hand's frame in the room (local space: A's columns its axes, -z down the barrel; p the grip) as last
// posed, for the scope
std::mutex g_scope_lock;
float g_scope_a[9], g_scope_p[3];
int64_t g_scope_ms = 0;

// a hand's aim orientation and grip position in C, for the head angles the camera was built from. kick: the gun hand
// with the recoil on it. With the off hand on a long gun, the gun hand's barrel turns to point at it
bool HandInCamera(float yaw, float pitch, int hand, float* AC, float* hc, bool kick = true) {
    float A[9], hp[3];
    if (!HandPose(hand, A, hp)) return false;
    const Options& opt = Opt();
    if (hand == 1 && opt.vr_gun_angle != 45) {
        // the gun's angle on the controller (user: a stock's mount): A · Rx, the barrel (-z) up for a positive angle
        float t = (float(opt.vr_gun_angle) - 45) * 3.14159265f / 180, c = std::cos(t), sn = std::sin(t);
        for (int i = 0; i < 3; i++) {
            float y = A[i * 3 + 1], z = A[i * 3 + 2];
            A[i * 3 + 1] = y * c + z * sn;
            A[i * 3 + 2] = -y * sn + z * c;
        }
    }
    float L[9], lp[3];
    bool stock = false;
    if (hand == 1 && opt.vr_gunstock && HandPose(0, L, lp)) {
        // a physical gunstock (user): both controllers fixed on it, the off one ahead of the gun hand along the
        // barrel (8-90 cm, within 35 degrees of it): aimed at it, no grip. Its height over the barrel's line taken off
        // along the gun's up
        float h = (float(opt.vr_stock_height) - 10) * 0.01f;
        for (int i = 0; i < 3; i++) lp[i] -= A[i * 3 + 1] * h;
        float d[3] = {lp[0] - hp[0], lp[1] - hp[1], lp[2] - hp[2]};
        float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        float along = len > 1e-4f ? -(d[0] * A[2] + d[1] * A[5] + d[2] * A[8]) / len : 0;
        stock = len > 0.08f && len < 0.9f && along > std::cos(35 * 3.14159265f / 180);
    }
    if (hand == 1 && (stock || (g_two_aim.load(std::memory_order_relaxed) && HandPose(0, L, lp)))) {
        float d0[3] = {-A[2], -A[5], -A[8]}, d1[3] = {lp[0] - hp[0], lp[1] - hp[1], lp[2] - hp[2]};
        float len = std::sqrt(d1[0] * d1[0] + d1[1] * d1[1] + d1[2] * d1[2]);
        if (len > 0.08f) {
            Normalize(d1);
            float R[9], B[9];
            RotateOnto(d0, d1, R);
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) B[i * 3 + j] = R[i * 3] * A[j] + R[i * 3 + 1] * A[3 + j] + R[i * 3 + 2] * A[6 + j];
            std::memcpy(A, B, sizeof(B));
        }
    }
    const float one_hand = g_long_gun.load(std::memory_order_relaxed) && !g_left_on.load(std::memory_order_relaxed)
                               ? kOneHandKick : 1.0f;
    float deg = std::min(std::fabs(g_kick.load(std::memory_order_relaxed)) * one_hand, kKickMax * one_hand);
    if (kick && hand == 1 && deg > 0.01f) {
        // (the walther: its kick is the game's biggest, and tipping it up read as the gun rocking in the hand, user)
        const bool turn = !g_kick_no_turn.load(std::memory_order_relaxed);
        // A · Rx(θ): the barrel (−z) up toward +y about the grip, then back along the barrel
        float t = turn ? deg * kKickTurn * 3.14159265f / 180 : 0, c = std::cos(t), sn = std::sin(t);
        for (int i = 0; i < 3; i++) {
            float y = A[i * 3 + 1], z = A[i * 3 + 2];
            A[i * 3 + 1] = y * c + z * sn;
            A[i * 3 + 2] = -y * sn + z * c;
        }
        const float back = turn ? deg * kKickBack : std::min(deg * kKickBack, 0.02f);  // (the walther: 2 cm at most)
        for (int i = 0; i < 3; i++) hp[i] += A[i * 3 + 2] * back;
    }
    if (hand == 1) {
        // out of a wall ahead (VrWallTick): the whole gun back along its barrel, the shot from where it's drawn
        float pull = g_wall_pull.load(std::memory_order_relaxed);
        if (pull > 0.001f)
            for (int i = 0; i < 3; i++) hp[i] += A[i * 3 + 2] * pull;
    }
    if (kick && hand == 1) {
        // the gun's frame in the room as posed (the scope's place, below)
        std::lock_guard lock(g_scope_lock);
        std::memcpy(g_scope_a, A, sizeof(g_scope_a));
        std::memcpy(g_scope_p, hp, sizeof(g_scope_p));
        g_scope_ms = NowMs();
    }
    float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    float U[9] = {cy, sy * sp, sy * cp, 0, cp, -sp, -sy, cy * sp, cy * cp};
    hp[1] += xr::kEyeRaise + VrViewLift();
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 3; k++) AC[i * 3 + k] = U[0 * 3 + i] * A[0 * 3 + k] + U[1 * 3 + i] * A[1 * 3 + k] + U[2 * 3 + i] * A[2 * 3 + k];
        hc[i] = U[0 * 3 + i] * hp[0] + U[1 * 3 + i] * hp[1] + U[2 * 3 + i] * hp[2];
    }
    return true;
}

// where a hand rests on the gun (C before K): the game's palm on it once the arms have been drawn, else kGrips
void Rest(const Frame& f, int hand, float* rest) {
    {
        std::lock_guard lock(g_lock);
        if (g_rest_ok && g_rest_code == g_code) {
            std::memcpy(rest, g_rest[hand], 12);
            return;
        }
    }
    for (int i = 0; i < 3; i++) rest[i] = f.grip[i] + (hand == 0 ? f.fwd[i] * f.fore : 0);
}
// the off hand on a shotgun's fore-end goes back with its pump (user: the hold locked on it), metres
float PumpBack() {
    int bone, slide, bullets;
    float travel, mid[3], up[3], top, bottom;
    bool measure;
    if (!VrShellGun() || !VrGunParts(bone, slide, travel, mid, up, measure, top, bottom, bullets)) return 0;
    return travel * VrSlide();
}

// ---- own hands: the game's arms mesh, posed by our bones. Its bones are 4x3 rows from c-36 (a vertex's index
// shorts are register offsets), mapping the mesh to the game's view space G: x left (the shader's projection has
// x negative, the d3d projection transform positive), so C is G with x and z flipped. AK and pistol arms share
// one rig (dumps, NOTES 2026-11-21): per arm the upper arm, the forearm (two bones on the left: the second
// twists), the hand, then the fingers; the mesh's +x arm is the left one. pivots in the mesh's own space from vertices weighted to both
// bones: the elbow and the wrist; the palm is the hand's middle
// twist: how much of the hand's roll about the forearm each forearm bone takes (the wrist end takes most)
struct ArmRig {
    int upper, fore[2], hand, finger_lo, finger_hi;
    float elbow[3], wrist[3], palm[3], side, twist[2];
};
// (user 2026-11-27: a share on each bone, 0.3 and 0.8, wrung the forearm's skin like a towel as the wrist turned:
// the whole forearm turns with the hand now, as one piece)
const ArmRig kRig[2] = {
    {9, {12, 15}, 18, 21, 63, {0.128f, -0.004f, -0.107f}, {0.116f, 0.005f, 0.137f}, {0.117f, -0.003f, 0.204f}, -1,
     {1, 1}},
    {66, {69, -1}, 72, 75, 129, {-0.128f, -0.004f, -0.107f}, {-0.118f, -0.015f, 0.145f}, {-0.118f, -0.003f, 0.205f}, 1,
     {1, 0}},
};
// the arms' bone rows by gun (each gun's .db names its skeleton from World: row = 3 x its place). Most are the
// table's; the uzi's lacks the left hand's last fingertip (its right arm is a row early), the m249's the left
// wrist's twist joint and that fingertip, and the magnum's speed loader comes first (both arms seven rows on)
ArmRig RigOf(const std::string& code, int h) {
    ArmRig a = kRig[h];
    auto set = [&](int upper, int f0, int f1, int hand, int lo, int hi) {
        a.upper = upper, a.fore[0] = f0, a.fore[1] = f1, a.hand = hand, a.finger_lo = lo, a.finger_hi = hi;
    };
    if (code == "SMG") {
        if (h == 0) a.finger_hi = 60;
        else set(63, 66, -1, 69, 72, 129);
    } else if (code == "HVY" || code == "HV5") {
        if (h == 0) set(9, 12, -1, 15, 18, 60), a.twist[0] = 1, a.twist[1] = 0;  // the forearm takes the twist
        else set(63, 66, -1, 69, 72, 123);
    } else if (code == "MGN" || code == "MG5") {
        if (h == 0) set(30, 33, 36, 39, 42, 84);
        else set(87, 90, -1, 93, 96, 129);
    }
    return a;
}
// the free off hand's roll about its controller's pointing axis, clockwise as seen down the arm (user: 45°; then
// 2026-11-26, the thumb down on an upright controller: dev_fakehands measured it 72° below the controller's right,
// and the user's check, the controller turned 90° clockwise showing the thumb on top and the fingers out to the
// right, puts it on the controller's left side: 108° more). 2026-11-27, the user's controller held normally (their
// screenshots): the thumb out to the left, flat, the fingers curled up like an open palm: the thumb belongs on the
// controller's top (+90° about the axis, from -180° now: the roll moves the thumb one for one, -45 gave -72): 270°
// round from here, -243 = +117
constexpr float kFreeHandRoll = 117.0f * 3.14159265f / 180;
constexpr int kBone0 = 96 - 36;     // the constant (hardware order) of bone offset 0
constexpr float kUpperArm = 0.28f;  // shoulder to elbow (the mesh only has the lower half of the upper arm)
constexpr float kShoulderOut = 0.17f, kShoulderDown = 0.24f, kShoulderBack = 0.06f;  // from the head's middle

// affine 3x4, row-major [R | t]
struct Aff {
    float m[12];
};
constexpr Aff kIdentityAff{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}};
Aff Compose(const Aff& a, const Aff& b) {  // a ∘ b
    Aff o{};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) o.m[i * 4 + j] = a.m[i * 4] * b.m[j] + a.m[i * 4 + 1] * b.m[4 + j] + a.m[i * 4 + 2] * b.m[8 + j];
        o.m[i * 4 + 3] = a.m[i * 4] * b.m[3] + a.m[i * 4 + 1] * b.m[7] + a.m[i * 4 + 2] * b.m[11] + a.m[i * 4 + 3];
    }
    return o;
}
void Apply(const Aff& a, const float* p, float* o) {
    float r[3];
    for (int i = 0; i < 3; i++) r[i] = a.m[i * 4] * p[0] + a.m[i * 4 + 1] * p[1] + a.m[i * 4 + 2] * p[2] + a.m[i * 4 + 3];
    std::memcpy(o, r, 12);
}
Aff FromRotPos(const float* R, const float* t) {
    Aff o{};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) o.m[i * 4 + j] = R[i * 3 + j];
        o.m[i * 4 + 3] = t[i];
    }
    return o;
}
Aff Inverse(const Aff& a) {  // rotation and translation only (K and the z flip)
    float R[9], t[3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) R[i * 3 + j] = a.m[j * 4 + i];
    for (int i = 0; i < 3; i++) t[i] = -(R[i * 3] * a.m[3] + R[i * 3 + 1] * a.m[7] + R[i * 3 + 2] * a.m[11]);
    return FromRotPos(R, t);
}
// x → R(x − from) + to
Aff Pivot(const float* R, const float* from, const float* to) {
    float t[3];
    for (int i = 0; i < 3; i++) t[i] = to[i] - (R[i * 3] * from[0] + R[i * 3 + 1] * from[1] + R[i * 3 + 2] * from[2]);
    return FromRotPos(R, t);
}
Aff Bone(const float (*c)[4], int off) {
    Aff o;
    for (int r = 0; r < 3; r++) std::memcpy(&o.m[r * 4], c[kBone0 + off + r], 16);
    return o;
}
void SetBone(float (*c)[4], int off, const Aff& a) {
    for (int r = 0; r < 3; r++) std::memcpy(c[kBone0 + off + r], &a.m[r * 4], 16);
}
// the walther's bolt and the shotguns' pumps are the player's (user): the game's cycling after each shot (the part
// back and forward, a hand on it) isn't drawn. While that part's bone is off its rest (on the gun's root, as every
// gun part at rest) it's put back on the root, and the arms keep their last pose on the gun (relative to its root)
bool WorkedGun(const std::string& code, int& root, int& slide) {
    if (code == "SNR" || code == "SN5") return root = 75, slide = 84, true;
    if (code == "SHG" || code == "SH5") return root = 108, slide = 111, true;
    if (code == "RM1" || code == "RM5") return root = 108, slide = 117, true;
    // the magnum: none of the game's animations on it (user: one took over at times): its root held at rest, the
    // cylinder (120) on it (vr_reload.cpp turns it); its arms held apart (VrPoseArms)
    if (code == "MGN" || code == "MG5") return root = 111, slide = 120, true;
    return false;
}
std::atomic<int64_t> g_cycled_ms{0};  // steady ms the game last had a worked gun's part off its rest
// a worked gun's root as it rests (user: the game's rock back after a shot, and every other move of the gun in the
// hand, isn't drawn; the player works the gun and the hand holds it): taken once a gun is out and settled (1.5 s
// after it's first drawn, not cycling), then the root is held there in every draw of it, gun and arms
struct RootRest {
    std::string code;
    int64_t since = 0;
    bool ok = false;
    Aff root;
};
RootRest g_root_rest;
std::mutex g_root_lock;
int64_t SteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
// each gun's root bone (its .db: the rifles' 75, the pistols', smgs' and shotguns' 108, ...: as LogBarrel)
int GunRoot(const std::string& code) {
    static const std::set<std::string> k108 = {"PST", "PS5", "GK1", "GK5", "SMG", "SM5", "SM3", "MC5",
                                               "RM1", "RM5", "SHG", "SH5", "RPG"};
    if (k108.count(code)) return 108;
    return code == "HVY" || code == "HV5" ? 90 : code == "MGN" || code == "MG5" ? 111 : code == "GRL" ? 105 : 75;
}
bool Cycling(const float (*c)[4], int root, int slide) {
    Aff r = Bone(c, root), sl = Bone(c, slide);
    float d = 0;
    for (int i = 0; i < 12; i++) d = std::max(d, std::fabs(r.m[i] - sl.m[i]));
    return d > 1e-3f;
}
std::string HeldGunCode() {
    std::lock_guard lock(g_lock);
    return g_code;
}

// rotation by angle about a unit axis, row-major
void AxisAngle(const float* u, float t, float* R) {
    float c = std::cos(t), s = std::sin(t), k = 1 - c;
    float m[9] = {c + u[0] * u[0] * k,        u[0] * u[1] * k - u[2] * s, u[0] * u[2] * k + u[1] * s,
                  u[1] * u[0] * k + u[2] * s, c + u[1] * u[1] * k,        u[1] * u[2] * k - u[0] * s,
                  u[2] * u[0] * k - u[1] * s, u[2] * u[1] * k + u[0] * s, c + u[2] * u[2] * k};
    std::memcpy(R, m, sizeof(m));
}
// the angle about axis from the forearm bone's y axis (turned by R, if given) to the hand bone's, both laid flat
// across the axis: how far the hand is rolled against the forearm
float HandRoll(const Aff& fore, const Aff& hand, const float* R, const float* axis) {
    float fy[3] = {fore.m[1], fore.m[5], fore.m[9]}, hy[3] = {hand.m[1], hand.m[5], hand.m[9]};
    if (R) {
        float t[3];
        for (int i = 0; i < 3; i++) t[i] = R[i * 3] * fy[0] + R[i * 3 + 1] * fy[1] + R[i * 3 + 2] * fy[2];
        std::memcpy(fy, t, sizeof(t));
    }
    for (float* v : {fy, hy}) {
        float d = v[0] * axis[0] + v[1] * axis[1] + v[2] * axis[2];
        for (int i = 0; i < 3; i++) v[i] -= axis[i] * d;
    }
    float x[3];
    Cross(fy, hy, x);
    return std::atan2(x[0] * axis[0] + x[1] * axis[1] + x[2] * axis[2], fy[0] * hy[0] + fy[1] * hy[1] + fy[2] * hy[2]);
}
float Dist(const float* a, const float* b) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

// ---- fingers: the game's finger bones curled by the controllers. A hand's fingers are found once per arms mesh
// from its vertices (bones sharing vertices are joined; the pivot between two is where they share), then each
// frame every joint is turned about the knuckle line from the game's pose to an angle between open and closed:
// the grip closes the middle, ring and little fingers, the trigger the index (resting on it: half), the thumb
// comes down when it touches the rest, the stick or a face button. Closed is the game's own pose on the gun when
// the hand holds it, a fist when it's free
struct Finger {
    int n = 0;
    int bone[3];
    float pivot[3][3];  // mesh space: where each bone meets its parent
    float dir[3][3];    // mesh space: each bone's direction (to the next pivot; the last to its own middle)
};
struct HandFingers {
    bool ok = false;
    Finger f[5];  // thumb, index, middle, ring, little
};
// by the gun (its rig) and the arms mesh's index count (render thread)
std::map<std::pair<std::string, uint32_t>, std::array<HandFingers, 2>> g_fingers;

constexpr float kDeg = 3.14159265f / 180;
// open: a relaxed hand; fist: closed when free (flex at the knuckle, middle and end joints)
constexpr float kOpenFlex[3] = {6 * kDeg, 8 * kDeg, 5 * kDeg};
constexpr float kFistFlex[3] = {80 * kDeg, 95 * kDeg, 55 * kDeg};
constexpr float kThumbOut = 40 * kDeg;  // the thumb lifted off when nothing touches it
// the game's thumb sits twisted on the mesh: turned back about its own line (user: 10-15° clockwise; mirrored
// between the hands)
constexpr float kThumbTwist = 12 * kDeg;
constexpr float kThumbRest = 0.6f;  // how far a free hand's thumb comes down when it touches (1: the game's pose)
// the gun hand's middle, ring and little fingers: closed on the gun, a grip press (not over the shoulder) opens or
// closes them (vr_body.cpp); closed again with each weapon
std::atomic<bool> g_gun_fingers_open{false};

void FindFingers(const std::string& code, const uint8_t* vb, uint32_t stride, const void* indices, bool index16,
                 uint32_t count, int32_t base, std::array<HandFingers, 2>& out) {
    // pivots between bone pairs from vertices weighted to both (the second weight over a quarter), and centroids
    std::map<std::pair<int, int>, std::array<double, 4>> pair;
    std::map<int, std::array<double, 4>> cent;
    std::vector<bool> seen;
    for (uint32_t i = 0; i < count; i++) {
        int64_t v = int64_t(index16 ? static_cast<const uint16_t*>(indices)[i] : static_cast<const uint32_t*>(indices)[i]) + base;
        if (v < 0) continue;
        if (size_t(v) >= seen.size()) seen.resize(size_t(v) + 1);
        if (seen[size_t(v)]) continue;
        seen[size_t(v)] = true;
        const uint8_t* p = vb + size_t(v) * stride;
        float pos[3];
        std::memcpy(pos, p, 12);
        const uint8_t* w = p + 12;
        int16_t ix[4];
        std::memcpy(ix, p + 16, 8);
        int a = -1, b = -1;
        for (int k = 0; k < 4; k++) {
            if (!w[k]) continue;
            auto& c = cent[ix[k]];
            for (int j = 0; j < 3; j++) c[j] += pos[j] * w[k];
            c[3] += w[k];
            if (a < 0 || w[k] > w[a]) b = a, a = k;
            else if (b < 0 || w[k] > w[b]) b = k;
        }
        if (a >= 0 && b >= 0 && w[b] > 60 && ix[a] != ix[b]) {
            auto& c = pair[{std::min(ix[a], ix[b]), std::max(ix[a], ix[b])}];
            for (int j = 0; j < 3; j++) c[j] += pos[j];
            c[3] += 1;
        }
    }
    auto pivot = [&](int x, int y, float* o) {
        auto it = pair.find({std::min(x, y), std::max(x, y)});
        if (it == pair.end() || it->second[3] < 2) return false;
        for (int j = 0; j < 3; j++) o[j] = float(it->second[j] / it->second[3]);
        return true;
    };
    for (int h = 0; h < 2; h++) {
        const ArmRig a = RigOf(code, h);
        HandFingers hf;
        std::vector<Finger> chains;
        std::set<int> used;
        for (int root = a.finger_lo; root <= a.finger_hi; root += 3) {
            Finger f;
            if (used.count(root) || !pivot(a.hand, root, f.pivot[0])) continue;
            f.bone[0] = root, f.n = 1;
            used.insert(root);
            // down the finger: the next bone sharing vertices with this one, higher in the list
            while (f.n < 3) {
                int next = -1;
                for (int b = f.bone[f.n - 1] + 3; b <= a.finger_hi && next < 0; b += 3)
                    if (!used.count(b) && pivot(f.bone[f.n - 1], b, f.pivot[f.n])) next = b;
                if (next < 0) break;
                f.bone[f.n++] = next;
                used.insert(next);
            }
            for (int k = 0; k < f.n; k++) {
                float to[3];
                if (k + 1 < f.n) {
                    std::memcpy(to, f.pivot[k + 1], 12);
                } else {
                    auto& c = cent[f.bone[k]];
                    for (int j = 0; j < 3; j++) to[j] = float(c[j] / std::max(c[3], 1.0));
                }
                for (int j = 0; j < 3; j++) f.dir[k][j] = to[j] - f.pivot[k][j];
                Normalize(f.dir[k]);
            }
            chains.push_back(f);
        }
        if (chains.size() < 5) {
            out[h] = hf;
            continue;
        }
        // the thumb: the chain rooted nearest the wrist; then index to little by distance from the thumb's root
        auto wrist_dist = [&](const Finger& f) { return Dist(f.pivot[0], a.wrist); };
        std::sort(chains.begin(), chains.end(), [&](const Finger& x, const Finger& y) { return wrist_dist(x) < wrist_dist(y); });
        hf.f[0] = chains[0];
        std::vector<Finger> rest(chains.begin() + 1, chains.end());
        std::sort(rest.begin(), rest.end(), [&](const Finger& x, const Finger& y) {
            return Dist(x.pivot[0], hf.f[0].pivot[0]) < Dist(y.pivot[0], hf.f[0].pivot[0]);
        });
        for (int i = 0; i < 4; i++) hf.f[i + 1] = rest[i];
        hf.ok = true;
        out[h] = hf;
        LOGR("vr arms: %s hand fingers: thumb %d, index %d, middle %d, ring %d, little %d (%zu chains)",
             h ? "right" : "left", hf.f[0].bone[0], hf.f[1].bone[0], hf.f[2].bone[0], hf.f[3].bone[0],
             hf.f[4].bone[0], chains.size());
    }
}

void RotOf(const Aff& a, const float* v, float* o) {
    for (int i = 0; i < 3; i++) o[i] = a.m[i * 4] * v[0] + a.m[i * 4 + 1] * v[1] + a.m[i * 4 + 2] * v[2];
}
float SignedAngle(const float* a, const float* b, const float* axis) {
    float x[3];
    Cross(a, b, x);
    return std::atan2(x[0] * axis[0] + x[1] * axis[1] + x[2] * axis[2], a[0] * b[0] + a[1] * b[1] + a[2] * b[2]);
}

// one hand's fingers curled in G (before the hand is moved): curl 0 open .. 1 closed per finger (thumb: down)
// geo: the curl's direction from the mesh's own hand (toward the thumb's side of the palm), not from the game's pose:
// a gun whose game left hand isn't a real pose (the uzi's clenched and hidden, the walther's turned) read it backward
// and "open" bent the fingers further into a fist (user, 2026-11-25: point with the off hand)
void CurlFingers(float (*c)[4], const ArmRig& a, const HandFingers& hf, const float* curl, bool on_gun, bool geo = false,
                 const std::string& code = {}) {
    Aff hb = Bone(c, a.hand);
    // the knuckle line, across the hand from the little finger to the index, in G
    float lat_m[3], lat[3];
    for (int j = 0; j < 3; j++) lat_m[j] = hf.f[1].pivot[0][j] - hf.f[4].pivot[0][j];
    Normalize(lat_m);
    RotOf(hb, lat_m, lat);
    Normalize(lat);
    // flexing curls toward the palm: the axis turned so the game's grip (curled) reads positive
    float sum = 0;
    for (int i = 1; i < 5; i++) {
        float hd[3], d[3], wr[3];
        for (int j = 0; j < 3; j++) wr[j] = hf.f[i].pivot[0][j] - a.wrist[j];
        Normalize(wr);
        RotOf(hb, wr, hd);
        RotOf(Bone(c, hf.f[i].bone[0]), hf.f[i].dir[0], d);
        sum += SignedAngle(hd, d, lat);
    }
    bool flip = sum < 0;
    {
        // the mesh's palm side: the thumb's root off the plane of the knuckles and the wrist; a curl (+ about the
        // knuckle line, little to index) carries a finger toward it
        float mid[3] = {}, v[3], pn[3], th[3];
        for (int i = 1; i < 5; i++)
            for (int j = 0; j < 3; j++) mid[j] += hf.f[i].pivot[0][j] * 0.25f;
        for (int j = 0; j < 3; j++) v[j] = mid[j] - a.wrist[j], th[j] = hf.f[0].pivot[0][j] - mid[j];
        Cross(lat_m, v, pn);
        bool geo_flip = pn[0] * th[0] + pn[1] * th[1] + pn[2] * th[2] < 0;
        static std::set<std::string> told;
        if (!code.empty() && told.insert(code + (a.side < 0 ? "L" : "R")).second)
            LOGR("vr arms: %s %s hand's curl: from its pose %s, from the mesh %s%s", code.c_str(), a.side < 0 ? "left" : "right",
                 flip ? "flipped" : "kept", geo_flip ? "flipped" : "kept", geo ? " (the mesh's used)" : "");
        if (geo) flip = geo_flip;
    }
    if (flip)
        for (float& v : lat) v = -v;
    for (int i = 0; i < 5; i++) {
        const Finger& f = hf.f[i];
        Aff acc = kIdentityAff;  // what the joints above did, in G
        float prev[3];
        {
            float wr[3];
            for (int j = 0; j < 3; j++) wr[j] = f.pivot[0][j] - a.wrist[j];
            Normalize(wr);
            RotOf(hb, wr, prev);
        }
        for (int k = 0; k < f.n; k++) {
            Aff b = Bone(c, f.bone[k]);
            float d[3], p[3];
            RotOf(b, f.dir[k], d);
            Normalize(d);
            Apply(b, f.pivot[k], p);
            if (i == 0 && k == 0) {
                // the thumb's twist, about its own line at its root
                float T[9];
                AxisAngle(d, kThumbTwist * a.side, T);
                acc = Pivot(T, p, p);
            }
            float flex = SignedAngle(prev, d, lat), want;
            if (i == 0) {
                // the thumb, lifted at its root away from the palm (the other way about the knuckle line from the
                // fingers' curl) with the joints above eased straight. Down on the gun is the game's own pose; on a
                // free hand it rests partway, the game's free pose lies flat and looks broken (user)
                float lift = on_gun ? 1 - curl[0] : 1 - curl[0] * kThumbRest;
                want = flex + (k == 0 ? kThumbOut : -flex * 0.7f) * lift;
            } else {
                float closed = on_gun ? flex : std::max(flex, kFistFlex[std::min(k, 2)]);
                want = kOpenFlex[std::min(k, 2)] + (closed - kOpenFlex[std::min(k, 2)]) * curl[i];
            }
            float R[9];
            AxisAngle(lat, want - flex, R);
            Aff turn = Pivot(R, p, p);
            acc = Compose(acc, turn);
            SetBone(c, f.bone[k], Compose(acc, b));
            std::memcpy(prev, d, sizeof(d));
        }
    }
}

}  // namespace

// K in C for the head angles the camera was built from, for a hand (1 right: the gun, 0 left: the left arm):
// v → rot·v + pos (row-major rot). frame: transform slot 4 [5..15]. render or game thread
bool VrGunPose(float yaw, float pitch, const float* frame, int hand, float* rot, float* pos) {
    Frame f;
    std::memcpy(&f, frame, sizeof(f));
    if (!(int(f.flags) & 1)) return false;
    // the left hand on the gun goes with the gun hand, as the game holds it
    bool on = hand == 0 && g_left_on.load(std::memory_order_relaxed);
    float AC[9], hc[3];
    if (!HandInCamera(yaw, pitch, on ? 1 : hand, AC, hc)) return false;
    if (hand == 0 && !on) {
        // the free off hand rolled about the controller's pointing axis: AC · Rz
        float c = std::cos(kFreeHandRoll), s = std::sin(kFreeHandRoll);
        for (int i = 0; i < 3; i++) {
            float x = AC[i * 3], y = AC[i * 3 + 1];
            AC[i * 3] = x * c + y * s;
            AC[i * 3 + 1] = -x * s + y * c;
        }
    }
    float G[9];
    GunAxes(f, G);
    // rot = AC · Gᵀ, the hand's rest on the gun put on its controller
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++) rot[i * 3 + k] = AC[i * 3 + 0] * G[k * 3 + 0] + AC[i * 3 + 1] * G[k * 3 + 1] + AC[i * 3 + 2] * G[k * 3 + 2];
    float rest[3];
    Rest(f, on ? 1 : hand, rest);
    for (int i = 0; i < 3; i++) pos[i] = hc[i] - (rot[i * 3] * rest[0] + rot[i * 3 + 1] * rest[1] + rot[i * 3 + 2] * rest[2]);
    return true;
}

// render thread, the arms' draw: their bones (the 192 vertex constants, hardware order) rewritten so each hand is on
// its controller (the gun hand on the gun's grip; the off hand on the gun where the game holds it, or on its own
// controller), the shoulders by the head, the elbows bent between (two bones, elbows down and out). The draw keeps
// the gun's K, so a bone B becomes D⁻¹ Δ D B, D the gun's drawing (G → C: x and z flipped, then K), Δ the change in C
// mesh: the arms' vertices and indices as drawn, to find the fingers from (once per mesh)
bool VrPoseArms(float yaw, float pitch, const float* frame, float (*c)[4], const uint8_t* vb, uint32_t stride,
                const void* indices, bool index16, uint32_t index_count, int32_t base_vertex) {
    float kr[9], kp[3];
    if (!VrGunPose(yaw, pitch, frame, 1, kr, kp)) return false;
    std::string code;
    {
        std::lock_guard lock(g_lock);
        code = g_code;
    }
    const ArmRig rig[2] = {RigOf(code, 0), RigOf(code, 1)};
    // dev_lefthand: the game's left hand on each gun, relative to the gun's root (once a gun, 2 s after it's out)
    if (std::filesystem::exists(app::ExeDir() / "dev_lefthand")) {
        static std::map<std::string, int64_t> first;
        static std::set<std::string> told;
        if (!first.count(code)) first[code] = SteadyMs();
        if (!told.count(code) && SteadyMs() - first[code] > 2000) {
            told.insert(code);
            Aff rel = Compose(Inverse(Bone(c, GunRoot(code))), Bone(c, rig[0].hand));
            float palm[3];
            Apply(rel, rig[0].palm, palm);
            LOGR("vr arms: %s left hand on root %d: %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f; "
                 "palm %.3f %.3f %.3f", code.c_str(), GunRoot(code), rel.m[0], rel.m[1], rel.m[2], rel.m[3], rel.m[4],
                 rel.m[5], rel.m[6], rel.m[7], rel.m[8], rel.m[9], rel.m[10], rel.m[11], palm[0], palm[1], palm[2]);
            // and the left hand's turn in the gun's own axes (GunAxes, C: the bone's G with x and z flipped), which
            // the free off hand is posed against: Q = Rᵀ F B F
            Frame fr;
            std::memcpy(&fr, frame, sizeof(fr));
            float R[9], Q[9];
            GunAxes(fr, R);
            Aff h = Bone(c, rig[0].hand);
            const float F[3] = {-1, 1, -1};
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) {
                    float v = 0;
                    for (int k = 0; k < 3; k++) v += R[k * 3 + i] * F[k] * h.m[k * 4 + j] * F[j];
                    Q[i * 3 + j] = v;
                }
            LOGR("vr arms: %s left hand in the gun's axes: %.4f %.4f %.4f | %.4f %.4f %.4f | %.4f %.4f %.4f", code.c_str(),
                 Q[0], Q[1], Q[2], Q[3], Q[4], Q[5], Q[6], Q[7], Q[8]);
        }
    }
    // the uzi's and walther's game left hand isn't on the gun (the uzi's hidden half a metre off, the walther's
    // turned at the stock's side: dev_lefthand, NOTES 2026-11-23), and the free off hand and the fore-grip take
    // theirs from it (user: turned, and neither could be held): the M16's left hand there instead, its angle to the
    // gun's root (measured, the AK's and SPAS's within a few degrees), its palm at the gun's fore-grip (kGrips)
    // every gun's free off hand (user 2026-11-26): the pistol's, level, thumb on top, at rest; the gun's own hold
    // only while the hand grips the gun
    const bool free_left = !g_left_on.load(std::memory_order_relaxed);
    if (code == "SMG" || code == "SM5" || code == "SNR" || code == "SN5" || free_left) {
        static const float kM16Left[9] = {0.0351f, 0.5319f, -0.8461f, -0.9744f, 0.2062f, 0.0892f,
                                          0.2219f, 0.8213f, 0.5255f};
        // the uzi (one-handed, user 2026-11-25): its free off hand as the pistol's, the pistol's left hand in the gun's
        // own axes (dev_lefthand: PST, Q = Rᵀ F B F) put on the uzi's (the root's own turn differs gun to gun: the
        // M16's hand on the uzi's root came palm up toward the gun)
        static const float kPstInGun[9] = {-0.0365f, 0.9956f, -0.0641f, -0.9940f, -0.0423f, -0.0869f,
                                           -0.0878f, 0.0615f, 0.9937f};
        // the walther (user, 2026-11-25): on its fore-end the AK's left hand in the gun's axes (a rifle's fore-grip,
        // the game's own; the walther's is turned at the stock's side, the M16's borrowed cupped it from under), free
        // the pistol's, as the uzi's
        static const float kAkInGun[9] = {0.0351f, -0.4000f, -0.9158f, 0.9743f, 0.2173f, -0.0576f,
                                          0.2220f, -0.8903f, 0.3974f};
        const bool uzi = code == "SMG" || code == "SM5";
        const float* in_gun = uzi || free_left ? kPstInGun : kAkInGun;
        const float* left = kM16Left;
        Frame fr;
        std::memcpy(&fr, frame, sizeof(fr));
        Aff root = Bone(c, GunRoot(code)), old_hand = Bone(c, rig[0].hand), hand = kIdentityAff;
        // the rotation: root's · the M16's
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) {
                float v = 0;
                for (int k = 0; k < 3; k++) v += root.m[i * 4 + k] * left[k * 3 + j];
                hand.m[i * 4 + j] = v;
            }
        {
            // B = F R Q F (G: x and z flipped from C)
            float R[9];
            GunAxes(fr, R);
            const float F[3] = {-1, 1, -1};
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) {
                    float v = 0;
                    for (int k = 0; k < 3; k++) v += R[i * 3 + k] * in_gun[k * 3 + j];
                    hand.m[i * 4 + j] = F[i] * v * F[j];
                }
        }
        // the palm at the fore-grip (C before K, x and z flipped into G)
        float want[3], at[3];
        for (int i = 0; i < 3; i++) want[i] = fr.grip[i] + fr.fwd[i] * fr.fore;
        want[0] = -want[0], want[2] = -want[2];
        Apply(hand, rig[0].palm, at);
        for (int i = 0; i < 3; i++) hand.m[i * 4 + 3] = want[i] - at[i];
        Aff delta = Compose(hand, Inverse(old_hand));
        for (int k = rig[0].hand; k <= rig[0].finger_hi; k += 3) SetBone(c, k, Compose(delta, Bone(c, k)));
    }
    // the walther or a shotgun cycling: its arms as they last were on the gun (bones 9 on, relative to the gun's root)
    int worked_root, worked_slide;
    const bool magnum = code == "MGN" || code == "MG5";
    if (magnum) {
        // the magnum (user: a game animation took over at times, the off hand spinning the cylinder, the gun brought
        // up): its arms as they were once it settled (the gun draw's root held), relative to the gun hand, always
        static Aff rel[44];
        bool settled;
        {
            std::lock_guard lock(g_root_lock);
            settled = g_root_rest.ok && g_root_rest.code == code;
        }
        if (settled) {
            static bool have = false;
            static std::string had;
            if (had != code) have = false, had = code;
            if (!have) {
                Aff inv = Inverse(Bone(c, rig[1].hand));
                for (int k = 3; k < 44; k++) rel[k] = Compose(inv, Bone(c, 3 * k));
                have = true;
                LOGR("vr arms: %s arms held as settled", code.c_str());
            }
            Aff h = Bone(c, rig[1].hand);
            for (int k = 3; k < 44; k++) SetBone(c, 3 * k, Compose(h, rel[k]));
        }
    } else if (WorkedGun(code, worked_root, worked_slide)) {
        static Aff rel[44];
        static bool have = false;
        static std::string for_code;
        if (for_code != code) have = false, for_code = code;
        Aff root = Bone(c, worked_root), hold = root;
        bool held = false;
        {
            std::lock_guard lock(g_root_lock);
            if (g_root_rest.ok && g_root_rest.code == code) hold = g_root_rest.root, held = true;
        }
        // the game's arms move with its rock: theirs kept from when the gun was at rest (its root where it's held),
        // put on the held root
        float off = 0;
        for (int i = 0; i < 12; i++) off = std::max(off, std::fabs(root.m[i] - hold.m[i]));
        if (!Cycling(c, worked_root, worked_slide) && (!held || off < 1e-3f)) {
            Aff inv = Inverse(root);
            for (int k = 3; k < 44; k++) rel[k] = Compose(inv, Bone(c, 3 * k));
            have = true;
        } else if (have) {
            // the free off hand keeps its own rest (user 2026-11-26: a shell taken, the hand turned 90°: the
            // snapshot held it as it was on the fore-end)
            for (int k = 3; k < 44; k++)
                if (!free_left || 3 * k < rig[0].hand || 3 * k > rig[0].finger_hi) SetBone(c, 3 * k, Compose(hold, rel[k]));
        }
    }
    // the fingers, from the controllers, before anything moves the hands
    auto found = g_fingers.find({code, index_count});
    if (found == g_fingers.end() && vb && indices) {
        std::array<HandFingers, 2> hf;
        FindFingers(code, vb, stride, indices, index16, index_count, base_vertex, hf);
        found = g_fingers.emplace(std::make_pair(code, index_count), hf).first;
    }
    {
        xr::Hand in[2];
        float head_unused[3];
        if (found != g_fingers.end() && xr::Hands(in[0], in[1], head_unused)) {
            // dev_curl in the app folder: "grip trigger thumb" (0..1) for both hands, to see the fingers without
            // the controllers
            static float fake[3] = {-1, -1, -1};
            static bool read = false;
            if (!read) {
                read = true;
                if (FILE* fc = std::fopen((app::ExeDir() / "dev_curl").string().c_str(), "r")) {
                    if (std::fscanf(fc, "%f %f %f", &fake[0], &fake[1], &fake[2]) != 3) fake[0] = -1;
                    std::fclose(fc);
                }
            }
            static float eased[2][5] = {};
            static auto last = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
            last = now;
            float k = 1 - std::exp(-dt * 18);  // about 55 ms to close
            for (int h = 0; h < 2; h++) {
                if (!found->second[h].ok) continue;
                float g = in[h].grip, t = in[h].trigger, th = in[h].thumb_touch ? 1.0f : 0.0f;
                if (fake[0] >= 0) g = fake[0], t = fake[1], th = fake[2];
                float idx = std::max(t, in[h].trigger_touch ? 0.35f : 0.0f);
                // a hand gripping the gun closes every finger on it, whatever else is pressed (the gun hand by its
                // toggle, the off hand while it holds); otherwise the fingers follow the controller
                // a shell is pinched between the thumb and index finger (user), the others loosely curled
                // (a launcher's round: wrapped round, user)
                bool pinch = h == 0 && VrMagInHand() && VrShellGun() && VrLauncherRoot() < 0;
                bool held = h == 1 ? !g_gun_fingers_open.load(std::memory_order_relaxed)
                                   : g_left_on.load(std::memory_order_relaxed) || (VrMagInHand() && !pinch) ||
                                         VrRackHeld();
                if (fake[0] >= 0) held = false;
                float want[5] = {th, idx, g, g, g};
                if (held) std::fill(want, want + 5, 1.0f);
                if (pinch) {
                    const float p[5] = {0.85f, 0.7f, 0.35f, 0.4f, 0.45f};
                    std::copy(p, p + 5, want);
                }
                if ((h == 0 && VrMagInHand() && VrShellGun() && VrLauncherRoot() >= 0) || (h == 1 && VrGrenadeInHand())) {
                    // a launcher's round (a rocket's bar, a 40 mm grenade) or the hand grenade: the fingers wrapped
                    // round it (user), not closed through it
                    const float p[5] = {0.65f, 0.6f, 0.6f, 0.6f, 0.55f};
                    std::copy(p, p + 5, want);
                    held = false;
                }
                if (h == 0 && VrMagInHand() && VrRevolver()) {
                    // a speed loader: held between the thumb and the fingers (user), round it
                    const float p[5] = {0.7f, 0.55f, 0.6f, 0.6f, 0.55f};
                    std::copy(p, p + 5, want);
                    held = false;
                }
                for (int i = 0; i < 5; i++) eased[h][i] += (want[i] - eased[h][i]) * k;
                // the guns whose game left hand is borrowed (uzi, walther): the curl's direction from the mesh
                const bool borrowed = h == 0 && (code == "SMG" || code == "SM5" || code == "SNR" || code == "SN5");
                CurlFingers(c, rig[h], found->second[h], eased[h], held || h == 1, borrowed, code);
            }
        }
    }
    float krs[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) krs[i * 3 + j] = kr[i * 3 + j] * (j == 1 ? 1.0f : -1.0f);  // K · S, S = (-1, 1, -1)
    Aff D = FromRotPos(krs, kp), Dinv = Inverse(D);
    // the game's palms on the gun (C before K: S·B·palm) for the next frames' rests, eased; held through a melee
    // swing, when the game's hands leave the gun
    if (!VrMeleeHidden()) {
        float rest[2][3];
        for (int h = 0; h < 2; h++) {
            Apply(Bone(c, rig[h].hand), rig[h].palm, rest[h]);
            rest[h][0] = -rest[h][0], rest[h][2] = -rest[h][2];
        }
        std::lock_guard lock(g_lock);
        bool fresh = !g_rest_ok || g_rest_code != g_code;
        for (int h = 0; h < 2; h++)
            for (int i = 0; i < 3; i++) g_rest[h][i] = fresh ? rest[h][i] : g_rest[h][i] + (rest[h][i] - g_rest[h][i]) * 0.2f;
        if (fresh) {
            Frame f;
            std::memcpy(&f, frame, sizeof(f));
            LOGR("vr arms: %s palms right %.3f %.3f %.3f left %.3f %.3f %.3f; kGrips handle %.3f %.3f %.3f",
                 g_code.c_str(), rest[1][0], rest[1][1], rest[1][2], rest[0][0], rest[0][1], rest[0][2], f.grip[0],
                 f.grip[1], f.grip[2]);
        }
        g_rest_ok = true;
        g_rest_code = g_code;
    }
    // the shoulders: from the head's middle, turned with the head's yaw (local space), into C
    xr::Hand hand[2];
    float head[3], hy, hpitch;
    if (!xr::Hands(hand[0], hand[1], head) || !xr::HeadAngles(hy, hpitch)) return false;
    float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    float U[9] = {cy, sy * sp, sy * cp, 0, cp, -sp, -sy, cy * sp, cy * cp};
    float right[3] = {std::cos(hy), 0, -std::sin(hy)}, back[3] = {std::sin(hy), 0, std::cos(hy)};
    // the off hand: on the gun, as the game holds it; off it, its own controller (that hand's K over the gun's)
    Aff left_delta = kIdentityAff;
    if (!g_left_on.load(std::memory_order_relaxed)) {
        float lr[9], lp[3];
        if (VrGunPose(yaw, pitch, frame, 0, lr, lp)) left_delta = Compose(FromRotPos(lr, lp), Inverse(FromRotPos(kr, kp)));
    } else if (float pb = PumpBack(); pb != 0) {
        // on a shotgun's fore-end: back with the pump (a launcher's tube: forward with it)
        Frame f;
        std::memcpy(&f, frame, sizeof(f));
        for (int i = 0; i < 3; i++) left_delta.m[i * 4 + 3] = -f.fwd[i] * pb;
    }
    for (int h = 0; h < 2; h++) {
        const ArmRig& a = rig[h];
        Aff hand_delta = h == 0 ? left_delta : kIdentityAff;
        // the game's pose as drawn: shoulder (the elbow back up the bind arm), elbow, wrist
        float sh_m[3] = {a.elbow[0], a.elbow[1], a.elbow[2] - kUpperArm};
        Aff up = Compose(D, Bone(c, a.upper)), hd = Compose(D, Bone(c, a.hand));
        float s0[3], e0[3], w0[3], w1[3];
        Apply(up, sh_m, s0);
        Apply(up, a.elbow, e0);
        Apply(hd, a.wrist, w0);
        Apply(hand_delta, w0, w1);
        float local[3], s1[3];
        for (int i = 0; i < 3; i++)
            local[i] = head[i] + right[i] * a.side * kShoulderOut + back[i] * kShoulderBack - (i == 1 ? kShoulderDown : 0);
        local[1] += xr::kEyeRaise + VrViewLift();
        for (int i = 0; i < 3; i++) s1[i] = U[i] * local[0] + U[3 + i] * local[1] + U[6 + i] * local[2];  // Uᵀ
        // two bones; out of reach, the shoulder comes to the hand
        float L1 = kUpperArm, L2 = Dist(a.elbow, a.wrist);
        float d[3] = {w1[0] - s1[0], w1[1] - s1[1], w1[2] - s1[2]};
        float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (dist < 1e-4f) continue;
        for (int i = 0; i < 3; i++) d[i] /= dist;
        float reach = L1 + L2 - 0.002f;
        if (dist > reach) {
            for (int i = 0; i < 3; i++) s1[i] = w1[i] - d[i] * reach;
            dist = reach;
        }
        dist = std::max(dist, std::fabs(L1 - L2) + 0.002f);
        float along = (L1 * L1 - L2 * L2 + dist * dist) / (2 * dist);
        float out = std::sqrt(std::max(L1 * L1 - along * along, 0.0f));
        // the elbow's way: toward where the forearm would come from with the wrist straight (the hand's own forearm
        // line, back from the wrist), with a little of down and out, a little back (C: x right, y up, z back) to
        // steady it. The fixed way alone (user 2026-11-27): with the hand at the chest the shoulder-to-wrist line
        // ran nearly along it, what was left of it swung round and the elbow rolled over
        Aff hand_now = Compose(hand_delta, hd);
        float he[3], hw[3];
        Apply(hand_now, a.elbow, he);  // the drawn elbow carried with the hand: the straight wrist's
        Apply(hand_now, a.wrist, hw);
        float fixed[3] = {a.side * 0.6f, -1, 0.3f}, hint[3];
        Normalize(fixed);
        {
            float fl[3] = {he[0] - hw[0], he[1] - hw[1], he[2] - hw[2]};
            Normalize(fl);
            for (int i = 0; i < 3; i++) hint[i] = w1[i] + fl[i] * L2 - s1[i];  // the straight wrist's elbow, from s1
        }
        float pole[3];
        for (int i = 0; i < 3; i++) pole[i] = hint[i] + fixed[i] * 0.15f;
        float pd = pole[0] * d[0] + pole[1] * d[1] + pole[2] * d[2];
        for (int i = 0; i < 3; i++) pole[i] -= d[i] * pd;
        if (pole[0] * pole[0] + pole[1] * pole[1] + pole[2] * pole[2] < 1e-6f) {
            float fd = fixed[0] * d[0] + fixed[1] * d[1] + fixed[2] * d[2];
            for (int i = 0; i < 3; i++) pole[i] = fixed[i] - d[i] * fd;
        }
        Normalize(pole);
        float e1[3];
        for (int i = 0; i < 3; i++) e1[i] = s1[i] + d[i] * along + pole[i] * out;
        {
            // dev_elbowlog: the left elbow's way from the shoulder-to-wrist line and the wrist from the shoulder
            // (C: x right, y up, z back), 4 a second: a roll-over shows as the way swinging round
            static const bool elog = std::filesystem::exists(app::ExeDir() / "dev_elbowlog");
            static int64_t next = 0;
            if (elog && h == 0 && SteadyMs() >= next) {
                next = SteadyMs() + 250;
                LOGR("dev_elbowlog: elbow way %.2f %.2f %.2f, wrist from the shoulder %.2f %.2f %.2f", pole[0], pole[1],
                     pole[2], w1[0] - s1[0], w1[1] - s1[1], w1[2] - s1[2]);
            }
        }
        // upper arm: turned from its drawn line onto shoulder → elbow, moved to the shoulder
        float u0[3] = {e0[0] - s0[0], e0[1] - s0[1], e0[2] - s0[2]}, u1[3] = {e1[0] - s1[0], e1[1] - s1[1], e1[2] - s1[2]};
        Normalize(u0);
        Normalize(u1);
        float R[9];
        RotateOnto(u0, u1, R);
        SetBone(c, a.upper, Compose(Dinv, Compose(Pivot(R, s0, s1), up)));
        // forearm bones: each from its own drawn elbow → wrist onto elbow → wrist, then twisted about that line by a
        // share of how much more the hand rolled about the forearm than in the game's pose
        Aff hand_new = Compose(hand_delta, hd);
        for (int k = 0; k < 2; k++) {
            int fb = a.fore[k];
            if (fb < 0) continue;
            Aff fa = Compose(D, Bone(c, fb));
            float fe[3], fw[3];
            Apply(fa, a.elbow, fe);
            Apply(fa, a.wrist, fw);
            float f0[3] = {fw[0] - fe[0], fw[1] - fe[1], fw[2] - fe[2]}, f1[3] = {w1[0] - e1[0], w1[1] - e1[1], w1[2] - e1[2]};
            Normalize(f0);
            Normalize(f1);
            RotateOnto(f0, f1, R);
            float roll = HandRoll(fa, hand_new, R, f1) - HandRoll(fa, hd, nullptr, f0);
            roll = std::remainder(roll, 2 * 3.14159265f);
            float T[9], RT[9];
            AxisAngle(f1, roll * a.twist[k], T);
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) RT[i * 3 + j] = T[i * 3] * R[j] + T[i * 3 + 1] * R[3 + j] + T[i * 3 + 2] * R[6 + j];
            SetBone(c, fb, Compose(Dinv, Compose(Pivot(RT, fe, e1), fa)));
        }
        // the hand and its fingers, moved together
        Aff hand_b = Compose(Dinv, hand_delta);
        SetBone(c, a.hand, Compose(hand_b, hd));
        for (int fb = a.finger_lo; fb <= a.finger_hi; fb += 3) SetBone(c, fb, Compose(hand_b, Compose(D, Bone(c, fb))));
    }
    // dev_fakehands: the free off hand's thumb and fingers in its controller's axes (x right, y its top, z back),
    // the wrist to the thumb's last joint and to the middle finger's knuckle: to set its roll by
    static const bool dev_hands = std::filesystem::exists(app::ExeDir() / "dev_fakehands");
    static int64_t told_ms = 0;
    if (dev_hands && !g_left_on.load(std::memory_order_relaxed) && found != g_fingers.end() && found->second[0].ok &&
        SteadyMs() - told_ms > 3000) {
        told_ms = SteadyMs();
        float AC[9], hc[3];
        if (HandInCamera(yaw, pitch, 0, AC, hc, false)) {
            const HandFingers& hf = found->second[0];
            const Finger &th = hf.f[0], &mid = hf.f[2];
            float w[3], t[3], m[3];
            Apply(Compose(D, Bone(c, rig[0].hand)), rig[0].wrist, w);
            Apply(Compose(D, Bone(c, th.bone[th.n - 1])), th.pivot[th.n - 1], t);
            Apply(Compose(D, Bone(c, mid.bone[0])), mid.pivot[0], m);
            float tc[3], mc[3];
            for (int j = 0; j < 3; j++) {
                tc[j] = mc[j] = 0;
                for (int i = 0; i < 3; i++) tc[j] += AC[i * 3 + j] * (t[i] - w[i]), mc[j] += AC[i * 3 + j] * (m[i] - w[i]);
            }
            Normalize(tc);
            Normalize(mc);
            float tv[3] = {t[0] - w[0], t[1] - w[1], t[2] - w[2]};  // in the view (x right, y up, z back)
            Normalize(tv);
            LOGR("vr arms: free left in its controller: thumb %.2f %.2f %.2f, fingers %.2f %.2f %.2f; thumb in the "
                 "view %.2f %.2f %.2f", tc[0], tc[1], tc[2], mc[0], mc[1], mc[2], tv[0], tv[1], tv[2]);
        }
    }
    return true;
}

// the magazine's middle and its up (the way it goes into the well) in its mesh, measured once per mesh from the
// vertices drawn: those weighted most to its bone. up: from the middle to the middle of its top tenth
struct MagShape {
    bool ok = false;
    float mid[3], up[3];
    float top = 0.05f, bottom = -0.05f;  // its farthest vertices from the middle along up (the seating test uses its top)
};
std::map<uint64_t, MagShape> g_mag_shapes;  // by index count and bone (render thread)

const MagShape& MeasureMag(int bone, const uint8_t* vb, uint32_t stride, uint32_t vbytes, const void* indices,
                           bool index16, uint32_t count, int32_t base) {
    uint64_t key = uint64_t(count) << 16 | uint32_t(bone);
    auto it = g_mag_shapes.find(key);
    if (it != g_mag_shapes.end()) return it->second;
    MagShape m;
    std::vector<std::array<float, 3>> pts;
    std::set<int64_t> seen;
    for (uint32_t i = 0; vb && indices && i < count; i++) {
        int64_t v = int64_t(index16 ? static_cast<const uint16_t*>(indices)[i] : static_cast<const uint32_t*>(indices)[i]) + base;
        if (v < 0 || (uint64_t(v) + 1) * stride > vbytes || !seen.insert(v).second) continue;
        const uint8_t* p = vb + size_t(v) * stride;
        const uint8_t* w = p + 12;
        int16_t ix[4];
        std::memcpy(ix, p + 16, 8);
        int top = 0;
        for (int k = 1; k < 4; k++)
            if (w[k] > w[top]) top = k;
        if (ix[top] != bone || w[top] < 128) continue;
        std::array<float, 3> q;
        std::memcpy(q.data(), p, 12);
        pts.push_back(q);
    }
    if (pts.size() >= 8) {
        for (int j = 0; j < 3; j++) {
            double s = 0;
            for (auto& q : pts) s += q[j];
            m.mid[j] = float(s / pts.size());
        }
        // its top: the tenth of its points highest in the mesh's y (a magazine goes up into its well)
        std::vector<std::array<float, 3>> by = pts;
        std::sort(by.begin(), by.end(), [](auto& a, auto& b) { return a[1] > b[1]; });
        size_t n = std::max<size_t>(1, by.size() / 10);
        float topm[3] = {};
        for (size_t i = 0; i < n; i++)
            for (int j = 0; j < 3; j++) topm[j] += by[i][j] / float(n);
        for (int j = 0; j < 3; j++) m.up[j] = topm[j] - m.mid[j];
        Normalize(m.up);
        m.top = -1, m.bottom = 1;
        for (auto& q : pts) {
            float d = (q[0] - m.mid[0]) * m.up[0] + (q[1] - m.mid[1]) * m.up[1] + (q[2] - m.mid[2]) * m.up[2];
            m.top = std::max(m.top, d), m.bottom = std::min(m.bottom, d);
        }
        m.ok = true;
        LOGR("vr reload: magazine bone %d of mesh %u: %zu vertices, middle %.3f %.3f %.3f, up %.2f %.2f %.2f, top %.3f "
             "bottom %.3f", bone, count, pts.size(), m.mid[0], m.mid[1], m.mid[2], m.up[0], m.up[1], m.up[2], m.top,
             m.bottom);
    } else {
        LOGR("vr reload: magazine bone %d of mesh %u: only %zu vertices; the table's middle used", bone, count,
             pts.size());
    }
    return g_mag_shapes.emplace(key, m).first->second;
}

float g_mag_mid_m[3] = {0, -0.03f, 0.12f};  // the held gun's magazine's middle in its mesh (render thread)
float g_mag_up_m[3] = {0, 1, 0};            // and its up

// the camera in the game world (game thread writes each gun frame): rows left, up, at, eye; the floor under the
// player (eye height over its position: vr_body.cpp's kStance)
constexpr uint32_t kStance = 0x2E0;
struct CamSnap {
    float rows[4][3];
    float feet;
    bool ok;
};
std::mutex g_cam_lock;
CamSnap g_cam{};
// C → world as an affine (the rows: C's x is -left, y up, z -at), and back
Aff CamToWorld(const CamSnap& s) {
    Aff a{};
    for (int i = 0; i < 3; i++) {
        a.m[i * 4 + 0] = -s.rows[0][i];
        a.m[i * 4 + 1] = s.rows[1][i];
        a.m[i * 4 + 2] = -s.rows[2][i];
        a.m[i * 4 + 3] = s.rows[3][i];
    }
    return a;
}
// a basis's frame: columns x, up, front, and its translation
Aff Frame3(const float* x, const float* up, const float* front, const float* t) {
    Aff a{};
    for (int i = 0; i < 3; i++) {
        a.m[i * 4 + 0] = x[i], a.m[i * 4 + 1] = up[i], a.m[i * 4 + 2] = front[i], a.m[i * 4 + 3] = t[i];
    }
    return a;
}
// a magazine's own frame in its mesh: its middle, its up, its front (the mesh's +z, toward the muzzle, square to up)
Aff MagFrame(const float* mid, const float* up_in) {
    float up[3] = {up_in[0], up_in[1], up_in[2]}, f[3] = {0, 0, 1}, x[3];
    Normalize(up);
    // a part that goes in along the barrel (a shell, a rocket, a grenade): its front is the mesh's top instead (the
    // barrel's way would leave no front square to it, and the frame flat)
    if (std::fabs(up[2]) > 0.9f) f[1] = 1, f[2] = 0;
    float d = f[2] * up[2] + f[1] * up[1] + f[0] * up[0];
    for (int i = 0; i < 3; i++) f[i] -= up[i] * d;
    Normalize(f);
    Cross(up, f, x);
    return Frame3(x, up, f, mid);
}

constexpr float kRockIn = 15 * 3.14159265f / 180;  // a magazine going in, at its farthest still lined up

// the off hand's magazine in C: its middle, up (the fist's axis, out past the thumb) and front (forward, square to
// it). The same as vr_reload.cpp's HeldMagOf, from HandPose (so dev_fakehands holds one too)
bool VrHeldMag(float yaw, float pitch, float* mid, float* up, float* front) {
    float A[9], hp[3];
    if (!HandPose(0, A, hp)) return false;
    float fl[3] = {-A[2], -A[5], -A[8]}, ul[3] = {A[1], A[4], A[7]};
    float c = std::cos(VrRodTilt()), s = std::sin(VrRodTilt()), rl[3], frl[3];
    for (int i = 0; i < 3; i++) rl[i] = c * fl[i] + s * ul[i], frl[i] = s * fl[i] - c * ul[i];
    float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    float U[9] = {cy, sy * sp, sy * cp, 0, cp, -sp, -sy, cy * sp, cy * cp};
    hp[1] += xr::kEyeRaise + VrViewLift();
    float hold = VrMagHold();
    for (int i = 0; i < 3; i++) {
        up[i] = U[i] * rl[0] + U[3 + i] * rl[1] + U[6 + i] * rl[2];
        front[i] = U[i] * frl[0] + U[3 + i] * frl[1] + U[6 + i] * frl[2];
        mid[i] = U[i] * hp[0] + U[3 + i] * hp[1] + U[6 + i] * hp[2];
    }
    for (int i = 0; i < 3; i++) mid[i] += up[i] * hold;
    return true;
}

// the magazine in the off hand, mesh → C: upright in the fist (its up along the fist's axis, its top out past the
// thumb, its front forward), fingers round it (vr_reload.cpp HeldMagOf)
Aff HandMag(float yaw, float pitch, const Aff& rest) {
    float mid[3], up[3], front[3], x[3];
    if (!VrHeldMag(yaw, pitch, mid, up, front)) return rest;
    Cross(up, front, x);
    return Compose(Frame3(x, up, front, mid), Inverse(MagFrame(g_mag_mid_m, g_mag_up_m)));
}

// a magazine out of the gun (dropped, knocked, thrown): it falls in the world (so it stays where it fell as the
// player moves), turning to lie flat as it lands on the floor under the player, and stays a minute
struct Drop {
    bool on = false;
    Aff w0;           // mesh → world as it left
    float v[3];       // world, m/s
    float land;       // seconds to the floor
    float axis[3];    // world: the turn that lays it flat
    float angle;
    float mid_w0[3];  // its middle as it left (world)
};
Drop g_drop;  // render thread
constexpr float kDropStay = 60;

// game thread: a hand's grip point and its velocity in the game's world (metres, m/s), for a throw (vr_grenade.cpp)
bool VrHandWorld(int hand, float* pos, float* vel) {
    float yaw, pitch, AC[9], hc[3], head[3];
    xr::Hand h[2];
    CamSnap cam;
    {
        std::lock_guard lock(g_cam_lock);
        cam = g_cam;
    }
    if (!cam.ok) return false;
    if (!VrLookAngles(yaw, pitch) || !xr::Hands(h[0], h[1], head) || !h[hand].ok ||
        !HandInCamera(yaw, pitch, hand, AC, hc, false)) {
        // dev_throw with the controllers asleep on the desk: from half a metre ahead of the eyes, 8 m/s on and up
        if (!std::filesystem::exists(app::ExeDir() / "dev_throw")) return false;
        // (its content a speed, m/s, to try harder throws: 8 when empty)
        static const float speed = [] {
            std::ifstream f(app::ExeDir() / "dev_throw");
            float s = 0;
            f >> s;
            return s > 0 ? s : 8.0f;
        }();
        const float at[3] = {0, 0, -0.5f}, v[3] = {0, speed * 0.25f, -speed};
        Aff c2w = CamToWorld(cam);
        Apply(c2w, at, pos);
        RotOf(c2w, v, vel);
        return true;
    }
    // the velocity into C as the hand's place is (the head's yaw and pitch the camera was built from)
    float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    float U[9] = {cy, sy * sp, sy * cp, 0, cp, -sp, -sy, cy * sp, cy * cp}, vc[3];
    for (int i = 0; i < 3; i++) vc[i] = U[i] * h[hand].vel[0] + U[3 + i] * h[hand].vel[1] + U[6 + i] * h[hand].vel[2];
    Aff c2w = CamToWorld(cam);
    Apply(c2w, hc, pos);
    RotOf(c2w, vc, vel);
    return true;
}

// a point of the game's world in local space (the inverse of VrHandWorld's way: world -> C -> local), for things
// drawn in the room that sit in the world (the silencer on the ground); any thread
bool VrWorldToLocal(const float* w, float* local) {
    float yaw, pitch;
    CamSnap cam;
    {
        std::lock_guard lock(g_cam_lock);
        cam = g_cam;
    }
    if (!cam.ok || !VrLookAngles(yaw, pitch)) return false;
    Aff c2w = CamToWorld(cam);
    float d[3] = {w[0] - c2w.m[3], w[1] - c2w.m[7], w[2] - c2w.m[11]}, c[3];
    for (int j = 0; j < 3; j++) c[j] = c2w.m[0 * 4 + j] * d[0] + c2w.m[1 * 4 + j] * d[1] + c2w.m[2 * 4 + j] * d[2];
    float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    float U[9] = {cy, sy * sp, sy * cp, 0, cp, -sp, -sy, cy * sp, cy * cp};
    for (int i = 0; i < 3; i++) local[i] = U[i * 3] * c[0] + U[i * 3 + 1] * c[1] + U[i * 3 + 2] * c[2];
    local[1] -= xr::kEyeRaise + VrViewLift();
    return true;
}

// a point in local space in the game's world (VrWorldToLocal backwards: local -> C -> world); any thread
bool VrLocalToWorld(const float* local, float* w) {
    float yaw, pitch;
    CamSnap cam;
    {
        std::lock_guard lock(g_cam_lock);
        cam = g_cam;
    }
    if (!cam.ok || !VrLookAngles(yaw, pitch)) return false;
    Aff c2w = CamToWorld(cam);
    float l[3] = {local[0], local[1] + xr::kEyeRaise + VrViewLift(), local[2]}, c[3];
    float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    float U[9] = {cy, sy * sp, sy * cp, 0, cp, -sp, -sy, cy * sp, cy * cp};
    for (int j = 0; j < 3; j++) c[j] = U[0 * 3 + j] * l[0] + U[1 * 3 + j] * l[1] + U[2 * 3 + j] * l[2];
    for (int i = 0; i < 3; i++)
        w[i] = c2w.m[i * 4 + 0] * c[0] + c2w.m[i * 4 + 1] * c[1] + c2w.m[i * 4 + 2] * c[2];
    w[0] += c2w.m[3], w[1] += c2w.m[7], w[2] += c2w.m[11];
    return true;
}

// the player in the game's world: the eye, its forward, the floor under it; any thread
bool VrPlayerWorld(float* eye, float* fwd, float* floor_y) {
    std::lock_guard lock(g_cam_lock);
    if (!g_cam.ok) return false;
    std::memcpy(eye, g_cam.rows[3], 12);
    std::memcpy(fwd, g_cam.rows[2], 12);
    *floor_y = g_cam.feet;
    return true;
}

void StartDrop(const Aff& from_c, const float* v_c) {
    CamSnap cam;
    {
        std::lock_guard lock(g_cam_lock);
        cam = g_cam;
    }
    if (!cam.ok) {
        g_drop.on = false;
        LOGR("vr reload: magazine dropped, no camera yet");
        return;
    }
    Aff c2w = CamToWorld(cam);
    Drop d;
    d.on = true;
    d.w0 = Compose(c2w, from_c);
    float vr[3];
    RotOf(c2w, v_c, vr);
    std::memcpy(d.v, vr, 12);
    Apply(d.w0, g_mag_mid_m, d.mid_w0);
    float up[3];
    RotOf(d.w0, g_mag_up_m, up);
    Normalize(up);
    // y(t) = y0 + vy t - 4.9 t^2 = the floor (plus its half thickness)
    float h = d.mid_w0[1] - (cam.feet + 0.015f);
    d.land = h > 0 ? (d.v[1] + std::sqrt(d.v[1] * d.v[1] + 19.6f * h)) / 9.8f : 0.0f;
    float Y[3] = {0, 1, 0};
    Cross(up, Y, d.axis);
    float al = std::sqrt(d.axis[0] * d.axis[0] + d.axis[1] * d.axis[1] + d.axis[2] * d.axis[2]);
    if (al > 1e-4f) {
        for (float& x : d.axis) x /= al;
        d.angle = -std::asin(std::clamp(up[1], -1.0f, 1.0f));
    } else {
        d.axis[0] = 1, d.axis[1] = 0, d.axis[2] = 0;
        d.angle = 1.5707963f;
    }
    g_drop = d;
    LOGR("vr reload: magazine dropped from %.2f %.2f %.2f (world), feet %.2f, lands in %.2f s", d.mid_w0[0],
         d.mid_w0[1], d.mid_w0[2], cam.feet, d.land);
}

// mesh → C of the dropped magazine now; false: none to draw
bool DropNow(float t, Aff& out) {
    if (!g_drop.on || t > kDropStay) return false;
    CamSnap cam;
    {
        std::lock_guard lock(g_cam_lock);
        cam = g_cam;
    }
    if (!cam.ok) return false;
    float tf = std::min(t, g_drop.land), k = g_drop.land > 0 ? tf / g_drop.land : 1.0f;
    float R[9], to[3];
    AxisAngle(g_drop.axis, g_drop.angle * k, R);
    for (int i = 0; i < 3; i++) to[i] = g_drop.mid_w0[i] + g_drop.v[i] * tf - (i == 1 ? 4.9f * tf * tf : 0);
    Aff w = Compose(Pivot(R, g_drop.mid_w0, to), g_drop.w0);
    out = Compose(Inverse(CamToWorld(cam)), w);
    return true;
}

// render thread, a gun list draw (the gun's, and the arms', which draw the magazine too): the magazine bone
// (vr_reload.cpp) in the gun, in the off hand (sliding into the well once lined up with it), or out (falling or
// thrown, then gone); the slide held back. The well and its axis go back to the reload. vb..base: the draw's
// vertices and indices, to measure the magazine by
// the magazine's bone set to b, and the rounds on its top (a bone of their own: -1 none) with it
// (the AK's rounds sit 1.4-2.6 cm over its magazine's lips in the mesh, hidden by the receiver in the gun: out of
// it they floated over the top, user 2026-11-26; sunk 2 cm down the magazine's axis there)
float RoundsSink() { return HeldGunCode().rfind("AK", 0) == 0 ? 0.02f : 0.0f; }
void SetMag(float (*c)[4], int bone, int bullets, const Aff& b) {
    SetBone(c, bone, b);
    if (bullets < 0) return;
    float sink = RoundsSink();
    if (sink <= 0) {
        SetBone(c, bullets, b);
        return;
    }
    float up[3] = {g_mag_up_m[0], g_mag_up_m[1], g_mag_up_m[2]};
    Normalize(up);
    Aff down = kIdentityAff;
    for (int i = 0; i < 3; i++) down.m[i * 4 + 3] = -up[i] * sink;
    SetBone(c, bullets, Compose(b, down));
}

// the magazines' shapes measured in the held gun's draws, by bone (render thread)
std::map<int, MagShape> g_shapes;
// render thread: another gun in hand (its parts differ): guns share bone numbers (AK and WA2000 81, pistol and Uzi
// 114), so the last one's measured shapes go, and its dropped magazine (drawn from the held gun's mesh) with them
void NoteGun(int bone, int slide, float travel, const float* mid, const float* up, float top, float bottom,
             int bullets) {
    static float last[13] = {};
    float now[13] = {float(bone), float(slide), travel, mid[0], mid[1], mid[2], up[0], up[1], up[2], top, bottom,
                     float(bullets), 1};
    if (std::memcmp(now, last, sizeof now) == 0) return;
    std::memcpy(last, now, sizeof now);
    g_shapes.clear();
    g_drop.on = false;
}

// ---- stocks: the viewmodels of the M16 (and its M203 and silver ones) and the M249 end at the receiver, a first
// person gun never showing its stock; in vr it's in view, so one is added: the M16 a carbine's 6-position stock on
// its buffer tube, the M249 its fixed stock (vr_stocks.h, lofted by tools/stocks.py from the user's photos), real
// size in the gun's mesh, on its root bone, with a texture of its own painted from the gun's (VrStockBake). Drawn as a second draw of the
// gun's, both windings (the gun draw's culling either way; vulkan.cpp turns it into a strip when the gun's is one)
struct StockMesh {
    std::vector<uint8_t> verts;  // 36 bytes each, as the gun's: position, weights, bones, normal, uv
    std::vector<uint16_t> idx;
};
uint32_t PackNormal(const float* n) {
    auto q = [](float v, int bits) {
        int m = (1 << (bits - 1)) - 1;
        return uint32_t(int(std::lround(std::clamp(v, -1.0f, 1.0f) * m))) & ((1u << bits) - 1);
    };
    return q(n[0], 11) | q(n[1], 11) << 11 | q(n[2], 10) << 22;
}
StockMesh MakeStock(const float* pos, const float* nrm, const float* uvs, size_t nverts, const uint16_t* idx,
                    size_t nidx, int bone) {
    StockMesh m;
    m.verts.resize(nverts * 36);
    for (size_t i = 0; i < nverts; i++) {
        uint8_t* b = &m.verts[i * 36];
        std::memcpy(b, pos + i * 3, 12);
        b[12] = 255;
        int16_t ix[4] = {int16_t(bone), 0, 0, 0};
        std::memcpy(b + 16, ix, 8);
        uint32_t pn = PackNormal(nrm + i * 3);
        std::memcpy(b + 24, &pn, 4);
        std::memcpy(b + 28, uvs + i * 2, 8);
    }
    m.idx.assign(idx, idx + nidx);
    for (size_t i = 0; i + 2 < nidx; i += 3) m.idx.insert(m.idx.end(), {idx[i], idx[i + 2], idx[i + 1]});
    return m;
}
// as MakeStock, each triangle on its own bone (tools/stocks.py's atlas: three vertices a triangle, in order)
StockMesh MakeParts(const float* pos, const float* nrm, const float* uvs, size_t nverts, const uint16_t* idx,
                    size_t nidx, const uint8_t* tri_bone) {
    StockMesh m = MakeStock(pos, nrm, uvs, nverts, idx, nidx, 0);
    for (size_t i = 0; i < nverts; i++) {
        int16_t b = tri_bone[i / 3];
        std::memcpy(&m.verts[i * 36 + 16], &b, 2);
    }
    return m;
}
// the SPAS 12's shell: its own is a mesh of its own the game draws only in its reload (which no longer runs), so
// one of ours on its shell bone (114), the same size and place (z 0.129..0.177, round y -0.054, 7.5 mm): a brass
// base 13 mm long and the hull, textured where the game's shell is in the gun's texture (u 0.947..1.0 round it; the
// hull v -0.067 at its base to -0.225 at its tip, the brass v -0.001..-0.067)
StockMesh MakeSpasShell() {
    constexpr int kSides = 16;
    constexpr float kY = -0.054f, kR = 0.0075f, kRb = 0.0079f, kZ0 = 0.129f, kZb = 0.142f, kZ1 = 0.177f;
    std::vector<float> pos, nrm, uv;
    std::vector<uint16_t> idx;
    auto vert = [&](float x, float y, float z, float nx, float ny, float nz, float u, float v) {
        pos.insert(pos.end(), {x, y, z});
        nrm.insert(nrm.end(), {nx, ny, nz});
        uv.insert(uv.end(), {u, v});
        return uint16_t(pos.size() / 3 - 1);
    };
    // the sides: rings at the base, the brass's end (both radii), the tip
    const float zs[4] = {kZ0, kZb, kZb, kZ1}, rs[4] = {kRb, kRb, kR, kR}, vs[4] = {-0.001f, -0.067f, -0.067f, -0.225f};
    for (int seg = 0; seg < 3; seg += 2) {
        uint16_t base = uint16_t(pos.size() / 3);
        for (int r = 0; r < 2; r++)
            for (int k = 0; k <= kSides; k++) {
                float a = 2 * 3.14159265f * k / kSides, c = std::cos(a), sn = std::sin(a);
                float t = float(k) / kSides, u = 0.947f + 0.053f * (t < 0.5f ? 2 * t : 2 - 2 * t);
                int i = seg + r;
                vert(rs[i] * c, kY + rs[i] * sn, zs[i], c, sn, 0, u, vs[i]);
            }
        for (int k = 0; k < kSides; k++) {
            uint16_t a0 = uint16_t(base + k), a1 = uint16_t(a0 + 1), b0 = uint16_t(base + kSides + 1 + k),
                     b1 = uint16_t(b0 + 1);
            idx.insert(idx.end(), {a0, b0, a1, a1, b0, b1});
        }
    }
    // the brass's step, and the two ends
    auto disc = [&](float z, float r0, float r1, float nz, float u, float v) {
        uint16_t base = uint16_t(pos.size() / 3);
        for (int k = 0; k <= kSides; k++) {
            float a = 2 * 3.14159265f * k / kSides, c = std::cos(a), sn = std::sin(a);
            vert(r0 * c, kY + r0 * sn, z, 0, 0, nz, u, v);
            vert(r1 * c, kY + r1 * sn, z, 0, 0, nz, u, v);
        }
        for (int k = 0; k < kSides; k++) {
            uint16_t a0 = uint16_t(base + 2 * k), a1 = uint16_t(a0 + 1), b0 = uint16_t(a0 + 2), b1 = uint16_t(a0 + 3);
            idx.insert(idx.end(), {a0, b0, a1, a1, b0, b1});
        }
    };
    disc(kZb, kR, kRb, 1, 0.975f, -0.03f);
    disc(kZ0, 0, kRb, -1, 0.975f, -0.03f);
    disc(kZ1, 0, kR, 1, 0.975f, -0.225f);
    return MakeStock(pos.data(), nrm.data(), uv.data(), pos.size() / 3, idx.data(), idx.size(), 114);
}
// the M203's 40 mm grenade (the game has none: its GrenadeShell bone 96 has no mesh): an M433 HEDP as the user's
// photo has it, 40 mm across (its sides measured at 0.164 mm a pixel): the rim (dark green, 7.4 mm, chamfered),
// the case (olive, to 60 mm), a dark joint, the projectile's silver body (61..93 mm), the gold ogive (a dome 28 mm
// high) and the fuze's pin; 121.5 mm long. On its bone,
// seated in the barrel's back from z 0.200 round y 0.0055, its tip forward. Painted (PaintM433) in a strip of its own
// below the stock's atlas: u round it (half the atlas's width), v from its base up
constexpr float kM433Len = 0.1215f, kM433Z0 = 0.200f;
constexpr int kM203CaseBone = 126;  // the spent case's (the BNS's skeleton ends at TopLead 102)
std::atomic<int64_t> g_case_out_ms{0};  // when the m203's tube opened on a fired round
constexpr uint32_t kM433TexW = 512, kM433TexH = 512;  // texels: round it, along it
StockMesh MakeM203Grenade(float u_span, float v0, float v1, float z0 = kM433Z0, float kY = 0.0055f, int bone = 96,
                          bool case_only = false) {
    constexpr int kSides = 24;
    // the profile from the rim's back to the fuze's pin, millimetres: along it, radius
    std::vector<float> zs = {0, 0.8f, 6.6f, 7.4f, 7.4f, 60.0f, 60.0f, 61.0f, 61.0f, 93.0f, 93.0f};
    std::vector<float> rs = {20.3f, 20.8f, 20.8f, 20.2f, 20.0f, 20.0f, 19.2f, 19.2f, 19.6f, 19.6f, 19.4f};
    if (case_only) {
        // the fired round's case alone (user: its brass half drops out of the tube): to 60 mm, its mouth shut dark
        zs = {0, 0.8f, 6.6f, 7.4f, 7.4f, 60.0f, 60.0f};
        rs = {20.3f, 20.8f, 20.8f, 20.2f, 20.0f, 20.0f, 0};
    } else {
        // the ogive: a half ellipse 19.4 round, 28 up (its top a little flattened, as the photo's)
        for (int i = 1; i <= 10; i++) {
            float t = float(i) / 10 * 3.14159265f / 2;
            zs.push_back(93.0f + 28.0f * std::sin(t));
            rs.push_back(19.4f * std::cos(t) + (i == 10 ? 0.8f : 0.0f));
        }
        zs.insert(zs.end(), {121.0f, 121.5f, 121.5f});  // the fuze's pin
        rs.insert(rs.end(), {0.5f, 0.5f, 0});
    }
    const int rings = int(zs.size());
    std::vector<float> pos, nrm, uv;
    std::vector<uint16_t> idx;
    auto vat = [&](float z_mm) { return v0 + (v1 - v0) * z_mm / (kM433Len * 1000); };
    for (int r = 0; r < rings; r++)
        for (int k = 0; k <= kSides; k++) {
            float a = 2 * 3.14159265f * k / kSides, c = std::cos(a), sn = std::sin(a);
            // the side's normal from its neighbours' slope (a step: square to it)
            int r0 = std::max(r - 1, 0), r1 = std::min(r + 1, rings - 1);
            float dz = zs[r1] - zs[r0], dr = rs[r0] - rs[r1], l = std::max(std::hypot(dz, dr), 1e-6f);
            float nr = dz / l, nz = dr / l;
            pos.insert(pos.end(), {rs[r] / 1000 * c, kY + rs[r] / 1000 * sn, z0 + zs[r] / 1000});
            nrm.insert(nrm.end(), {nr * c, nr * sn, nz});
            uv.insert(uv.end(), {u_span * k / kSides, vat(zs[r])});
        }
    for (int r = 0; r + 1 < rings; r++)
        for (int k = 0; k < kSides; k++) {
            uint16_t a0 = uint16_t(r * (kSides + 1) + k), a1 = uint16_t(a0 + 1), b0 = uint16_t(a0 + kSides + 1),
                     b1 = uint16_t(b0 + 1);
            idx.insert(idx.end(), {a0, b0, a1, a1, b0, b1});
        }
    // the rim's back: a disc
    uint16_t mid = uint16_t(pos.size() / 3);
    pos.insert(pos.end(), {0, kY, z0});
    nrm.insert(nrm.end(), {0, 0, -1});
    uv.insert(uv.end(), {u_span * 0.5f, vat(0.3f)});
    for (int k = 0; k < kSides; k++) idx.insert(idx.end(), {mid, uint16_t(k + 1), uint16_t(k)});
    return MakeStock(pos.data(), nrm.data(), uv.data(), pos.size() / 3, idx.data(), idx.size(), bone);
}
// the letters painted on our models: 5 x 7, each row's 5 bits left to right (only the ones they carry)
const std::map<char, std::array<uint8_t, 7>>& Font5x7() {
    static const std::map<char, std::array<uint8_t, 7>> kFont = {
        {'A', {14, 17, 17, 31, 17, 17, 17}}, {'B', {30, 17, 17, 30, 17, 17, 30}}, {'C', {14, 17, 16, 16, 16, 17, 14}},
        {'D', {30, 17, 17, 17, 17, 17, 30}}, {'E', {31, 16, 16, 30, 16, 16, 31}}, {'F', {31, 16, 16, 30, 16, 16, 16}},
        {'G', {14, 17, 16, 23, 17, 17, 15}}, {'H', {17, 17, 17, 31, 17, 17, 17}}, {'I', {14, 4, 4, 4, 4, 4, 14}},
        {'J', {7, 2, 2, 2, 2, 18, 12}},      {'L', {16, 16, 16, 16, 16, 16, 31}}, {'M', {17, 27, 21, 21, 17, 17, 17}},
        {'O', {14, 17, 17, 17, 17, 17, 14}}, {'P', {30, 17, 17, 30, 16, 16, 16}}, {'R', {30, 17, 17, 30, 20, 18, 17}},
        {'S', {15, 16, 16, 14, 1, 1, 30}},   {'T', {31, 4, 4, 4, 4, 4, 4}},       {'U', {17, 17, 17, 17, 17, 17, 14}},
        {'W', {17, 17, 17, 21, 21, 21, 10}}, {'Z', {31, 1, 2, 4, 8, 16, 31}},     {'0', {14, 17, 19, 21, 25, 17, 14}},
        {'1', {4, 12, 4, 4, 4, 4, 14}},      {'2', {14, 17, 1, 2, 4, 8, 31}},     {'3', {31, 2, 4, 2, 1, 17, 14}},
        {'4', {2, 6, 10, 18, 31, 2, 2}},     {'5', {31, 16, 30, 1, 1, 17, 14}},   {',', {0, 0, 0, 0, 12, 4, 8}},
        {'-', {0, 0, 0, 31, 0, 0, 0}},       {' ', {0, 0, 0, 0, 0, 0, 0}},
        {'N', {17, 25, 21, 19, 17, 17, 17}}, {'Y', {17, 17, 10, 4, 4, 4, 4}}};
    return kFont;
}
// its paint, BGRA, into the rows from y0 (kM433TexH of them: row 0 the rim's back) of an image ow wide (the first
// kM433TexW columns round it): the colours of the photo's, and its printing (5 x 7 letters)
void PaintM433(uint8_t* out, uint32_t ow, uint32_t y0) {
    struct Rgb {
        float r, g, b;
    };
    const Rgb kRim{38, 44, 34}, kCase{58, 66, 50}, kSilver{196, 199, 203}, kGroove{120, 122, 126},
        kGold{196, 152, 44}, kInk{18, 18, 18}, kEmboss{40, 46, 35};
    const float px_mm = kM433TexH / (kM433Len * 1000);
    auto put = [&](int x, int y, Rgb c) {
        if (y < 0 || y >= int(kM433TexH)) return;
        x = ((x % int(kM433TexW)) + int(kM433TexW)) % int(kM433TexW);
        uint8_t* o = out + (size_t(y0 + uint32_t(y)) * ow + uint32_t(x)) * 4;
        o[0] = uint8_t(std::clamp(c.b, 0.0f, 255.0f));
        o[1] = uint8_t(std::clamp(c.g, 0.0f, 255.0f));
        o[2] = uint8_t(std::clamp(c.r, 0.0f, 255.0f));
        o[3] = 255;
    };
    for (int y = 0; y < int(kM433TexH); y++) {
        float z = (y + 0.5f) / px_mm;
        Rgb c = z < 7.4f ? kRim : z < 60 ? kCase : z < 61 ? kGroove : z < 93 ? kSilver : kGold;
        // a little of the metal's sheen and grain along it (the light does the rest)
        for (int x = 0; x < int(kM433TexW); x++) {
            float n = float((x * 7919 + y * 104729) % 97) / 97.0f - 0.5f;
            float g = z < 60 ? 3 : 6;
            put(x, y, {c.r + n * g, c.g + n * g, c.b + n * g});
        }
    }
    const auto& kFont = Font5x7();
    // a line centred round the front (u 0: the mesh's +x, the side the player sees), its letters' tops toward the
    // nose; dot: texels a dot, at mm up its baseline
    auto line = [&](const char* text, float mm, int dot, Rgb ink) {
        int n = int(std::strlen(text)), w = n * 6 * dot - dot;
        int x0 = -w / 2, ybase = int(mm * px_mm);
        for (int i = 0; i < n; i++) {
            auto g = kFont.find(text[i]);
            if (g == kFont.end()) continue;
            for (int row = 0; row < 7; row++)
                for (int col = 0; col < 5; col++)
                    if (g->second[row] >> (4 - col) & 1)
                        for (int dy = 0; dy < dot; dy++)
                            for (int dx = 0; dx < dot; dx++)
                                put(x0 + (i * 6 + col) * dot + dx, ybase + (6 - row) * dot + dy, ink);
        }
    };
    // (baselines from the photo's front, against the nearest band's edge there: the camera looks down on them)
    line("FUZE, PIBD, M550", 97.6f, 2, kInk);
    line("LOT 2213425", 93.9f, 2, kInk);
    line("PROJ  40MM", 73.3f, 3, kInk);
    line("M433  HEDP", 64.3f, 3, kInk);
    line("CTG40MM HEDP", 28.7f, 2, kEmboss);
    line("AMMLOT GA-WS-14-1", 24.1f, 2, kEmboss);
}
// the hand grenade (user's photos: an M67; its "INERT" left off, user): its body a ball 64 mm across, the fuze's body on top, the spoon
// down its side (mesh +x), the ring pin on the other side. Ours, drawn in the right hand while one's held (the gun's
// extra mesh on bone 3, "Extra", which the guns' own meshes don't use), with its own texture (PaintM67): the body
// round its left 384 columns, the metal parts' colours at u 0.875 (v: fuze 1/8, spoon 3/8, ring 5/8)
constexpr int kM67Bone = 3;
constexpr uint32_t kM67TexW = 512, kM67TexH = 256;
StockMesh MakeM67() {
    std::vector<float> pos, nrm, uv;
    std::vector<uint16_t> idx;
    auto vert = [&](float x, float y, float z, float nx, float ny, float nz, float u, float v) {
        pos.insert(pos.end(), {x / 1000, y / 1000, z / 1000});
        nrm.insert(nrm.end(), {nx, ny, nz});
        uv.insert(uv.end(), {u, v});
        return uint16_t(pos.size() / 3 - 1);
    };
    auto grid = [&](uint16_t first, int rows, int cols) {  // (rows + 1) x (cols + 1) vertices
        for (int r = 0; r < rows; r++)
            for (int k = 0; k < cols; k++) {
                uint16_t a0 = uint16_t(first + r * (cols + 1) + k), a1 = uint16_t(a0 + 1),
                         b0 = uint16_t(a0 + cols + 1), b1 = uint16_t(b0 + 1);
                idx.insert(idx.end(), {a0, b0, a1, a1, b0, b1});
            }
    };
    constexpr float kPi = 3.14159265f, kR = 32;
    constexpr float kBodyU = 384.0f / kM67TexW;
    // the body: a ball, open at the top under the fuze (latitudes -90..72 degrees)
    {
        constexpr int kLat = 18, kLon = 32;
        uint16_t first = uint16_t(pos.size() / 3);
        for (int i = 0; i <= kLat; i++) {
            float lat = (-90 + 162.0f * i / kLat) * kPi / 180;
            for (int k = 0; k <= kLon; k++) {
                float lon = 2 * kPi * k / kLon;
                float nx = std::cos(lat) * std::cos(lon), ny = std::sin(lat), nz = std::cos(lat) * std::sin(lon);
                vert(kR * nx, kR * ny, kR * nz, nx, ny, nz, kBodyU * k / kLon, float(i) / kLat);
            }
        }
        grid(first, kLat, kLon);
    }
    // the fuze's body: a cylinder r 10 from inside the ball to 50 mm up, and its top
    const float fu = 0.875f, fv = 1.0f / 8, su = 0.875f, sv = 3.0f / 8, ru = 0.875f, rv = 5.0f / 8;
    {
        constexpr int kSides = 16;
        uint16_t first = uint16_t(pos.size() / 3);
        for (float y : {28.0f, 50.0f})
            for (int k = 0; k <= kSides; k++) {
                float a = 2 * kPi * k / kSides;
                vert(10 * std::cos(a), y, 10 * std::sin(a), std::cos(a), 0, std::sin(a), fu, fv);
            }
        grid(first, 1, kSides);
        uint16_t mid = vert(0, 50, 0, 0, 1, 0, fu, fv);
        uint16_t ring = uint16_t(pos.size() / 3);
        for (int k = 0; k <= kSides; k++) {
            float a = 2 * kPi * k / kSides;
            vert(10 * std::cos(a), 50, 10 * std::sin(a), 0, 1, 0, fu, fv);
        }
        for (int k = 0; k < kSides; k++) idx.insert(idx.end(), {mid, uint16_t(ring + k + 1), uint16_t(ring + k)});
    }
    // the spoon: a strip 14 mm wide over the fuze's top, out and down the ball's side (+x) to 45 degrees under its
    // middle, 1.5 mm off the ball
    {
        std::vector<std::array<float, 2>> path = {{-6, 52}, {8, 52}, {14, 49}, {18, 44}};
        for (int i = 0; i <= 10; i++) {
            float t = (55 - 100.0f * i / 10) * kPi / 180;
            path.push_back({33.5f * std::cos(t), 33.5f * std::sin(t)});
        }
        uint16_t first = uint16_t(pos.size() / 3);
        for (size_t i = 0; i < path.size(); i++) {
            size_t i0 = i ? i - 1 : 0, i1 = std::min(i + 1, path.size() - 1);
            float tx = path[i1][0] - path[i0][0], ty = path[i1][1] - path[i0][1], l = std::max(std::hypot(tx, ty), 1e-6f);
            float nx = ty / l, ny = -tx / l;  // out from the ball
            if (nx * path[i][0] + ny * (path[i][1]) < 0) nx = -nx, ny = -ny;
            for (float z : {-7.0f, 7.0f}) vert(path[i][0], path[i][1], z, nx, ny, 0, su, sv);
        }
        grid(first, int(path.size()) - 1, 1);
    }
    // the ring pin: a ring 26 mm across of 3 mm wire on the fuze's other side, upright
    {
        constexpr int kMajor = 20, kMinor = 6;
        const float cx = -20, cy = 44, rm = 13, rw = 1.6f;
        uint16_t first = uint16_t(pos.size() / 3);
        for (int i = 0; i <= kMajor; i++) {
            float a = 2 * kPi * i / kMajor;
            for (int k = 0; k <= kMinor; k++) {
                float b = 2 * kPi * k / kMinor;
                float nx = std::cos(b) * std::cos(a), ny = std::cos(b) * std::sin(a), nz = std::sin(b);
                vert(cx + (rm + rw * std::cos(b)) * std::cos(a), cy + (rm + rw * std::cos(b)) * std::sin(a), rw * nz, nx,
                     ny, nz, ru, rv);
            }
        }
        grid(first, kMajor, kMinor);
    }
    return MakeStock(pos.data(), nrm.data(), uv.data(), pos.size() / 3, idx.data(), idx.size(), kM67Bone);
}
// its texture, BGRA (the photo's colours): the body olive with its yellow printing (no "INERT": user) (its lines centred on the
// side facing the player, mesh -z: away from the spoon), the metal parts' patches
void PaintM67(std::vector<uint8_t>& out) {
    out.assign(size_t(kM67TexW) * kM67TexH * 4, 255);
    auto put = [&](int x, int y, int r, int g, int b) {
        if (y < 0 || y >= int(kM67TexH)) return;
        x = ((x % 384) + 384) % 384;  // round the body
        uint8_t* o = &out[(size_t(y) * kM67TexW + uint32_t(x)) * 4];
        o[0] = uint8_t(std::clamp(b, 0, 255)), o[1] = uint8_t(std::clamp(g, 0, 255)), o[2] = uint8_t(std::clamp(r, 0, 255));
    };
    for (int y = 0; y < int(kM67TexH); y++)
        for (int x = 0; x < int(kM67TexW); x++) {
            int n = (x * 7919 + y * 104729) % 9 - 4;
            uint8_t* o = &out[(size_t(y) * kM67TexW + uint32_t(x)) * 4];
            int r = 62, g = 70, b = 50;  // olive drab
            if (x >= 384) {
                int part = y * 4 / int(kM67TexH);  // fuze, spoon, ring, (spare)
                r = part == 0 ? 78 : part == 1 ? 58 : 92, g = part == 0 ? 82 : part == 1 ? 62 : 92,
                b = part == 0 ? 70 : part == 1 ? 56 : 62;
            }
            o[0] = uint8_t(b + n), o[1] = uint8_t(g + n), o[2] = uint8_t(r + n);
        }
    const auto& font = Font5x7();
    // centred at longitude 270 degrees (mesh -z), its letters' tops toward the fuze; at latitude lat
    auto line = [&](const char* text, float lat, int dot, int r, int g, int b) {
        int n = int(std::strlen(text)), w = n * 6 * dot - dot;
        int xc = 384 * 3 / 4, ybase = int((lat + 90) / 162.0f * kM67TexH);
        for (int i = 0; i < n; i++) {
            auto f = font.find(text[i]);
            if (f == font.end()) continue;
            for (int row = 0; row < 7; row++)
                for (int col = 0; col < 5; col++)
                    if (f->second[row] >> (4 - col) & 1)
                        for (int dy = 0; dy < dot; dy++)
                            for (int dx = 0; dx < dot; dx++)
                                put(xc - w / 2 + (i * 6 + col) * dot + dx, ybase + (6 - row) * dot + dy, r, g, b);
        }
    };
    line("GRENADE,HAND,FRAG,DELAY", 6, 2, 205, 196, 70);
    line("COMB B IP-51-514D", -12, 2, 205, 196, 70);
}
// render thread: the M67 for the world's grenades (vulkan.cpp swaps their draw): its vertices as theirs (24 bytes:
// position, packed normal, uv: the guns' 36 less their bone words), its indices (both windings)
bool VrM67World(const uint8_t*& verts, uint32_t& bytes, const uint16_t*& idx, uint32_t& count) {
    static const StockMesh m = [] {
        StockMesh s = MakeM67(), w;
        size_t n = s.verts.size() / 36;
        w.verts.resize(n * 24);
        for (size_t i = 0; i < n; i++) {
            std::memcpy(&w.verts[i * 24], &s.verts[i * 36], 12);
            std::memcpy(&w.verts[i * 24 + 12], &s.verts[i * 36 + 24], 12);
        }
        w.idx = s.idx;
        return w;
    }();
    verts = m.verts.data(), bytes = uint32_t(m.verts.size());
    idx = m.idx.data(), count = uint32_t(m.idx.size());
    return true;
}
// render thread, once each: an extra mesh's own texture (vulkan.cpp makes its image): 1 the M67, 2 the M433
bool VrOwnTexture(int id, std::vector<uint8_t>& px, uint32_t& w, uint32_t& h) {
    if (id == 1) {
        PaintM67(px);
        w = kM67TexW, h = kM67TexH;
        return true;
    }
    if (id == 2) {
        px.assign(size_t(kM433TexW) * kM433TexH * 4, 255);
        PaintM433(px.data(), kM433TexW, 0);
        w = kM433TexW, h = kM433TexH;
        return true;
    }
    return false;
}
// render thread: which own texture the held gun's extra mesh takes now (0: its stock's bake, or the gun's)
int VrExtraOwn() {
    if (VrGrenadeInHand()) return 1;
    return HeldGunCode() == "GRL" ? 2 : 0;
}
// one extra mesh of two (each of its own bone)
StockMesh Merge(const StockMesh& a, const StockMesh& b) {
    StockMesh m = a;
    uint16_t base = uint16_t(a.verts.size() / 36);
    m.verts.insert(m.verts.end(), b.verts.begin(), b.verts.end());
    for (uint16_t i : b.idx) m.idx.push_back(uint16_t(i + base));
    return m;
}
const StockMesh* StockOf(const std::string& code) {
    static const StockMesh m16 = MakeStock(kM16StockPos, kM16StockNrm, kM16StockUv, std::size(kM16StockPos) / 3,
                                           kM16StockIdx, std::size(kM16StockIdx), 75);
    // the M249's para stock and its right side (tools/m249stock.py): the atlas's v squeezed into the stock's rows,
    // the game's own texture (256 rows) baked under them (BakeStock)
    static const StockMesh m249 = [] {
        const float h = float(kM249StockAtlas[1]), all = h + 256;
        std::vector<float> uv(kM249StockUv, kM249StockUv + std::size(kM249StockUv));
        for (size_t i = 1; i < uv.size(); i += 2) uv[i] *= h / all;
        return MakeStock(kM249StockPos, kM249StockNrm, uv.data(), std::size(kM249StockPos) / 3, kM249StockIdx,
                         std::size(kM249StockIdx), 90);
    }();
    // the M16+M203: its stock and the grenade, the stock's atlas with the grenade's strip below it (BakeStock)
    static const StockMesh bns = [] {
        const float h = float(kM16StockAtlas[1]), all = h + kM433TexH;
        std::vector<float> uv(kM16StockUv, kM16StockUv + std::size(kM16StockUv));
        for (size_t i = 1; i < uv.size(); i += 2) uv[i] *= h / all;
        StockMesh stock = MakeStock(kM16StockPos, kM16StockNrm, uv.data(), std::size(kM16StockPos) / 3, kM16StockIdx,
                                    std::size(kM16StockIdx), 75);
        StockMesh round = Merge(stock, MakeM203Grenade(float(kM433TexW) / kM16StockAtlas[0], h / all, 1.0f));
        // the spent case (VrM203CaseOut) on a bone the m16's skeleton doesn't use
        return Merge(round, MakeM203Grenade(float(kM433TexW) / kM16StockAtlas[0], h / all, 1.0f, kM433Z0, 0.0055f,
                                            kM203CaseBone, true));
    }();
    if (code == "BNS") return &bns;
    // the grenade launcher's round: the M433 too (user), where its shell sits (its back at z 0.109 round y 0.018), on
    // bone 3 ("Extra": the game's own, wider, on 114 is kept collapsed), textured by its own image (VrOwnTexture 2)
    static const StockMesh grl = MakeM203Grenade(1.0f, 0.0f, 1.0f, 0.109f, 0.018f, 3);
    if (code == "GRL") return &grl;
    if (code == "M16" || code == "M45") return &m16;
    if (code == "HVY" || code == "HV5") return &m249;
    static const StockMesh spas_shell = MakeSpasShell();
    if (code == "SHG" || code == "SH5") return &spas_shell;
    return nullptr;
}
// steady ms the game last cycled the held gun's bolt or pump (after a shot, in a mode that does: vr_reload.cpp)
int64_t VrGameCycledMs() { return g_cycled_ms.load(std::memory_order_relaxed); }
// the held gun's extra mesh is its shell (vulkan.cpp draws it in the hand too)
bool VrStockShell() {
    std::string code = HeldGunCode();
    return code == "SHG" || code == "SH5" || code == "BNS" || code == "GRL";  // the M203's grenade (its stock is folded away there)
}

// diagnostic, once per gun: the angle between its barrel in the mesh (its root bone's +z, the muzzle's way) and the
// muzzle locator the hand aims by (the frame's forward), both in C before K (S·B, as the arms' palms)
void LogBarrel(const float* frame, float (*c)[4], uint32_t count) {
    if (count >= 2880 && count <= 2970) return;  // the arms
    std::string code;
    {
        std::lock_guard lock(g_lock);
        code = g_code;
    }
    static std::set<std::string> told;
    if (told.count(code)) return;
    int root = code == "HVY" || code == "HV5" ? 90 : code == "MGN" || code == "MG5" ? 111 : code == "GRL" ? 105 : 75;
    static const std::set<std::string> k108 = {"PST", "PS5", "GK1", "GK5", "SMG", "SM5", "SM3", "MC5",
                                               "RM1", "RM5", "SHG", "SH5", "RPG"};
    if (k108.count(code)) root = 108;
    told.insert(code);
    Frame f;
    std::memcpy(&f, frame, sizeof(f));
    const float z[3] = {0, 0, 1};
    float b[3];
    RotOf(Bone(c, root), z, b);
    b[0] = -b[0], b[2] = -b[2];  // S
    Normalize(b);
    float right[3];
    Cross(f.fwd, f.up, right);
    float fw = b[0] * f.fwd[0] + b[1] * f.fwd[1] + b[2] * f.fwd[2];
    float up = b[0] * f.up[0] + b[1] * f.up[1] + b[2] * f.up[2];
    float rt = b[0] * right[0] + b[1] * right[1] + b[2] * right[2];
    constexpr float kD = 180 / 3.14159265f;
    LOGR("vr gun: %s barrel (bone %d +z) against the muzzle's forward: along %.3f, up %.2f deg, right %.2f deg "
         "(mesh %u)", code.c_str(), root, fw, std::asin(std::clamp(up, -1.0f, 1.0f)) * kD,
         std::asin(std::clamp(rt, -1.0f, 1.0f)) * kD, count);
}

bool VrPoseGun(float yaw, float pitch, const float* frame, float (*c)[4], const uint8_t* vb, uint32_t stride,
               uint32_t vbytes, const void* indices, bool index16, uint32_t count, int32_t base) {
    LogBarrel(frame, c, count);
    if (VrGunAway()) {
        // phase 6 (vr_grenade.cpp): every bone of the gun's collapsed to a point (not seen); a grenade held: the M67
        // on bone 3 at the right palm, its fuze up the controller's top, its spoon against the palm (mesh +x to the
        // controller's -x), its middle 3 cm ahead of the grip point
        float kr[9], kp[3];
        if (!VrGunPose(yaw, pitch, frame, 1, kr, kp)) return false;
        float krs[9];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) krs[i * 3 + j] = kr[i * 3 + j] * (j == 1 ? 1.0f : -1.0f);  // K · S
        Aff Dinv = Inverse(FromRotPos(krs, kp));
        Aff point{};
        for (int i = 0; i < 3; i++) point.m[i * 4 + 3] = Bone(c, 0).m[i * 4 + 3];
        for (int k = 0; k + 2 < 132; k += 3) SetBone(c, k, point);
        float AC[9], hc[3];
        if (VrGrenadeInHand() && HandInCamera(yaw, pitch, 1, AC, hc, false)) {
            float R[9], at[3];
            for (int i = 0; i < 3; i++) {
                R[i * 3 + 0] = -AC[i * 3 + 0], R[i * 3 + 1] = AC[i * 3 + 1], R[i * 3 + 2] = -AC[i * 3 + 2];
                at[i] = hc[i] - AC[i * 3 + 2] * 0.03f;
            }
            SetBone(c, kM67Bone, Compose(Dinv, FromRotPos(R, at)));
        }
        return true;
    }
    int worked_root, worked_slide;
    if (std::string wc = HeldGunCode(); WorkedGun(wc, worked_root, worked_slide)) {
        bool cycling = Cycling(c, worked_root, worked_slide);
        if (cycling) g_cycled_ms.store(SteadyMs(), std::memory_order_relaxed);
        Aff hold;
        bool held = false;
        {
            std::lock_guard lock(g_root_lock);
            RootRest& r = g_root_rest;
            if (r.code != wc) r = RootRest{wc, SteadyMs(), false, {}};
            // (the walther's handle never sits on its root even at rest: settled by time alone after 3 s)
            int64_t out = SteadyMs() - r.since;
            if (!r.ok && ((!cycling && out > 1500) || out > 3000)) {
                r.root = Bone(c, worked_root), r.ok = true;
                LOGR("vr gun: %s root held at rest", wc.c_str());
            }
            if (r.ok) hold = r.root, held = true;
        }
        // the part on the root (its own cycling not drawn), the root at rest (no rock): the whole gun turned back
        // with it, every bone (user: the walther still rocked, its magazine, handle and the rest are bones of their
        // own the game rocks with the root)
        // dev_rocklog: for 1.5 s after a shot, each gun draw: the game's root against the held one (the largest
        // difference in its rotation), the handle against the root, the kick, the gun's forward in the camera
        static const bool rocklog = std::filesystem::exists(app::ExeDir() / "dev_rocklog");
        if (rocklog && SteadyMs() - g_shot_ms.load() < 1500) {
            Aff r = Bone(c, worked_root);
            float dr = 0;
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) dr = std::max(dr, std::fabs(r.m[i * 4 + j] - hold.m[i * 4 + j]));
            float kr[9], kp[3];
            bool p = VrGunPose(yaw, pitch, frame, 1, kr, kp);
            float hy = 0, hpch = 0;
            xr::HeadAngles(hy, hpch);
            LOGR("dev_rocklog: +%lld ms held %d root off %.4f cycling %d kick %.2f fwd %.3f %.3f %.3f at %.3f %.3f %.3f "
                 "cam yaw %.2f pitch %.2f head yaw %.2f pitch %.2f",
                 (long long)(SteadyMs() - g_shot_ms.load()), held, dr, cycling, g_kick.load(), p ? kr[2] : 0,
                 p ? kr[5] : 0, p ? kr[8] : 0, kp[0], kp[1], kp[2], yaw * 57.3f, pitch * 57.3f, hy * 57.3f, hpch * 57.3f);
        }
        if (held) {
            Aff delta = Compose(hold, Inverse(Bone(c, worked_root)));
            for (int k = 0; kBone0 + k + 2 < 192; k += 3) SetBone(c, k, Compose(delta, Bone(c, k)));
        }
        SetBone(c, worked_slide, Bone(c, worked_root));
    }
    int bone, slide, bullets;
    float travel, mag_mid[3], mag_up[3], mag_top, mag_bottom;
    bool measure;
    if (!VrGunParts(bone, slide, travel, mag_mid, mag_up, measure, mag_top, mag_bottom, bullets)) return false;
    NoteGun(bone, slide, travel, mag_mid, mag_up, mag_top, mag_bottom, bullets);
    float kr[9], kp[3];
    if (!VrGunPose(yaw, pitch, frame, 1, kr, kp)) return false;
    float krs[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) krs[i * 3 + j] = kr[i * 3 + j] * (j == 1 ? 1.0f : -1.0f);  // K · S
    Aff D = FromRotPos(krs, kp), Dinv = Inverse(D);
    if (VrRevolver()) {
        // the magnum (vr_reload.cpp): the cylinder (120) and its crane (117) swing out to the player's left on the crane's
        // pin under the cylinder's front (a turn about the barrel's way: 80 degrees out), the cylinder turning on its
        // own axis as it's spun; its rounds (12..27) in its chambers while it has rounds; the speed loader (9, its
        // rounds with it) only in the hand (the hand pass) or going in behind the swung-out cylinder
        constexpr int kCyl = 120, kCrane = 117;
        constexpr float kSwing = 80 * 3.14159265f / 180;
        // (the mesh's +x is the player's left, C's -x: measured, the cylinder swung on the headset)
        const float z3[3] = {0, 0, 1}, pin[3] = {0.002f, 0.018f, 0}, axis_at[3] = {0.001f, 0.045f, 0};
        float R1[9], R2[9];
        AxisAngle(z3, -VrCylOut() * kSwing, R1);  // out to the left
        AxisAngle(z3, VrCylSpin(), R2);
        Aff S = Pivot(R1, pin, pin), SSp = Compose(S, Pivot(R2, axis_at, axis_at));
        int state = VrMagState();
        float along = VrMagInsert();
        // the game keeps the loader and its rounds' bones parked out of view but in its reload; in the mesh they sit
        // in the cylinder's chambers, so they're drawn on the cylinder's bone
        // (the cylinder and crane from the gun's root, not their own bones: the game's animations turn them, the off
        // hand spinning the cylinder (user: only ours)); at rest both sit as the root does
        const Aff cyl = Bone(c, 111);
        Aff loader_rest = Compose(D, Compose(cyl, S));  // the loader seated in the swung cylinder, in C
        float mid[3], up[3];
        Apply(loader_rest, mag_mid, mid);
        RotOf(loader_rest, mag_up, up);
        Normalize(up);
        std::memcpy(g_mag_mid_m, mag_mid, 12);
        std::memcpy(g_mag_up_m, mag_up, 12);
        VrMagWell(mid, up, mag_top, mag_bottom);
        auto gone = [&](int k) {
            Aff g{};
            for (int i = 0; i < 3; i++) g.m[i * 4 + 3] = Bone(c, k).m[i * 4 + 3];
            SetBone(c, k, g);
        };
        {
            // diagnostic: where the cylinder's middle is, swung, against where it sits shut (C: x right, y up), once
            // it's well out
            static bool told = false;
            if (!told && VrCylOut() > 0.95f) {
                told = true;
                float m0[3], m1[3];
                const float mid_cyl[3] = {0.001f, 0.045f, 0.006f};
                Aff shut = Compose(D, cyl), open = Compose(D, Compose(cyl, S));
                Apply(shut, mid_cyl, m0);
                Apply(open, mid_cyl, m1);
                float right[3], upv[3];
                const float xm[3] = {1, 0, 0}, ym[3] = {0, 1, 0};
                RotOf(shut, xm, right);
                RotOf(shut, ym, upv);
                LOGR("vr gun: cylinder swung %.3f %.3f %.3f (C) from %.3f %.3f %.3f; mesh +x is C %.2f %.2f %.2f, +y %.2f "
                     "%.2f %.2f", m1[0] - m0[0], m1[1] - m0[1], m1[2] - m0[2], m0[0], m0[1], m0[2], right[0], right[1],
                     right[2], upv[0], upv[1], upv[2]);
            }
        }
        SetBone(c, kCyl, Compose(cyl, SSp));
        SetBone(c, kCrane, Compose(cyl, S));
        if (state == 2 && along >= 0) {
            // the loader going in: on the chambers' line, from where it was
            Aff out = kIdentityAff;
            for (int i = 0; i < 3; i++) out.m[i * 4 + 3] = -up[i] * along;
            Aff L = Compose(Dinv, Compose(out, loader_rest));
            SetBone(c, bone, L);
            for (int k = 12; k <= 27; k += 3) SetBone(c, k, L);  // its rounds with it (one piece in the mesh)
        } else {
            gone(bone);
            for (int k = 12; k <= 27; k += 3) {
                if (VrCylRounds() && state != 2) SetBone(c, k, Compose(cyl, SSp));
                else gone(k);
            }
        }
        return true;
    }
    // the slide, held back by a lock or a hand: its bone back along the mesh's -z. A gun whose lock holds another
    // part (the m16's bolt) moves that with the lock or the hand, its handle with the hand only
    const int lock_bone = VrLockBone();
    const float handle = lock_bone >= 0 ? VrHandle() : VrSlide();
    for (auto [bone, by] : {std::pair{slide, handle}, std::pair{lock_bone, VrSlide()}}) {
        if (bone < 0 || by <= 0.001f) continue;
        Aff back = kIdentityAff;
        back.m[11] = -travel * by;
        SetBone(c, bone, Compose(Bone(c, bone), back));
    }
    // the rack point where the gun's own draw has it (not the arms', whose bones are another set)
    int rack_bone;
    float rack_m[3], rack_to_m[3];
    if ((count < 2880 || count > 2970) && VrRackPart(rack_bone, rack_m, rack_to_m)) {
        float at[3], to[3], up[3], fwd[3];
        const float um[3] = {0, 1, 0}, fm[3] = {0, 0, 1};
        Aff rb = Compose(D, Bone(c, rack_bone));
        Apply(rb, rack_m, at);
        Apply(rb, rack_to_m, to);
        RotOf(rb, um, up);
        RotOf(rb, fm, fwd);
        Normalize(up);
        Normalize(fwd);
        VrRackAt(at, to, up, fwd);
    }
    // the magazine's shape: measured where this draw has it (the other draw of the pair may not)
    const MagShape& measured = MeasureMag(bone, vb, stride, vbytes, indices, index16, count, base);
    if (measured.ok) g_shapes[bone] = measured;  // from whichever draw of the pair has it
    const MagShape& shape = g_shapes[bone];
    bool use = measure && shape.ok;
    float mid_m[3], up_m[3];
    std::memcpy(mid_m, use ? shape.mid : mag_mid, 12);
    std::memcpy(g_mag_mid_m, mid_m, 12);
    std::memcpy(up_m, use ? shape.up : mag_up, 12);
    std::memcpy(g_mag_up_m, up_m, 12);
    Aff rest = Compose(D, Bone(c, bone));  // the magazine as the game has it (in the gun), in C
    // the M16's M203 while it's the rifle's turn (B203's is below): its grenade (ours) in it while loaded, the barrel
    // shut
    const int launch_root = VrLauncherRoot();
    if (HeldGunCode() == "BNS") {
        // the m203's spent case: out of the opened barrel's back, falling (the gun's down), gone in half a second
        constexpr int kRoot = 75;
        float t = float(SteadyMs() - g_case_out_ms.load(std::memory_order_relaxed)) / 1000;
        Aff at{};
        for (int i = 0; i < 3; i++) at.m[i * 4 + 3] = Bone(c, kRoot).m[i * 4 + 3];
        if (t >= 0 && t < 0.5f) {
            Aff fall = kIdentityAff;
            fall.m[3] = 0.04f * t;                                // a little aside
            fall.m[7] = -4.9f * t * t;                            // down
            fall.m[11] = 0.13f - 0.6f * t;  // from the open barrel's back, rearward
            at = Compose(Bone(c, kRoot), fall);
        }
        SetBone(c, kM203CaseBone, at);
    }
    if (launch_root < 0 && HeldGunCode() == "BNS") {
        constexpr int kGrenade = 96, kRoot = 75;
        if (VrM203Loaded()) {
            SetBone(c, kGrenade, Bone(c, kRoot));
        } else {
            Aff gone{};
            for (int i = 0; i < 3; i++) gone.m[i * 4 + 3] = Bone(c, kRoot).m[i * 4 + 3];
            SetBone(c, kGrenade, gone);
        }
    }
    // a launcher's round: on the root (the game hides it when empty), in its tube where the tube is (open: forward)
    if (launch_root >= 0) {
        Aff tube = kIdentityAff;
        if (slide >= 0) tube.m[11] = -travel * VrSlide();
        rest = Compose(D, Compose(Bone(c, launch_root), tube));
    } else if (int part_root = VrPartRoot(); part_root >= 0) {
        rest = Compose(D, Bone(c, part_root));  // (the SPAS's shell: its bone parked by the game)
    }
    float mid[3], up[3];
    Apply(rest, mid_m, mid);
    RotOf(rest, up_m, up);
    Normalize(up);
    static Aff last_gun = kIdentityAff;  // the gun's magazine where it was last drawn (in the gun, or sliding in)
    // the well where the gun is now (the game still has the magazine in it), in or out of it
    VrMagWell(mid, up, use ? shape.top : mag_top, use ? shape.bottom : mag_bottom);
    int state = VrMagState();
    const bool shells = VrShellGun();  // a shotgun's shell: only ever seen in the hand or going in
    if (launch_root >= 0 && HeldGunCode() == "GRL") {
        Aff gone{};  // the grenade launcher's own round (wider than our M433, on bone 3): never seen
        for (int i = 0; i < 3; i++) gone.m[i * 4 + 3] = Bone(c, launch_root).m[i * 4 + 3];
        SetBone(c, 114, gone);
    }
    if (launch_root >= 0 && state != 2) {
        // in the gun while loaded (the rocket on its front, the grenade in its tube), else nowhere
        if (VrLauncherLoaded()) {
            last_gun = rest;
            SetMag(c, bone, bullets, Compose(Dinv, rest));
        } else {
            Aff gone{};
            for (int i = 0; i < 3; i++) gone.m[i * 4 + 3] = Bone(c, launch_root).m[i * 4 + 3];
            SetMag(c, bone, bullets, gone);
        }
        return true;
    }
    if ((state == 0 || state == 3) && !shells) {
        last_gun = rest;
        return true;  // the game's own
    }
    float along = VrMagInsert();
    if (state == 2 && along >= 0) {
        // seating (its top reached the well): on its axis, sliding home from where it was; rocked forward the more it's
        // out (the AK's goes in front first and swings back to seat)
        Aff out = kIdentityAff;
        for (int i = 0; i < 3; i++) out.m[i * 4 + 3] = -up[i] * along;
        float fwd[3], side[3], R[9];
        float fm[3] = {0, 0, 1};
        RotOf(rest, fm, fwd);
        Cross(up, fwd, side);
        Normalize(side);
        AxisAngle(side, shells ? 0.0f : -kRockIn * std::clamp(along / 0.06f, 0.0f, 1.0f), R);
        float pivot[3];
        for (int i = 0; i < 3; i++) pivot[i] = mid[i] + up[i] * 0.05f + fwd[i] * 0.02f;
        last_gun = Compose(out, Compose(Pivot(R, pivot, pivot), rest));
        SetMag(c, bone, bullets, Compose(Dinv, last_gun));
        return true;
    }
    // out of the gun: a new drop (dropped, knocked or thrown, from the gun or the hand) starts falling in the world,
    // drawn by its own pass (VrDropMag); the gun's own is hidden: collapsed to a point where it sat (vertices only
    // partly weighted to it barely move)
    static float last_t = 1e9f;
    float t = VrMagOutSeconds();
    if (t < last_t && t < 0.5f) {
        float v[3];
        VrMagThrow(v);
        StartDrop(VrMagFromHand() ? HandMag(yaw, pitch, rest) : last_gun, v);
    }
    last_t = t;
    Aff gone{};
    for (int i = 0; i < 3; i++) gone.m[i * 4 + 3] = Bone(c, bone).m[i * 4 + 3];
    SetMag(c, bone, bullets, gone);
    return true;
}

// render thread, once per gun texture: the stock's own texture (vulkan.cpp uploads it and binds it for the stock's
// draw alone), painted the way the game's guns are (user: shadows in the crevices, darkened edges): each triangle's
// cell (tools/stocks.py lays them out) filled with the receiver patch tiled at its point on the stock, mirrored at
// each repeat, shaded down to the gun's own plastic (its pistol grip's mean; user: the bare patch read too light; the
// M249's a little darker again), then darkened by how much of the stock is near and in front of it (disks of its own surface within 3 cm)
// and by nearness to a sharp edge. From the game's texture at run time: no texel of it is kept in the source.
// tex: the gun's texture, BGRA texels, 512 x 256
struct StockSrc {
    const float *pos, *nrm, *uv, *st;
    const uint8_t* edge;
    size_t tris;
    uint32_t aw, ah;
    int patch[4], tone[4];  // texels of the gun's texture
    float px_per_m;         // the patch's scale on the stock
    float darker;           // and on top of the tone
    uint32_t tw = 512, th = 256;  // the gun's texture's size
    const uint8_t* mat = nullptr;  // a whole gun's: each triangle's material (its look: kParaLook)
    float atlas_ppm = 600;         // tools/stocks.py's ATLAS_PX_PER_M for it
};
// a stock's materials (tools/m249stock.py): from the game's texture, a patch of it (texels) at its scale and
// gain, or painted (patch empty: the box's green, the rounds' brass, the dark of slots and links); paint is also
// every material's colour for our own texture (dev_m249own: no texel of the game's)
struct Look {
    int patch[4];
    float px_per_m, gain;
    uint8_t paint[3];  // RGB
    uint8_t alpha;     // the game's guns' alpha is their shine (its dull metal 15-30, its brass rounds 200)
};
const Look kParaLook[] = {
    {{248, 120, 296, 152}, 1200, 1.0f, {52, 54, 56}, 31},   // metal: the receiver's flat
    {{240, 175, 280, 210}, 1200, 1.0f, {40, 40, 42}, 18},   // polymer: the pistol grip's
    {{60, 130, 230, 186}, 1200, 1.0f, {42, 43, 45}, 14},    // the handguard: its grooved side
    {{248, 120, 296, 152}, 1200, 0.75f, {40, 41, 43}, 31},  // the barrel's steel, the stock's tubes
    {{0, 0, 0, 0}, 0, 1.0f, {66, 76, 44}, 16},              // the box: green (user), matte
    {{0, 0, 0, 0}, 0, 1.0f, {136, 107, 66}, 200},           // the rounds' brass (the game's round's tone, shine)
    {{0, 0, 0, 0}, 0, 1.0f, {24, 24, 26}, 8},               // slots, holes, links
};
const StockSrc* StockSrcOf(const std::string& code) {
    static const StockSrc m16 = {kM16StockPos, kM16StockNrm, kM16StockUv, kM16StockSt, kM16StockEdge,
                                 std::size(kM16StockEdge), kM16StockAtlas[0], kM16StockAtlas[1],
                                 {358, 192, 390, 207}, {35, 120, 85, 200}, 600, 0.36f};
    // the para stock (tools/m249stock.py): its parts each with a look (kParaLook: the receiver's metal, the grip's
    // polymer, the dark steel)
    static const StockSrc m249 = [] {
        StockSrc s = {kM249StockPos, kM249StockNrm, kM249StockUv, kM249StockSt, kM249StockEdge, std::size(kM249StockEdge),
                      kM249StockAtlas[0], kM249StockAtlas[1], {248, 120, 296, 152}, {248, 120, 296, 152}, 1200, 1.0f};
        s.mat = kM249StockMat, s.atlas_ppm = kM249StockPxPerM;
        return s;
    }();
    // the M16+M203's own texture is 256 x 256, at half the M16's (its receiver's flat at 180..208 x 198..208, its
    // pistol grip at 20..30 x 150..175: read off a dump, dev_dumptex)
    static const StockSrc bns = {kM16StockPos, kM16StockNrm, kM16StockUv, kM16StockSt, kM16StockEdge,
                                 std::size(kM16StockEdge), kM16StockAtlas[0], kM16StockAtlas[1],
                                 {180, 198, 208, 208}, {20, 150, 30, 175}, 300, 0.36f, 256, 256};
    if (code == "BNS") return &bns;
    if (code == "M16" || code == "M45") return &m16;
    if (code == "HVY" || code == "HV5") return &m249;
    return nullptr;
}
bool BakeStock(const std::string& code, const uint8_t* tex, uint32_t pitch, uint32_t tw, uint32_t th,
               std::vector<uint8_t>& out, uint32_t& ow, uint32_t& oh) {
    const StockSrc* s = StockSrcOf(code);
    // dev_dumptex: the gun's texture saved (BGRA rows, tw x th) to pick its patches from
    if (std::filesystem::exists(app::ExeDir() / "dev_dumptex")) {
        std::ofstream f(app::ExeDir() / ("tex_" + code + ".bgra"), std::ios::binary);
        for (uint32_t y = 0; y < th; y++) f.write(reinterpret_cast<const char*>(tex + size_t(y) * pitch), tw * 4);
    }
    if (!s || tw != s->tw || th != s->th) {
        LOGR("vr gun: no stock bake for %s (its texture %ux%u)", code.c_str(), tw, th);
        return false;
    }
    const auto started = std::chrono::steady_clock::now();
    auto texel = [&](int x, int y) { return tex + size_t(y) * pitch + size_t(x) * 4; };
    auto mean = [&](const int* r) {
        double sum = 0;
        int n = 0;
        for (int y = r[1]; y < r[3]; y++)
            for (int x = r[0]; x < r[2]; x++, n++) sum += (texel(x, y)[0] + texel(x, y)[1] + texel(x, y)[2]) / 3.0;
        return n ? sum / n : 0.0;
    };
    const float tone = s->mat ? 1.0f : (mean(s->patch) > 0 ? float(mean(s->tone) / mean(s->patch)) : 1.0f) * s->darker;
    static const bool own = std::filesystem::exists(app::ExeDir() / "dev_m249own");
    using V3 = std::array<float, 3>;
    auto P = [&](size_t i) { return V3{s->pos[i * 3], s->pos[i * 3 + 1], s->pos[i * 3 + 2]}; };
    auto N = [&](size_t i) { return V3{s->nrm[i * 3], s->nrm[i * 3 + 1], s->nrm[i * 3 + 2]}; };
    auto dot = [](const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    auto unit = [&](V3 a) {
        float l = std::sqrt(dot(a, a));
        if (l > 1e-9f)
            for (float& c : a) c /= l;
        return a;
    };
    // the occluders: each triangle cut into disks about 1 cm across, facing out as its corners' normals do
    struct Disk {
        V3 c, n;
        float area;
    };
    std::vector<Disk> disks;
    for (size_t t = 0; t < s->tris; t++) {
        V3 a = P(t * 3), b = P(t * 3 + 1), c = P(t * 3 + 2);
        V3 ab{b[0] - a[0], b[1] - a[1], b[2] - a[2]}, ac{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        V3 x{ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2], ab[0] * ac[1] - ab[1] * ac[0]};
        float area = 0.5f * std::sqrt(dot(x, x));
        if (area < 1e-9f) continue;
        V3 n = unit({N(t * 3)[0] + N(t * 3 + 1)[0] + N(t * 3 + 2)[0], N(t * 3)[1] + N(t * 3 + 1)[1] + N(t * 3 + 2)[1],
                     N(t * 3)[2] + N(t * 3 + 1)[2] + N(t * 3 + 2)[2]});
        float longest = std::max({std::sqrt(dot(ab, ab)), std::sqrt(dot(ac, ac)),
                                  std::hypot(c[0] - b[0], c[1] - b[1], c[2] - b[2])});
        // by area (a long thin strip a few disks, not one every centimetre of its length)
        int k = std::clamp(int(std::ceil(std::sqrt(2 * area) / 0.010f)), 1, int(std::ceil(longest / 0.010f)));
        for (int i = 0; i < k; i++)
            for (int j = 0; j < k - i; j++)
                for (int up = 0; up < 2; up++) {
                    if (up && j == k - i - 1) continue;
                    // the small triangle's middle in barycentric steps of 1/k
                    float u = (i + (up ? 2 : 1) / 3.0f) / k, v = (j + (up ? 2 : 1) / 3.0f) / k;
                    disks.push_back({{a[0] + ab[0] * u + ac[0] * v, a[1] + ab[1] * u + ac[1] * v,
                                      a[2] + ab[2] * u + ac[2] * v},
                                     n, area / float(k * k)});
                }
    }
    constexpr float kReach = 0.03f;
    // a flat grid of kReach cells over the stock's box
    V3 lo{1e9f, 1e9f, 1e9f};
    for (const Disk& d : disks)
        for (int c = 0; c < 3; c++) lo[c] = std::min(lo[c], d.c[c]);
    auto cell = [&](const V3& p) {
        return std::array<int, 3>{int(std::floor((p[0] - lo[0]) / kReach)), int(std::floor((p[1] - lo[1]) / kReach)),
                                  int(std::floor((p[2] - lo[2]) / kReach))};
    };
    std::array<int, 3> dim{1, 1, 1};
    for (const Disk& d : disks) {
        auto g = cell(d.c);
        for (int c = 0; c < 3; c++) dim[c] = std::max(dim[c], g[c] + 1);
    }
    std::vector<std::vector<uint32_t>> grid(size_t(dim[0]) * dim[1] * dim[2]);
    for (uint32_t i = 0; i < disks.size(); i++) {
        auto g = cell(disks[i].c);
        grid[(size_t(g[2]) * dim[1] + g[1]) * dim[0] + g[0]].push_back(i);
    }
    auto occlusion = [&](const V3& p, const V3& n) {
        std::array<int, 3> g = cell(p);
        float occ = 0;
        for (int dx = -1; dx <= 1; dx++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dz = -1; dz <= 1; dz++) {
                    int gx = g[0] + dx, gy = g[1] + dy, gz = g[2] + dz;
                    if (gx < 0 || gy < 0 || gz < 0 || gx >= dim[0] || gy >= dim[1] || gz >= dim[2]) continue;
                    for (uint32_t i : grid[(size_t(gz) * dim[1] + gy) * dim[0] + gx]) {
                        const Disk& d = disks[i];
                        V3 to{d.c[0] - p[0], d.c[1] - p[1], d.c[2] - p[2]};
                        float r2 = dot(to, to);
                        if (r2 < 1e-8f || r2 > kReach * kReach) continue;
                        float r = std::sqrt(r2);
                        float cr = dot(n, to) / r, ce = -dot(d.n, to) / r;
                        if (cr <= 0.05f || ce <= 0) continue;
                        occ += d.area * cr * ce / (3.14159265f * r2 + d.area);
                    }
                }
        return occ;
    };
    // the patch: mirrored at each repeat, bilinear
    const float pw = float(s->patch[2] - s->patch[0] - 1), ph = float(s->patch[3] - s->patch[1] - 1);
    const float wm = (pw + 1) / s->px_per_m, hm = (ph + 1) / s->px_per_m;
    auto fold = [](float t) {
        t = std::fmod(std::fabs(t), 2.0f);
        return t < 1 ? t : 2 - t;
    };
    // a whole gun's: each material's patch, or its paint (with a little grain, so it isn't flat)
    auto sample_look = [&](const Look& k, float sx, float sy, float* o) {
        if (own || !k.patch[2]) {
            uint32_t h = uint32_t(int(sx * 4000) * 73856093) ^ uint32_t(int(sy * 4000) * 19349663);
            h ^= h >> 13, h *= 0x5bd1e995, h ^= h >> 15;
            float n = (float(h & 255) / 255.0f - 0.5f) * 10;
            for (int c = 0; c < 3; c++) o[c] = k.paint[2 - c] + n;  // BGRA
            o[3] = k.alpha;
            return;
        }
        const float kw = float(k.patch[2] - k.patch[0] - 1), kh = float(k.patch[3] - k.patch[1] - 1);
        float fx = k.patch[0] + fold(sx / ((kw + 1) / k.px_per_m)) * kw;
        float fy = k.patch[1] + fold(sy / ((kh + 1) / k.px_per_m)) * kh;
        int x0 = int(fx), y0 = int(fy);
        int x1 = std::min(x0 + 1, k.patch[2] - 1), y1 = std::min(y0 + 1, k.patch[3] - 1);
        float ax = fx - x0, ay = fy - y0;
        for (int c = 0; c < 4; c++)
            o[c] = ((texel(x0, y0)[c] * (1 - ax) + texel(x1, y0)[c] * ax) * (1 - ay) +
                    (texel(x0, y1)[c] * (1 - ax) + texel(x1, y1)[c] * ax) * ay) * (c < 3 ? k.gain : 1.0f);
    };
    auto sample = [&](float sx, float sy, float* o) {
        float fx = s->patch[0] + fold(sx / wm) * pw, fy = s->patch[1] + fold(sy / hm) * ph;
        int x0 = int(fx), y0 = int(fy);
        int x1 = std::min(x0 + 1, s->patch[2] - 1), y1 = std::min(y0 + 1, s->patch[3] - 1);
        float ax = fx - x0, ay = fy - y0;
        for (int c = 0; c < 4; c++)
            o[c] = (texel(x0, y0)[c] * (1 - ax) + texel(x1, y0)[c] * ax) * (1 - ay) +
                   (texel(x0, y1)[c] * (1 - ax) + texel(x1, y1)[c] * ax) * ay;
    };
    const bool m433 = code == "BNS";  // the M203's grenade painted below the stock (its uvs are scaled for it)
    // the M249: its own texture under the stock's rows, for its right side's panels (their uvs onto the left's
    // texels: VrStock)
    const bool game_tex = code == "HVY" || code == "HV5";
    const uint32_t sh = s->ah;          // the stock's rows
    ow = std::max(s->aw, game_tex ? tw : 0u), oh = sh + (m433 ? kM433TexH : 0) + (game_tex ? th : 0);
    out.assign(size_t(ow) * oh * 4, 0);
    {  // the gaps between cells: the patch's mean, so filtering at a cell's rim never pulls in black
        float m[4];
        sample(0.5f * wm, 0.5f * hm, m);
        for (size_t i = 0; i < size_t(ow) * oh; i++)
            for (int c = 0; c < 4; c++) out[i * 4 + c] = uint8_t(std::clamp(m[c] * (c < 3 ? tone : 1.0f), 0.0f, 255.0f));
    }
    constexpr float kEdge = 0.35f, kEdgeWidth = 0.0015f, kOcc = 1.1f;
    const float px_m = 1.0f / s->atlas_ppm;  // tools/stocks.py ATLAS_PX_PER_M
    for (size_t t = 0; t < s->tris; t++) {
        float U[3][2];
        for (int k = 0; k < 3; k++) U[k][0] = s->uv[(t * 3 + k) * 2] * ow, U[k][1] = s->uv[(t * 3 + k) * 2 + 1] * sh;
        float den = (U[1][1] - U[2][1]) * (U[0][0] - U[2][0]) + (U[2][0] - U[1][0]) * (U[0][1] - U[2][1]);
        if (std::fabs(den) < 1e-6f) continue;
        int x0 = std::max(0, int(std::floor(std::min({U[0][0], U[1][0], U[2][0]}))) - 2);
        int x1 = std::min(int(ow) - 1, int(std::ceil(std::max({U[0][0], U[1][0], U[2][0]}))) + 1);
        int y0 = std::max(0, int(std::floor(std::min({U[0][1], U[1][1], U[2][1]}))) - 2);
        int y1 = std::min(int(sh) - 1, int(std::ceil(std::max({U[0][1], U[1][1], U[2][1]}))) + 1);
        // the occlusion worked out once a 3 x 3 block of texels (at the block's first texel), the rest share it
        const int bw = (x1 - x0) / 3 + 1;
        std::vector<float> occ_block(size_t(bw) * ((y1 - y0) / 3 + 1), -1.0f);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                float px = x + 0.5f, py = y + 0.5f;
                float b[3];
                b[0] = ((U[1][1] - U[2][1]) * (px - U[2][0]) + (U[2][0] - U[1][0]) * (py - U[2][1])) / den;
                b[1] = ((U[2][1] - U[0][1]) * (px - U[2][0]) + (U[0][0] - U[2][0]) * (py - U[2][1])) / den;
                b[2] = 1 - b[0] - b[1];
                // past the triangle (its cell's rim): its nearest point's values
                for (float& v : b) v = std::max(v, 0.0f);
                float sum = b[0] + b[1] + b[2];
                for (float& v : b) v /= sum;
                V3 p{}, n{};
                float st[2] = {0, 0};
                for (int k = 0; k < 3; k++) {
                    size_t i = t * 3 + k;
                    for (int c = 0; c < 3; c++) p[c] += b[k] * s->pos[i * 3 + c], n[c] += b[k] * s->nrm[i * 3 + c];
                    st[0] += b[k] * s->st[i * 2], st[1] += b[k] * s->st[i * 2 + 1];
                }
                n = unit(n);
                float col[4];
                if (s->mat) {
                    // (the right side's: 7 the left's texels always, 10 on a look of its own for option B)
                    int mt = s->mat[t] >= 10 ? s->mat[t] - 10 : s->mat[t] == 7 ? 0 : s->mat[t];
                    sample_look(kParaLook[std::min<size_t>(mt, std::size(kParaLook) - 1)], st[0], st[1], col);
                } else {
                    sample(st[0], st[1], col);
                }
                float& ob = occ_block[size_t((y - y0) / 3) * bw + (x - x0) / 3];
                if (ob < 0) ob = occlusion(p, n);
                float shade = std::clamp(1.0f - kOcc * ob, 0.35f, 1.0f);
                for (int k = 0; k < 3; k++) {
                    if (!(s->edge[t] >> k & 1)) continue;
                    const float* a = U[k];
                    const float* e = U[(k + 1) % 3];
                    float ex = e[0] - a[0], ey = e[1] - a[1];
                    float f = std::clamp(((px - a[0]) * ex + (py - a[1]) * ey) / std::max(ex * ex + ey * ey, 1e-6f), 0.0f, 1.0f);
                    float d = std::hypot(px - a[0] - ex * f, py - a[1] - ey * f) * px_m;
                    shade *= 1 - kEdge * std::exp(-d / kEdgeWidth);
                }
                uint8_t* o = &out[(size_t(y) * ow + x) * 4];
                for (int c = 0; c < 3; c++) o[c] = uint8_t(std::clamp(col[c] * tone * shade, 0.0f, 255.0f));
                o[3] = uint8_t(std::clamp(col[3] * shade, 0.0f, 255.0f));
            }
    }
    if (m433) PaintM433(out.data(), ow, sh);
    if (game_tex)
        for (uint32_t y = 0; y < th; y++) std::memcpy(&out[(size_t(sh + y) * ow) * 4], texel(0, y), size_t(tw) * 4);
    LOGR("vr gun: stock texture baked, %ux%u, %zu disks, the patch shaded by %.2f, in %.0f ms", ow, oh, disks.size(),
         tone, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
    return true;
}

// the bake on a thread of its own (a second or so on the headset: the render thread never waits for it). Start: the
// gun's texture (BGRA) for the held gun, once per key (the texture's address); take: 0 still baking, 1 done (out
// filled, once), -1 none for this gun. Render thread
struct StockBakeJob {
    std::atomic<int> state{0};
    std::vector<uint8_t> out;
    uint32_t w = 0, h = 0;
    std::string code;
};
std::set<std::string> g_baked;  // guns whose bake the renderer has taken (render thread)
std::map<uint32_t, std::shared_ptr<StockBakeJob>> g_bakes;
void VrStockBakeStart(uint32_t key, std::vector<uint8_t> tex, uint32_t pitch, uint32_t tw, uint32_t th) {
    if (g_bakes.count(key)) return;
    auto job = std::make_shared<StockBakeJob>();
    g_bakes[key] = job;
    std::string code = HeldGunCode();
    job->code = code;
    std::thread([job, code, tex = std::move(tex), pitch, tw, th] {
        bool ok = BakeStock(code, tex.data(), pitch, tw, th, job->out, job->w, job->h);
        if (ok) TestEvent("stock:" + code);
        job->state = ok ? 1 : -1;
    }).detach();
}
int VrStockBakeTake(uint32_t key, std::vector<uint8_t>& out, uint32_t& w, uint32_t& h) {
    auto it = g_bakes.find(key);
    if (it == g_bakes.end()) return -1;
    int st = it->second->state;
    if (st == 1) {
        out.swap(it->second->out);
        w = it->second->w, h = it->second->h;
        it->second->state = 2;  // taken
        g_baked.insert(it->second->code);
    }
    return st == 2 ? -1 : st;
}

// render thread: the stock drawn with this gun draw, if the held gun gets one and this is the draw its pistol grip is
// in (the stock takes that texture's black): vertices as the gun's (36 bytes), 16 bit indices
// the M249's right side (tools/m249stock.py, the stock's mesh): each panel triangle's uvs onto the game's own
// texels where the left has the same surface, mirrored (the game's way: every gun's right side is drawn from its
// left's texels): the left triangle under its middle's (z, y), the nearest the mirrored wall; its uv map carried on
// to the panel's corners, into the copy of the game's texture under the stock's rows (BakeStock): every panel (the
// user's option A; B, the receiver's own baked cells, is gone).
// Once, from the game's draw (its strip's or list's triangles)
StockMesh M249RightSide(const StockMesh& base, const uint8_t* vb, uint32_t stride, uint32_t vbytes, const void* indices,
                        bool index16, uint32_t count, int32_t base_v, bool strip) {
    StockMesh m = base;
    struct Left {
        float z[3], y[3], u[3], v[3], x;
    };
    std::vector<Left> left;
    const uint32_t nv = vbytes / stride;
    auto at = [&](uint32_t k) {
        return int64_t(index16 ? static_cast<const uint16_t*>(indices)[k] : static_cast<const uint32_t*>(indices)[k]) +
               base_v;
    };
    for (uint32_t k = 0; k + 2 < count; k += strip ? 1 : 3) {
        int64_t t[3] = {at(k), at(k + 1), at(k + 2)};
        if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) continue;
        if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[0] >= nv || t[1] >= nv || t[2] >= nv) continue;
        float p[3][3], uv[3][2];
        for (int j = 0; j < 3; j++) {
            std::memcpy(p[j], vb + size_t(t[j]) * stride, 12);
            std::memcpy(uv[j], vb + size_t(t[j]) * stride + 28, 8);
        }
        float e1[3], e2[3], n[3];
        for (int c = 0; c < 3; c++) e1[c] = p[1][c] - p[0][c], e2[c] = p[2][c] - p[0][c];
        n[0] = e1[1] * e2[2] - e1[2] * e2[1], n[1] = e1[2] * e2[0] - e1[0] * e2[2], n[2] = e1[0] * e2[1] - e1[1] * e2[0];
        float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        float cx = (p[0][0] + p[1][0] + p[2][0]) / 3;
        if (l < 1e-10f || std::fabs(n[0]) < 0.6f * l || cx < 0.004f) continue;
        Left L;
        for (int j = 0; j < 3; j++) L.z[j] = p[j][2], L.y[j] = p[j][1], L.u[j] = uv[j][0], L.v[j] = uv[j][1];
        L.x = cx;
        left.push_back(L);
    }
    auto bary = [](const Left& L, float z, float y, float* b) {
        float den = (L.y[1] - L.y[2]) * (L.z[0] - L.z[2]) + (L.z[2] - L.z[1]) * (L.y[0] - L.y[2]);
        if (std::fabs(den) < 1e-12f) return false;
        b[0] = ((L.y[1] - L.y[2]) * (z - L.z[2]) + (L.z[2] - L.z[1]) * (y - L.y[2])) / den;
        b[1] = ((L.y[2] - L.y[0]) * (z - L.z[2]) + (L.z[0] - L.z[2]) * (y - L.y[2])) / den;
        b[2] = 1 - b[0] - b[1];
        return true;
    };
    const float sh = float(kM249StockAtlas[1]), all = sh + 256, aw = float(std::max(kM249StockAtlas[0], 512u));
    const size_t tris = std::size(kM249StockMat);
    int done = 0, missed = 0;
    for (size_t t = 0; t < tris; t++) {
        uint8_t mt = kM249StockMat[t];
        if (mt != 7 && mt < 10) continue;
        float c[3] = {};
        for (int j = 0; j < 3; j++)
            for (int k = 0; k < 3; k++) c[k] += kM249StockPos[(t * 3 + j) * 3 + k] / 3;
        // the left triangle under the middle, the nearest the mirrored wall (else the nearest at all)
        const Left* best = nullptr;
        float best_d = 1e9f;
        for (const Left& L : left) {
            float b[3];
            if (!bary(L, c[2], c[1], b)) continue;
            float out = std::max({-b[0], -b[1], -b[2], 0.0f});
            float d = std::fabs(L.x + c[0]) + out * 0.05f;
            if (d < best_d) best_d = d, best = &L;
        }
        if (!best) {
            missed++;
            continue;
        }
        float mu = (best->u[0] + best->u[1] + best->u[2]) / 3, mv = (best->v[0] + best->v[1] + best->v[2]) / 3;
        float fu = std::floor(mu), fv = std::floor(mv);
        for (int j = 0; j < 3; j++) {
            size_t vi = t * 3 + j;
            float b[3];
            bary(*best, kM249StockPos[vi * 3 + 2], kM249StockPos[vi * 3 + 1], b);
            float u = std::clamp(b[0] * best->u[0] + b[1] * best->u[1] + b[2] * best->u[2] - fu, 0.0f, 1.0f);
            float v = std::clamp(b[0] * best->v[0] + b[1] * best->v[1] + b[2] * best->v[2] - fv, 0.0f, 1.0f);
            float uv[2] = {u * 512 / aw, (sh + v * 256) / all};
            std::memcpy(&m.verts[vi * 36 + 28], uv, 8);
        }
        done++;
    }
    LOGR("vr gun: M249 right side: %d panel triangles on the left's texels (%d none), %zu left faces", done, missed,
         left.size());
    return m;
}

// the WA2000's right side (user: option A, as the M249's; the game modelled 21% of it, its wooden stock's and
// fore-end's sides missing, their outline cut through by the thumbhole and the cheek's triangle): every left side
// face of the gun's own bone (75) with nothing across from it on the right (no right face under its (z, y) within
// 5 mm of its mirrored x) copied there: x turned, its normal's x turned, its own uvs (the left's texels at the
// mirrored spot, the game's way), both windings. Drawn with the gun's own texture. Once, from the game's draw
StockMesh WaRightSide(const uint8_t* vb, uint32_t stride, uint32_t vbytes, const void* indices, bool index16,
                      uint32_t count, int32_t base_v, bool strip) {
    StockMesh m;
    struct Face {
        int64_t v[3];
        float z[3], y[3], x;
        int16_t bone;
    };
    std::vector<Face> left, right;
    const uint32_t nv = vbytes / stride;
    auto at = [&](uint32_t k) {
        return int64_t(index16 ? static_cast<const uint16_t*>(indices)[k] : static_cast<const uint32_t*>(indices)[k]) +
               base_v;
    };
    for (uint32_t k = 0; k + 2 < count; k += strip ? 1 : 3) {
        int64_t t[3] = {at(k), at(k + 1), at(k + 2)};
        if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) continue;
        if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[0] >= nv || t[1] >= nv || t[2] >= nv) continue;
        float p[3][3];
        for (int j = 0; j < 3; j++) std::memcpy(p[j], vb + size_t(t[j]) * stride, 12);
        float e1[3], e2[3], n[3];
        for (int c = 0; c < 3; c++) e1[c] = p[1][c] - p[0][c], e2[c] = p[2][c] - p[0][c];
        n[0] = e1[1] * e2[2] - e1[2] * e2[1], n[1] = e1[2] * e2[0] - e1[0] * e2[2], n[2] = e1[0] * e2[1] - e1[1] * e2[0];
        float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l < 1e-10f || std::fabs(n[0]) < 0.6f * l) continue;
        Face f;
        for (int j = 0; j < 3; j++) f.v[j] = t[j], f.z[j] = p[j][2], f.y[j] = p[j][1];
        f.x = (p[0][0] + p[1][0] + p[2][0]) / 3;
        std::memcpy(&f.bone, vb + size_t(t[0]) * stride + 16, 2);
        if (f.x > 0.004f && f.bone == 75) left.push_back(f);
        else if (f.x < -0.004f) right.push_back(f);
    }
    auto inside = [](const Face& f, float z, float y) {
        float den = (f.y[1] - f.y[2]) * (f.z[0] - f.z[2]) + (f.z[2] - f.z[1]) * (f.y[0] - f.y[2]);
        if (std::fabs(den) < 1e-12f) return false;
        float a = ((f.y[1] - f.y[2]) * (z - f.z[2]) + (f.z[2] - f.z[1]) * (y - f.y[2])) / den;
        float b = ((f.y[2] - f.y[0]) * (z - f.z[2]) + (f.z[0] - f.z[2]) * (y - f.y[2])) / den;
        return a >= -0.01f && b >= -0.01f && a + b <= 1.01f;
    };
    std::map<int64_t, uint16_t> copied;  // a left vertex: its copy
    size_t filled = 0;
    for (const Face& f : left) {
        float cz = (f.z[0] + f.z[1] + f.z[2]) / 3, cy = (f.y[0] + f.y[1] + f.y[2]) / 3;
        bool covered = false;
        for (const Face& r : right)
            if (std::fabs(r.x + f.x) < 0.005f && inside(r, cz, cy)) {
                covered = true;
                break;
            }
        if (covered) continue;
        uint16_t c[3];
        for (int j = 0; j < 3; j++) {
            auto it = copied.find(f.v[j]);
            if (it == copied.end()) {
                size_t o = m.verts.size();
                if (o / 36 >= 0xFFFF) return m;
                m.verts.insert(m.verts.end(), vb + size_t(f.v[j]) * stride, vb + size_t(f.v[j]) * stride + 36);
                float x;
                std::memcpy(&x, &m.verts[o], 4);
                x = -x;
                std::memcpy(&m.verts[o], &x, 4);
                uint32_t pn;
                std::memcpy(&pn, &m.verts[o + 24], 4);
                pn = (pn & ~0x7FFu) | (uint32_t(-int32_t(pn & 0x7FF)) & 0x7FF);
                std::memcpy(&m.verts[o + 24], &pn, 4);
                it = copied.emplace(f.v[j], uint16_t(o / 36)).first;
            }
            c[j] = it->second;
        }
        m.idx.insert(m.idx.end(), {c[0], c[1], c[2], c[0], c[2], c[1]});
        filled++;
    }
    LOGR("vr gun: WA2000 right side: %zu of %zu left faces copied across (%zu right faces there)", filled, left.size(),
         right.size());
    return m;
}

bool VrStock(const uint8_t* vb, uint32_t stride, uint32_t vbytes, const void* indices, bool index16, uint32_t count,
             int32_t base, bool strip, const uint8_t*& verts, uint32_t& vbytes_out, const uint16_t*& idx,
             uint32_t& icount) {
    if (stride != 36 || !vb || !indices) return false;
    std::string code = HeldGunCode();
    if (VrGunAway()) {
        // phase 6: the gun away: a grenade in the hand (the M67, on the gun's biggest draw), or nothing
        static std::map<std::string, uint32_t> biggest;
        uint32_t& big = biggest[code];
        big = std::max(big, count);
        if (!VrGrenadeInHand() || count != big) return false;
        static const StockMesh m67 = MakeM67();
        verts = m67.verts.data(), vbytes_out = uint32_t(m67.verts.size());
        idx = m67.idx.data(), icount = uint32_t(m67.idx.size());
        return true;
    }
    if ((code == "SNR" || code == "SN5") && count > 3000) {
        // the WA2000's right side (its main draw: the gun's), with the gun's own texture
        static const StockMesh wa = WaRightSide(vb, stride, vbytes, indices, index16, count, base, strip);
        if (wa.idx.empty()) return false;
        verts = wa.verts.data(), vbytes_out = uint32_t(wa.verts.size());
        idx = wa.idx.data(), icount = uint32_t(wa.idx.size());
        return true;
    }
    const StockMesh* m = StockOf(code);
    if (!m) return false;
    static std::map<std::pair<std::string, uint32_t>, bool> has_grip;
    auto key = std::make_pair(code, count);
    auto it = has_grip.find(key);
    if (it == has_grip.end()) {
        int root = GunRoot(code);
        bool found = false;
        for (uint32_t i = 0; i < count && !found; i++) {
            int64_t v = int64_t(index16 ? static_cast<const uint16_t*>(indices)[i] : static_cast<const uint32_t*>(indices)[i]) + base;
            if (v < 0 || (uint64_t(v) + 1) * stride > vbytes) continue;
            const uint8_t* p = vb + size_t(v) * stride;
            float pos[3];
            int16_t ix;
            std::memcpy(pos, p, 12);
            std::memcpy(&ix, p + 16, 2);
            found = ix == root && p[12] > 250 && pos[1] < -0.05f && pos[2] > -0.12f && pos[2] < 0;
        }
        it = has_grip.emplace(key, found).first;
        LOGR("vr gun: %s stock %s the draw of %u indices", code.c_str(), found ? "with" : "not with", count);
    }
    if (!it->second) return false;
    if (code == "HVY" || code == "HV5") {
        static const StockMesh right = M249RightSide(*m, vb, stride, vbytes, indices, index16, count, base, strip);
        m = &right;
    }
    verts = m->verts.data(), vbytes_out = uint32_t(m->verts.size());
    idx = m->idx.data(), icount = uint32_t(m->idx.size());
    // dev_stockcut in the app folder: index ranges "from to" (fractions of the stock's triangles, both windings
    // kept), each drawn alone in turn for 3 s, to find a part by
    static const std::vector<std::array<float, 2>> dev_cut = [] {
        std::vector<std::array<float, 2>> v;
        std::ifstream f(app::ExeDir() / "dev_stockcut");
        for (float a, b; f >> a >> b;) v.push_back({a, b});
        return v;
    }();
    if (!dev_cut.empty()) {
        static const auto t0 = std::chrono::steady_clock::now();
        size_t i = size_t(std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count() / 3) % dev_cut.size();
        static std::map<std::pair<const StockMesh*, size_t>, std::vector<uint16_t>> cut;
        auto k = std::make_pair(m, i);
        if (!cut.count(k)) {
            size_t tris = m->idx.size() / 6, a = size_t(dev_cut[i][0] * tris), b = size_t(dev_cut[i][1] * tris);
            auto& v = cut[k];
            v.assign(m->idx.begin() + a * 3, m->idx.begin() + b * 3);
            v.insert(v.end(), m->idx.begin() + (tris + a) * 3, m->idx.begin() + (tris + b) * 3);
        }
        static size_t told = SIZE_MAX;
        if (told != i) told = i, LOGR("vr gun: dev_stockcut %.3f %.3f", dev_cut[i][0], dev_cut[i][1]);
        idx = cut[k].data(), icount = uint32_t(cut[k].size());
    }
    return true;
}

// render thread: a gun draw's texture (BGRA rows) saved for dev_dumpgun (dumps/tex_<code>_<key>_<w>x<h>.bgra)
void VrDumpGunTexture(uint32_t key, const uint8_t* px, uint32_t pitch, uint32_t w, uint32_t h) {
    std::filesystem::create_directories(app::ExeDir() / "dumps");
    char name[96];
    std::snprintf(name, sizeof(name), "tex_%s_%08X_%ux%u.bgra", HeldGunCode().c_str(), key, w, h);
    std::ofstream f(app::ExeDir() / "dumps" / name, std::ios::binary);
    for (uint32_t y = 0; y < h; y++) f.write(reinterpret_cast<const char*>(px + size_t(y) * pitch), w * 4);
    LOGR("vr gun: dev_dumpgun texture %s", name);
}
// render thread, the gun's draw (any gun): dev_dumpgun saves it once a gun and draw (dumps/gun_<code>_<count>.bin:
// vertex count, index count, 16-bit, base, strip; the vertices; the indices) to model from. A draw swapped for
// another (none now: the M249 Para was scrapped, user); false: as it is
bool VrMirror(const uint8_t* vb, uint32_t stride, uint32_t vbytes, const void* indices, bool index16, uint32_t count,
              int32_t base, bool strip, const uint8_t*&, uint32_t&, const void*&, uint32_t&) {
    if (stride != 36 || !vb || !indices || !count) return false;
    static const bool dump = std::filesystem::exists(app::ExeDir() / "dev_dumpgun");
    if (!dump) return false;
    std::string code = HeldGunCode();
    static std::set<std::pair<std::string, uint32_t>> dumped;
    if (dumped.insert({code, count}).second) {
        std::filesystem::create_directories(app::ExeDir() / "dumps");
        std::ofstream f(app::ExeDir() / "dumps" / ("gun_" + code + "_" + std::to_string(count) + ".bin"),
                        std::ios::binary);
        int32_t head[5] = {int32_t(vbytes / stride), int32_t(count), index16, base, strip};
        f.write(reinterpret_cast<const char*>(head), sizeof(head));
        f.write(reinterpret_cast<const char*>(vb), std::streamsize(vbytes / stride) * stride);
        f.write(static_cast<const char*>(indices), std::streamsize(count) * (index16 ? 2 : 4));
        LOGR("vr gun: dev_dumpgun %s draw of %u indices", code.c_str(), count);
    }
    return false;
}

// render thread, the gun's draw drawn again for a dropped magazine (on its way down or on the floor), whatever the
// gun's own is doing: every bone collapsed but the magazine's. false: no draw
bool VrDropMag(float yaw, float pitch, const float* frame, float (*c)[4]) {
    int bone, slide, bullets;
    float travel, mag_mid[3], mag_up[3], mag_top, mag_bottom;
    bool measure;
    if (!VrGunParts(bone, slide, travel, mag_mid, mag_up, measure, mag_top, mag_bottom, bullets)) return false;
    NoteGun(bone, slide, travel, mag_mid, mag_up, mag_top, mag_bottom, bullets);
    Aff now;
    if (!DropNow(VrMagOutSeconds(), now)) return false;
    float kr[9], kp[3];
    if (!VrGunPose(yaw, pitch, frame, 1, kr, kp)) return false;
    float krs[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) krs[i * 3 + j] = kr[i * 3 + j] * (j == 1 ? 1.0f : -1.0f);  // K · S
    Aff Dinv = Inverse(FromRotPos(krs, kp));
    Aff b = Compose(Dinv, now);
    Aff point{};
    for (int i = 0; i < 3; i++) point.m[i * 4 + 3] = Bone(c, bone).m[i * 4 + 3];
    const bool revolver = VrRevolver();  // the speed loader's six rounds go with it
    for (int k = 0; k + 2 < 132; k += 3)
        SetBone(c, k, k == bone || k == bullets || (revolver && k >= 12 && k <= 27) ? b : point);
    return true;
}

// render thread, the gun's draw drawn again for the magazine in the off hand (while the gun's own is in the gun,
// falling, or gone): every bone collapsed but the magazine's, which is put on the palm. false: no second draw
bool VrHandMag(float yaw, float pitch, const float* frame, float (*c)[4]) {
    int state = VrMagState();
    if (!(state == 3 || (state == 2 && VrMagInsert() < 0))) return false;
    int bone, slide, bullets;
    float travel, mag_mid[3], mag_up[3], mag_top, mag_bottom;
    bool measure;
    if (!VrGunParts(bone, slide, travel, mag_mid, mag_up, measure, mag_top, mag_bottom, bullets)) return false;
    float kr[9], kp[3];
    if (!VrGunPose(yaw, pitch, frame, 1, kr, kp)) return false;
    float krs[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) krs[i * 3 + j] = kr[i * 3 + j] * (j == 1 ? 1.0f : -1.0f);  // K · S
    Aff D = FromRotPos(krs, kp), Dinv = Inverse(D);
    const int part_root = VrPartRoot();  // a launcher's round, the SPAS's shell: the game may have hidden its bone
    Aff rest = Compose(D, Bone(c, part_root >= 0 ? part_root : bone));
    Aff b = Compose(Dinv, HandMag(yaw, pitch, rest));
    // the rest collapsed onto the held part's own middle: a triangle partly on its bone stays on it (user
    // 2026-11-27: on the SPAS, collapsed to the shell bone's place, which the game parks off the gun, they reached
    // from the off hand across the view, a gun-like shape in the hand)
    Aff point{};
    float mid[3];
    Apply(b, mag_mid, mid);
    for (int i = 0; i < 3; i++) point.m[i * 4 + 3] = mid[i];
    {
        static int64_t told = 0;
        static const bool dev_hands = std::filesystem::exists(app::ExeDir() / "dev_fakehands");
        if (dev_hands && NowMs() - told > 2000) {
            told = NowMs();
            float d = 0, e = 0, mc[3], AC[9], hc[3];
            for (int i = 0; i < 3; i++) d += std::pow(point.m[i * 4 + 3] - Bone(c, bone).m[i * 4 + 3], 2.0f);
            Apply(D, mid, mc);  // (G to C)
            if (HandInCamera(yaw, pitch, 0, AC, hc, false))
                for (int i = 0; i < 3; i++) e += (mc[i] - hc[i]) * (mc[i] - hc[i]);
            LOGR("vr gun: the held part's middle %.3f m from its bone's place, %.3f m from the palm", std::sqrt(d),
                 std::sqrt(e));
        }
    }
    for (int k = 0; k + 2 < 132; k += 3) SetBone(c, k, k == bone || k == bullets ? b : point);
    return true;
}

// game thread: take or let go of the gun with the off hand: a grip press within reach of where it holds the gun
// takes hold; letting go of the grip lets go, or with toggle grip the next press. On a long gun, holding it aims the
// barrel along the line between the hands
float g_fore_at[3];  // where the off hand takes the gun, C (game thread; dev_reload goes there)
bool g_fore_ok = false;
void LeftHold(const Frame& f) {
    float yaw, pitch, rot[9], pos[3], AC[9], hc[3];
    xr::Hand hand[2];
    float head[3];
    static bool was = false;
    bool on = g_left_on.load();
    bool near = false;
    if ((int(f.flags) & 3) == 3 && VrLookAngles(yaw, pitch) && xr::Hands(hand[0], hand[1], head) &&
        VrGunPose(yaw, pitch, &f.grip[0], 1, rot, pos) && HandInCamera(yaw, pitch, 0, AC, hc, false)) {
        float fore[3], d2 = 0;
        Rest(f, 0, fore);
        for (int i = 0; i < 3; i++)
            g_fore_at[i] = rot[i * 3] * fore[0] + rot[i * 3 + 1] * fore[1] + rot[i * 3 + 2] * fore[2] + pos[i];
        // the fore-grip is a stretch along the gun under the barrel, not a point (user 2026-11-27: the AK's grip
        // pulled its magazine instead, the hand 5-10 cm ahead of the charging handle; after a shotgun's reload no
        // grip took): from kForeBack of the grip-to-fore-grip way back (short of the magazine well) to kForeAhead on
        // along the barrel (the mesh's +z)
        constexpr float kForeBack = 0.4f, kForeAhead = 0.12f;
        {
            float v[3], z[3];
            for (int i = 0; i < 3; i++) v[i] = hc[i] - g_fore_at[i], z[i] = rot[i * 3 + 2];
            float t = std::clamp(v[0] * z[0] + v[1] * z[1] + v[2] * z[2], -kForeBack * f.fore, kForeAhead);
            for (int i = 0; i < 3; i++) d2 += (v[i] - z[i] * t) * (v[i] - z[i] * t);
        }
        g_fore_ok = true;
        near = d2 < kReach * kReach;
        g_left_fore.store(std::sqrt(d2), std::memory_order_relaxed);
        float g = hand[0].grip;
        bool press = g > 0.6f && !was;
        was = g > 0.6f;
        // every grip press near the gun, for the next miss: how far from the fore-grip's stretch
        if (press && d2 < 0.3f * 0.3f)
            LOGR("vr gun: off hand grip %.1f cm from the fore-grip (reach %.0f), on %d, mag %d, slide %d, racking %d",
                 std::sqrt(d2) * 100, kReach * 100, int(on), int(VrMagInHand()), int(VrSlideHeld()), int(VrRacking()));
        // a grip still held from a magazine seated or a handle let go of takes the fore-grip as the hand gets there
        // (user 2026-11-26: after a load the off hand couldn't grip; it took a fresh press)
        static bool carry = false;
        if (g < 0.4f) carry = false;
        else if (VrMagInHand() || VrSlideHeld()) carry = true;
        static const bool no_carry = std::filesystem::exists(app::ExeDir() / "dev_nocarry");  // (the fix's test)
        const bool take = press || (carry && !no_carry && !VrMagInHand() && !VrSlideHeld());
        // hold (the default): held while the grip is; toggle (VR settings): the next press lets go
        bool toggle = Opt().vr_grip_toggle;
        if (on && (toggle ? press : g < 0.4f)) {
            on = false;
        } else if (!on && take && near) {
            on = true, carry = false;
            xr::Pulse(0, 0.5f, 0.04f);
        }
        static const bool fake_on = std::filesystem::exists(app::ExeDir() / "dev_lefton");  // held on, no hands
        if (fake_on) on = true;
    } else {
        on = false;
        g_left_fore.store(1e9f, std::memory_order_relaxed);
    }
    // a magazine in the off hand, or the slide in it: it can't hold the gun as well. The reload's zones (the magazine,
    // the chest) give way to the fore-grip within its reach (user 2026-11-26: after a reload, the magazine's zone took
    // every grip at the AK's and shotguns' fore-ends). A pump or the M203's tube is worked from the fore-grip, held:
    // only the slide or handle in the hand counts (user 2026-11-26: VrRackHeld, the pump's too, let go of every
    // fore-grip taken on a shotgun waiting for its pump, after any shot or a shell loaded)
    const bool was_on = on;
    const char* why = VrMagInHand() ? "a magazine or round in the hand" : VrSlideHeld() ? "the slide in the hand"
                                                                        : VrRacking() && !near ? "a reload zone" : nullptr;
    if (why) on = false, near = false;
    {
        static bool told_on = false;
        if (on != told_on) LOGR("vr gun: off hand %s the gun%s%s", on ? "on" : "off", why && was_on ? ": " : "",
                                why && was_on ? why : "");
        told_on = on;
    }
    g_left_on.store(on);
    g_two_aim.store(on && f.fore >= 0.05f);  // a pistol's hands are too close to aim by
    // the left grip isn't the game's white button while it holds or is about to take something
    xr::TakeLeftGrip(on || near || VrRacking() || VrMagInHand());
}

// game thread, the gun list about to be drawn: the gun's frame from this frame's muzzle, for the renderer
void VrGunFrame(X86Context& ctx) {
    float m[16];
    if (!Active() || !Muzzle(ctx, m)) {
        d3d::VrGunUsed(nullptr);
        g_left_on.store(false);
        xr::TakeLeftGrip(false);
        return;
    }
    {
        // the camera in the world, for a dropped magazine to stay where it fell (the renderer's latest)
        std::lock_guard lock(g_cam_lock);
        for (int r = 0; r < 4; r++) std::memcpy(g_cam.rows[r], Row(r), 12);
        g_cam.feet = Row(3)[1] - (G<float>(Player() + kStance) - 0.2f);
        g_cam.ok = true;
    }
    Frame f{};
    float mc[3], left[3];
    ToCamera(m + 12, true, mc);
    ToCamera(m + 0, false, left);
    ToCamera(m + 4, false, f.up);
    ToCamera(m + 8, false, f.fwd);
    // the locator may carry a scale or a little shear: square it, the barrel kept
    Normalize(f.fwd);
    float right[3] = {-left[0], -left[1], -left[2]};
    Cross(right, f.fwd, f.up);  // up = right × fwd in a right handed frame (x right, y up, z back)
    Normalize(f.up);
    std::string code = weapons::HeldCode();
    VrScopeHeld(code);
    auto it = kGrips.find(code);
    const Grip& g = it == kGrips.end() ? kDefaultGrip : it->second;
    for (int i = 0; i < 3; i++) f.grip[i] = mc[i] - f.fwd[i] * g.back - f.up[i] * g.down;
    f.fore = g.back - g.fore;
    static const bool dev_two = std::filesystem::exists(app::ExeDir() / "dev_twohand");  // split any gun's arms
    f.flags = 1 + (g.two || dev_two ? 2 : 0);
    {
        // a worked gun once it's settled (its root held, g_root_rest): its frame as it was then, away from a shot.
        // The frame is the game's muzzle, which the game rocks with the gun after each shot; holding the gun's bones
        // alone left the gun turning in the hand with it (user: the walther still rocked; logged, its root 0.74 off
        // its rest for a second after a shot)
        static std::string held_code;
        static Frame held{};
        static bool have = false;
        static int64_t came_ms = 0;
        int wr, ws;
        if (code != held_code) have = false, held_code = code, came_ms = SteadyMs();
        if (WorkedGun(code, wr, ws)) {
            bool settled;
            {
                std::lock_guard rl(g_root_lock);
                settled = g_root_rest.ok && g_root_rest.code == code;
            }
            // (and not before the game's raise animation is done: a gun switched back to had its root's rest from
            // before, so the frame was held mid-raise, 45 degrees up for good, user 2026-11-26)
            if (!have && settled && SteadyMs() - g_shot_ms.load() > 1500 && SteadyMs() - came_ms > 2000) {
                held = f, have = true;
                LOGR("vr gun: %s frame held at rest", code.c_str());
            }
            if (have) {
                std::memcpy(f.grip, held.grip, 12);
                std::memcpy(f.fwd, held.fwd, 12);
                std::memcpy(f.up, held.up, 12);
            }
        }
    }
    {
        std::lock_guard lock(g_lock);
        g_frame = f;
        if (code != g_code) {
            g_code = code;
            g_gun_fingers_open.store(false);  // a new weapon comes out held
            LOGR("vr gun: weapon %s, grip %.2f back %.2f down, fore-grip %.2f%s", code.c_str(), g.back, g.down, g.fore,
                 it == kGrips.end() ? " (default)" : "");
        }
    }
    LeftHold(f);
    g_long_gun.store(f.fore >= 0.05f && code != "SMG" && code != "SM5");  // (the uzi's one-handed: user)
    VrReloadTick();
    // the recoil, and its strongest per gun in the log for tuning
    float kick = G<float>(Player() + kRecoilKick);
    if (!std::isfinite(kick)) kick = 0;
    g_kick.store(kick, std::memory_order_relaxed);
    g_kick_no_turn.store(code == "SNR" || code == "SN5", std::memory_order_relaxed);
    static float most = 0;
    static std::string most_code;
    if (code != most_code) {
        if (most > 0) LOGR("vr gun: %s kicked up to %.2f degrees", most_code.c_str(), most);
        most = 0, most_code = code;
    }
    most = std::max(most, std::fabs(kick));
    d3d::VrGunUsed(&f.grip[0]);
}

// the gun is drawn in the hand (else head-locked as the game has it, and the melee swing hidden)
bool VrGunInHand() {
    if (!Active()) return false;
    float A[9], p[3];
    std::lock_guard lock(g_lock);
    return (int(g_frame.flags) & 1) && HandPose(1, A, p);
}

// game thread: a game-world matrix (d3d rows: axes, then the position) of something on the player's gun, moved with
// the gun to the hand. false (unmoved) when there is no hand or no frame yet
bool VrGunMap(float* m, bool axes) {
    float yaw, pitch, rot[9], pos[3];
    if (!Active() || !VrLookAngles(yaw, pitch)) return false;
    Frame f;
    {
        std::lock_guard lock(g_lock);
        f = g_frame;
    }
    if (!VrGunPose(yaw, pitch, &f.grip[0], 1, rot, pos)) return false;
    for (int r = axes ? 0 : 3; r < 4; r++) {
        bool point = r == 3;
        float c[3], k[3];
        ToCamera(m + 4 * r, point, c);
        for (int i = 0; i < 3; i++)
            k[i] = rot[i * 3] * c[0] + rot[i * 3 + 1] * c[1] + rot[i * 3 + 2] * c[2] + (point ? pos[i] : 0);
        ToWorld(k, point, m + 4 * r);
    }
    return true;
}

// game thread: a grip press of the gun hand that wasn't a weapon switch opens or closes its fingers on the gun
void VrToggleGunFingers() { g_gun_fingers_open.store(!g_gun_fingers_open.load()); }

// the left hand holds the gun (any thread)
bool VrLeftOnGun() { return g_left_on.load(std::memory_order_relaxed); }
float VrLeftFore() { return g_left_fore.load(std::memory_order_relaxed); }
// game thread: where the off hand takes the gun (C); false before the first gun frame
bool VrForeAt(float* c) {
    std::memcpy(c, g_fore_at, sizeof(g_fore_at));
    return g_fore_ok;
}
// game thread: the m203's tube opened on a fired round: its case drops out (vr_reload.cpp)
void VrM203CaseOut() {
    g_case_out_ms.store(SteadyMs());
    LOGR("vr gun: the m203's spent case out");
}

// a shot's buzz by gun: the light quick ones short and soft, the heavy ones long and hard (user: firing per weapon)
struct Feel {
    float amp, secs;
};
Feel ShotFeel(const std::string& code) {
    static const std::map<std::string, Feel> kFeel = {
        {"PST", {0.42f, 0.035f}}, {"PS5", {0.42f, 0.035f}},  // pistols
        {"GK1", {0.4f, 0.03f}},   {"GK5", {0.4f, 0.03f}},    // glock: lighter, quicker
        {"MGN", {0.85f, 0.08f}},  {"MG5", {0.85f, 0.08f}},   // the magnum: a slam
        {"SMG", {0.32f, 0.025f}}, {"SM5", {0.32f, 0.025f}},  // uzi: fast, a light rattle
        {"SM3", {0.3f, 0.022f}},  {"MC5", {0.3f, 0.022f}},   // mac-10: faster still
        {"MP1", {0.36f, 0.03f}},  {"MP5", {0.36f, 0.03f}},   // mp5
        {"P90", {0.34f, 0.028f}}, {"P95", {0.34f, 0.028f}},  // p90
        {"AK1", {0.62f, 0.05f}},  {"AK5", {0.62f, 0.05f}},   // ak-47: the heavier rifle round
        {"M16", {0.52f, 0.042f}}, {"M45", {0.52f, 0.042f}},  // m16
        {"BNS", {0.52f, 0.042f}},                            // m16 with the m203
        {"ASR", {0.5f, 0.04f}},   {"AS5", {0.5f, 0.04f}},    // g36
        {"HVY", {0.66f, 0.05f}},  {"HV5", {0.66f, 0.05f}},   // m249: heavy, sustained
        {"SNR", {0.9f, 0.09f}},   {"SN5", {0.9f, 0.09f}},    // wa2000
        {"SHG", {0.95f, 0.1f}},   {"SH5", {0.95f, 0.1f}},    // spas-12
        {"RM1", {0.95f, 0.11f}},  {"RM5", {0.95f, 0.11f}},   // remington 870
    };
    auto it = kFeel.find(code);
    return it == kFeel.end() ? Feel{0.5f, 0.05f} : it->second;
}

// game thread: a launcher's shot (coop_world.cpp's wraps of the player's rocket and launched grenade): a big shove,
// the rpg's the biggest, through both hands when the off hand's on it (user: bigger guns more)
void VrLauncherKick(bool rocket) {
    float amp = rocket ? 1.0f : 0.85f, secs = rocket ? 0.2f : 0.12f;
    xr::Pulse(1, amp, secs);
    if (VrLeftOnGun() || rocket) xr::Pulse(0, amp * 0.8f, secs);
    TestEvent(rocket ? "rocket_kick" : "grenade_kick");
}

// a light touch on a hand (reloading's steps, pickups: user, a sensation of touch); any thread
void VrTouch(int hand, float amp) { xr::Pulse(hand, amp, 0.018f); }

// game thread: the game's shot direction (from the camera along its view, spread added) turned from the head's view
// to the gun hand's: the drawn barrel is the hand's forward, so the shot follows it at any range. Mapping it with K
// instead carried the viewmodel's toe-in, a constant angle off the barrel that grew into a miss with distance
bool VrGunAim(float* dir) {
    float yaw, pitch, AC[9], hc[3];
    // kicked, as drawn: the game's direction doesn't carry its kick (logged against its camera: the M249 0.9 deg up
    // at no kick, 0.8 at 3.5), so the unkicked hand put sustained fire under the rising barrel (user: low at range)
    if (!Active() || !VrLookAngles(yaw, pitch) || !HandInCamera(yaw, pitch, 1, AC, hc, true)) return false;
    float c[3], k[3];
    ToCamera(dir, false, c);
    for (int i = 0; i < 3; i++) k[i] = AC[i * 3] * c[0] + AC[i * 3 + 1] * c[1] + AC[i * 3 + 2] * c[2];
    ToWorld(k, false, dir);
    return true;
}

// game thread: the player's rocket (0x1E35F0's &matrix and &velocity, coop_world.cpp) out of the tube in the hand
// along its barrel (user: rockets didn't leave the tube): the matrix moved with the gun to the hand, the velocity
// turned onto the barrel, its speed kept. Copies in our own buffer (the game's may be its own live matrix), the
// arguments pointed there; false leaves them
bool VrLauncherAim(uint32_t& matrix, uint32_t& vel) {
    if (!matrix || !vel) return false;
    float m[16], v[3];
    std::memcpy(m, HostPtr(matrix), sizeof(m));
    std::memcpy(v, HostPtr(vel), sizeof(v));
    float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (speed < 1e-3f || !VrGunMap(m, true)) return false;
    float d[3] = {v[0] / speed, v[1] / speed, v[2] / speed};
    if (!VrGunAim(d)) return false;
    static const uint32_t buf = mem::AllocSystem(96, 16);
    float* h = static_cast<float*>(HostPtr(buf));
    std::memcpy(h, m, sizeof(m));
    for (int i = 0; i < 3; i++) h[16 + i] = d[i] * speed;
    matrix = buf, vel = buf + 64;
    static int logged = 0;
    if (logged++ < 4) LOGR("vr gun: rocket from the tube at %.2f %.2f %.2f, along %.2f %.2f %.2f", m[12], m[13], m[14], d[0], d[1], d[2]);
    return true;
}

}  // namespace game

// The player's bullet (0x1DB7B0; ecx its origin, stack: the gun, the direction, range, speed, flags; its one call
// for the player at 0x1DC0D1, black-seamless-coop's notes): in vr it leaves the muzzle as drawn in the hand, along
// the hand's aim, the game's spread kept. Other callers (co-op's ghosts) are left alone
X86_EXTERN_FUNC(__imp__sub_001DB7B0);
// (co-op: coop_shots.cpp wraps 0x1DB7B0 and calls this in the game's place: co-op, then the hand's aim, then the game)
void VrFireShot(X86Context& ctx, uint8_t* base) {
    using namespace game;
    uint32_t dir = R32(ctx.esp + 8);
    float m[16];
    if (Active() && R32(ctx.esp) == kFromPlayerShot && dir && Muzzle(ctx, m)) {
        float d[16] = {};
        std::memcpy(d, HostPtr(dir), 12);  // the muzzle as the position (moved with the gun), the direction aimed
        std::memcpy(d + 12, m + 12, 12);
        if (VrGunMap(d, false) && VrGunAim(d)) {
            if (g_long_gun.load() && !VrLeftOnGun()) {
                // one-handed: the shot off the barrel's line, anywhere within kOneHandSpread, more toward the middle
                static uint32_t seed = 12345;
                auto rnd = [] { seed = seed * 1664525u + 1013904223u; return float(seed >> 8) / float(1 << 24); };
                float a = rnd() * 6.2831853f, r = std::sqrt(rnd()) * kOneHandSpread * 3.14159265f / 180;
                float up[3] = {0, 1, 0}, s1[3], s2[3];
                if (std::fabs(d[1]) > 0.9f) up[0] = 1, up[1] = 0;
                s1[0] = d[1] * up[2] - d[2] * up[1], s1[1] = d[2] * up[0] - d[0] * up[2], s1[2] = d[0] * up[1] - d[1] * up[0];
                Normalize(s1);
                s2[0] = d[1] * s1[2] - d[2] * s1[1], s2[1] = d[2] * s1[0] - d[0] * s1[2], s2[2] = d[0] * s1[1] - d[1] * s1[0];
                float ca = std::cos(a) * std::sin(r), sa = std::sin(a) * std::sin(r), cr = std::cos(r);
                for (int i = 0; i < 3; i++) d[i] = d[i] * cr + s1[i] * ca + s2[i] * sa;
                Normalize(d);
            }
            static uint32_t buf = mem::AllocSystem(32, 16);
            float o[8] = {d[12], d[13], d[14], G<float>(ctx.ecx + 12), d[0], d[1], d[2], G<float>(dir + 12)};
            std::memcpy(HostPtr(buf), o, sizeof(o));
            ctx.ecx = buf;
            W32(ctx.esp + 8, buf + 16);
            // the kick in the hands, by gun (user: each its own, bigger guns more): its strength and length, a
            // little more as the game's kick builds in sustained fire; the off hand on the gun feels most of it
            Feel f = ShotFeel(weapons::HeldCode());
            float amp = std::min(f.amp + std::fabs(g_kick.load()) * 0.02f, 1.0f);
            xr::Pulse(1, amp, f.secs);
            if (VrLeftOnGun()) xr::Pulse(0, amp * 0.75f, f.secs);
            g_shot_ms.store(SteadyMs());
            static int shots = 0;
            if (shots++ % 50 == 0) LOGR("vr gun: shot from the hand, direction %.2f %.2f %.2f", o[4], o[5], o[6]);
        }
    }
    __imp__sub_001DB7B0(ctx, base);
}

// ---- scopes (user, the design of 2026-11-20): the scope as a lens. Its view is drawn by a third replay of the frame
// (render_thread.cpp, vulkan.cpp: stereo::kScopeEye) from a camera at its eyepiece down the barrel, a narrow field,
// and each eye draws that picture inside the eyepiece's circle (the eyes' filter pass), turned level with the eye,
// a reticle over it, dark when the eye is off the scope's axis. So aiming is bringing the scope to the eye; the
// headset's view never zooms, and the game's own scope (its aim-in zoom and overlay) isn't used in VR. The
// Walther WA2000 (SNR, SN5) is the game's one scoped gun. dev_scope "up back radius [side]" (metres) moves the eyepiece
// for tuning; dev_scope_any draws the lens with the eye off its axis too (tests with dev_fakehands)
namespace game {
namespace {
// the eyepiece's centre from the grip, in the gun's frame: up its y, back along +z, right along x; its glass's
// radius (fitted to the WA2000 in the hand from headset shots, 2026-11-24)
struct ScopeFit {
    float up = 0.14f, back = -0.04f, radius = 0.022f, side = -0.025f;
};
const ScopeFit& Fit() {
    static const ScopeFit f = [] {
        ScopeFit s;
        std::ifstream in(app::ExeDir() / "dev_scope");
        float u, b, r, x;
        if (in >> u >> b >> r) s.up = u, s.back = b, s.radius = r;
        if (in >> x) s.side = x;
        return s;
    }();
    return f;
}
constexpr float kScopeHalfTan = 0.06f;  // the view's half height (about 3.4 degrees: some 4x in the glass)
std::atomic<bool> g_scoped{false};      // a scoped gun in the hand (from the gun's per-frame update)

bool ScopeFrame(float* A, float* eyepiece) {
    if (!g_scoped.load(std::memory_order_relaxed)) return false;
    float p[3];
    {
        std::lock_guard lock(g_scope_lock);
        if (NowMs() - g_scope_ms > 250) return false;
        std::memcpy(A, g_scope_a, sizeof(g_scope_a));
        std::memcpy(p, g_scope_p, sizeof(p));
    }
    const ScopeFit& f = Fit();
    for (int i = 0; i < 3; i++) eyepiece[i] = p[i] + A[i * 3] * f.side + A[i * 3 + 1] * f.up + A[i * 3 + 2] * f.back;
    return true;
}

void QuatOf(const float* m, float* q) {
    // a rotation matrix (rows) to x, y, z, w
    float t = m[0] + m[4] + m[8];
    if (t > 0) {
        float s = std::sqrt(t + 1) * 2;
        q[3] = 0.25f * s, q[0] = (m[7] - m[5]) / s, q[1] = (m[2] - m[6]) / s, q[2] = (m[3] - m[1]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        float s = std::sqrt(1 + m[0] - m[4] - m[8]) * 2;
        q[3] = (m[7] - m[5]) / s, q[0] = 0.25f * s, q[1] = (m[1] + m[3]) / s, q[2] = (m[2] + m[6]) / s;
    } else if (m[4] > m[8]) {
        float s = std::sqrt(1 + m[4] - m[0] - m[8]) * 2;
        q[3] = (m[2] - m[6]) / s, q[0] = (m[1] + m[3]) / s, q[1] = 0.25f * s, q[2] = (m[5] + m[7]) / s;
    } else {
        float s = std::sqrt(1 + m[8] - m[0] - m[4]) * 2;
        q[3] = (m[3] - m[1]) / s, q[0] = (m[2] + m[6]) / s, q[1] = (m[5] + m[7]) / s, q[2] = 0.25f * s;
    }
}
}  // namespace

// the gun's axes (A: columns, -z down the barrel) and its muzzle in local space, as last posed (the silencer)
bool VrGunLocal(float* A, float* muzzle) {
    float p[3];
    {
        std::lock_guard lock(g_scope_lock);
        if (NowMs() - g_scope_ms > 250) return false;
        std::memcpy(A, g_scope_a, sizeof(g_scope_a));
        std::memcpy(p, g_scope_p, sizeof(p));
    }
    auto it = kGrips.find(weapons::HeldCode());
    const Grip& g = it == kGrips.end() ? kDefaultGrip : it->second;
    // the grip sits back and down from the muzzle by kGrips' back and down
    for (int i = 0; i < 3; i++) muzzle[i] = p[i] - A[i * 3 + 2] * g.back + A[i * 3 + 1] * g.down;
    return true;
}

// the reticle's distance down the barrel: what the game's own ray hits there (VrReticleTick), kReticleFar if nothing
constexpr float kReticleFar = 60.0f, kReticleUp = 0.06f;
std::atomic<float> g_ret_range{20.0f};

// game thread, each tick: the barrel's line (from the gun, kReticleFar out) through the game's line test, the one
// that keeps the gun out of walls (0x1DAFF0: eax &from, ecx &to; the world *0x2D45D4, flags, an entity it passes
// through, 0, &result; al set on a hit, the point first in the result, as 0xB7489 reads it). The reticle then sits
// on the wall or floor the shot would hit, at its depth in both eyes (user: the reticle at what the barrel hits)
void VrReticleTick(X86Context& ctx) {
    if (!Opt().vr_reticle) return;
    float A[9], p[3];
    {
        std::lock_guard lock(g_scope_lock);
        if (NowMs() - g_scope_ms > 250) return;
        std::memcpy(A, g_scope_a, sizeof(A));
        std::memcpy(p, g_scope_p, sizeof(p));
    }
    uint32_t world = R32(0x2D45D4);
    if (!world) return;
    float a[3], b[3], wa[3], wb[3];
    for (int i = 0; i < 3; i++) a[i] = p[i] + A[i * 3 + 1] * kReticleUp, b[i] = a[i] - A[i * 3 + 2] * kReticleFar;
    if (!VrLocalToWorld(a, wa) || !VrLocalToWorld(b, wb)) return;
    static const uint32_t buf = mem::AllocSystem(256, 16);
    float* h = static_cast<float*>(HostPtr(buf));
    std::memset(h, 0, 256);
    std::memcpy(h, wa, 12);
    std::memcpy(h + 4, wb, 12);
    uint32_t eax = ctx.eax, ecx = ctx.ecx;
    ctx.eax = buf, ctx.ecx = buf + 16;
    uint32_t hit = CallGuest(ctx, 0x1DAFF0, {world, 1, 0, 0, buf + 32}) & 0xFF;
    ctx.eax = eax, ctx.ecx = ecx;
    float range = kReticleFar;
    if (hit) {
        float lp[3];
        if (VrWorldToLocal(h + 8, lp)) {
            float d[3] = {lp[0] - a[0], lp[1] - a[1], lp[2] - a[2]};
            range = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        }
    }
    g_ret_range.store(std::clamp(range, 0.3f, kReticleFar), std::memory_order_relaxed);
    static const bool down = std::filesystem::exists(app::ExeDir() / "dev_raydown");
    if (down && NowMs() % 4000 < 40) {
        // dev_raydown: the same ray straight down 5 m from the gun (the floor must hit), the line test checked
        std::memset(h, 0, 256);
        std::memcpy(h, wa, 12);
        std::memcpy(h + 4, wa, 12);
        h[5] -= 5;
        ctx.eax = buf, ctx.ecx = buf + 16;
        uint32_t dh = CallGuest(ctx, 0x1DAFF0, {world, 1, 0, 0, buf + 32}) & 0xFF;
        ctx.eax = eax, ctx.ecx = ecx;
        LOGR("vr reticle: down ray %s, result %.2f %.2f %.2f %.2f %.2f %.2f", dh ? "hits" : "misses", h[8], h[9], h[10],
             h[11], h[12], h[13]);
    }
    static int logged = 0;
    if (logged < 6 && NowMs() % 4000 < 40) {
        logged++;
        LOGR("vr reticle: the ray %s at %.1f m (from %.1f %.1f %.1f, result %.1f %.1f %.1f)", hit ? "hits" : "misses",
             range, wa[0], wa[1], wa[2], h[8], h[9], h[10]);
    }
}

// game thread, each tick: how far the gun goes into a wall (user: it pushed through them). The game's line test from
// 30 cm behind the grip (by the chest, outside the wall) to 3 cm past the muzzle; where it hits short of that end,
// the gun is pulled back by what's left, eased so it slides rather than jumps; nothing hit, it eases back out
void VrWallTick(X86Context& ctx) {
    float pull = g_wall_pull.load(std::memory_order_relaxed);
    float A[9], p[3];
    bool posed = false;
    {
        std::lock_guard lock(g_scope_lock);
        posed = NowMs() - g_scope_ms <= 250;
        std::memcpy(A, g_scope_a, sizeof(A));
        std::memcpy(p, g_scope_p, sizeof(p));
    }
    uint32_t world = R32(0x2D45D4);
    if (!Opt().vr_wall_pull || !posed || !world || VrMeleeHidden()) {
        g_wall_pull.store(pull * 0.8f < 0.002f ? 0.0f : pull * 0.8f, std::memory_order_relaxed);
        return;
    }
    auto it = kGrips.find(weapons::HeldCode());
    const Grip& g = it == kGrips.end() ? kDefaultGrip : it->second;
    constexpr float kBehind = 0.3f, kPast = 0.03f;
    float s[3], e[3], ws[3], we[3];
    for (int i = 0; i < 3; i++) {
        float raw = p[i] - A[i * 3 + 2] * pull;  // the grip as the hand holds it, before this pull
        s[i] = raw + A[i * 3 + 2] * kBehind;
        e[i] = raw - A[i * 3 + 2] * (g.back + kPast);
    }
    if (!VrLocalToWorld(s, ws) || !VrLocalToWorld(e, we)) return;
    static const uint32_t buf = mem::AllocSystem(256, 16);
    float* h = static_cast<float*>(HostPtr(buf));
    std::memset(h, 0, 256);
    std::memcpy(h, ws, 12);
    std::memcpy(h + 4, we, 12);
    uint32_t eax = ctx.eax, ecx = ctx.ecx;
    ctx.eax = buf, ctx.ecx = buf + 16;
    bool hit = CallGuest(ctx, 0x1DAFF0, {world, 1, 0, 0, buf + 32}) & 0xFF;
    ctx.eax = eax, ctx.ecx = ecx;
    float want = 0;
    if (hit) {
        float lp[3];
        if (VrWorldToLocal(h + 8, lp)) {
            float d[3] = {lp[0] - s[0], lp[1] - s[1], lp[2] - s[2]};
            float at = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            want = std::clamp(kBehind + g.back + kPast - at, 0.0f, g.back + kPast);
        }
    }
    // in quickly (no frame through the wall), out slowly
    float next = want > pull ? pull + (want - pull) * 0.6f : pull + (want - pull) * 0.25f;
    if (next < 0.002f) next = 0;
    g_wall_pull.store(next, std::memory_order_relaxed);
    static bool was = false;
    if ((next > 0.02f) != was) {
        was = next > 0.02f;
        if (was) {
            xr::Pulse(1, 0.2f, 0.02f);  // the gun touching the wall
            TestEvent("wall_pull");
        }
        LOGR("vr gun: %s a wall (%.2f m back)", was ? "pulled back from" : "clear of", next);
    }
}

// the barrel's reticle (user): where the barrel's line meets the world (VrReticleTick), in each eye; off with no gun
// in the hand, a melee swing on, or RETICLE off
bool VrReticle(int eye, float* ret) {
    if (!Opt().vr_reticle || VrMeleeHidden()) return false;
    float A[9], p[3];
    {
        std::lock_guard lock(g_scope_lock);
        if (NowMs() - g_scope_ms > 250) return false;
        std::memcpy(A, g_scope_a, sizeof(A));
        std::memcpy(p, g_scope_p, sizeof(p));
    }
    xr::EyeView ev;
    if (!xr::RenderEye(eye, ev)) return false;
    const float kRange = g_ret_range.load(std::memory_order_relaxed), kUp = kReticleUp;
    constexpr float kRadius = 0.0095f;  // tan of its radius (about half a degree)
    float pt[3];
    for (int i = 0; i < 3; i++) pt[i] = p[i] + A[i * 3 + 1] * kUp - A[i * 3 + 2] * kRange;
    const float* q = ev.quat;
    float xx = q[0] * q[0], yy = q[1] * q[1], zz = q[2] * q[2];
    float xy = q[0] * q[1], xz = q[0] * q[2], yz = q[1] * q[2], wx = q[3] * q[0], wy = q[3] * q[1], wz = q[3] * q[2];
    float R[9] = {1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy), 2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx),
                  2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy)};
    float d[3] = {pt[0] - ev.pos[0], pt[1] - ev.pos[1], pt[2] - ev.pos[2]}, c[3];
    for (int i = 0; i < 3; i++) c[i] = R[0 * 3 + i] * d[0] + R[1 * 3 + i] * d[1] + R[2 * 3 + i] * d[2];
    if (c[2] > -0.2f) return false;
    float tx = c[0] / -c[2], ty = c[1] / -c[2];
    float l = ev.tan[0], r = ev.tan[1], u = ev.tan[2], dn = ev.tan[3];
    ret[0] = (tx - l) / (r - l);
    ret[1] = (u - ty) / (u - dn);
    ret[2] = kRadius / (r - l);
    ret[3] = kRadius / (u - dn);
    return ret[0] > -0.1f && ret[0] < 1.1f && ret[1] > -0.1f && ret[1] < 1.1f;
}

void VrScopeHeld(const std::string& code) { g_scoped.store(code == "SNR" || code == "SN5", std::memory_order_relaxed); }

bool VrScopeWanted() {
    float A[9], e[3];
    xr::EyeView ev;
    if (!ScopeFrame(A, e) || !xr::RenderEye(0, ev)) return false;
    float d[3] = {e[0] - ev.pos[0], e[1] - ev.pos[1], e[2] - ev.pos[2]};
    return d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 0.5f * 0.5f;  // (within half a metre of the head)
}

bool VrScopeEye(xr::EyeView& ev) {
    float A[9], e[3];
    if (!ScopeFrame(A, e)) return false;
    std::memcpy(ev.pos, e, sizeof(e));
    QuatOf(A, ev.quat);
    // the game's frame is 4:3: as wide again by that, square pixels (the glass uses its middle square)
    ev.tan[0] = -kScopeHalfTan * 4 / 3, ev.tan[1] = kScopeHalfTan * 4 / 3;
    ev.tan[2] = kScopeHalfTan, ev.tan[3] = -kScopeHalfTan;
    return true;
}

bool VrScopeLens(int eye, float* lens) {
    float A[9], e[3];
    xr::EyeView ev;
    if (!ScopeFrame(A, e) || !xr::RenderEye(eye, ev)) return false;
    // the eye's axes (columns), the eyepiece in them
    const float* q = ev.quat;
    float xx = q[0] * q[0], yy = q[1] * q[1], zz = q[2] * q[2];
    float xy = q[0] * q[1], xz = q[0] * q[2], yz = q[1] * q[2], wx = q[3] * q[0], wy = q[3] * q[1], wz = q[3] * q[2];
    float R[9] = {1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy), 2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx),
                  2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy)};
    float d[3] = {e[0] - ev.pos[0], e[1] - ev.pos[1], e[2] - ev.pos[2]}, c[3];
    for (int i = 0; i < 3; i++) c[i] = R[0 * 3 + i] * d[0] + R[1 * 3 + i] * d[1] + R[2 * 3 + i] * d[2];
    if (c[2] > -0.02f) return false;  // behind the eye, or at it
    float tx = c[0] / -c[2], ty = c[1] / -c[2], tr = Fit().radius / -c[2];
    float l = ev.tan[0], r = ev.tan[1], u = ev.tan[2], dn = ev.tan[3];
    lens[0] = (tx - l) / (r - l);
    lens[1] = (u - ty) / (u - dn);
    lens[2] = tr / (r - l);
    lens[3] = tr / (u - dn);
    // off the axis: the eye seen from the eyepiece against the scope's back (+z), dark past about 12 degrees
    float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    float along = -(d[0] * A[2] + d[1] * A[5] + d[2] * A[8]) / len;
    static const bool any = std::filesystem::exists(app::ExeDir() / "dev_scope_any");
    float off = std::acos(std::clamp(along, -1.0f, 1.0f)) * 180 / 3.14159265f;
    lens[4] = any ? 0.0f : std::clamp((off - 5) / 7, 0.0f, 1.0f);
    // the gun's x axis in the eye: its roll against the eye's
    float gx[3];
    for (int i = 0; i < 3; i++) gx[i] = R[0 * 3 + i] * A[0] + R[1 * 3 + i] * A[3] + R[2 * 3 + i] * A[6];
    float roll = std::atan2(gx[1], gx[0]);
    lens[6] = std::cos(roll), lens[7] = std::sin(roll);
    return true;
}
}  // namespace game
