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
// vr_reload.cpp: the magazine
int VrMagState();
int VrMagBone();
float VrSlide();
bool VrGunParts(int& mag, int& slide, float& travel, float* mid, float* up, bool& measure, float& top, float& bottom,
                int& bullets);
bool VrRacking();
bool VrRackHeld();
float VrMagOutSeconds();
void VrMagWell(const float* c, const float* axis, float top, float bottom);
bool VrRackPart(int& bone, float* pt, float* to);
void VrRackAt(const float* c, const float* to, const float* up, const float* fwd);
float VrMagInsert();
void VrMagThrow(float* v);
bool VrMagInHand();
void VrReloadTick();
bool VrMagFromHand();
float VrMagHold();
float VrRodTilt();

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
        if (h == 0) set(9, 12, -1, 15, 18, 60), a.twist[0] = 0.5f, a.twist[1] = 0;  // the forearm takes the twist
        else set(63, 66, -1, 69, 72, 123);
    } else if (code == "MGN" || code == "MG5") {
        if (h == 0) set(30, 33, 36, 39, 42, 84);
        else set(87, 90, -1, 93, 96, 129);
    }
    return a;
}
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
// the walther's bolt is the player's (user): the game's cycling after each shot (its slide back and forward, a hand
// on it) isn't drawn. While its slide bone is off its rest (on the gun's root, as every gun part at rest) the slide
// is put back on the root, and the arms keep their last pose on the gun (relative to its root)
bool BoltGun(const std::string& code) { return code == "SNR" || code == "SN5"; }
constexpr int kBoltRoot = 75, kBoltSlide = 84;
bool Cycling(const float (*c)[4]) {
    Aff r = Bone(c, kBoltRoot), sl = Bone(c, kBoltSlide);
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
void CurlFingers(float (*c)[4], const ArmRig& a, const HandFingers& hf, const float* curl, bool on_gun) {
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
    std::string code;
    {
        std::lock_guard lock(g_lock);
        code = g_code;
    }
    const ArmRig rig[2] = {RigOf(code, 0), RigOf(code, 1)};
    // the walther cycling: its arms as they last were on the gun (bones 9 on, relative to the gun's root)
    if (BoltGun(code)) {
        static Aff rel[44];
        static bool have = false;
        static std::string for_code;
        if (for_code != code) have = false, for_code = code;
        Aff root = Bone(c, kBoltRoot);
        if (!Cycling(c)) {
            Aff inv = Inverse(root);
            for (int k = 3; k < 44; k++) rel[k] = Compose(inv, Bone(c, 3 * k));
            have = true;
        } else if (have) {
            for (int k = 3; k < 44; k++) SetBone(c, 3 * k, Compose(root, rel[k]));
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
                bool held = h == 1 ? !g_gun_fingers_open.load(std::memory_order_relaxed)
                                   : g_left_on.load(std::memory_order_relaxed) || VrMagInHand() || VrRackHeld();
                if (fake[0] >= 0) held = false;
                float want[5] = {th, idx, g, g, g};
                if (held) std::fill(want, want + 5, 1.0f);
                for (int i = 0; i < 5; i++) eased[h][i] += (want[i] - eased[h][i]) * k;
                CurlFingers(c, rig[h], found->second[h], eased[h], held || h == 1);
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
void SetMag(float (*c)[4], int bone, int bullets, const Aff& b) {
    SetBone(c, bone, b);
    if (bullets >= 0) SetBone(c, bullets, b);
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
// person gun never showing its stock; in vr it's in view, so one is added (user: from photos of a carbine's
// collapsible stock and the M249's fixed one). Real size, in the gun's mesh (metres; z to the muzzle, y up), on its
// root bone, coloured from the texture of the gun's own black pistol grip; drawn as a second draw of the gun's
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
void StockVertex(StockMesh& m, const float* p, const float* n, int bone, float u, float v) {
    uint8_t b[36] = {};
    std::memcpy(b, p, 12);
    b[12] = 255;
    int16_t ix[4] = {int16_t(bone), 0, 0, 0};
    std::memcpy(b + 16, ix, 8);
    uint32_t pn = PackNormal(n);
    std::memcpy(b + 24, &pn, 4);
    float uv[2] = {u, v};
    std::memcpy(b + 28, uv, 8);
    m.verts.insert(m.verts.end(), b, b + 36);
}
// a flat face (3+ points, convex or star shaped from its middle), both windings (the gun draw's culling either way)
void StockFace(StockMesh& m, const std::vector<std::array<float, 3>>& pts, const float* n, int bone, float u, float v) {
    uint16_t at = uint16_t(m.verts.size() / 36);
    float mid[3] = {};
    for (auto& q : pts)
        for (int i = 0; i < 3; i++) mid[i] += q[i] / float(pts.size());
    StockVertex(m, mid, n, bone, u, v);
    for (auto& q : pts) StockVertex(m, q.data(), n, bone, u, v);
    for (uint16_t k = 0; k < pts.size(); k++) {
        uint16_t a = uint16_t(at + 1 + k), b = uint16_t(at + 1 + (k + 1) % pts.size());
        m.idx.insert(m.idx.end(), {at, a, b, at, b, a});
    }
}
// a side profile (z, y) extruded across x (-w..w): its two sides and the rim round it
void StockSlab(StockMesh& m, const std::vector<std::array<float, 2>>& zy, float w, int bone, float u, float v) {
    std::vector<std::array<float, 3>> l, r;
    for (auto& q : zy) l.push_back({-w, q[1], q[0]}), r.push_back({w, q[1], q[0]});
    const float nl[3] = {-1, 0, 0}, nr[3] = {1, 0, 0};
    StockFace(m, l, nl, bone, u, v);
    StockFace(m, r, nr, bone, u, v);
    float cz = 0, cy = 0;
    for (auto& q : zy) cz += q[0] / zy.size(), cy += q[1] / zy.size();
    for (size_t k = 0; k < zy.size(); k++) {
        const auto& a = zy[k];
        const auto& b = zy[(k + 1) % zy.size()];
        // the rim's normal: square to the edge in the profile, away from its middle
        float n[3] = {0, b[0] - a[0], -(b[1] - a[1])};
        Normalize(n);
        float mz = (a[0] + b[0]) / 2 - cz, my = (a[1] + b[1]) / 2 - cy;
        if (n[2] * mz + n[1] * my < 0)
            for (float& x : n) x = -x;
        StockFace(m, {{-w, a[1], a[0]}, {w, a[1], a[0]}, {w, b[1], b[0]}, {-w, b[1], b[0]}}, n, bone, u, v);
    }
}
// a tube along z (eight sides), its back end closed
void StockTube(StockMesh& m, float y, float rad, float z0, float z1, int bone, float u, float v) {
    constexpr int kSides = 8;
    std::vector<std::array<float, 3>> e1;
    for (int k = 0; k < kSides; k++) {
        float a0 = 2 * 3.14159265f * k / kSides, a1 = 2 * 3.14159265f * (k + 1) / kSides, am = (a0 + a1) / 2;
        float n[3] = {std::cos(am), std::sin(am), 0};
        StockFace(m, {{rad * std::cos(a0), y + rad * std::sin(a0), z0}, {rad * std::cos(a1), y + rad * std::sin(a1), z0},
                      {rad * std::cos(a1), y + rad * std::sin(a1), z1}, {rad * std::cos(a0), y + rad * std::sin(a0), z1}},
                  n, bone, u, v);
        e1.push_back({rad * std::cos(a0), y + rad * std::sin(a0), z1});
    }
    const float nb[3] = {0, 0, -1};
    StockFace(m, e1, nb, bone, u, v);
}
const StockMesh* StockOf(const std::string& code) {
    // m16 (receiver's back at z -0.12, the bore at y 0.071): the buffer tube on the bore's line, the carbine's
    // stock slid over its back (14 cm, 4.4 cm wide, the butt 15 cm tall)
    static const StockMesh m16 = [] {
        StockMesh m;
        const float u = 0.0817f, v = -0.1590f;  // the pistol grip's black
        StockTube(m, 0.066f, 0.0155f, -0.115f, -0.33f, 75, u, v);
        StockSlab(m, {{{-0.215f, 0.086f}, {-0.355f, 0.090f}, {-0.355f, -0.062f}, {-0.335f, -0.062f},
                       {-0.245f, 0.045f}, {-0.215f, 0.048f}}},
                  0.022f, 75, u, v);
        return m;
    }();
    // m249 (receiver's back at z -0.14, its top y 0.105): the fixed stock, its top on the receiver's line, its
    // underside sweeping down to a 17 cm butt 30 cm back
    static const StockMesh m249 = [] {
        StockMesh m;
        const float u = 0.4785f, v = -0.0369f;
        StockSlab(m, {{{-0.125f, 0.104f}, {-0.435f, 0.094f}, {-0.435f, -0.076f}, {-0.405f, -0.078f},
                       {-0.215f, 0.000f}, {-0.125f, 0.022f}}},
                  0.021f, 90, u, v);
        return m;
    }();
    if (code == "M16" || code == "M45" || code == "BNS") return &m16;
    if (code == "HVY" || code == "HV5") return &m249;
    return nullptr;
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
    if (BoltGun(HeldGunCode()) && Cycling(c)) SetBone(c, kBoltSlide, Bone(c, kBoltRoot));
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
    // the slide, held back by a lock or a hand: its bone back along the mesh's -z
    if (slide >= 0 && VrSlide() > 0.001f) {
        Aff back = kIdentityAff;
        back.m[11] = -travel * VrSlide();
        SetBone(c, slide, Compose(Bone(c, slide), back));
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
    float mid[3], up[3];
    Apply(rest, mid_m, mid);
    RotOf(rest, up_m, up);
    Normalize(up);
    static Aff last_gun = kIdentityAff;  // the gun's magazine where it was last drawn (in the gun, or sliding in)
    // the well where the gun is now (the game still has the magazine in it), in or out of it
    VrMagWell(mid, up, use ? shape.top : mag_top, use ? shape.bottom : mag_bottom);
    int state = VrMagState();
    if (state == 0 || state == 3) {
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
        AxisAngle(side, -kRockIn * std::clamp(along / 0.06f, 0.0f, 1.0f), R);
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

// render thread: the stock drawn with this gun draw, if the held gun gets one and this is the draw its pistol grip is
// in (the stock takes that texture's black): vertices as the gun's (36 bytes), 16 bit indices
bool VrStock(const uint8_t* vb, uint32_t stride, uint32_t vbytes, const void* indices, bool index16, uint32_t count,
             int32_t base, const uint8_t*& verts, uint32_t& vbytes_out, const uint16_t*& idx, uint32_t& icount) {
    if (stride != 36 || !vb || !indices) return false;
    std::string code = HeldGunCode();
    const StockMesh* m = StockOf(code);
    if (!m) return false;
    static std::map<std::pair<std::string, uint32_t>, bool> has_grip;
    auto key = std::make_pair(code, count);
    auto it = has_grip.find(key);
    if (it == has_grip.end()) {
        int root = code == "HVY" || code == "HV5" ? 90 : 75;
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
    verts = m->verts.data(), vbytes_out = uint32_t(m->verts.size());
    idx = m->idx.data(), icount = uint32_t(m->idx.size());
    return true;
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
    for (int k = 0; k + 2 < 132; k += 3) SetBone(c, k, k == bone || k == bullets ? b : point);
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
    Aff rest = Compose(D, Bone(c, bone));
    Aff b = Compose(Dinv, HandMag(yaw, pitch, rest));
    Aff point{};
    for (int i = 0; i < 3; i++) point.m[i * 4 + 3] = Bone(c, bone).m[i * 4 + 3];
    for (int k = 0; k + 2 < 132; k += 3) SetBone(c, k, k == bone || k == bullets ? b : point);
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
    if (VrMagInHand() || VrRacking()) on = false, near = false;  // a magazine in the off hand: it can't hold the gun as well
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
    VrReloadTick();
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
    // kicked, as drawn: the game's direction doesn't carry its kick (logged against its camera: the M249 0.9 deg up
    // at no kick, 0.8 at 3.5), so the unkicked hand put sustained fire under the rising barrel (user: low at range)
    if (!Active() || !VrLookAngles(yaw, pitch) || !HandInCamera(yaw, pitch, 1, AC, hc, true)) return false;
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
