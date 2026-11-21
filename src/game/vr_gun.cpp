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
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "app/app.h"
#include "common/options.h"
#include "core/log.h"
#include "d3d/stereo.h"
#include "game/layout.h"
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
float VrViewLift();                           // vr_body.cpp
bool VrLookAngles(float& yaw, float& pitch);  // vr_body.cpp
bool VrMeleeHidden();                          // vr_body.cpp

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
    {"SMG", {0.17f, 0.10f, 0.08f, true}},  {"SM5", {0.17f, 0.10f, 0.08f, true}},
    {"SM3", {0.14f, 0.09f, 0.06f, true}},  {"MC5", {0.14f, 0.09f, 0.06f, true}},
    {"MP1", {0.30f, 0.08f, 0.15f, true}},  {"MP5", {0.36f, 0.08f, 0.20f, true}},
    {"P90", {0.25f, 0.08f, 0.08f, true}},  {"P95", {0.25f, 0.08f, 0.08f, true}},
    {"AK1", {0.68f, 0.08f, 0.25f, true}},  {"AK5", {0.68f, 0.08f, 0.25f, true}},
    {"ASR", {0.38f, 0.08f, 0.20f, true}},  {"AS5", {0.38f, 0.08f, 0.20f, true}},
    {"M16", {0.50f, 0.08f, 0.28f, true}},  {"M45", {0.50f, 0.08f, 0.28f, true}},
    {"BNS", {0.50f, 0.08f, 0.28f, true}},  {"SHG", {0.55f, 0.08f, 0.30f, true}},
    {"SH5", {0.55f, 0.08f, 0.30f, true}},  {"RM1", {0.58f, 0.08f, 0.30f, true}},
    {"RM5", {0.58f, 0.08f, 0.30f, true}},  {"SNR", {0.45f, 0.08f, 0.20f, true}},
    {"SN5", {0.50f, 0.08f, 0.25f, true}},  {"HVY", {0.55f, 0.08f, 0.30f, true}},
    {"HV5", {0.55f, 0.08f, 0.30f, true}},  {"RPG", {0.45f, 0.10f, 0.20f, true}},
    {"GRL", {0.30f, 0.10f, 0.12f, true}},
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
std::atomic<float> g_kick{0};  // degrees, the game's latest (game thread), read by the render thread too
// the left hand on the gun: gripped at the fore-grip (or the gun hand, on a pistol), the left arm drawn where the
// game holds it; off it, the left arm follows the left controller
std::atomic<bool> g_left_on{false};
constexpr float kReach = 0.15f;  // metres from the fore-grip a grip press takes hold
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
        // dev_fakehands_side: the gun hand turned to point left across the view, the gun seen side on
        static const bool side = std::filesystem::exists(app::ExeDir() / "dev_fakehands_side");
        if (side && h == 1) {
            // A · Ry(90°): x' = -z, z' = x
            for (int i = 0; i < 3; i++) {
                float x = r[i * 3], z = r[i * 3 + 2];
                A[i * 3] = -z, A[i * 3 + 2] = x;
            }
        }
        // dev_fakeleft_low: the left hand down by the hip, off the gun
        static const bool low = std::filesystem::exists(app::ExeDir() / "dev_fakeleft_low");
        const float off[2][3] = {{low ? -0.30f : -0.10f, low ? -0.50f : -0.14f, low ? -0.20f : -0.55f},
                                 {side ? 0.10f : 0.03f, -0.10f, -0.45f}};
        for (int i = 0; i < 3; i++) pos[i] = head[i] + r[i * 3] * off[h][0] + r[i * 3 + 1] * off[h][1] + r[i * 3 + 2] * off[h][2];
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

// a hand's aim orientation and grip position in C, for the head angles the camera was built from. kick: the gun hand
// with the recoil on it. With the off hand on a long gun, the gun hand's barrel turns to point at it
bool HandInCamera(float yaw, float pitch, int hand, float* AC, float* hc, bool kick = true) {
    float A[9], hp[3];
    if (!HandPose(hand, A, hp)) return false;
    float L[9], lp[3];
    if (hand == 1 && g_two_aim.load(std::memory_order_relaxed) && HandPose(0, L, lp)) {
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
    float deg = std::min(std::fabs(g_kick.load(std::memory_order_relaxed)), kKickMax);
    if (kick && hand == 1 && deg > 0.01f) {
        // A · Rx(θ): the barrel (−z) up toward +y about the grip, then back along the barrel
        float t = deg * kKickTurn * 3.14159265f / 180, c = std::cos(t), sn = std::sin(t);
        for (int i = 0; i < 3; i++) {
            float y = A[i * 3 + 1], z = A[i * 3 + 2];
            A[i * 3 + 1] = y * c + z * sn;
            A[i * 3 + 2] = -y * sn + z * c;
        }
        for (int i = 0; i < 3; i++) hp[i] += A[i * 3 + 2] * deg * kKickBack;
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
const ArmRig kRig[2] = {
    {9, {12, 15}, 18, 21, 63, {0.128f, -0.004f, -0.107f}, {0.116f, 0.005f, 0.137f}, {0.117f, -0.003f, 0.204f}, -1,
     {0.3f, 0.8f}},
    {66, {69, -1}, 72, 75, 129, {-0.128f, -0.004f, -0.107f}, {-0.118f, -0.015f, 0.145f}, {-0.118f, -0.003f, 0.205f}, 1,
     {0.5f, 0}},
};
// the free off hand's roll about its controller's pointing axis (user: 45° clockwise as seen down the arm)
constexpr float kFreeHandRoll = -45.0f * 3.14159265f / 180;
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
std::map<uint32_t, std::array<HandFingers, 2>> g_fingers;  // by the arms mesh's index count (render thread)

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

void FindFingers(const uint8_t* vb, uint32_t stride, const void* indices, bool index16, uint32_t count, int32_t base,
                 std::array<HandFingers, 2>& out) {
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
        const ArmRig& a = kRig[h];
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
void CurlFingers(float (*c)[4], const HandFingers& hf, int h, const float* curl, bool on_gun) {
    const ArmRig& a = kRig[h];
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
    if (sum < 0)
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
    // the fingers, from the controllers, before anything moves the hands
    auto found = g_fingers.find(index_count);
    if (found == g_fingers.end() && vb && indices) {
        std::array<HandFingers, 2> hf;
        FindFingers(vb, stride, indices, index16, index_count, base_vertex, hf);
        found = g_fingers.emplace(index_count, hf).first;
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
                bool held = h == 1 ? !g_gun_fingers_open.load(std::memory_order_relaxed)
                                   : g_left_on.load(std::memory_order_relaxed);
                if (fake[0] >= 0) held = false;
                float want[5] = {th, idx, g, g, g};
                if (held) std::fill(want, want + 5, 1.0f);
                for (int i = 0; i < 5; i++) eased[h][i] += (want[i] - eased[h][i]) * k;
                CurlFingers(c, found->second[h], h, eased[h], held || h == 1);
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
            Apply(Bone(c, kRig[h].hand), kRig[h].palm, rest[h]);
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
    }
    for (int h = 0; h < 2; h++) {
        const ArmRig& a = kRig[h];
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
        // elbows down and out, a little back (C is the head's view at simulation: x right, y up, z back)
        float pole[3] = {a.side * 0.6f, -1, 0.3f};
        float pd = pole[0] * d[0] + pole[1] * d[1] + pole[2] * d[2];
        for (int i = 0; i < 3; i++) pole[i] -= d[i] * pd;
        Normalize(pole);
        float e1[3];
        for (int i = 0; i < 3; i++) e1[i] = s1[i] + d[i] * along + pole[i] * out;
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
    return true;
}

// game thread: take or let go of the gun with the off hand: a grip press within reach of where it holds the gun
// takes hold; letting go of the grip lets go, or with toggle grip the next press. On a long gun, holding it aims the
// barrel along the line between the hands
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
        for (int i = 0; i < 3; i++) {
            float at = rot[i * 3] * fore[0] + rot[i * 3 + 1] * fore[1] + rot[i * 3 + 2] * fore[2] + pos[i];
            d2 += (at - hc[i]) * (at - hc[i]);
        }
        near = d2 < kReach * kReach;
        float g = hand[0].grip;
        bool press = g > 0.6f && !was;
        was = g > 0.6f;
        // hold (the default): held while the grip is; toggle (VR settings): the next press lets go
        bool toggle = Opt().vr_grip_toggle;
        if (on && (toggle ? press : g < 0.4f)) {
            on = false;
        } else if (!on && press && near) {
            on = true;
            xr::Pulse(0, 0.5f, 0.04f);
        }
        static const bool fake_on = std::filesystem::exists(app::ExeDir() / "dev_lefton");  // held on, no hands
        if (fake_on) on = true;
    } else {
        on = false;
    }
    g_left_on.store(on);
    g_two_aim.store(on && f.fore >= 0.05f);  // a pistol's hands are too close to aim by
    xr::TakeLeftGrip(on || near);
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
    auto it = kGrips.find(code);
    const Grip& g = it == kGrips.end() ? kDefaultGrip : it->second;
    for (int i = 0; i < 3; i++) f.grip[i] = mc[i] - f.fwd[i] * g.back - f.up[i] * g.down;
    f.fore = g.back - g.fore;
    static const bool dev_two = std::filesystem::exists(app::ExeDir() / "dev_twohand");  // split any gun's arms
    f.flags = 1 + (g.two || dev_two ? 2 : 0);
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
    // the recoil, and its strongest per gun in the log for tuning
    float kick = G<float>(Player() + kRecoilKick);
    if (!std::isfinite(kick)) kick = 0;
    g_kick.store(kick, std::memory_order_relaxed);
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

// game thread: the game's shot direction (from the camera along its view, spread added) turned from the head's view
// to the gun hand's: the drawn barrel is the hand's forward, so the shot follows it at any range. Mapping it with K
// instead carried the viewmodel's toe-in, a constant angle off the barrel that grew into a miss with distance
bool VrGunAim(float* dir) {
    float yaw, pitch, AC[9], hc[3];
    // unkicked: the game's direction already carries its kick
    if (!Active() || !VrLookAngles(yaw, pitch) || !HandInCamera(yaw, pitch, 1, AC, hc, false)) return false;
    float c[3], k[3];
    ToCamera(dir, false, c);
    for (int i = 0; i < 3; i++) k[i] = AC[i * 3] * c[0] + AC[i * 3 + 1] * c[1] + AC[i * 3 + 2] * c[2];
    ToWorld(k, false, dir);
    return true;
}

}  // namespace game

// The player's bullet (0x1DB7B0; ecx its origin, stack: the gun, the direction, range, speed, flags; its one call
// for the player at 0x1DC0D1, black-seamless-coop's notes): in vr it leaves the muzzle as drawn in the hand, along
// the hand's aim, the game's spread kept. Other callers (co-op's ghosts) are left alone
X86_EXTERN_FUNC(__imp__sub_001DB7B0);
X86_FUNC(sub_001DB7B0) {
    using namespace game;
    uint32_t dir = R32(ctx.esp + 8);
    float m[16];
    if (Active() && R32(ctx.esp) == kFromPlayerShot && dir && Muzzle(ctx, m)) {
        float d[16] = {};
        std::memcpy(d, HostPtr(dir), 12);  // the muzzle as the position (moved with the gun), the direction aimed
        std::memcpy(d + 12, m + 12, 12);
        if (VrGunMap(d, false) && VrGunAim(d)) {
            static uint32_t buf = mem::AllocSystem(32, 16);
            float o[8] = {d[12], d[13], d[14], G<float>(ctx.ecx + 12), d[0], d[1], d[2], G<float>(dir + 12)};
            std::memcpy(HostPtr(buf), o, sizeof(o));
            ctx.ecx = buf;
            W32(ctx.esp + 8, buf + 16);
            // the kick in the hands: a buzz on each shot, stronger the more the gun kicks (both hands on a two
            // handed gun with the left hand on it)
            float amp = std::min(0.45f + std::fabs(g_kick.load()) * 0.08f, 1.0f);
            xr::Pulse(1, amp, 0.06f);
            if (VrLeftOnGun()) xr::Pulse(0, amp * 0.7f, 0.06f);
            static int shots = 0;
            if (shots++ % 50 == 0) LOGR("vr gun: shot from the hand, direction %.2f %.2f %.2f", o[4], o[5], o[6]);
        }
    }
    __imp__sub_001DB7B0(ctx, base);
}
