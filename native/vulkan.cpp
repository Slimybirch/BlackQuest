// The title's Direct3D 8 calls drawn with Vulkan 1.1, dynamic rendering and a timeline semaphore. The SDK's
// translated shaders are HLSL, which glslang turns into SPIR-V (vk_shader.cpp).
#include <deque>
#include <condition_variable>
#include <zlib.h>
#include <vulkan/vulkan.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <thread>
#include <vector>

#include <dxgiformat.h>
#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

#include "app/app.h"
#include "common/aspect.h"
#include "common/menu.h"
#include "common/options.h"
#include "core/log.h"
#include "d3d/gpu_frame.h"
#include "d3d/stereo.h"
#include "gpu/backend.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "host.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "vk_shader.h"
#include "xr.h"

namespace app {
void SetPanelPass(bool on);  // src/common/menu.cpp: imgui is drawing the settings panel's own image
}
namespace game {
float VrViewLift();  // src/game/vr_body.cpp
bool VrGunPose(float yaw, float pitch, const float* frame, int hand, float* rot, float* pos);  // src/game/vr_gun.cpp
bool VrPoseArms(float yaw, float pitch, const float* frame, float (*c)[4], const uint8_t* vb, uint32_t stride,
                const void* indices, bool index16, uint32_t index_count, int32_t base_vertex);  // vr_gun.cpp: own hands
bool VrMeleeHidden();  // src/game/vr_body.cpp: a melee swing is on
bool VrStock(const uint8_t* vb, uint32_t stride, uint32_t vbytes, const void* indices, bool index16, uint32_t count,
             int32_t base, bool strip, const uint8_t*& verts, uint32_t& vbytes_out, const uint16_t*& idx,
             uint32_t& icount);
bool VrMirror(const uint8_t* vb, uint32_t stride, uint32_t vbytes, const void* indices, bool index16, uint32_t count,
              int32_t base, bool strip, const uint8_t*& verts, uint32_t& vbytes_out, const void*& idx,
              uint32_t& icount);  // vr_gun.cpp
void VrDumpGunTexture(uint32_t key, const uint8_t* px, uint32_t pitch, uint32_t w, uint32_t h);  // vr_gun.cpp
bool VrMenuOverPlay();  // vr_body.cpp: the game paused over play (its pause menu)
float VrVignette();     // vr_body.cpp: the comfort vignette's darkening now, 0..1
bool VrScopeWanted();                    // vr_gun.cpp: a scoped gun held up near the eye
bool VrScopeEye(xr::EyeView& ev);        // vr_gun.cpp: the scope's view (its camera at the eyepiece, down the barrel)
bool VrScopeLens(int eye, float* lens);  // vr_gun.cpp: where its glass is in an eye's image (the filter's lens, lens2)
bool VrReticle(int eye, float* ret);     // vr_gun.cpp: where the barrel points in an eye's image (the filter's ret)
bool VrSilencerPose(float* rear, float* front, float* radius);  // vr_silencer.cpp: our silencer, local space
int VrSilencerIcon();  // vr_silencer.cpp: -1 hidden, 0 dark (none in the inventory), 1 bright (one carried)
// the held gun's stock texture painted from its own on a thread (start once a key; take: 0 baking, 1 done, -1 none)
void VrStockBakeStart(uint32_t key, std::vector<uint8_t> tex, uint32_t pitch, uint32_t tw, uint32_t th);
int VrStockBakeTake(uint32_t key, std::vector<uint8_t>& out, uint32_t& w, uint32_t& h);
void VrHudWanted(bool& card, bool& tip);  // vr_totest.cpp: the checklist's card, the off hand's hint, this frame
void VrTotestCardPass(bool on);   // the imgui frame being drawn is the card's
bool VrStockShell();  // the extra mesh is the held shotgun's shell (drawn in the hand too)
bool VrGrenadeInHand();  // vr_grenade.cpp: a hand grenade held (the extra mesh is it)
int64_t VrThrownMs();    // vr_grenade.cpp: steady ms of the last throw (dev_drawdiff)
bool VrM67World(const uint8_t*& verts, uint32_t& bytes, const uint16_t*& idx, uint32_t& count);  // vr_gun.cpp
bool VrOwnTexture(int id, std::vector<uint8_t>& px, uint32_t& w, uint32_t& h);  // vr_gun.cpp: 1 M67, 2 M433
int VrExtraOwn();        // vr_gun.cpp: the extra mesh's own texture now (0 none)
bool VrPoseGun(float yaw, float pitch, const float* frame, float (*c)[4], const uint8_t* vb, uint32_t stride,
               uint32_t vbytes, const void* indices, bool index16, uint32_t count, int32_t base);  // vr_gun.cpp
bool VrRecording();    // src/game/vr_body.cpp: dev_dumpreload's window
bool VrHandMag(float yaw, float pitch, const float* frame, float (*c)[4]);  // vr_gun.cpp: the magazine in hand
bool VrDropMag(float yaw, float pitch, const float* frame, float (*c)[4]);  // vr_gun.cpp: a dropped one
}

namespace gpu {

namespace {

constexpr int kFrames = 3;
constexpr VkDeviceSize kUploadSize = 32ull << 20;  // per frame: vertices, indices, constants, texture staging
constexpr VkDeviceSize kUboRange = 3072;           // b0 and b1: 192 vertex constants, DrawConstants under it
constexpr VkDeviceSize kUboAlign = 256;
constexpr VkFormat kBgra = VK_FORMAT_B8G8R8A8_UNORM;
constexpr uint32_t kSetsPerPool = 1024;
static_assert(sizeof(DrawConstants) <= kUboRange);

const char* kBlitHlsl = R"(
cbuffer B : register(b1) { float4 uvrect; float4 opts; };
Texture2D img : register(t0);
Texture2D lut : register(t1);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uvrect.xy + t * uvrect.zw;
    return o;
}
float4 psmain(O i) : SV_Target {
    float4 c = 0;
    int n = max((int)opts.z, 1);
    float2 px = fwidth(i.uv);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) c += img.SampleLevel(ls, i.uv + px * ((float2(x, y) + 0.5) / n - 0.5), 0);
    c /= n * n;
    c.rgb *= opts.w;
    if (opts.x > 0) c.rgb = float3(lut.Sample(ps, float2(c.r, 0.5)).r, lut.Sample(ps, float2(c.g, 0.5)).g, lut.Sample(ps, float2(c.b, 0.5)).b);
    return float4(c.rgb, opts.y > 0 ? c.a : 1);
}
)";

// the screen filters, the frame onto the screen through one of them. src.xy a texel of the frame, src.z seconds for
// what moves (grain, VHS), src.w the colour look. pscrt: crt.x the lines, a whole number of pixels each so they never
// beat against the screen's; crt.y how dark the gaps between them are, crt.z the tube's curve, crt.w the phosphor
// mask's dimming; look.x the corners' shade, look.y the brightness the lines and mask take back. post.x HDR on,
// post.y FXAA on; both go before the filter, psplain is them alone
const char* kFilterHlsl = R"(
cbuffer B : register(b1) { float4 uvrect; float4 opts; float4 crt; float4 look; float4 src; float4 post; float4 lens; float4 lens2; float4 ret; float4 silA; float4 silB; float4 tans; float4 icon; float4 iconst; };
Texture2D img : register(t0);
Texture2D lut : register(t1);
Texture2D scope : register(t2);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uvrect.xy + t * uvrect.zw;
    return o;
}
float3 Fetch(float2 uv) {
    float3 c = img.SampleLevel(ls, uv, 0).rgb;
    if (opts.x > 0) c = float3(lut.Sample(ps, float2(c.r, 0.5)).r, lut.Sample(ps, float2(c.g, 0.5)).g, lut.Sample(ps, float2(c.b, 0.5)).b);
    return c;
}
float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
float Noise(float2 p) { return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453); }
float Corners(float2 uv, float k) { return pow(saturate(16 * uv.x * uv.y * (1 - uv.x) * (1 - uv.y)), k); }

// a Reinhard tone map against the console's overbright look, with the saturation and the shadows it loses given back
float3 Tone(float3 c) {
    if (post.x <= 0) return c;
    float l = dot(c, float3(0.2126, 0.7152, 0.0722));
    float shadow = 1 - smoothstep(0, 0.8, l);
    c = lerp(l, c, 1 + shadow);
    float3 mapped = pow(max(c / (c + 1), 0), 1 / 2.2);
    c = lerp(c, mapped, 0.8);
    return saturate(c * (1 - shadow * 0.5));
}

// FXAA: across an edge the luma's direction says, two taps or four, the four unless they reach past the corners
float3 Fxaa(float2 uv) {
    float2 px = src.xy;
    float3 m = Fetch(uv);
    float nw = Luma(Fetch(uv + float2(-1, -1) * px)), ne = Luma(Fetch(uv + float2(1, -1) * px));
    float sw = Luma(Fetch(uv + float2(-1, 1) * px)), se = Luma(Fetch(uv + float2(1, 1) * px));
    float lm = Luma(m);
    float lo = min(lm, min(min(nw, ne), min(sw, se))), hi = max(lm, max(max(nw, ne), max(sw, se)));
    if (hi - lo < max(0.0312, hi * 0.125)) return m;
    float2 dir = float2(-((nw + ne) - (sw + se)), (nw + sw) - (ne + se));
    float reduce = max((nw + ne + sw + se) * (0.25 / 8), 1.0 / 128);
    dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + reduce), -8, 8) * px;
    float3 a = 0.5 * (Fetch(uv + dir * (1.0 / 3 - 0.5)) + Fetch(uv + dir * (2.0 / 3 - 0.5)));
    float3 b = a * 0.5 + 0.25 * (Fetch(uv - dir * 0.5) + Fetch(uv + dir * 0.5));
    float lb = Luma(b);
    return lb < lo || lb > hi ? a : b;
}

// what the headset draws over an eye's frame: the scope's glass (lens) and the barrel's reticle (ret)
float3 Overlay(float2 uv, float3 c) {
    // lens: the scope's glass in this eye (xy its centre in the frame's 0..1, zw its radii there); lens2: x how dark
    // (the eye off the scope's axis), zw the cos and sin of the gun's roll against the eye. Its picture (4:3, the
    // middle square used) turned upright, a reticle over it, its rim shaded
    if (lens.z > 0) {
        float2 f = (uv - uvrect.xy) / uvrect.zw;
        float2 d = (f - lens.xy) / lens.zw;
        float r = length(d);
        if (r < 1) {
            float2 up = float2(d.x, -d.y);
            float2 s = float2(lens2.z * up.x + lens2.w * up.y, -lens2.w * up.x + lens2.z * up.y);
            float3 v = scope.SampleLevel(ls, float2(0.5 + s.x * 0.375, 0.5 - s.y * 0.5), 0).rgb;
            // (the game's gamma ramp, as the eyes' frames have it)
            if (opts.x > 0) v = float3(lut.Sample(ps, float2(v.r, 0.5)).r, lut.Sample(ps, float2(v.g, 0.5)).g, lut.Sample(ps, float2(v.b, 0.5)).b);
            v = Tone(v);
            float hair = (abs(s.x) < 0.014 || abs(s.y) < 0.014) && r < 0.92 ? 1 : 0;
            float post_ = abs(s.y) < 0.03 && abs(s.x) > 0.45 && r < 0.92 ? 1 : 0;  // the thick outer posts
            v *= 1 - max(hair, post_) * 0.95;
            v *= (1 - smoothstep(0.82, 1.0, r)) * (1 - lens2.x);
            c = v;
        }
    }
    // ret: where the barrel points in this eye (xy its point in the frame's 0..1, zw its radius there): a ring and a
    // dot, light with a dark edge, readable on bright and dark alike
    if (ret.z > 0) {
        float2 f = (uv - uvrect.xy) / uvrect.zw;
        float r = length((f - ret.xy) / ret.zw);
        float ring = smoothstep(0.62, 0.72, r) * (1 - smoothstep(0.9, 1.0, r));
        float dot_ = 1 - smoothstep(0.14, 0.2, r);
        float edge = smoothstep(0.5, 0.62, r) * (1 - smoothstep(0.72, 0.8, r)) + smoothstep(0.9, 1.0, r) * (1 - smoothstep(1.0, 1.12, r));
        c = lerp(c, c * 0.25, saturate(edge) * 0.8);
        c = lerp(c, float3(0.55, 1.0, 0.6), saturate(max(ring, dot_)) * 0.9);
    }
    // silA/silB: our silencer (its back end and front in the eye's space, x right y up, -z ahead; silA.w its radius,
    // silB.w on), ray cast against its tube and collar with the eye's tangents (tans: left, right, up, down);
    // matte black, a soft key light, two grooves and the vent hole (the user's photo, no markings)
    if (silB.w > 0) {
        float2 f = (uv - uvrect.xy) / uvrect.zw;
        float3 d = normalize(float3(lerp(tans.x, tans.y, f.x), lerp(tans.z, tans.w, f.y), -1));
        float3 ax = silB.xyz - silA.xyz;
        float len = length(ax);
        ax /= len;
        float best = 1e9, along = 0, part = 0;
        float3 nrm = 0;
        // the tube (radius silA.w from 7.5% of the length) and the collar (72% of it, behind that)
        for (int k = 0; k < 2; k++) {
            float rad = k == 0 ? silA.w : silA.w * 0.72;
            float s0 = k == 0 ? len * 0.075 : 0, s1 = k == 0 ? len : len * 0.075;
            float3 oc = -silA.xyz;
            float dv = dot(d, ax), ov = dot(oc, ax);
            float3 dp = d - dv * ax, op = oc - ov * ax;
            float qa = dot(dp, dp), qb = 2 * dot(dp, op), qc = dot(op, op) - rad * rad;
            float disc = qb * qb - 4 * qa * qc;
            if (disc > 0) {
                float t = (-qb - sqrt(disc)) / (2 * qa);
                float s = ov + t * dv;
                if (t > 0 && s > s0 && s < s1 && t < best) {
                    best = t; along = s / len; part = k;
                    nrm = normalize(t * d - silA.xyz - s * ax);
                }
            }
            // the front's flat end (the tube's) and the collar's back
            float se = k == 0 ? len : 0;
            float denom = dot(d, ax);
            if (abs(denom) > 1e-4) {
                float t = dot(silA.xyz + se * ax, ax) / denom;
                float3 p = t * d - silA.xyz - se * ax;
                if (t > 0 && dot(p, p) < rad * rad && t < best) { best = t; along = se / len; part = k; nrm = k == 0 ? ax : -ax; }
            }
        }
        if (best < 1e8) {
            float3 light = normalize(float3(0.3, 0.8, 0.5));
            float3 v = -d;
            float diff = saturate(dot(nrm, light));
            float spec = pow(saturate(dot(reflect(-light, nrm), v)), 24);
            float3 col = float3(0.045, 0.045, 0.05) + diff * float3(0.16, 0.16, 0.17) + spec * 0.28;
            if (part > 0.5) col *= 0.8;  // the thread collar, a little darker
            float g1 = abs(along - 0.42), g2 = abs(along - 0.58);
            if (part < 0.5 && (g1 < 0.006 || g2 < 0.006)) col *= 0.35;  // the grooves
            float3 hit = best * d - silA.xyz;
            float3 sideRef = normalize(cross(ax, float3(0, 0, 1)) + 1e-4);
            if (part < 0.5 && abs(along - 0.88) < 0.012 && dot(normalize(hit - dot(hit, ax) * ax), sideRef) > 0.96) col = 0.01;  // the vent hole
            c = col;
        }
    }
    // icon: the inventory's silencer under the ammunition (xy its centre in the frame, zw its half size there), the
    // user's icon drawn flat and horizontal: a slate tube, its lower half plum, a light stripe along it, two dark bands
    // near the front, a brass thread collar at the back. iconst.x: 1 carried (bright, a glow round its edge), 0 dark
    if (icon.z > 0) {
        float2 f = (uv - uvrect.xy) / uvrect.zw;
        float2 q = (f - icon.xy) / icon.zw;  // x -1..1 back to front, y -1..1 top to bottom
        float aa = 2 * length(fwidth(q));
        // the tube: a rounded box, corners 0.22
        float2 bq = abs(float2(q.x - 0.12, q.y)) - float2(0.86 - 0.22, 0.52 - 0.22);
        float tube = length(max(bq, 0)) + min(max(bq.x, bq.y), 0) - 0.22;
        float collar = max(abs(q.x + 0.86) - 0.14, abs(q.y) - 0.3);
        float shape = min(tube, collar);
        float3 col = q.y < 0.02 ? float3(0.34, 0.36, 0.45) : float3(0.29, 0.22, 0.30);
        if (abs(q.y + 0.24) < 0.06) col = float3(0.46, 0.49, 0.58);          // the light stripe
        if (abs(q.x - 0.5) < 0.05 || abs(q.x - 0.72) < 0.05) col *= 0.7;      // the bands
        if (collar < tube) col = abs(q.x + 0.82) < 0.04 ? float3(0.84, 0.75, 0.40) : float3(0.63, 0.45, 0.20);
        float bright = iconst.x;
        float inside = 1 - smoothstep(-aa, aa, shape);
        float3 shown = bright > 0.5 ? col * 1.35 : col * 0.38;
        c = lerp(c, shown, inside * (bright > 0.5 ? 0.97 : 0.7));
        if (bright > 0.5) {
            // the glow: soft, outside the edge
            float glow = exp(-max(shape, 0) * 9) * (1 - inside);
            c += float3(0.55, 0.9, 0.8) * glow * 0.55;
        }
    }
    return c;
}

// the frame at a point with what the graphics settings add before a filter
// post.z: a dev lift of the shadows (dev_bright), to see models in a dark scene
// post.w: the comfort vignette (an eye's frame: its edges darkened, by this much at the rim)
float3 Frame(float2 uv) {
    float3 c = Tone(post.y > 0 ? Fxaa(uv) : Fetch(uv));
    c = post.z > 0 ? pow(c, 1 / post.z) : c;
    if (post.w > 0) {
        float2 v = (uv - uvrect.xy) / uvrect.zw * 2 - 1;
        c *= 1 - post.w * smoothstep(0.45, 1.05, length(v));
    }
    return Overlay(uv, c);
}

float4 psplain(O i) : SV_Target { return float4(Frame(i.uv), 1); }

float4 pscrt(O i) : SV_Target {
    float2 cc = i.uv * 2 - 1;
    cc *= 1 + crt.z * cc.yx * cc.yx;
    if (any(abs(cc) > 1)) return float4(0, 0, 0, 1);
    float2 uv = cc * 0.5 + 0.5;
    float3 c = Frame(uv);
    // the beam is brightest at a line's middle, and a bright one spreads over the gap
    float f = frac(uv.y * crt.x);
    float gap = cos(3.14159265 * f);
    float scan = 1 - crt.y * gap * gap;
    scan = lerp(scan, 1, saturate(Luma(c)) * 0.4);
    // an aperture grille: each column of pixels one of the phosphors
    float col = fmod(floor(i.p.x), 3);
    float3 grille = 1 - crt.w * float3(col != 0, col != 1, col != 2);
    return float4(saturate(c * scan * grille * Corners(uv, look.x) * look.y), 1);
}

// contrast adaptive: an edge gains what its neighbours lack, less where it is near black or white already
float4 pssharpen(O i) : SV_Target {
    float3 c = Fetch(i.uv);
    float3 n = Fetch(i.uv - float2(0, src.y)), s = Fetch(i.uv + float2(0, src.y));
    float3 w = Fetch(i.uv - float2(src.x, 0)), e = Fetch(i.uv + float2(src.x, 0));
    float3 lo = min(c, min(min(n, s), min(w, e))), hi = max(c, max(max(n, s), max(w, e)));
    float3 amp = sqrt(saturate(min(lo, 1 - hi) / max(hi, 1e-4)));
    return float4(Overlay(i.uv, Tone(saturate(c + (4 * c - n - s - w - e) * amp * 0.35))), 1);
}

float4 pscolor(O i) : SV_Target {
    float2 uv = i.uv;
    float t = src.z;
    int mode = (int)src.w;
    if (mode == 4) {
        // VHS: lines torn sideways a little, a band of worse tracking rolling down
        float row = floor(uv.y * 240);
        float band = saturate(1 - abs(frac(uv.y - t * 0.07) - 0.5) * 12);
        uv.x += (Noise(float2(row, floor(t * 30))) - 0.5) * (0.0015 + band * 0.006);
    }
    float3 c = Frame(uv);
    if (mode == 0) {  // vibrant
        c = lerp(Luma(c), c, 1.35);
        c = (c - 0.5) * 1.08 + 0.5;
    } else if (mode == 1) {  // noir: black and white, hard, grain
        float l = saturate((Luma(c) - 0.5) * 1.35 + 0.5);
        l += (Noise(i.p.xy + frac(t) * 97) - 0.5) * 0.05;
        c = l * Corners(uv, 0.35);
    } else if (mode == 2) {  // sepia
        c = float3(dot(c, float3(0.393, 0.769, 0.189)), dot(c, float3(0.349, 0.686, 0.168)),
                   dot(c, float3(0.272, 0.534, 0.131)));
        c *= Corners(uv, 0.2);
    } else if (mode == 3) {  // night vision: brightened green, grain, faint lines, a dark rim
        float l = pow(saturate(Luma(c)), 0.7) * 1.5;
        l += (Noise(i.p.xy + frac(t) * 131) - 0.5) * 0.12;
        l *= 1 - 0.12 * fmod(floor(i.p.y), 2);
        c = float3(0.25, 1.0, 0.35) * l * Corners(uv, 0.6);
    } else if (mode == 4) {  // VHS: colour bleeding sideways, washed out, grain
        float off = src.x * 2.5;
        c.r = Fetch(uv + float2(off, 0)).r;
        c.b = Fetch(uv - float2(off, 0)).b;
        c = lerp(Luma(c), c, 0.8) * 0.95 + 0.03;
        c += (Noise(i.p.xy + frac(t) * 173) - 0.5) * 0.06;
    }
    return float4(saturate(c), 1);
}
)";

const char* kDepthHlsl = R"(
Texture2D<float> img : register(t0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float psmain(O i) : SV_Depth { return img.SampleLevel(ps, i.uv, 0); }
)";

// ---- the console's values as Vulkan's

float Bits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void Color(uint32_t argb, float* o) {
    o[0] = ((argb >> 16) & 255) / 255.0f;
    o[1] = ((argb >> 8) & 255) / 255.0f;
    o[2] = (argb & 255) / 255.0f;
    o[3] = (argb >> 24) / 255.0f;
}

VkBlendFactor Blend(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 1: return VK_BLEND_FACTOR_ONE;
        case 0x300: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
        case 0x301: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
        case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 0x306: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
        case 0x307: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 0x308: return alpha ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return VK_BLEND_OP_SUBTRACT;
        case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case 0x8007: return VK_BLEND_OP_MIN;
        case 0x8008: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
    }
}

// D3DCMP_NEVER 0x200 to ALWAYS 0x207: VkCompareOp has the same order, and the shaders take the same numbers
uint32_t CmpIndex(uint32_t v) { return v >= 0x200 && v <= 0x207 ? v - 0x200 : 7; }
VkCompareOp Cmp(uint32_t v) { return VkCompareOp(CmpIndex(v)); }

VkStencilOp StencilOp(uint32_t v) {
    switch (v) {
        case 0: return VK_STENCIL_OP_ZERO;
        case 0x1E01: return VK_STENCIL_OP_REPLACE;
        case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case 0x150A: return VK_STENCIL_OP_INVERT;
        case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_KEEP;
    }
}

VkSamplerAddressMode Address(uint32_t v) {
    switch (v) {
        case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 3: case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

// the input assembler's formats the SDK's vertex.cpp names
VkFormat VertexFormat(uint32_t dxgi) {
    switch (dxgi) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case DXGI_FORMAT_R32G32B32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
        case DXGI_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
        case DXGI_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
        case DXGI_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
        case DXGI_FORMAT_R16G16B16A16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
        case DXGI_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
        case DXGI_FORMAT_R16G16_SNORM: return VK_FORMAT_R16G16_SNORM;
        case DXGI_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SINT;
        case DXGI_FORMAT_R16_SNORM: return VK_FORMAT_R16_SNORM;
        case DXGI_FORMAT_R16_SINT: return VK_FORMAT_R16_SINT;
        case DXGI_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
        default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

// ---- DXT on the CPU, for GPUs without BC textures (most phones)

void Rgb565(uint16_t c, uint8_t* bgr) {
    bgr[2] = uint8_t((c >> 11) * 255 / 31);
    bgr[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
    bgr[0] = uint8_t((c & 31) * 255 / 31);
}

// one 4x4 block's colours, BGRA, out 16 x 4 bytes; dxt1 alpha for the 3 colour mode
void ColourBlock(const uint8_t* b, bool dxt1, uint8_t* out) {
    uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
    uint8_t pal[4][4] = {};
    Rgb565(c0, pal[0]);
    Rgb565(c1, pal[1]);
    for (int i = 0; i < 4; i++) pal[i][3] = 255;
    for (int k = 0; k < 3; k++) {
        if (c0 > c1 || !dxt1) {
            pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
        } else {
            pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = 0;
        }
    }
    if (dxt1 && c0 <= c1) pal[3][3] = 0;
    uint32_t bits = uint32_t(b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24);
    for (int i = 0; i < 16; i++) std::memcpy(out + i * 4, pal[(bits >> (i * 2)) & 3], 4);
}

// a level of DXT1, 3 or 5 blocks (bw x bh per slice, row_pitch bytes a block row) into w x h BGRA texels
void DecodeDxt(Host host, const std::vector<uint8_t>& in, uint32_t row_pitch, uint32_t w, uint32_t h, uint32_t z,
               std::vector<uint8_t>& out) {
    uint32_t block = host == Host::BC1 ? 8 : 16;
    uint32_t bw = std::max(1u, (w + 3) / 4), bh = std::max(1u, (h + 3) / 4);
    out.assign(size_t(w) * h * z * 4, 0);
    uint8_t px[64];
    for (uint32_t s = 0; s < z; s++)
        for (uint32_t by = 0; by < bh; by++)
            for (uint32_t bx = 0; bx < bw; bx++) {
                size_t at = (size_t(s) * bh + by) * row_pitch + size_t(bx) * block;
                if (at + block > in.size()) continue;
                const uint8_t* b = &in[at];
                ColourBlock(host == Host::BC1 ? b : b + 8, host == Host::BC1, px);
                if (host == Host::BC2) {
                    for (int i = 0; i < 16; i++) px[i * 4 + 3] = uint8_t(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
                } else if (host == Host::BC3) {
                    uint8_t a[8] = {b[0], b[1]};
                    if (a[0] > a[1]) {
                        for (int i = 1; i < 7; i++) a[i + 1] = uint8_t(((7 - i) * a[0] + i * a[1]) / 7);
                    } else {
                        for (int i = 1; i < 5; i++) a[i + 1] = uint8_t(((5 - i) * a[0] + i * a[1]) / 5);
                        a[6] = 0;
                        a[7] = 255;
                    }
                    uint64_t bits = 0;
                    for (int i = 0; i < 6; i++) bits |= uint64_t(b[2 + i]) << (8 * i);
                    for (int i = 0; i < 16; i++) px[i * 4 + 3] = a[(bits >> (3 * i)) & 7];
                }
                for (uint32_t y = 0; y < 4 && by * 4 + y < h; y++)
                    for (uint32_t x = 0; x < 4 && bx * 4 + x < w; x++)
                        std::memcpy(&out[((size_t(s) * h + by * 4 + y) * w + bx * 4 + x) * 4], px + (y * 4 + x) * 4, 4);
            }
}

// ---- device memory: 64 MB blocks per memory type, first fit inside them; big allocations get their own

class Allocator {
public:
    struct Alloc {
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkDeviceSize off = 0, size = 0;
        uint8_t* map = nullptr;
        int block = -1;  // -1: memory of its own
    };

    void Init(VkPhysicalDevice pd, VkDevice dev) {
        dev_ = dev;
        vkGetPhysicalDeviceMemoryProperties(pd, &props_);
    }

    bool Get(const VkMemoryRequirements& r, VkMemoryPropertyFlags want, VkMemoryPropertyFlags nice, bool linear, Alloc& out) {
        int type = Type(r.memoryTypeBits, want | nice);
        if (type < 0) type = Type(r.memoryTypeBits, want);
        if (type < 0) return false;
        bool host = props_.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        std::lock_guard lock(m_);
        if (r.size > kBlock / 4) {
            out = {};
            if (!Allocate(uint32_t(type), r.size, out.mem)) return false;
            out.size = r.size;
            if (host) vkMapMemory(dev_, out.mem, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&out.map));
            return true;
        }
        for (size_t i = 0; i < blocks_.size(); i++) {
            Block& b = blocks_[i];
            if (b.type != uint32_t(type) || b.linear != linear) continue;
            if (Carve(b, r, out)) {
                out.block = int(i);
                return true;
            }
        }
        Block b;
        b.type = uint32_t(type);
        b.linear = linear;
        if (!Allocate(b.type, kBlock, b.mem)) return false;
        if (host) vkMapMemory(dev_, b.mem, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&b.map));
        b.free[0] = kBlock;
        blocks_.push_back(std::move(b));
        Carve(blocks_.back(), r, out);
        out.block = int(blocks_.size() - 1);
        return true;
    }

    void Free(Alloc& a) {
        if (!a.mem) return;
        std::lock_guard lock(m_);
        if (a.block < 0) {
            vkFreeMemory(dev_, a.mem, nullptr);
        } else {
            auto& free = blocks_[size_t(a.block)].free;
            auto it = free.emplace(a.off, a.size).first;
            if (auto next = std::next(it); next != free.end() && it->first + it->second == next->first) {
                it->second += next->second;
                free.erase(next);
            }
            if (it != free.begin()) {
                auto prev = std::prev(it);
                if (prev->first + prev->second == it->first) {
                    prev->second += it->second;
                    free.erase(it);
                }
            }
        }
        a = {};
    }

    bool HostCoherent(const Alloc& a) const {
        uint32_t type = a.block < 0 ? own_type_ : blocks_[size_t(a.block)].type;
        return props_.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }

private:
    static constexpr VkDeviceSize kBlock = 64ull << 20;
    struct Block {
        VkDeviceMemory mem = VK_NULL_HANDLE;
        uint32_t type = 0;
        bool linear = false;
        uint8_t* map = nullptr;
        std::map<VkDeviceSize, VkDeviceSize> free;  // offset to size
    };

    int Type(uint32_t bits, VkMemoryPropertyFlags want) const {
        for (uint32_t i = 0; i < props_.memoryTypeCount; i++)
            if ((bits >> i & 1) && (props_.memoryTypes[i].propertyFlags & want) == want) return int(i);
        return -1;
    }

    bool Allocate(uint32_t type, VkDeviceSize size, VkDeviceMemory& mem) {
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = size;
        ai.memoryTypeIndex = type;
        own_type_ = type;
        if (vkAllocateMemory(dev_, &ai, nullptr, &mem) == VK_SUCCESS) return true;
        LOGE("vulkan: out of device memory (%llu bytes)", (unsigned long long)size);
        return false;
    }

    bool Carve(Block& b, const VkMemoryRequirements& r, Alloc& out) {
        for (auto it = b.free.begin(); it != b.free.end(); ++it) {
            VkDeviceSize o = it->first, s = it->second;
            VkDeviceSize at = (o + r.alignment - 1) / r.alignment * r.alignment;
            if (at + r.size > o + s) continue;
            b.free.erase(it);
            if (at > o) b.free[o] = at - o;
            if (at + r.size < o + s) b.free[at + r.size] = o + s - at - r.size;
            out.mem = b.mem;
            out.off = at;
            out.size = r.size;
            out.map = b.map ? b.map + at : nullptr;
            return true;
        }
        return false;
    }

    VkDevice dev_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties props_{};
    std::vector<Block> blocks_;
    uint32_t own_type_ = 0;
    std::mutex m_;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;    // sampled: every level, a depth image's depth alone
    VkImageView attach = VK_NULL_HANDLE;  // as a render target: level 0, depth and stencil
    Allocator::Alloc mem;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    uint32_t w = 1, h = 1, d = 1, layers = 1, levels = 1;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool depth = false, stencil = false;
    explicit operator bool() const { return image != VK_NULL_HANDLE; }
};

struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    Allocator::Alloc mem;
};

// a console surface the renderer draws into
struct Target {
    Image img;
    bool depth = false;
    uint32_t gw = 0, gh = 0, w = 0, h = 0;
    uint32_t data = 0, bytes = 0;
    uint64_t epoch = 0, sig = 0;  // last lock seen, guest memory as last matched
    bool drawn = false;           // the GPU wrote it since the last readback
    std::vector<uint8_t> read;    // what Sync left in guest memory
    Image fb;                     // copy read by draws that sample the target they render to
};

struct Tex {
    Image img;
    TextureDesc d;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    bool decode = false;  // DXT decoded on the CPU
    uint32_t levels = 1;
    uint64_t sig = 0;
    uint64_t frame = 0;
    uint64_t palette = 0;
    uint32_t uploads = 0;  // its contents uploaded this many times (phase 8's dump skips the ones that keep changing)
    uint64_t hash = 0;     // TextureHash of its last upload, with TEXTURES on HD PACK (the pack's key)
};

struct PsoKey {
    uint64_t vs;
    uint64_t ps;
    uint32_t regs;
    uint32_t topo;  // Topology, strip at bit 4
    uint32_t has_rt;
    uint32_t has_ds;
    uint32_t elements[16];  // per register: DXGI format, buffer << 8, offset << 16
    uint32_t strides[16];
    uint32_t buffers;
    uint32_t blend, src, dst, op, wmask;
    uint32_t zen, zwrite, zfunc;
    uint32_t sten, sfunc, smask, swmask, sfail, szfail, spass;
    uint32_t cull, ccw, fill;
    int32_t zbias;
    float zslope;
    uint32_t rtfmt;
};

struct BlitOptions {
    bool gamma = false;
    bool alpha = false;
    uint32_t taps = 1;
    float mul = 1;
    bool over = false;  // premultiplied over the target, which keeps its alpha
};

struct DrawTextures {
    TexInfo info[4];
    VkImageView view[4] = {};
    VkFormat fmt[4] = {};
    uint32_t levels[4] = {1, 1, 1, 1};
    bool hd[4] = {};  // an HD pack image bound in the stage (its own mips always used)
    bool rt[4] = {};
    float k[4] = {1, 1, 1, 1};
    float wide[4] = {};
    bool blur[4] = {};
    uint32_t gw[4] = {};  // a render target's guest width, 0 for any other texture
};

struct DrawShaders {
    bool prog = false;
    bool fixed_transform = false;
    FixedKey fk;
    uint64_t vkey = 0, vh = 0, pkey = 0, ph = 0;
    VkShaderModule vs = VK_NULL_HANDLE, ps = VK_NULL_HANDLE;
};

struct GuestExtent {
    uint32_t w, h;
};

// ---- what the console's state asks of a draw

// the game's 2D layer leaves oPos.w at 0: screen space, drawn with w = 1 as XYZRHW is
std::string VertexProgram(const VertexShader& vs, uint16_t regs, uint64_t decode) {
    const std::string clamp = "    p.w = magclamp(p.w);\n";
    const std::string screen_space = "    p.w = p.w == 0 ? 1 : magclamp(p.w);\n";
    std::string src = VertexProgramHlsl(vs.program, regs, decode);
    size_t at = src.find(clamp);
    if (at != std::string::npos) src.replace(at, clamp.size(), screen_space);
    return src;
}

// a point list writes its size: Vulkan leaves it undefined otherwise; empty when the source has no main to wrap
std::string PointsHlsl(const std::string& src) {
    size_t at = src.find("VOut main(");
    if (at == std::string::npos) return {};
    size_t p = at + 10, sp = src.find(' ', p);
    std::string type = src.substr(p, sp - p);
    // the module's entry point is named main: the program's own main goes by another name
    std::string out = src;
    out.replace(at, 10, "VOut vs_body(");
    return out + "\nVOut main_points(" + type + " i, out float psize : PSIZE) { psize = 1; return vs_body(i); }\n";
}

bool UnitTexcoords(const Assembled& a) {
    constexpr float kLo = -1e-4f, kHi = 1.0001f;
    constexpr uint32_t kSmallDrawVertices = 4096;
    if (!a.vertices.empty() || !a.vertex_count || a.vertex_count > kSmallDrawVertices) return false;
    bool any = false;
    for (int r = 0; r < 16; r++) {
        const VertexElement& e = a.elements[size_t(r)];
        if (!(a.regs >> r & 1) || e.format != DXGI_FORMAT_R32G32_FLOAT) continue;
        const VertexBuffer& vb = a.buffers[e.buffer];
        if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return false;
        for (uint32_t v = 0; v < a.vertex_count; v++) {
            float uv[2];
            std::memcpy(uv, vb.src + size_t(v) * vb.stride + e.offset, sizeof(uv));
            if (!(uv[0] >= kLo && uv[0] <= kHi && uv[1] >= kLo && uv[1] <= kHi)) return false;
        }
        any = true;
    }
    return any;
}

constexpr uint32_t kTaddressWrap = 1;
constexpr uint32_t kTaddressClamp = 3;

bool AnyStageWraps(const State& s) {
    for (const auto& t : s.tss)
        if (t[kTssAddressU] == kTaddressWrap || t[kTssAddressV] == kTaddressWrap) return true;
    return false;
}

constexpr uint32_t kFmtD16 = 0x2C;
constexpr uint32_t kFmtLinD16 = 0x30;
uint8_t ShadowDepthKind(uint32_t format) { return format == kFmtD16 || format == kFmtLinD16 ? 2 : 1; }

uint32_t GuestPitch(uint32_t width) { return (width * 4 + 63) & ~63u; }

constexpr uint32_t kSigWholeMax = 65536;
constexpr uint32_t kSigSamples = 64;
constexpr uint32_t kSigSampleBytes = 64;

uint64_t MemSig(uint32_t data, uint32_t total) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    uint64_t sig = Hash(&total, sizeof(total));
    if (total <= kSigWholeMax) return Hash(p, total, sig);
    for (uint32_t i = 0; i < kSigSamples; i++) {
        const uint8_t* sample = p + uint64_t(total - kSigSampleBytes) * i / (kSigSamples - 1);
        sig = Hash(sample, kSigSampleBytes, sig);
    }
    return sig;
}

bool Changed(const Target& t) {
    if (t.drawn || t.read.empty()) return false;
    uint32_t row = t.gw * 4, pitch = GuestPitch(t.gw);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++)
        if (std::memcmp(p + size_t(y) * pitch, &t.read[size_t(y) * row], row)) return true;
    return false;
}

bool LatchCpuWrite(Target& t) {
    uint64_t e = DirtyEpoch(t.data);
    if (e <= t.epoch) return false;
    t.epoch = e;
    uint64_t sig = MemSig(t.data, t.bytes);
    if (sig == t.sig && !Changed(t)) return false;
    t.sig = sig;
    return true;
}

std::vector<uint8_t> LatchCpuChanges(Target& t) {
    std::vector<uint8_t> px(t.read.size(), 0);
    uint32_t guest_pitch = GuestPitch(t.gw);
    const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++)
        for (uint32_t x = 0; x < t.gw; x++) {
            const uint8_t* c = guest + size_t(y) * guest_pitch + x * 4;
            size_t i = (size_t(y) * t.gw + x) * 4;
            if (!std::memcmp(c, &t.read[i], 4)) continue;
            std::memcpy(&t.read[i], c, 4);
            std::memcpy(&px[i], c, 3);
            px[i + 3] = 255;
        }
    return px;
}

TexKind TextureKind(const TextureDesc& d) {
    if (d.cube) return kTexCube;
    if (d.depth > 1) return kTex3D;
    return kTex2D;
}

bool SameTexture(const Tex& tx, const TextureDesc& d) {
    return tx.img && tx.d.format == d.format && tx.d.width == d.width && tx.d.height == d.height &&
           tx.d.depth == d.depth && tx.d.levels == d.levels && tx.d.cube == d.cube && tx.d.pitch == d.pitch;
}

struct StagePalette {
    uint32_t colours[256] = {};
    uint32_t va = 0;
    uint64_t hash = 0;
};

void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& palette) {
    constexpr uint32_t kFmtP8 = 0x0B, kPaletteCommon = 0, kPaletteData = 4, kPaletteSizeShift = 30;
    if (d.format != kFmtP8 || !s.palettes[size_t(stage)]) return;
    uint32_t header = s.palettes[size_t(stage)];
    palette.va = mem::VirtualFromPhysical(kernel::R32(header + kPaletteData));
    uint32_t size_code = kernel::R32(header + kPaletteCommon) >> kPaletteSizeShift;
    size_t bytes = (std::size(palette.colours) >> size_code) * sizeof(uint32_t);
    std::memcpy(palette.colours, kernel::HostPtr(palette.va), bytes);
    palette.hash = Hash(palette.colours, sizeof(palette.colours));
}

constexpr uint32_t kTexfLinear = 2;
constexpr uint32_t kTexfAnisotropic = 3;

uint32_t SimpleState(const State& s, uint32_t method) { return s.simple[method >> 2]; }

DrawShaders ClassifyShaders(const State& s, const Assembled& a) {
    DrawShaders sh;
    sh.prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    sh.fixed_transform = !sh.prog && !a.pretransformed;
    if (!sh.prog) sh.fk = MakeFixedKey(s);
    return sh;
}

void EnsureProgramHash(const State& s) {
    if (!s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
}

constexpr uint64_t kDeclarationSalt = 0x10000;
constexpr uint64_t kPretransformedSalt = 0x5A5A;
constexpr uint64_t kPointsSalt = 0x9017;
constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;

uint64_t VertexKey(const State& s, const Assembled& a, const DrawShaders& sh) {
    uint64_t seed = sh.prog ? s.vs->hash ^ a.regs : Hash(&s.vs_handle, 4, a.regs | kDeclarationSalt);
    uint64_t vkey = Hash(&a.decode, 8, seed);
    if (!sh.prog && a.pretransformed) vkey ^= kPretransformedSalt;
    if (!sh.prog) vkey = Hash(&sh.fk, sizeof(sh.fk), vkey);
    if (a.topology == Topology::Points) vkey ^= kPointsSalt;
    return vkey;
}

std::string VertexHlsl(const State& s, const Assembled& a, const DrawShaders& sh) {
    if (sh.prog) return VertexProgram(*s.vs, a.regs, a.decode);
    return FixedVertexHlsl(a.regs, a.pretransformed, a.decode, sh.fk);
}

void KeyBlend(const State& s, PsoKey& k) {
    k.blend = SimpleState(s, kMBlendEnable) != 0;
    if (k.blend) {
        k.src = SimpleState(s, kMSrcBlend);
        k.dst = SimpleState(s, kMDestBlend);
        k.op = SimpleState(s, kMBlendOp);
    }
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    // console ARGB bytes to Vulkan's RGBA bits
    k.wmask = ((cw >> 16) & 1) | ((cw >> 8) & 1) << 1 | (cw & 1) << 2 | ((cw >> 24) & 1) << 3;
}

void KeyRasterizer(const State& s, const Assembled& a, PsoKey& k) {
    constexpr uint32_t kCullCw = 0x900;
    uint32_t cull = s.rs[kRsCullMode];
    k.cull = cull != 0 && a.topology == Topology::Triangles;
    k.ccw = cull == kCullCw;
    k.fill = s.rs[kRsFillMode];
}

constexpr float kAlphaTestAlways = 7.0f;

// FOG off leaves the constants as fog disabled: the shaders' fog factor is then 1, the colour as drawn
void FillFogConstants(const State& s, DrawConstants& c) {
    if (!s.rs[kRsFogEnable] || !game::Opt().fog) return;
    c.fog[0] = Bits(s.rs[kRsFogStart]);
    c.fog[1] = Bits(s.rs[kRsFogEnd]);
    c.fog[2] = Bits(s.rs[kRsFogDensity]);
    c.fog[3] = float(s.rs[kRsFogTableMode]);
}

void FillSurfaceConstants(const State& s, GuestExtent size, DrawConstants& c) {
    c.surf[0] = float(size.w);
    c.surf[1] = float(size.h);
    c.surf[2] = 1.0f / s.zscale;
    Color(s.rs[kRsFogColor], c.fog_color);
    c.misc[0] = float(SimpleState(s, kMAlphaRef) & 0xFF);
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) ? float(CmpIndex(SimpleState(s, kMAlphaFunc))) : kAlphaTestAlways;
    c.misc[2] = float(CmpIndex(s.rs[kRsShadowFunc]));
    c.misc[3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;
}

void FillCombinerConstants(const State& s, DrawConstants& c) {
    for (int i = 0; i < 8; i++) {
        Color(SimpleState(s, kMPsConstant0 + i * 4), c.pc[i * 2]);
        Color(SimpleState(s, kMPsConstant1 + i * 4), c.pc[i * 2 + 1]);
    }
    Color(SimpleState(s, kMPsFinalConstant0), c.pc[16]);
    Color(SimpleState(s, kMPsFinalConstant1), c.pc[17]);
    Color(s.rs[kRsTextureFactor], c.tfactor);
}

constexpr float kGlowBoxTexels = 3.0f;
constexpr float kBlurTapTexels = 7.0f / 6;

void FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c) {
    c.texsize[i][0] = float(d.width);
    c.texsize[i][1] = float(d.height);
    // the console samples pixels at integer positions, the host half a pixel on: targets are read half a texel back
    if (!stages.rt[i]) return;
    float k = stages.k[i];
    c.texsize[i][2] = -0.5f / (float(d.width) * k);
    c.texsize[i][3] = -0.5f / (float(d.height) * k);
    if (stages.wide[i] > 0) {
        bool blur = stages.blur[i];
        float fw = blur ? kBlurTapTexels : kGlowBoxTexels;
        float per_texel = blur ? 1.0f : 0.5f;
        float n = std::ceil(fw * stages.wide[i] * per_texel);
        c.spread[i][0] = fw / (n * float(d.width) * k);
        c.spread[i][1] = fw / (n * float(d.height) * k);
        c.spread[i][2] = n;
    }
}

void FillTextureConstants(const State& s, const DrawTextures& stages, DrawConstants& c) {
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (s.textures[size_t(i)] && Describe(s.textures[size_t(i)], d)) FillTexelConstants(i, d, stages, c);
        const auto& t = s.tss[size_t(i)];
        c.bump[i][0] = Bits(t[kTssBumpEnvMat00]);
        c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
        c.bump[i][2] = Bits(t[kTssBumpEnvMat10]);
        c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
        c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]);
        c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
    }
}

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++)
            o[r * 4 + col] = x[r * 4] * y[col] + x[r * 4 + 1] * y[4 + col] + x[r * 4 + 2] * y[8 + col] +
                             x[r * 4 + 3] * y[12 + col];
}

void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++) o[col * 4 + r] = m[r * 4 + col];
}

void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0 ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; r++)
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (col + 1) % 3, c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
}

constexpr size_t kTsView = 2;
constexpr size_t kTsProjection = 3;
constexpr size_t kTsTexture0 = 16;
constexpr size_t kTsWorld = 256;

// blackquest stereo -----------------------------------------------------------------------------------------------

// column-vector 4x4s here, m[row * 4 + col]
void Mul4(const float* a, const float* b, float* o) {
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    std::memcpy(o, r, sizeof(r));
}

bool Invert4(const float* m, float* o) {
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (std::fabs(det) < 1e-12f) return false;
    for (int i = 0; i < 16; i++) o[i] = inv[i] / det;
    return true;
}

constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
// the first person arms' mesh in the gun list (both arms; the gun is the other draw): 2919 to 2927 indices on every
// weapon logged (pistol, AK, shotgun, M16, SMG), the guns 5156 to 6773. textures move, so the count it is
constexpr bool IsArms(uint32_t indices) { return indices >= 2880 && indices <= 2970; }
// game units per metre: assumed 1 until phase 2's scale check (the eye height the player matrix adds, 0.2 below
// the head, reads as metres)
constexpr float kUnitsPerMetre = 1.0f;
// the view's raise over the headset's own height: xr::kEyeRaise. the view follows the headset up and down, and
// game::VrViewLift gives back the game's camera drop while a real crouch crouches it (vr_body.cpp)
using xr::kEyeRaise;
// the head-locked hud plane: metres ahead and half its width (34 degrees each side: user, the hud 15-20% bigger, was
// 30), its height by the screen's shape
constexpr float kHudDistance = 1.5f;
constexpr float kHudHalfWidth = 1.02f;
// the hud brighter (user: its dark made it hard to read as health ran low): each of its pixels' colour times this,
// its saturation lifted by this (vr_gun's shader hook, DrawConstants ext)
constexpr float kHudGain = 1.35f, kHudSaturation = 0.25f;

// the hud's brightness in every pixel shader's last line: ext.x the gain, ext.y the saturation's lift, both 0 (no
// change) but for the hud's draws
struct VrShaderHooks : gpu::ShaderHooks {
    std::string Output() const override {
        return "if (ext.x > 0) {\n"
               "    float l = dot(outc.rgb, float3(0.299, 0.587, 0.114));\n"
               "    outc.rgb = lerp(l.xxx, outc.rgb, 1 + ext.y) * ext.x;\n"
               "}\n"
               "outc.rgb = saturate(outc.rgb);\n";
    }
};
VrShaderHooks g_vr_hooks;

// row-major 3x3 rotation of a unit quaternion (x, y, z, w)
void QuatMatrix(const float* q, float* R) {
    float xx = q[0] * q[0], yy = q[1] * q[1], zz = q[2] * q[2];
    float xy = q[0] * q[1], xz = q[0] * q[2], yz = q[1] * q[2], wx = q[3] * q[0], wy = q[3] * q[1], wz = q[3] * q[2];
    float r[9] = {1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy),
                  2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx),
                  2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy)};
    std::memcpy(R, r, sizeof(r));
}

// the 2d layer (hud, flash over play) on a plane fixed ahead of the head: its clip position, the target's as the
// game drew it, becomes a point on the plane, seen by the eye from its offset off the head's middle. without it
// each eye showed the hud at the same spot of its own (asymmetric) frustum, which doesn't fuse
bool HudMatrix(int eye, float aspect, float* out) {
    xr::EyeView ev, other;
    if (!xr::RenderEye(eye, ev) || !xr::RenderEye(1 - eye, other)) return false;
    // the pause menu (a menu page over play: user, fixed in space): the plane stays in the room where it was ahead
    // of the head as the menu came up, upright, turned with the head's yaw; the eye looks at it from where it is
    {
        static bool fixed = false;
        static float c[3], Rp[9];
        // (dev_hudlocked: never fixed, to compare)
        static const bool locked = std::filesystem::exists(app::ExeDir() / "dev_hudlocked");
        if (locked || !::game::VrMenuOverPlay()) {
            fixed = false;
        } else {
            if (!fixed) {
                fixed = true;
                LOGR("vulkan: the game paused over play: the hud plane fixed in the room");
                float h[3], R[9];
                for (int i = 0; i < 3; i++) h[i] = (ev.pos[i] + other.pos[i]) * 0.5f;
                QuatMatrix(ev.quat, R);
                float fx = -R[2], fz = -R[8], l = std::sqrt(fx * fx + fz * fz);  // the view's -z, level
                if (l < 1e-3f) fx = 0, fz = -1, l = 1;
                fx /= l, fz /= l;
                c[0] = h[0] + fx * kHudDistance, c[1] = h[1], c[2] = h[2] + fz * kHudDistance;
                // the plane's x right, y up, z back toward the head
                float r[9] = {-fz, 0, fx, 0, 1, 0, -fx, 0, -fz};
                std::memcpy(Rp, r, sizeof(r));
            }
            float Re[9];
            QuatMatrix(ev.quat, Re);
            // A = Reᵀ Rp (the plane's axes in the eye), b = Reᵀ (c - eye)
            float A[9], b[3], d[3] = {c[0] - ev.pos[0], c[1] - ev.pos[1], c[2] - ev.pos[2]};
            for (int i = 0; i < 3; i++) {
                b[i] = Re[0 * 3 + i] * d[0] + Re[1 * 3 + i] * d[1] + Re[2 * 3 + i] * d[2];
                for (int j = 0; j < 3; j++)
                    A[i * 3 + j] = Re[0 * 3 + i] * Rp[0 * 3 + j] + Re[1 * 3 + i] * Rp[1 * 3 + j] + Re[2 * 3 + i] * Rp[2 * 3 + j];
            }
            float l = ev.tan[0], r = ev.tan[1], u = ev.tan[2], dn = ev.tan[3];
            float wh = kHudHalfWidth, hh = kHudHalfWidth / aspect;
            // the clip's (x, y) to the eye's (X, Y) and its depth Z = -z, each a row over (x, y, z, 1)
            float X[4] = {A[0] * wh, A[1] * hh, 0, b[0]}, Y[4] = {A[3] * wh, A[4] * hh, 0, b[1]},
                  Z[4] = {-A[6] * wh, -A[7] * hh, 0, -b[2]};
            for (int k = 0; k < 4; k++) {
                out[0 * 4 + k] = (2 * X[k] - (r + l) * Z[k]) / (r - l);
                out[1 * 4 + k] = (2 * Y[k] - (u + dn) * Z[k]) / (u - dn);
                out[2 * 4 + k] = 1e-4f * Z[k];
                out[3 * 4 + k] = Z[k];
            }
            return true;
        }
    }
    float d[3] = {(ev.pos[0] - other.pos[0]) * 0.5f, (ev.pos[1] - other.pos[1]) * 0.5f,
                  (ev.pos[2] - other.pos[2]) * 0.5f};
    float R[9];
    QuatMatrix(ev.quat, R);
    float e[3];
    for (int i = 0; i < 3; i++) e[i] = R[0 * 3 + i] * d[0] + R[1 * 3 + i] * d[1] + R[2 * 3 + i] * d[2];
    float l = ev.tan[0], r = ev.tan[1], u = ev.tan[2], dn = ev.tan[3];
    float wh = kHudHalfWidth, hh = kHudHalfWidth / aspect, z = kHudDistance + e[2];
    // depth just off zero: in front of everything, inside vulkan's clip volume
    float m[16] = {2 * wh / (r - l), 0, 0, (-2 * e[0] - (r + l) * z) / (r - l),
                   0, 2 * hh / (u - dn), 0, (-2 * e[1] - (u + dn) * z) / (u - dn),
                   0, 0, 0, 1e-4f * z,
                   0, 0, 0, z};
    std::memcpy(out, m, sizeof(m));
    return true;
}

// the clip position as the game drew it for its one camera, moved to what an eye sees: back through the game's
// viewport and projection to its view space, to the eye (head pose from the local space's origin, which the game
// camera stands for), through the eye's own frustum with the game's depth rows, and the viewport again.
// E = VP · P_eye · V_eye · P_game⁻¹ · VP⁻¹, column vectors; the shader's clip is D3D's (y up, z 0..1)
// gun: the viewmodel hangs off the head, not the room: only the eye's offset from the head's middle moves it, so
// it stays at the face wherever the player stands (the room's origin is where the session started)
// arm: the gun list's arms split between the hands (1 the gun hand's part, 0 the left arm), plane then the clip
// plane on the game's clip position that keeps that part (EyeHlsl)
bool EyeMatrix(const State& s, GuestExtent size, int eye, bool gun, float* out, int arm = -1, float* plane = nullptr) {
    xr::EyeView ev;
    if (!(eye == stereo::kScopeEye ? ::game::VrScopeEye(ev) : xr::RenderEye(eye, ev))) return false;
    const float* P = s.transforms[kTsProjection].data();  // d3d rows: clip = v · P
    float pc[16], vpc[16];
    Transpose(P, pc);
    // perspective only: w from the view depth, no constant w
    if (pc[14] == 0 || pc[15] != 0) return false;
    const auto& v = s.viewport;
    float sx = float(v.w) / size.w, sy = float(v.h) / size.h;
    float ox = (2.0f * v.x + v.w) / size.w - 1, oy = 1 - (2.0f * v.y + v.h) / size.h;
    float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
    Transpose(vpm, vpc);
    float pinv[16], vpinv[16];
    if (!Invert4(pc, pinv) || !Invert4(vpc, vpinv)) return false;
    // the eye's frustum in the game's view space: x right, y up, depth d = f·z with f the sign w takes from z
    float f = pc[14] > 0 ? 1.0f : -1.0f;
    float l = ev.tan[0], r = ev.tan[1], u = ev.tan[2], dn = ev.tan[3];
    float fx = pc[0] < 0 ? -1.0f : 1.0f, fy = pc[5] < 0 ? -1.0f : 1.0f;
    float pe[16] = {fx * 2 / (r - l), 0, -fx * f * (r + l) / (r - l), 0,
                    0, fy * 2 / (u - dn), -fy * f * (u + dn) / (u - dn), 0,
                    pc[8], pc[9], pc[10], pc[11],
                    pc[12], pc[13], pc[14], pc[15]};
    // game view → openxr (S flips the depth axis when the game looks along +z), into the eye, and back
    float R[9];
    QuatMatrix(ev.quat, R);
    // phase 3: the game's camera already looks where the head did when the frame was simulated (vr_body.cpp
    // VrLook, angles in transform slot 4): the eye turns and moves by what's left, in that camera's frame
    float U[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    if (s.transforms[4][4] == 1.0f) {
        float cy = std::cos(s.transforms[4][2]), sy = std::sin(s.transforms[4][2]);
        float cp = std::cos(s.transforms[4][3]), sp = std::sin(s.transforms[4][3]);
        // Ry(yaw) Rx(pitch)
        float u[9] = {cy, sy * sp, sy * cp, 0, cp, -sp, -sy, cy * sp, cy * cp};
        std::memcpy(U, u, sizeof(u));
        float r[9];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) r[i * 3 + j] = U[0 * 3 + i] * R[0 * 3 + j] + U[1 * 3 + i] * R[1 * 3 + j] + U[2 * 3 + i] * R[2 * 3 + j];
        std::memcpy(R, r, sizeof(r));
    }
    float sz = -f;  // S = diag(1, 1, sz)
    // phase 4: the gun list moved to the hand by K (vr_gun.cpp), then seen from the room like the world
    float krot[9], kpos[3];
    bool hand = gun && s.transforms[4][4] == 1.0f &&
                ::game::VrGunPose(s.transforms[4][2], s.transforms[4][3], &s.transforms[4][5], arm == 0 ? 0 : 1, krot,
                                  kpos);
    if (hand) gun = false;
    if (arm >= 0 && !hand) return false;
    float px = ev.pos[0] * kUnitsPerMetre, py = (ev.pos[1] + kEyeRaise + ::game::VrViewLift()) * kUnitsPerMetre, pz = ev.pos[2] * kUnitsPerMetre;
    xr::EyeView other;
    if (gun && xr::RenderEye(1 - eye, other)) {
        px = (ev.pos[0] - other.pos[0]) * 0.5f * kUnitsPerMetre;
        py = (ev.pos[1] - other.pos[1]) * 0.5f * kUnitsPerMetre;
        pz = (ev.pos[2] - other.pos[2]) * 0.5f * kUnitsPerMetre;
    }
    {
        float w[3] = {px, py, pz};
        px = U[0] * w[0] + U[3] * w[1] + U[6] * w[2];
        py = U[1] * w[0] + U[4] * w[1] + U[7] * w[2];
        pz = U[2] * w[0] + U[5] * w[1] + U[8] * w[2];
    }
    // V = S · Rᵀ · T(−p) · S
    float S[3] = {1, 1, sz};
    float view[16] = {};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) view[i * 4 + j] = S[i] * R[j * 3 + i] * S[j];
    for (int i = 0; i < 3; i++) view[i * 4 + 3] = -S[i] * (R[0 * 3 + i] * px + R[1 * 3 + i] * py + R[2 * 3 + i] * pz);
    view[15] = 1;
    if (hand) {
        // K in the game's view space: S K S
        float k[16] = {};
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) k[i * 4 + j] = S[i] * krot[i * 3 + j] * S[j];
            k[i * 4 + 3] = S[i] * kpos[i] * kUnitsPerMetre;
        }
        k[15] = 1;
        Mul4(view, k, view);
    }
    (void)plane;  // the clip-plane split of the arms is gone (own hands pose their bones)
    float m[16];
    Mul4(vpc, pe, m);
    Mul4(m, view, m);
    Mul4(m, pinv, m);
    Mul4(m, vpinv, m);
    std::memcpy(out, m, sizeof(m));
    return true;
}

// the shaders' clip position through the matrix pushed with each draw: identity unless a stereo world draw
std::string EyeHlsl(std::string src) {
    bool any = false;
    for (const char* var : {"q", "cp"}) {
        std::string at = std::string("    o.pos = ") + var + ";\n";
        size_t p = src.find(at);
        if (p == std::string::npos) continue;
        src.replace(p, at.size(), std::string("    o.pos = mul(eye_m, ") + var + ");\n");
        any = true;
    }
    if (!any) return src;
    // and a clip distance from the pushed plane (zero: nothing clipped), which splits the gun list's arms
    const std::string tail = "    float fogamt : TEXCOORD4;\n};";
    if (size_t p = src.find(tail); p != std::string::npos) {
        src.insert(p + tail.size() - 2, "    float eye_cd : SV_ClipDistance0;\n");
        for (const char* var : {"q", "cp"}) {
            std::string at = std::string("    o.pos = mul(eye_m, ") + var + ");\n";
            if (size_t a = src.find(at); a != std::string::npos)
                src.insert(a + at.size(), std::string("    o.eye_cd = dot(eye_clip, ") + var + ");\n");
        }
    }
    // a cbuffer, not a struct variable: glslang makes the latter a member of $Global, a uniform nothing fills
    return "[[vk::push_constant]] cbuffer EyePc { row_major float4x4 eye_m; float4 eye_clip; };\n" + src;
}

void FillTransformConstants(const State& s, GuestExtent size, DrawConstants& c) {
    float wv[16], wvp[16];
    MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
    MatMul(wv, s.transforms[kTsProjection].data(), wvp);
    const auto& v = s.viewport;
    float sx = float(v.w) / size.w, sy = float(v.h) / size.h;
    float ox = (2.0f * v.x + v.w) / size.w - 1, oy = 1 - (2.0f * v.y + v.h) / size.h;
    float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
    float full[16];
    MatMul(wvp, vpm, full);
    Transpose(full, c.wvp);
    Transpose(wv, c.wv);
    float n[16] = {};
    InverseTranspose3x3(wv, n);
    Transpose(n, c.wvn);
    for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), c.tm[t]);
}

void LightToView(const std::array<uint32_t, 26>& l, const float* vm, float (&o)[7][4]) {
    float f[26];
    std::memcpy(f, l.data(), sizeof(f));
    for (int j = 0; j < 4; j++) {
        o[0][j] = f[1 + j];
        o[1][j] = f[5 + j];
        o[2][j] = f[9 + j];
    }
    const float* pos = f + 13;
    const float* dir = f + 16;
    for (int j = 0; j < 3; j++) {
        o[3][j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
        o[4][j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
    }
    o[3][3] = f[19];
    o[4][3] = f[20];
    o[5][0] = f[21];
    o[5][1] = f[22];
    o[5][2] = f[23];
    o[6][0] = std::cos(f[24] * 0.5f);
    o[6][1] = std::cos(f[25] * 0.5f);
}

void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c) {
    std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
    c.mat[4][0] = s.material[16];
    Color(s.rs[kRsAmbient], c.amb);
    const float* vm = s.transforms[kTsView].data();
    for (int i = 0; i < 8; i++)
        if ((fk.types >> (i * 2)) & 3) LightToView(s.lights[size_t(i)], vm, c.lt[i]);
}

struct GuestRect {
    int32_t left, top, right, bottom;
};
GuestRect ReadGuestRect(uint32_t rects, uint32_t i) {
    uint32_t r = rects + i * 16;
    return {int32_t(kernel::R32(r)), int32_t(kernel::R32(r + 4)), int32_t(kernel::R32(r + 8)),
            int32_t(kernel::R32(r + 12))};
}
void ReadGuestPoint(uint32_t points, uint32_t i, int32_t& x, int32_t& y) {
    x = int32_t(kernel::R32(points + i * 8));
    y = int32_t(kernel::R32(points + i * 8 + 4));
}
uint32_t ScaleUnsigned(int32_t v, float sc) { return uint32_t(uint32_t(v) * sc); }

uint64_t TargetMapKey(uint32_t data, bool depth) { return uint64_t(data) << 1 | (depth ? 1 : 0); }

// ---- layouts: the stages and accesses an image's layout stands for

void LayoutMasks(VkImageLayout l, VkPipelineStageFlags& st, VkAccessFlags& ac) {
    switch (l) {
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            st = VK_PIPELINE_STAGE_TRANSFER_BIT;
            ac = VK_ACCESS_TRANSFER_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            st = VK_PIPELINE_STAGE_TRANSFER_BIT;
            ac = VK_ACCESS_TRANSFER_READ_BIT;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            st = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
            ac = VK_ACCESS_SHADER_READ_BIT;
            break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            st = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            ac = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            st = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            ac = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
            st = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            ac = 0;
            break;
        default:
            st = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            ac = 0;
            break;
    }
}

bool HasStencil(VkFormat f) {
    return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT;
}

class VulkanBackend final : public Backend {
public:
    ~VulkanBackend() override = default;
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override;
    void Present(const Surface& front) override;
    // blackquest: the eye the next commands draw for (render thread)
    void BeginEye(int eye);
    bool Stereo() const { return stereo_; }
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override;
    bool DeliverLate(uint32_t data) override;
    void RecordLate(uint32_t data) override;
    void Flush() override { Submit(true, false); }
    void SetGamma(const uint8_t* ramp) override {
        std::memcpy(ramp_, ramp, sizeof(ramp_));
        ramp_dirty_ = true;
    }
    void WaitGpuFrame(uint64_t n);

private:
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        Buffer upload;
        VkDeviceSize used = 0;
        uint64_t value = 0;  // the timeline's value once the GPU is past this frame
        std::vector<Image> images;
        std::vector<Buffer> buffers;
        std::vector<VkDescriptorPool> pools;
        size_t pool_at = 0;  // the descriptor pool sets come from
        std::unordered_map<uint64_t, VkDescriptorSet> sets;
        VkSemaphore acquire = VK_NULL_HANDLE;
    };
    struct Alloc {
        VkBuffer buf;
        VkDeviceSize off;
        uint8_t* cpu;
    };
    struct Late {
        Buffer rb[kFrames];
        Image down;
        uint64_t fence[kFrames] = {};
        uint64_t frame[kFrames] = {};
        uint64_t delivered = 0;
        uint32_t gw = 0, gh = 0, next = 0;
    };

    Frame& CurrentFrame() { return frames_[frame_ % kFrames]; }
    VkCommandBuffer Cb() { return CurrentFrame().cb; }

    // Init, in order
    bool CreateInstance();
    bool CreateSurface();
    bool PickDevice();
    bool CreateDevice();
    bool CreateSwapchain();
    void DestroySwapchain();
    bool CreateLayouts();
    bool CreateFrames();
    bool CreateFixedImages();
    bool InitUi();

    // memory and objects
    bool CreateImage(Image& im, VkFormat fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t layers, uint32_t levels,
                     VkImageUsageFlags usage, VkImageViewType vt, bool cube = false);
    bool CreateBuffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage, bool host);
    void Destroy(Image& im);
    void Destroy(Buffer& b);
    void Retire(Image& im) {
        if (rend_color_ == &im || rend_depth_ == &im) EndRender();
        if (im) CurrentFrame().images.push_back(im);
        im = {};
    }
    void Retire(Buffer& b) {
        if (b.buf) CurrentFrame().buffers.push_back(b);
        b = {};
    }
    Alloc Upload(VkDeviceSize size, VkDeviceSize align = 16);
    Alloc Constants(const void* data, size_t bytes);

    // command recording
    void Begin();
    void Submit(bool wait, bool present);
    void WaitValue(uint64_t v);
    uint64_t Completed();
    VkFence SubmitFence(uint64_t value);
    void PollFences();
    void ResetFrame(Frame& f);
    void Transition(Image& im, VkImageLayout to);
    void BeginRender(Image* color, Image* depth);
    void EndRender();
    // without dynamic rendering: render passes that only load and store, and framebuffers per pair of views
    VkRenderPass Pass(VkFormat color, VkFormat depth, bool clear = false);
    VkFramebuffer Framebuffer(VkFormat color_fmt, VkFormat depth_fmt, VkImageView color, VkImageView depth,
                              uint32_t w, uint32_t h);
    void DropFramebuffers(VkImageView v);
    void SetViewport(float x, float y, float w, float h);
    void SetScissor(int32_t x0, int32_t y0, int32_t x1, int32_t y1);
    VkDescriptorSet Set(const VkImageView views[4], const VkSampler samplers[4]);
    void BindSet(VkDescriptorSet set, VkDeviceSize vc, VkDeviceSize dc);

    // shaders and pipelines
    VkShaderModule Module(const std::string& src, const char* entry, vkshader::Stage stage, uint64_t& hash);
    VkPipeline FullScreenPipeline(const char* hlsl, const char* ps_entry, VkFormat color, VkFormat depth, bool over);
    VkPipeline EnsurePipeline(const PsoKey& k, VkShaderModule vs, VkShaderModule ps);
    VkSampler Sampler(const VkSamplerCreateInfo& ci);
    VkSampler StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp);

    // targets
    Target* FindTarget(const Surface& s, bool depth);
    Target* EnsureTarget(const Surface& s, bool depth);
    void ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it);
    void UploadTarget(Target& t, const Surface& s);
    bool PixelImage(Image& im, const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h);
    void Blit(Image& dst, const VkRect2D& to, Image& src, const float uvrect[4], const BlitOptions& o);
    Image* Feedback(Target& t);
    bool DepthCopy(const State& s);
    bool Shrink(Image& src, uint32_t w, uint32_t h, Image& down, uint32_t taps = 1);
    Image* CopySource(Target& t, Image& down);
    bool ReadBack(Image& im, uint32_t w, uint32_t h, std::vector<uint8_t>& out);

    // draws
    void EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh);
    void EnsurePixelShader(const PixelKey& pk, DrawShaders& sh);
    bool BindStageTextures(const State& s, DrawTextures& stages);
    VkImageView BindTexture(const State& s, int stage, DrawTextures& stages);
    bool GlowComposite(const State& s, const DrawTextures& stages) const;
    VkImageView BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box,
                                  DrawTextures& stages);
    VkImageView BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                 DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key);
    void UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p, const uint32_t* colours);
    void BuildPipelineKey(const State& s, const Assembled& a, const DrawShaders& sh, PsoKey& k);
    void KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k);
    void ClearTarget(Target& t, const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color,
                     float z, uint32_t stencil);

    // present
    bool AcquireImage(uint32_t& index);
    void DrawFrame(Target* t, uint32_t index);
    void RenderUi();
    void UploadGammaRamp();
    void UploadSkin();
    void ReleaseOldTextures();
    void ApplySettings();
    void SavePipelineCache();

    bool xr_ = false;  // blackquest: drawing for the headset through openxr, not to an android window
    // stereo: each eye its own render targets, the other eye's waiting in other_targets_ until BeginEye swaps them
    bool stereo_ = false;
    int eye_ = 0;
    std::unordered_map<uint64_t, Target> other_targets_;
    // the scope's view (stereo::kScopeEye): its targets, or the right eye's while it replays; its last picture
    std::unordered_map<uint64_t, Target> scope_targets_;
    Image scope_img_;
    uint64_t scope_frame_ = 0;  // presents_ when it was kept
    void KeepScope(Target& t);
    // a few seconds' draws, logged: all, world ones, world ones left unmoved; the last world draw's matrices
    uint32_t stat_draws_ = 0, stat_world_ = 0, stat_unmoved_ = 0;
    uint32_t stat_effect_ = 0, stat_gun_ = 0, stat_hud_ = 0, stat_flat_z_ = 0;
    // per eye's replay: the world lists' projection, which effects drawn outside the lists share
    float scene_proj_[2][16] = {};
    bool scene_proj_ok_[2] = {};
    bool last_theater_ = true;  // the last present went to the theater screen: no hud plane
    uint32_t frame_world_ = 0;
    std::set<std::array<uint32_t, 3>> dev_kinds_;  // dev_drawdiff: this frame's world draws' kinds  // world draws since the last present: none puts the frame on the theater screen
    bool panel_ = false;        // DrawFrame is drawing the settings panel's quad: cleared transparent
    float stat_proj_[16] = {};
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    uint32_t surface_epoch_ = 0;
    VkPhysicalDevice pd_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props_{};
    VkPhysicalDeviceFeatures feats_{};
    uint32_t api_ = VK_API_VERSION_1_1;
    uint32_t qfam_ = 0;
    VkDevice dev_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::vector<const char*> dev_exts_;
    bool bc_ = false;
    // phase 8, the HD pack's images by hash: resident ones, the bytes they hold, this frame's uploads
    struct HdImage {
        Image img;
        uint64_t used = 0;
        size_t bytes = 0;
    };
    std::unordered_map<uint64_t, HdImage> hd_;
    size_t hd_bytes_ = 0;
    uint64_t hd_frame_ = 0;
    size_t hd_frame_bytes_ = 0;
    const Image* HdFor(uint64_t hash);
    VkFormat depth_fmt_ = VK_FORMAT_D24_UNORM_S8_UINT;
    bool depth_linear_ = false;
    bool r32f_linear_ = false;
    Allocator alloc_;
    PFN_vkCmdBeginRenderingKHR begin_rendering_ = nullptr;
    PFN_vkCmdEndRenderingKHR end_rendering_ = nullptr;
    PFN_vkWaitSemaphoresKHR wait_semaphores_ = nullptr;
    PFN_vkGetSemaphoreCounterValueKHR counter_value_ = nullptr;
    bool dynamic_ = true;  // dynamic rendering; Vulkan 1.1 drivers (older Adrenos) only have render passes
    bool timeline_ok_ = true;  // without it a fence per submit stands in for the timeline's values
    struct Pending {
        uint64_t value;
        VkFence fence;
    };
    std::mutex fence_lock_;  // the game thread waits on frames too
    std::vector<Pending> pending_;
    std::vector<VkFence> spare_fences_;
    uint64_t fenced_ = 0;  // the highest value whose fence signalled
    std::unordered_map<uint64_t, VkRenderPass> passes_;
    struct Fb {
        VkImageView color, depth;
        VkFramebuffer fb;
    };
    std::unordered_map<uint64_t, Fb> framebuffers_;

    VkSwapchainKHR swap_ = VK_NULL_HANDLE;
    VkFormat swap_fmt_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D swap_extent_{};
    std::vector<VkImage> swap_images_;
    std::vector<VkImageView> swap_views_;
    std::vector<VkSemaphore> swap_done_;
    bool vsync_ = true;
    bool swap_stale_ = false;
    bool surface_lost_ = false;  // the driver said the window is gone: the surface is made again too
    uint32_t surface_failures_ = 0;  // presents in a row without a surface, for the log
    uint32_t present_index_ = 0;  // the image the frame's submit signals the present of

    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipelineCache cache_ = VK_NULL_HANDLE;
    size_t cache_saved_ = 0;
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    std::atomic<uint64_t> value_{0};
    Frame frames_[kFrames];
    uint64_t frame_ = 0;

    // the rendering under way: its attachments
    bool rendering_ = false;
    Image* rend_color_ = nullptr;
    Image* rend_depth_ = nullptr;

    std::unordered_map<uint64_t, VkShaderModule> modules_;
    std::unordered_map<uint64_t, uint64_t> shader_src_;
    std::unordered_map<uint64_t, VkPipeline> pipelines_;
    std::unordered_map<uint64_t, VkSampler> samplers_;
    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint64_t, Tex> textures_;
    Image lut_;
    Image dummy_[3];  // 2D, 3D, cube
    VkSampler linear_ = VK_NULL_HANDLE, point_ = VK_NULL_HANDLE;
    VkDescriptorPool ui_pool_ = VK_NULL_HANDLE;
    bool ui_ = false;
    uint8_t ramp_[768] = {};
    bool ramp_dirty_ = false;
    Image skin_;  // the touch controls' button skins (touch.cpp), made once
    bool skin_tried_ = false;
    bool gamma_ = false;
    Target* draw_rt_ = nullptr;
    Target* draw_ds_ = nullptr;
    uint32_t guest_w_ = 640, guest_h_ = 480;
    float scale_ = 1.0f;
    Assembled asm_;
    std::unordered_map<uint32_t, Late> late_;
    std::mutex late_lock_;
    std::atomic<uint64_t> presents_seen_{0};
    std::atomic<uint64_t> present_value_[kFrames + 1] = {};
    uint64_t presents_ = 0;
    double fps_ = 0;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t fps_n_ = 0;
};

#define VK_CHECK(x)                                                    \
    do {                                                               \
        VkResult _r = (x);                                             \
        if (_r != VK_SUCCESS) {                                        \
            LOGE("vulkan: %s failed (%d)", #x, int(_r));               \
            return false;                                              \
        }                                                              \
    } while (0)

// ---- Init

bool VulkanBackend::Init(uint32_t width, uint32_t height) {
    gpu::g_shader_hooks = &g_vr_hooks;  // the hud's brightness (before any shader is made)
    guest_w_ = width;
    guest_h_ = height;
    vkshader::Init(app::ExeDir() / "shadercache");
    xr_ = xr::Init();
    if (!CreateInstance() || !CreateSurface() || !PickDevice() || !CreateDevice()) return false;
    if (xr_ && !xr::CreateSession(instance_, pd_, dev_, qfam_)) return false;
    stereo_ = xr_ && xr::Stereo();
    alloc_.Init(pd_, dev_);
    if (!CreateLayouts() || !CreateFrames() || !CreateSwapchain() || !CreateFixedImages()) return false;
    Begin();
    ui_ = InitUi();
    scale_ = app::RenderScale(guest_h_);
    LOGI("vulkan: %ux%u guest, %ux%u screen, render scale %.3f", width, height, swap_extent_.width,
         swap_extent_.height, scale_);
    return true;
}

bool VulkanBackend::CreateInstance() {
    uint32_t n = 0;
    // the headset needs no window surface: the runtime adds the instance extensions it wants itself
    const char* const* sdl = xr_ ? nullptr : SDL_Vulkan_GetInstanceExtensions(&n);
    std::vector<const char*> exts(sdl, sdl + n);
    uint32_t loader = VK_API_VERSION_1_1;
    vkEnumerateInstanceVersion(&loader);
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = app::kGame.log;
    ai.pEngineName = "XboxXBE-Recomp";
    ai.apiVersion = std::min<uint32_t>(loader, VK_API_VERSION_1_3);
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &ai;
    ci.enabledExtensionCount = uint32_t(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    if (xr_) return xr::CreateVkInstance(ci, instance_);
    VK_CHECK(vkCreateInstance(&ci, nullptr, &instance_));
    return true;
}

// the epoch is taken only once the surface is made: until then each present tries again
bool VulkanBackend::CreateSurface() {
    if (xr_) return true;
    uint32_t epoch = host::SurfaceEpoch();
    // Android has not handed the window back yet (resuming, or a surface change): wait for it without asking SDL,
    // which only fails, and log the outage once, not every present
    SDL_PropertiesID props = SDL_GetWindowProperties(host::Window());
    if (!SDL_GetPointerProperty(props, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr)) {
        if (surface_failures_++ == 0) LOGI("vulkan: waiting for Android's window");
        return false;
    }
    if (!SDL_Vulkan_CreateSurface(host::Window(), instance_, nullptr, &surface_)) {
        surface_ = VK_NULL_HANDLE;
        if (surface_failures_++ == 0) LOGE("vulkan: no surface: %s", SDL_GetError());
        return false;
    }
    if (surface_failures_) LOGI("vulkan: surface made after %u tries", surface_failures_ + 1);
    surface_failures_ = 0;
    surface_epoch_ = epoch;
    surface_lost_ = false;
    return true;
}

bool HasExtension(const std::vector<VkExtensionProperties>& list, const char* name) {
    return std::any_of(list.begin(), list.end(), [&](const VkExtensionProperties& e) { return !std::strcmp(e.extensionName, name); });
}

bool VulkanBackend::PickDevice() {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(instance_, &n, pds.data());
    // the headset's runtime says which GPU it composites with
    if (xr_) {
        VkPhysicalDevice want = VK_NULL_HANDLE;
        if (!xr::PhysicalDevice(instance_, want)) return false;
        pds = {want};
    }
    // rejections are reported, not warned: release logs drop warnings and these are what a bug report needs
    if (pds.empty()) LOGE("vulkan: the driver lists no GPU");
    for (VkPhysicalDevice pd : pds) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd, &p);
        LOGR("GPU: %s, Vulkan %u.%u.%u, driver %08X", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
             VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion);
        if (p.apiVersion < VK_API_VERSION_1_1) {
            LOGE("vulkan: %s is below Vulkan 1.1", p.deviceName);
            continue;
        }
        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> ext(en);
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, ext.data());
        uint32_t api = std::min<uint32_t>(p.apiVersion, VK_API_VERSION_1_3);
        bool swapchain = HasExtension(ext, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        bool dynamic = api >= VK_API_VERSION_1_3 || HasExtension(ext, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
        if (!swapchain && !xr_) {
            LOGE("vulkan: %s lacks swapchain", p.deviceName);
            continue;
        }
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qs.data());
        for (uint32_t q = 0; q < qn; q++) {
            VkBool32 present = xr_ ? VK_TRUE : VK_FALSE;  // the runtime presents, from any graphics queue
            if (!xr_) vkGetPhysicalDeviceSurfaceSupportKHR(pd, q, surface_, &present);
            if (!(qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            pd_ = pd;
            props_ = p;
            api_ = api;
            qfam_ = q;
            dev_exts_.clear();
            if (!xr_) dev_exts_.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
            auto want = [&](const char* name) {
                if (HasExtension(ext, name)) dev_exts_.push_back(name);
            };
            if (api < VK_API_VERSION_1_3) {
                want(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
                want(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
                want(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
            }
            dynamic_ = dynamic;
            // the 1.1 drivers' timeline extension lost the device on the first submit (Mali-G76): fences there
            timeline_ok_ = api >= VK_API_VERSION_1_2;
            if (!dynamic_) LOGR("vulkan: no dynamic rendering, using render passes");
            if (!timeline_ok_) LOGR("vulkan: Vulkan 1.1, using fences for timeline semaphores");
            return true;
        }
        LOGE("vulkan: %s has no graphics queue that can present", p.deviceName);
    }
    LOGE("vulkan: no GPU with Vulkan 1.1 that can present");
    return false;
}

bool VulkanBackend::CreateDevice() {
    VkPhysicalDeviceTimelineSemaphoreFeaturesKHR ts{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR};
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    // the chain holds only what is used: a 1.1 driver may not know the rest
    auto chain = [&]() -> void* {
        ts.pNext = nullptr;
        dr.pNext = timeline_ok_ ? &ts : nullptr;
        return dynamic_ ? (void*)&dr : timeline_ok_ ? (void*)&ts : nullptr;
    };
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, chain()};
    vkGetPhysicalDeviceFeatures2(pd_, &f2);
    if (timeline_ok_ && !ts.timelineSemaphore) {
        LOGR("vulkan: timeline semaphores not supported, using fences");
        timeline_ok_ = false;
    }
    if (dynamic_ && !dr.dynamicRendering) {
        LOGR("vulkan: dynamic rendering listed but not supported, using render passes");
        dynamic_ = false;
    }
    feats_ = {};
    feats_.samplerAnisotropy = f2.features.samplerAnisotropy;
    feats_.fillModeNonSolid = f2.features.fillModeNonSolid;
    feats_.textureCompressionBC = f2.features.textureCompressionBC;
    feats_.textureCompressionASTC_LDR = f2.features.textureCompressionASTC_LDR;  // (the HD pack's, phase 8)
    feats_.shaderClipDistance = f2.features.shaderClipDistance;  // the gun list's arms split (EyeHlsl)
    bc_ = feats_.textureCompressionBC;
    LOGR("vulkan: BC %d, ASTC LDR %d", int(bc_), int(feats_.textureCompressionASTC_LDR));
    VkPhysicalDeviceFeatures2 on{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, chain()};
    on.features = feats_;
    dr.dynamicRendering = VK_TRUE;
    ts.timelineSemaphore = VK_TRUE;
    float prio = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = qfam_;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &on};
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = uint32_t(dev_exts_.size());
    ci.ppEnabledExtensionNames = dev_exts_.data();
    if (xr_) {
        if (!xr::CreateVkDevice(pd_, ci, dev_)) return false;
    } else {
        VK_CHECK(vkCreateDevice(pd_, &ci, nullptr, &dev_));
    }
    vkGetDeviceQueue(dev_, qfam_, 0, &queue_);
    bool core13 = api_ >= VK_API_VERSION_1_3, core12 = api_ >= VK_API_VERSION_1_2;
    begin_rendering_ = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(
        vkGetDeviceProcAddr(dev_, core13 ? "vkCmdBeginRendering" : "vkCmdBeginRenderingKHR"));
    end_rendering_ = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(
        vkGetDeviceProcAddr(dev_, core13 ? "vkCmdEndRendering" : "vkCmdEndRenderingKHR"));
    wait_semaphores_ = reinterpret_cast<PFN_vkWaitSemaphoresKHR>(
        vkGetDeviceProcAddr(dev_, core12 ? "vkWaitSemaphores" : "vkWaitSemaphoresKHR"));
    counter_value_ = reinterpret_cast<PFN_vkGetSemaphoreCounterValueKHR>(
        vkGetDeviceProcAddr(dev_, core12 ? "vkGetSemaphoreCounterValue" : "vkGetSemaphoreCounterValueKHR"));
    if ((dynamic_ && (!begin_rendering_ || !end_rendering_)) || (timeline_ok_ && (!wait_semaphores_ || !counter_value_))) {
        LOGE("vulkan: device functions missing");
        return false;
    }
    // the depth buffer: 24 bit with stencil as on the console where the GPU has it
    auto usable = [&](VkFormat f) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(pd_, f, &fp);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        return (fp.optimalTilingFeatures & need) == need;
    };
    depth_fmt_ = usable(VK_FORMAT_D24_UNORM_S8_UINT) ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(pd_, depth_fmt_, &fp);
    depth_linear_ = fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    vkGetPhysicalDeviceFormatProperties(pd_, VK_FORMAT_R32_SFLOAT, &fp);
    r32f_linear_ = fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    LOGI("vulkan: depth %s, BC textures %s", depth_fmt_ == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8",
         bc_ ? "on the GPU" : "decoded on the CPU");
    return true;
}

bool VulkanBackend::CreateSwapchain() {
    if (xr_) {
        // the virtual screen's images stand in for the window's: DrawFrame draws the game's frame into them
        swap_fmt_ = xr::ScreenViewFormat();
        swap_extent_ = xr::ScreenExtent();
        swap_images_ = xr::ScreenImages();
        for (VkImage im : swap_images_) {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = im;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = swap_fmt_;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView v = VK_NULL_HANDLE;
            VK_CHECK(vkCreateImageView(dev_, &vi, nullptr, &v));
            swap_views_.push_back(v);
        }
        return !swap_images_.empty();
    }
    if (!surface_) return false;
    VkSurfaceCapabilitiesKHR caps;
    VkResult cr = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd_, surface_, &caps);
    if (cr != VK_SUCCESS) {
        if (cr == VK_ERROR_SURFACE_LOST_KHR) surface_lost_ = true;
        LOGE("vulkan: surface capabilities failed (%d)", int(cr));
        return false;
    }
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd_, surface_, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd_, surface_, &n, formats.data());
    if (formats.empty()) return false;
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats)
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    // the window's own size, landscape; Android's compositor turns the picture with the screen
    int w, h;
    host::WindowPixels(w, h);
    if (w <= 0 || h <= 0) {
        w = int(caps.currentExtent.width);
        h = int(caps.currentExtent.height);
    }
    if (w <= 0 || h <= 0) return false;
    vsync_ = app::Get().vsync;
    uint32_t pn = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd_, surface_, &pn, nullptr);
    std::vector<VkPresentModeKHR> modes(pn);
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd_, surface_, &pn, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync_) {
        for (VkPresentModeKHR m : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR})
            if (std::find(modes.begin(), modes.end(), m) != modes.end()) {
                mode = m;
                break;
            }
    }
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = {uint32_t(w), uint32_t(h)};
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                          ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                          : caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swap_;
    // retiring the old chain gives its images back to the window: the GPU must be done drawing them
    if (swap_) vkQueueWaitIdle(queue_);
    VkSwapchainKHR fresh = VK_NULL_HANDLE;
    VkResult sr = vkCreateSwapchainKHR(dev_, &ci, nullptr, &fresh);
    if (sr != VK_SUCCESS) {
        if (sr == VK_ERROR_SURFACE_LOST_KHR || sr == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) surface_lost_ = true;
        LOGE("vulkan: swap chain %dx%d failed (%d)", w, h, int(sr));
        return false;
    }
    DestroySwapchain();
    swap_ = fresh;
    swap_fmt_ = chosen.format;
    swap_extent_ = ci.imageExtent;
    uint32_t count = 0;
    vkGetSwapchainImagesKHR(dev_, swap_, &count, nullptr);
    swap_images_.resize(count);
    vkGetSwapchainImagesKHR(dev_, swap_, &count, swap_images_.data());
    for (VkImage im : swap_images_) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = im;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = swap_fmt_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v = VK_NULL_HANDLE;
        vkCreateImageView(dev_, &vi, nullptr, &v);
        swap_views_.push_back(v);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore s = VK_NULL_HANDLE;
        vkCreateSemaphore(dev_, &si, nullptr, &s);
        swap_done_.push_back(s);
    }
    swap_stale_ = false;
    LOGI("vulkan: swap chain %ux%u, %u images, %s", w, h, count, vsync_ ? "vsync" : "no vsync");
    return true;
}

// the old chain's views and semaphores; the chain itself when it is not being replaced
void VulkanBackend::DestroySwapchain() {
    if (!swap_) return;
    vkQueueWaitIdle(queue_);
    for (VkImageView v : swap_views_) {
        DropFramebuffers(v);
        vkDestroyImageView(dev_, v, nullptr);
    }
    for (VkSemaphore s : swap_done_) vkDestroySemaphore(dev_, s, nullptr);
    swap_views_.clear();
    swap_done_.clear();
    swap_images_.clear();
    vkDestroySwapchainKHR(dev_, swap_, nullptr);
    swap_ = VK_NULL_HANDLE;
}

bool VulkanBackend::CreateLayouts() {
    VkDescriptorSetLayoutBinding b[vkshader::kBindings] = {};
    for (uint32_t i = 0; i < vkshader::kBindings; i++) {
        b[i].binding = i;
        b[i].descriptorCount = 1;
        b[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                              : i < vkshader::kBindingSamplers ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                                               : VK_DESCRIPTOR_TYPE_SAMPLER;
        b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = vkshader::kBindings;
    li.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(dev_, &li, nullptr, &set_layout_));
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &set_layout_;
    // the eye matrix every draw pushes (EyeHlsl)
    VkPushConstantRange eye{VK_SHADER_STAGE_VERTEX_BIT, 0, 80};  // and the clip plane
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &eye;
    VK_CHECK(vkCreatePipelineLayout(dev_, &pi, nullptr, &layout_));
    // pipelines from earlier runs: building them is what stutters on a phone
    std::vector<char> blob;
    std::ifstream f(app::ExeDir() / "shadercache" / "pipelines.bin", std::ios::binary);
    if (f) blob.assign(std::istreambuf_iterator<char>(f), {});
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = blob.size();
    ci.pInitialData = blob.data();
    if (vkCreatePipelineCache(dev_, &ci, nullptr, &cache_) != VK_SUCCESS) {
        ci.initialDataSize = 0;
        VK_CHECK(vkCreatePipelineCache(dev_, &ci, nullptr, &cache_));
    }
    cache_saved_ = blob.size();
    return true;
}

bool VulkanBackend::CreateFrames() {
    VkSemaphoreTypeCreateInfoKHR ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO_KHR};
    ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE_KHR;
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
    if (timeline_ok_) VK_CHECK(vkCreateSemaphore(dev_, &si, nullptr, &timeline_));
    for (Frame& f : frames_) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = qfam_;
        VK_CHECK(vkCreateCommandPool(dev_, &pi, nullptr, &f.pool));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(dev_, &ai, &f.cb));
        if (!CreateBuffer(f.upload, kUploadSize,
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          true))
            return false;
        VkSemaphoreCreateInfo bi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(dev_, &bi, nullptr, &f.acquire));
    }
    return true;
}

bool VulkanBackend::CreateFixedImages() {
    constexpr VkImageUsageFlags kSampled = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!CreateImage(lut_, kBgra, 256, 1, 1, 1, 1, kSampled, VK_IMAGE_VIEW_TYPE_2D) ||
        !CreateImage(dummy_[0], kBgra, 1, 1, 1, 1, 1, kSampled, VK_IMAGE_VIEW_TYPE_2D) ||
        !CreateImage(dummy_[1], kBgra, 1, 1, 1, 1, 1, kSampled, VK_IMAGE_VIEW_TYPE_3D) ||
        !CreateImage(dummy_[2], kBgra, 1, 1, 1, 6, 1, kSampled, VK_IMAGE_VIEW_TYPE_CUBE, true))
        return false;
    VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
    sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sc.maxLod = VK_LOD_CLAMP_NONE;
    linear_ = Sampler(sc);
    sc.magFilter = sc.minFilter = VK_FILTER_NEAREST;
    sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    point_ = Sampler(sc);
    // the first frame's commands: the fixed images cleared to black, the ramp the identity
    Begin();
    VkClearColorValue black{};
    for (Image* im : {&lut_, &dummy_[0], &dummy_[1], &dummy_[2]}) {
        Transition(*im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, im->layers};
        vkCmdClearColorImage(Cb(), im->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &r);
        Transition(*im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    for (int i = 0; i < 768; i++) ramp_[i] = uint8_t(i & 255);
    UploadGammaRamp();
    Submit(true, false);
    // Submit(true) began the next recording; Init begins once more
    vkEndCommandBuffer(Cb());
    vkResetCommandPool(dev_, CurrentFrame().pool, 0);
    return true;
}

bool VulkanBackend::InitUi() {
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = 16;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(dev_, &pi, nullptr, &ui_pool_) != VK_SUCCESS) return false;
    std::lock_guard lock(app::UiLock());
    if (!ImGui::GetCurrentContext()) return false;
    ImGui_ImplVulkan_InitInfo ii{};
    ii.Instance = instance_;
    ii.PhysicalDevice = pd_;
    ii.Device = dev_;
    ii.QueueFamily = qfam_;
    ii.Queue = queue_;
    ii.DescriptorPool = ui_pool_;
    ii.MinImageCount = 2;
    ii.ImageCount = uint32_t(std::max<size_t>(swap_images_.size(), 2));
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.UseDynamicRendering = dynamic_;
    ii.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swap_fmt_;
    if (!dynamic_) ii.RenderPass = Pass(swap_fmt_, VK_FORMAT_UNDEFINED);
    ImGui::GetIO().BackendRendererName = "vulkan";
    return ImGui_ImplVulkan_Init(&ii);
}

// ---- memory and objects

bool VulkanBackend::CreateImage(Image& im, VkFormat fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t layers,
                                uint32_t levels, VkImageUsageFlags usage, VkImageViewType vt, bool cube) {
    im = {};
    im.fmt = fmt;
    im.w = w;
    im.h = h;
    im.d = d;
    im.layers = layers;
    im.levels = levels;
    im.depth = fmt == depth_fmt_;
    im.stencil = im.depth && HasStencil(fmt);
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ci.imageType = d > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, d};
    ci.mipLevels = levels;
    ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev_, &ci, nullptr, &im.image) != VK_SUCCESS) return false;
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(dev_, im.image, &r);
    if (!alloc_.Get(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false, im.mem) ||
        vkBindImageMemory(dev_, im.image, im.mem.mem, im.mem.off) != VK_SUCCESS) {
        Destroy(im);
        return false;
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.image;
    vi.viewType = vt;
    vi.format = fmt;
    vi.subresourceRange = {VkImageAspectFlags(im.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0,
                           levels, 0, layers};
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) vkCreateImageView(dev_, &vi, nullptr, &im.view);
    if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.subresourceRange = {VkImageAspectFlags(im.depth ? VK_IMAGE_ASPECT_DEPTH_BIT | (im.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)
                                                           : VK_IMAGE_ASPECT_COLOR_BIT),
                               0, 1, 0, 1};
        vkCreateImageView(dev_, &vi, nullptr, &im.attach);
    }
    return true;
}

bool VulkanBackend::CreateBuffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
    b = {};
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    if (vkCreateBuffer(dev_, &ci, nullptr, &b.buf) != VK_SUCCESS) return false;
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(dev_, b.buf, &r);
    VkMemoryPropertyFlags want = host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                      : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VkMemoryPropertyFlags nice = host && (usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0;
    if (!alloc_.Get(r, want, nice, true, b.mem) || vkBindBufferMemory(dev_, b.buf, b.mem.mem, b.mem.off) != VK_SUCCESS) {
        Destroy(b);
        return false;
    }
    return true;
}

void VulkanBackend::Destroy(Image& im) {
    if (im.view) vkDestroyImageView(dev_, im.view, nullptr);
    DropFramebuffers(im.attach);
    if (im.attach) vkDestroyImageView(dev_, im.attach, nullptr);
    if (im.image) vkDestroyImage(dev_, im.image, nullptr);
    alloc_.Free(im.mem);
    im = {};
}

void VulkanBackend::Destroy(Buffer& b) {
    if (b.buf) vkDestroyBuffer(dev_, b.buf, nullptr);
    alloc_.Free(b.mem);
    b = {};
}

// bytes in this frame's ring; a big upload gets a buffer of its own, released with the frame. A full ring flushes:
// callers upload before they begin rendering and bind
VulkanBackend::Alloc VulkanBackend::Upload(VkDeviceSize size, VkDeviceSize align) {
    if (size > kUploadSize / 4) {
        Buffer b;
        if (!CreateBuffer(b, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          true))
            return {VK_NULL_HANDLE, 0, nullptr};
        Alloc a{b.buf, 0, b.mem.map};
        CurrentFrame().buffers.push_back(b);
        return a;
    }
    Frame* f = &CurrentFrame();
    VkDeviceSize o = (f->used + align - 1) / align * align;
    if (o + size > kUploadSize) {
        Submit(true, false);
        f = &CurrentFrame();
        o = 0;
    }
    f->used = o + size;
    return {f->upload.buf, o, f->upload.mem.map + o};
}

VulkanBackend::Alloc VulkanBackend::Constants(const void* data, size_t bytes) {
    Alloc a = Upload(kUboRange, kUboAlign);
    std::memcpy(a.cpu, data, std::min<size_t>(bytes, kUboRange));
    return a;
}

// ---- command recording

void VulkanBackend::Begin() {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(Cb(), &bi);
    rendering_ = false;
    rend_color_ = rend_depth_ = nullptr;
}

uint64_t VulkanBackend::Completed() {
    if (!timeline_ok_) {
        std::lock_guard lock(fence_lock_);
        PollFences();
        return fenced_;
    }
    uint64_t v = 0;
    counter_value_(dev_, timeline_, &v);
    return v;
}

// the signalled fences, oldest first, back to the spares; called with fence_lock_ held
void VulkanBackend::PollFences() {
    size_t done = 0;
    while (done < pending_.size() && vkGetFenceStatus(dev_, pending_[done].fence) == VK_SUCCESS) {
        fenced_ = pending_[done].value;
        vkResetFences(dev_, 1, &pending_[done].fence);
        spare_fences_.push_back(pending_[done].fence);
        done++;
    }
    pending_.erase(pending_.begin(), pending_.begin() + done);
}

// the fence the submit of this value signals; called with fence_lock_ held
VkFence VulkanBackend::SubmitFence(uint64_t value) {
    VkFence f = VK_NULL_HANDLE;
    if (!spare_fences_.empty()) {
        f = spare_fences_.back();
        spare_fences_.pop_back();
    } else {
        VkFenceCreateInfo ci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(dev_, &ci, nullptr, &f) != VK_SUCCESS) return VK_NULL_HANDLE;
    }
    pending_.push_back({value, f});
    return f;
}

void VulkanBackend::WaitValue(uint64_t v) {
    if (!v || Completed() >= v) return;
    if (!timeline_ok_) {
        // the lock is held through the wait so no fence is reset under it; the GPU needs no thread to get there
        std::lock_guard lock(fence_lock_);
        for (const Pending& p : pending_)
            if (p.value >= v) {
                vkWaitForFences(dev_, 1, &p.fence, VK_TRUE, UINT64_MAX);
                break;
            }
        PollFences();
        return;
    }
    VkSemaphoreWaitInfoKHR wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO_KHR};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline_;
    wi.pValues = &v;
    wait_semaphores_(dev_, &wi, UINT64_MAX);
}

// the frame's garbage freed and its pools reset, once the GPU is past it
void VulkanBackend::ResetFrame(Frame& f) {
    WaitValue(f.value);
    for (Image& im : f.images) Destroy(im);
    for (Buffer& b : f.buffers) Destroy(b);
    f.images.clear();
    f.buffers.clear();
    for (VkDescriptorPool p : f.pools) vkResetDescriptorPool(dev_, p, 0);
    f.pool_at = 0;
    f.sets.clear();
    f.used = 0;
}

// present: the frame's last commands, waiting on the acquired image and signalling its present semaphore
void VulkanBackend::Submit(bool wait, bool present) {
    EndRender();
    Frame& f = CurrentFrame();
    vkEndCommandBuffer(f.cb);
    uint64_t signal = ++value_;
    // without the timeline only the present semaphore is signalled, and a fence tells when the GPU is past
    VkSemaphore signals[2] = {timeline_, VK_NULL_HANDLE};
    uint64_t values[2] = {signal, 0};
    uint32_t first = timeline_ok_ ? 0 : 1;
    VkTimelineSemaphoreSubmitInfoKHR ti{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR};
    ti.signalSemaphoreValueCount = present ? 2 : 1;
    ti.pSignalSemaphoreValues = values;
    uint64_t wait_value = 0;
    ti.waitSemaphoreValueCount = present ? 1 : 0;
    ti.pWaitSemaphoreValues = &wait_value;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, timeline_ok_ ? &ti : nullptr};
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (present) {
        signals[1] = swap_done_[present_index_];
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f.acquire;
        si.pWaitDstStageMask = &stage;
    }
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cb;
    si.signalSemaphoreCount = (present ? 2 : 1) - first;
    si.pSignalSemaphores = signals + first;
    VkResult r;
    if (timeline_ok_) {
        r = vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    } else {
        std::lock_guard lock(fence_lock_);
        r = vkQueueSubmit(queue_, 1, &si, SubmitFence(signal));
    }
    if (r != VK_SUCCESS)
        LOGE("vulkan: submit %llu%s failed (%d)", (unsigned long long)signal, present ? " (present)" : "", int(r));
    f.value = signal;
    if (wait) {
        ResetFrame(f);
        vkResetCommandPool(dev_, f.pool, 0);
        Begin();
    }
}

void VulkanBackend::Transition(Image& im, VkImageLayout to) {
    if (im.layout == to) return;
    EndRender();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    VkPipelineStageFlags src, dst;
    LayoutMasks(im.layout, src, b.srcAccessMask);
    LayoutMasks(to, dst, b.dstAccessMask);
    if (im.layout == VK_IMAGE_LAYOUT_UNDEFINED) src = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    b.oldLayout = im.layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im.image;
    VkImageAspectFlags aspect = im.depth ? VK_IMAGE_ASPECT_DEPTH_BIT | (im.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)
                                         : VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange = {aspect, 0, im.levels, 0, im.layers};
    vkCmdPipelineBarrier(Cb(), src, dst, 0, 0, nullptr, 0, nullptr, 1, &b);
    im.layout = to;
}

// a rendering over the whole of the attachments, which keep what they hold; the one under way carries on when it
// has the same ones
void VulkanBackend::BeginRender(Image* color, Image* depth) {
    if (rendering_ && color == rend_color_ && depth == rend_depth_) return;
    EndRender();
    if (color) Transition(*color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (depth) Transition(*depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    // the renderings before wrote the same attachments: their writes come first
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkPipelineStageFlags st = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    vkCmdPipelineBarrier(Cb(), st, st, 0, 1, &mb, 0, nullptr, 0, nullptr);
    rendering_ = true;
    rend_color_ = color;
    rend_depth_ = depth;
    if (!dynamic_) {
        Image* any = color ? color : depth;
        VkFormat cf = color ? color->fmt : VK_FORMAT_UNDEFINED, df = depth ? depth->fmt : VK_FORMAT_UNDEFINED;
        VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        bi.renderPass = Pass(cf, df);
        bi.framebuffer = Framebuffer(cf, df, color ? color->attach : VK_NULL_HANDLE, depth ? depth->attach : VK_NULL_HANDLE,
                                     any->w, any->h);
        bi.renderArea = {{0, 0}, {any->w, any->h}};
        vkCmdBeginRenderPass(Cb(), &bi, VK_SUBPASS_CONTENTS_INLINE);
        return;
    }
    VkRenderingAttachmentInfoKHR ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
    VkRenderingAttachmentInfoKHR da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
    VkRenderingInfoKHR ri{VK_STRUCTURE_TYPE_RENDERING_INFO_KHR};
    Image* any = color ? color : depth;
    ri.renderArea = {{0, 0}, {any->w, any->h}};
    ri.layerCount = 1;
    if (color) {
        ca.imageView = color->attach;
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
    }
    if (depth) {
        da.imageView = depth->attach;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.pDepthAttachment = &da;
        if (depth->stencil) ri.pStencilAttachment = &da;
    }
    begin_rendering_(Cb(), &ri);
}

void VulkanBackend::EndRender() {
    if (!rendering_) return;
    if (dynamic_)
        end_rendering_(Cb());
    else
        vkCmdEndRenderPass(Cb());
    rendering_ = false;
    rend_color_ = rend_depth_ = nullptr;
}

// the layouts stay the attachment ones throughout, as with dynamic rendering: barriers outside do the transitions.
// pipelines only need a compatible pass, which load ops are not part of
VkRenderPass VulkanBackend::Pass(VkFormat color, VkFormat depth, bool clear) {
    const uint64_t k[3] = {uint64_t(color), uint64_t(depth), uint64_t(clear)};
    uint64_t key = Hash(k, sizeof(k));
    if (auto it = passes_.find(key); it != passes_.end()) return it->second;
    VkAttachmentDescription at[2] = {};
    VkAttachmentReference cr{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dr{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    uint32_t n = 0;
    if (color != VK_FORMAT_UNDEFINED) {
        at[n] = {0, color, VK_SAMPLE_COUNT_1_BIT, clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
                 VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        cr.attachment = n++;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &cr;
    }
    if (depth != VK_FORMAT_UNDEFINED) {
        at[n] = {0, depth, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
                 VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        dr.attachment = n++;
        sp.pDepthStencilAttachment = &dr;
    }
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = n;
    ci.pAttachments = at;
    ci.subpassCount = 1;
    ci.pSubpasses = &sp;
    VkRenderPass rp = VK_NULL_HANDLE;
    if (vkCreateRenderPass(dev_, &ci, nullptr, &rp) != VK_SUCCESS) LOGE("vulkan: render pass failed");
    passes_[key] = rp;
    return rp;
}

VkFramebuffer VulkanBackend::Framebuffer(VkFormat color_fmt, VkFormat depth_fmt, VkImageView color, VkImageView depth,
                                         uint32_t w, uint32_t h) {
    const uint64_t k[4] = {uint64_t(color), uint64_t(depth), w, h};
    uint64_t key = Hash(k, sizeof(k));
    if (auto it = framebuffers_.find(key); it != framebuffers_.end()) return it->second.fb;
    VkImageView views[2];
    uint32_t n = 0;
    if (color) views[n++] = color;
    if (depth) views[n++] = depth;
    VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    ci.renderPass = Pass(color_fmt, depth_fmt);
    ci.attachmentCount = n;
    ci.pAttachments = views;
    ci.width = w;
    ci.height = h;
    ci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(dev_, &ci, nullptr, &fb) != VK_SUCCESS) LOGE("vulkan: framebuffer %ux%u failed", w, h);
    framebuffers_[key] = {color, depth, fb};
    return fb;
}

// a view's framebuffers go with it, once the GPU is past them
void VulkanBackend::DropFramebuffers(VkImageView v) {
    if (!v || framebuffers_.empty()) return;
    for (auto it = framebuffers_.begin(); it != framebuffers_.end();) {
        if (it->second.color == v || it->second.depth == v) {
            vkDestroyFramebuffer(dev_, it->second.fb, nullptr);
            it = framebuffers_.erase(it);
        } else {
            ++it;
        }
    }
}

// in the D3D convention, y down from the top: a negative height turns Vulkan's clip space the same way up
void VulkanBackend::SetViewport(float x, float y, float w, float h) {
    VkViewport vp{x, y + h, w, -h, 0, 1};
    vkCmdSetViewport(Cb(), 0, 1, &vp);
}

void VulkanBackend::SetScissor(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    VkRect2D r{{std::max(x0, 0), std::max(y0, 0)}, {uint32_t(std::max(x1 - std::max(x0, 0), 0)), uint32_t(std::max(y1 - std::max(y0, 0), 0))}};
    vkCmdSetScissor(Cb(), 0, 1, &r);
}

// the draw's descriptor set: the ring at b0 and b1 (offsets bound with it), the views and samplers; one per
// combination a frame
VkDescriptorSet VulkanBackend::Set(const VkImageView views[4], const VkSampler samplers[4]) {
    Frame& f = CurrentFrame();
    uint64_t key = Hash(views, sizeof(VkImageView) * 4, Hash(samplers, sizeof(VkSampler) * 4));
    if (auto it = f.sets.find(key); it != f.sets.end()) return it->second;
    VkDescriptorSet set = VK_NULL_HANDLE;
    for (;;) {
        if (f.pool_at == f.pools.size()) {
            VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kSetsPerPool * 2},
                                             {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSetsPerPool * 4},
                                             {VK_DESCRIPTOR_TYPE_SAMPLER, kSetsPerPool * 4}};
            VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pi.maxSets = kSetsPerPool;
            pi.poolSizeCount = 3;
            pi.pPoolSizes = sizes;
            VkDescriptorPool p = VK_NULL_HANDLE;
            if (vkCreateDescriptorPool(dev_, &pi, nullptr, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
            f.pools.push_back(p);
        }
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = f.pools[f.pool_at];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set_layout_;
        if (vkAllocateDescriptorSets(dev_, &ai, &set) == VK_SUCCESS) break;
        f.pool_at++;
    }
    VkDescriptorBufferInfo bi{f.upload.buf, 0, kUboRange};
    VkDescriptorImageInfo ii[4], si[4];
    VkWriteDescriptorSet w[vkshader::kBindings];
    for (uint32_t i = 0; i < vkshader::kBindings; i++) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        if (i < 2) {
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            w[i].pBufferInfo = &bi;
        } else if (i < vkshader::kBindingSamplers) {
            uint32_t s = i - vkshader::kBindingTextures;
            ii[s] = {VK_NULL_HANDLE, views[s] ? views[s] : dummy_[0].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            w[i].pImageInfo = &ii[s];
        } else {
            uint32_t s = i - vkshader::kBindingSamplers;
            si[s] = {samplers[s] ? samplers[s] : point_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            w[i].pImageInfo = &si[s];
        }
    }
    vkUpdateDescriptorSets(dev_, vkshader::kBindings, w, 0, nullptr);
    f.sets[key] = set;
    return set;
}

void VulkanBackend::BindSet(VkDescriptorSet set, VkDeviceSize vc, VkDeviceSize dc) {
    uint32_t offs[2] = {uint32_t(vc), uint32_t(dc)};
    vkCmdBindDescriptorSets(Cb(), VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 2, offs);
}

// ---- shaders and pipelines

VkShaderModule VulkanBackend::Module(const std::string& src, const char* entry, vkshader::Stage stage, uint64_t& hash) {
    uint64_t seed = Hash(entry, std::strlen(entry), stage == vkshader::Stage::Vertex ? 1 : 2);
    hash = Hash(src.data(), src.size(), seed);
    if (auto it = modules_.find(hash); it != modules_.end()) return it->second;
    std::vector<uint32_t> spirv;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkshader::Compile(src, entry, stage, hash, spirv)) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = spirv.size() * 4;
        ci.pCode = spirv.data();
        if (vkCreateShaderModule(dev_, &ci, nullptr, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
    }
    modules_[hash] = m;
    return m;
}

// a full screen pass: the triangle from SV_VertexID, solid, no culling; depth set: writes depth only
VkPipeline VulkanBackend::FullScreenPipeline(const char* hlsl, const char* ps_entry, VkFormat color, VkFormat depth,
                                             bool over) {
    uint64_t key = Hash(hlsl, std::strlen(hlsl), uint64_t(color) << 32 | uint64_t(depth) << 1 | (over ? 1 : 0));
    key = Hash(ps_entry, std::strlen(ps_entry), key);
    if (auto it = pipelines_.find(key); it != pipelines_.end()) return it->second;
    uint64_t hv, hp;
    VkShaderModule vs = Module(hlsl, "vs", vkshader::Stage::Vertex, hv);
    VkShaderModule ps = Module(hlsl, ps_entry, vkshader::Stage::Pixel, hp);
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vs && ps) {
        VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = vs;
        st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = ps;
        st[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = ds.depthWriteEnable = depth != VK_FORMAT_UNDEFINED;
        ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        VkPipelineColorBlendAttachmentState ba{};
        ba.colorWriteMask = 0xF;
        if (over) {
            ba.blendEnable = VK_TRUE;
            ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        }
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = color != VK_FORMAT_UNDEFINED ? 1 : 0;
        cb.pAttachments = &ba;
        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfoKHR rinfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
        rinfo.colorAttachmentCount = cb.attachmentCount;
        rinfo.pColorAttachmentFormats = &color;
        rinfo.depthAttachmentFormat = depth;
        rinfo.stencilAttachmentFormat = HasStencil(depth) ? depth : VK_FORMAT_UNDEFINED;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, dynamic_ ? &rinfo : nullptr};
        if (!dynamic_) pi.renderPass = Pass(color, depth);
        pi.stageCount = 2;
        pi.pStages = st;
        pi.pVertexInputState = &vi;
        pi.pInputAssemblyState = &ia;
        pi.pViewportState = &vp;
        pi.pRasterizationState = &rs;
        pi.pMultisampleState = &ms;
        pi.pDepthStencilState = &ds;
        pi.pColorBlendState = &cb;
        pi.pDynamicState = &dy;
        pi.layout = layout_;
        if (vkCreateGraphicsPipelines(dev_, cache_, 1, &pi, nullptr, &pipe) != VK_SUCCESS) pipe = VK_NULL_HANDLE;
    }
    pipelines_[key] = pipe;
    return pipe;
}

VkPipeline VulkanBackend::EnsurePipeline(const PsoKey& k, VkShaderModule vs, VkShaderModule ps) {
    uint64_t h = Hash(&k, sizeof(k));
    if (auto it = pipelines_.find(h); it != pipelines_.end()) return it->second;
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = ps;
    st[1].pName = "main";
    // the attributes at their register's location, as vk_shader.cpp numbers the inputs
    VkVertexInputBindingDescription vb[16];
    VkVertexInputAttributeDescription va[16];
    uint32_t na = 0;
    for (uint32_t i = 0; i < k.buffers; i++) vb[i] = {i, k.strides[i], VK_VERTEX_INPUT_RATE_VERTEX};
    for (uint32_t r = 0; r < 16; r++) {
        if (!((k.regs >> r) & 1)) continue;
        uint32_t e = k.elements[r];
        va[na++] = {r, (e >> 8) & 0xFF, VertexFormat(e & 0xFF), e >> 16};
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = k.buffers;
    vi.pVertexBindingDescriptions = vb;
    vi.vertexAttributeDescriptionCount = na;
    vi.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    Topology topo = Topology(k.topo & 0xF);
    bool strip = k.topo >> 4;
    ia.topology = topo == Topology::Points ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                  : topo == Topology::Lines ? (strip ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP : VK_PRIMITIVE_TOPOLOGY_LINE_LIST)
                                            : (strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    constexpr uint32_t kFillWireframe = 0x1B01;
    rs.polygonMode = k.fill == kFillWireframe && feats_.fillModeNonSolid ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = k.cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = k.ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.depthBiasEnable = k.zbias != 0 || k.zslope != 0;
    rs.depthBiasConstantFactor = float(k.zbias);
    rs.depthBiasSlopeFactor = k.zslope;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = k.zen;
    ds.depthWriteEnable = k.zwrite;
    ds.depthCompareOp = Cmp(k.zfunc);
    ds.stencilTestEnable = k.sten;
    VkStencilOpState so{StencilOp(k.sfail), StencilOp(k.spass), StencilOp(k.szfail), Cmp(k.sfunc), k.smask, k.swmask, 0};
    ds.front = ds.back = so;
    VkPipelineColorBlendAttachmentState ba{};
    ba.blendEnable = k.blend;
    ba.srcColorBlendFactor = Blend(k.src, false);
    ba.dstColorBlendFactor = Blend(k.dst, false);
    ba.colorBlendOp = BlendOp(k.op);
    ba.srcAlphaBlendFactor = Blend(k.src, true);
    ba.dstAlphaBlendFactor = Blend(k.dst, true);
    ba.alphaBlendOp = BlendOp(k.op);
    ba.colorWriteMask = k.wmask;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = k.has_rt ? 1 : 0;
    cb.pAttachments = &ba;
    VkDynamicState dyn[4] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                             VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 4;
    dy.pDynamicStates = dyn;
    VkFormat color = VkFormat(k.rtfmt);
    VkPipelineRenderingCreateInfoKHR rinfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    rinfo.colorAttachmentCount = cb.attachmentCount;
    rinfo.pColorAttachmentFormats = &color;
    rinfo.depthAttachmentFormat = k.has_ds ? depth_fmt_ : VK_FORMAT_UNDEFINED;
    rinfo.stencilAttachmentFormat = k.has_ds && HasStencil(depth_fmt_) ? depth_fmt_ : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, dynamic_ ? &rinfo : nullptr};
    if (!dynamic_) pi.renderPass = Pass(k.has_rt ? color : VK_FORMAT_UNDEFINED, k.has_ds ? depth_fmt_ : VK_FORMAT_UNDEFINED);
    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = layout_;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev_, cache_, 1, &pi, nullptr, &pipe) != VK_SUCCESS) {
        LOGE("vulkan: pipeline creation failed");
        pipe = VK_NULL_HANDLE;
    }
    pipelines_[h] = pipe;
    return pipe;
}

VkSampler VulkanBackend::Sampler(const VkSamplerCreateInfo& ci) {
    uint64_t key = Hash(&ci.magFilter, sizeof(VkSamplerCreateInfo) - offsetof(VkSamplerCreateInfo, magFilter));
    if (auto it = samplers_.find(key); it != samplers_.end()) return it->second;
    VkSampler s = VK_NULL_HANDLE;
    vkCreateSampler(dev_, &ci, nullptr, &s);
    samplers_[key] = s;
    return s;
}

// fixed border colours only: the console's nearest
VkBorderColor Border(uint32_t argb) {
    if ((argb >> 24) < 128) return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    uint32_t sum = ((argb >> 16) & 255) + ((argb >> 8) & 255) + (argb & 255);
    return sum >= 384 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
}

VkSampler VulkanBackend::StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp) {
    VkSamplerCreateInfo sd{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    const auto& t = s.tss[size_t(i)];
    uint32_t mag = t[kTssMagFilter], min = t[kTssMinFilter], mip = t[kTssMipFilter];
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    // an HD pack image: its mips used even where the game's texture had none (or the game set no mip filter), else a
    // 1024 image drawn small speckles (headset shot 2026-11-24, a roof panel)
    if (stages.hd[i] && lin(min)) mip = kTexfLinear;
    bool filterable = stages.fmt[i] != VK_FORMAT_R32_SFLOAT || r32f_linear_;
    sd.minFilter = lin(min) && filterable ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sd.magFilter = lin(mag) && filterable ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sd.mipmapMode = mip == kTexfLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    bool anisotropic = min == kTexfAnisotropic || mag == kTexfAnisotropic;
    uint32_t an = anisotropic ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
    uint32_t aniso = app::Get().aniso;
    bool mipped = mip && stages.levels[i] > 1;
    if (aniso && lin(min) && mipped && !stages.info[i].shadow) an = aniso;
    if (an > 1 && feats_.samplerAnisotropy) {
        sd.anisotropyEnable = VK_TRUE;
        sd.maxAnisotropy = std::min(float(an), props_.limits.maxSamplerAnisotropy);
    }
    uint32_t u = t[kTssAddressU], v = t[kTssAddressV];
    sd.addressModeU = Address(clamp && u == kTaddressWrap ? kTaddressClamp : u);
    sd.addressModeV = Address(clamp && v == kTaddressWrap ? kTaddressClamp : v);
    sd.addressModeW = Address(t[kTssAddressW]);
    float limit = props_.limits.maxSamplerLodBias;
    sd.mipLodBias = std::clamp(Bits(t[kTssMipLodBias]), -limit, limit);
    sd.minLod = std::min(float(t[kTssMaxMipLevel]), float(stages.levels[i] - 1));
    sd.maxLod = mip ? VK_LOD_CLAMP_NONE : sd.minLod;
    sd.borderColor = Border(t[kTssBorderColor]);
    if (stages.info[i].shadow) {
        // the console tests texel OP r, Vulkan r OP texel
        static const uint8_t swap[8] = {0, 4, 2, 6, 1, 5, 3, 7};
        bool linear = (lin(min) || lin(mag)) && depth_linear_;
        sd.minFilter = sd.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sd.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sd.compareEnable = VK_TRUE;
        sd.compareOp = VkCompareOp(swap[CmpIndex(s.rs[kRsShadowFunc])]);
        sd.anisotropyEnable = VK_FALSE;
        sd.maxAnisotropy = 1;
    }
    return Sampler(sd);
}

// ---- targets

Target* VulkanBackend::FindTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data), gw, gh;
    SurfaceSize(s, gw, gh);
    auto it = targets_.find(TargetMapKey(data, depth));
    if (it == targets_.end() || it->second.gw != gw || it->second.gh != gh) return nullptr;
    return &it->second;
}

void VulkanBackend::ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it) {
    if (draw_rt_ == &it->second) draw_rt_ = nullptr;
    if (draw_ds_ == &it->second) draw_ds_ = nullptr;
    Retire(it->second.img);
    Retire(it->second.fb);
    targets_.erase(it);
}

Target* VulkanBackend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data), gw, gh;
    SurfaceSize(s, gw, gh);
    uint64_t key = TargetMapKey(data, depth);
    auto it = targets_.find(key);
    if (it != targets_.end() && it->second.gw == gw && it->second.gh == gh) return &it->second;
    if (it != targets_.end()) ReleaseTarget(it);
    Target t;
    t.depth = depth;
    t.gw = gw;
    t.gh = gh;
    // (the scope's view at the game's own size: its picture is small in the eye)
    const float sc = eye_ == stereo::kScopeEye ? 1.0f : scale_;
    t.w = std::max(1u, uint32_t(std::lround(gw * sc)));
    t.h = std::max(1u, uint32_t(std::lround(gh * sc)));
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (!CreateImage(t.img, depth ? depth_fmt_ : kBgra, t.w, t.h, 1, 1, 1, usage, VK_IMAGE_VIEW_TYPE_2D)) {
        LOGE("vulkan: no %ux%u target", t.w, t.h);
        return nullptr;
    }
    // a new target starts cleared
    Transition(t.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (depth) {
        VkClearDepthStencilValue one{1, 0};
        VkImageSubresourceRange r{VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | (t.img.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)), 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(Cb(), t.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &r);
    } else {
        VkClearColorValue black{};
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(Cb(), t.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &r);
    }
    t.data = data;
    t.bytes = depth ? gw * gh * 4 : GuestPitch(gw) * gh;
    t.epoch = DirtyEpoch(data);
    t.sig = MemSig(data, t.bytes);
    Target& ref = targets_[key];
    ref = std::move(t);
    return &ref;
}

// BGRA pixels into a new w x h image, left readable
bool VulkanBackend::PixelImage(Image& im, const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h) {
    if (!CreateImage(im, kBgra, w, h, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_VIEW_TYPE_2D))
        return false;
    Alloc a = Upload(VkDeviceSize(w) * h * 4, 16);
    if (!a.cpu) return false;
    for (uint32_t y = 0; y < h; y++) std::memcpy(a.cpu + size_t(y) * w * 4, &px[size_t(y) * pitch], w * 4);
    Transition(im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c{a.off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyBufferToImage(Cb(), a.buf, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    Transition(im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

// CPU pixels of a colour surface into its target, scaled; after a Sync only the changed ones, over the picture
void VulkanBackend::UploadTarget(Target& t, const Surface& s) {
    TextureDesc d;
    if (!DescribeSurface(s, d) && !Describe(s.header, d)) return;
    if (Format(d.format).depth) return;
    std::vector<uint8_t> px;
    uint32_t pitch;
    bool over = !t.drawn && d.width == t.gw && d.height == t.gh && t.read.size() == size_t(t.gw) * t.gh * 4;
    if (over) {
        pitch = t.gw * 4;
        px = LatchCpuChanges(t);
    } else {
        DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    }
    Image tex;
    if (!PixelImage(tex, px, pitch, d.width, d.height)) return;
    float uv[4] = {0, 0, 1, 1};
    Blit(t.img, {{0, 0}, {t.w, t.h}}, tex, uv, {.alpha = true, .over = over});
    Retire(tex);
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
}

// src over the rect of dst through the blit shader: its constants first, then the rendering
void VulkanBackend::Blit(Image& dst, const VkRect2D& to, Image& src, const float uvrect[4], const BlitOptions& o) {
    float c[8] = {uvrect[0], uvrect[1], uvrect[2], uvrect[3], o.gamma ? 1.0f : 0.0f, o.alpha ? 1.0f : 0.0f,
                  float(o.taps), o.mul};
    Alloc cb = Constants(c, sizeof(c));
    VkPipeline pipe = FullScreenPipeline(kBlitHlsl, "psmain", dst.fmt, VK_FORMAT_UNDEFINED, o.over);
    if (!pipe) return;
    Transition(src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(lut_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const VkImageView views[4] = {src.view, lut_.view, VK_NULL_HANDLE, VK_NULL_HANDLE};
    const VkSampler samplers[4] = {linear_, point_, point_, point_};
    VkDescriptorSet set = Set(views, samplers);
    BeginRender(&dst, nullptr);
    vkCmdBindPipeline(Cb(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    BindSet(set, cb.off, cb.off);
    SetViewport(float(to.offset.x), float(to.offset.y), float(to.extent.width), float(to.extent.height));
    SetScissor(0, 0, int32_t(dst.w), int32_t(dst.h));
    vkCmdDraw(Cb(), 3, 1, 0, 0);
}

// a copy of the target to read while drawing to it
Image* VulkanBackend::Feedback(Target& t) {
    if (!t.fb && !CreateImage(t.fb, t.img.fmt, t.w, t.h, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              VK_IMAGE_VIEW_TYPE_2D))
        return nullptr;
    Transition(t.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(t.fb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageAspectFlags aspect = t.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageCopy c{{aspect, 0, 0, 1}, {0, 0, 0}, {aspect, 0, 0, 1}, {0, 0, 0}, {t.w, t.h, 1}};
    vkCmdCopyImage(Cb(), t.img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.fb.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    Transition(t.fb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return &t.fb;
}

// a depth buffer drawn as colour into another depth buffer's memory: copied as depth
bool VulkanBackend::DepthCopy(const State& s) {
    TextureDesc d;
    if (!s.rt.header || !s.textures[0] || !Describe(s.textures[0], d) || Format(d.format).depth || d.cube ||
        d.depth != 1)
        return false;
    auto di = targets_.find(TargetMapKey(mem::VirtualFromPhysical(s.rt.data), true));
    auto si = targets_.find(TargetMapKey(d.data, true));
    if (di == targets_.end() || si == targets_.end() || di == si) return false;
    Target& dst = di->second;
    Target& src = si->second;
    Alloc cb = Constants(nullptr, 0);
    VkPipeline pipe = FullScreenPipeline(kDepthHlsl, "psmain", VK_FORMAT_UNDEFINED, depth_fmt_, false);
    if (!pipe) return false;
    Transition(src.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const VkImageView views[4] = {src.img.view, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    const VkSampler samplers[4] = {linear_, point_, point_, point_};
    VkDescriptorSet set = Set(views, samplers);
    BeginRender(nullptr, &dst.img);
    vkCmdBindPipeline(Cb(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    BindSet(set, cb.off, cb.off);
    SetViewport(0, 0, float(dst.w), float(dst.h));
    SetScissor(0, 0, int32_t(dst.w), int32_t(dst.h));
    vkCmdDraw(Cb(), 3, 1, 0, 0);
    dst.drawn = true;
    return true;
}

// src filtered into a new w x h BGRA image, left as a copy source
bool VulkanBackend::Shrink(Image& src, uint32_t w, uint32_t h, Image& down, uint32_t taps) {
    if (!down || down.w != w || down.h != h) {
        Retire(down);
        if (!CreateImage(down, kBgra, w, h, 1, 1, 1,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_VIEW_TYPE_2D))
            return false;
    }
    float uv[4] = {0, 0, 1, 1};
    Blit(down, {{0, 0}, {w, h}}, src, uv, {.alpha = true, .taps = taps});
    Transition(down, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    return true;
}

// the target as a guest sized BGRA copy source: itself, or shrunk into down
Image* VulkanBackend::CopySource(Target& t, Image& down) {
    if (t.depth) return nullptr;
    if (t.w != t.gw || t.h != t.gh) {
        uint32_t taps = uint32_t(std::ceil(float(t.w) / t.gw - 1e-3f));
        return Shrink(t.img, t.gw, t.gh, down, std::max(taps, 1u)) ? &down : nullptr;
    }
    Transition(t.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    return &t.img;
}

// a copy source to the CPU, rows packed; waits for the GPU
bool VulkanBackend::ReadBack(Image& im, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    Buffer rb;
    if (!CreateBuffer(rb, VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) return false;
    VkBufferImageCopy c{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImageToBuffer(Cb(), im.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, &c);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(Cb(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    Submit(true, false);
    out.assign(rb.mem.map, rb.mem.map + size_t(w) * h * 4);
    Destroy(rb);
    return true;
}

// ---- draws

void VulkanBackend::EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh) {
    if (auto it = shader_src_.find(sh.vkey); it != shader_src_.end()) {
        sh.vh = it->second;
        sh.vs = modules_[sh.vh];
        return;
    }
    std::string src = VertexHlsl(s, a, sh);
    const char* entry = "main";
    if (a.topology == Topology::Points) {
        std::string points = PointsHlsl(src);
        if (!points.empty()) {
            src = std::move(points);
            entry = "main_points";
        }
    }
    src = EyeHlsl(std::move(src));
    sh.vs = Module(src, entry, vkshader::Stage::Vertex, sh.vh);
    shader_src_[sh.vkey] = sh.vh;
}

void VulkanBackend::EnsurePixelShader(const PixelKey& pk, DrawShaders& sh) {
    if (auto it = shader_src_.find(sh.pkey ^ kPixelKeySalt); it != shader_src_.end()) {
        sh.ph = it->second;
        sh.ps = modules_[sh.ph];
        return;
    }
    sh.ps = Module(PixelHlsl(pk), "main", vkshader::Stage::Pixel, sh.ph);
    shader_src_[sh.pkey ^ kPixelKeySalt] = sh.ph;
}

constexpr float kUpscaledMin = 1.01f;

VkImageView VulkanBackend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box,
                                             DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    if (&t == draw_ds_) return VK_NULL_HANDLE;
    bool feedback = &t == draw_rt_;
    stages.rt[stage] = true;
    stages.gw[stage] = t.depth ? 0 : t.gw;
    if (!t.depth && LatchCpuWrite(t)) {
        Surface su;
        su.header = s.textures[size_t(stage)];
        UploadTarget(t, su);
    }
    info.kind = kTex2D;
    Target* rt = draw_rt_;
    float k = float(t.w) / (t.gw * stages.k[stage]);
    bool blur = rt && t.gw < guest_w_ && rt->gw < guest_w_;
    if (!t.depth && box && k > kUpscaledMin && rt && rt != &t && (blur || t.gw > rt->gw || t.gh > rt->gh)) {
        stages.wide[stage] = k;
        stages.blur[stage] = blur;
        info.flags |= kTexWide;
    }
    stages.fmt[stage] = t.img.fmt;
    if (feedback) {
        Image* fb = Feedback(t);
        return fb ? fb->view : VK_NULL_HANDLE;
    }
    if (t.depth) info.shadow = ShadowDepthKind(d.format);
    Transition(t.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return t.img.view;
}

bool VulkanBackend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key) {
    Retire(tx.img);
    tx = Tex{};
    tx.d = d;
    bool bc = fi.compressed;
    tx.decode = bc && !bc_;
    switch (fi.host) {
        case Host::BC1: tx.fmt = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; break;
        case Host::BC2: tx.fmt = VK_FORMAT_BC2_UNORM_BLOCK; break;
        case Host::BC3: tx.fmt = VK_FORMAT_BC3_UNORM_BLOCK; break;
        case Host::R32F: tx.fmt = VK_FORMAT_R32_SFLOAT; break;
        default: tx.fmt = kBgra; break;
    }
    if (tx.decode) tx.fmt = kBgra;
    uint32_t w = d.width, h = d.height;
    tx.levels = d.levels;
    if (bc && ((w & 3) || (h & 3))) {
        w = (w + 3) & ~3u;
        h = (h + 3) & ~3u;
        tx.levels = 1;
    }
    VkImageViewType vt = d.cube ? VK_IMAGE_VIEW_TYPE_CUBE : d.depth > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    if (!CreateImage(tx.img, tx.fmt, w, h, d.cube ? 1 : d.depth, d.cube ? 6 : 1, tx.levels,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, vt, d.cube)) {
        textures_.erase(key);
        return false;
    }
    return true;
}

// Phase 8: the texture dump (VR SETTINGS, DUMP TEXTURES). Each 2D texture the game uploads, keyed by a hash of its
// level 0 as the game holds it (the Xbox's swizzled or DXT bytes: the same on every run and every device) with its
// format, size and palette, decoded to RGBA and written once as textures_dump/<hash>.png, with a line in
// textures_dump/index.csv (hash, format, width, height, levels). A writer thread does the PNGs; one rewritten more
// than twice (a film's frames, a texture drawn into) isn't dumped
uint64_t TextureHash(const TextureDesc& d, const uint8_t* p, uint64_t palette) {
    uint64_t seed = uint64_t(d.format) << 40 ^ uint64_t(d.width) << 20 ^ d.height;
    return gpu::Hash(p, LevelSize(d, 0), seed) ^ palette;
}

struct DumpJob {
    uint64_t hash;
    uint32_t w, h, format, levels;
    std::vector<uint8_t> bgra;
    char from;  // 'd' drawn, 'l' found loaded (the scan)
};
std::mutex g_dump_lock;
std::condition_variable g_dump_cv;
std::deque<DumpJob> g_dump_jobs;
std::set<uint64_t> g_dumped;

void WritePng(const std::filesystem::path& path, const uint8_t* bgra, uint32_t w, uint32_t h) {
    std::vector<uint8_t> raw(size_t(w * 4 + 1) * h);
    for (uint32_t y = 0; y < h; y++) {
        uint8_t* r = &raw[size_t(w * 4 + 1) * y];
        r[0] = 0;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t* s = bgra + (size_t(y) * w + x) * 4;
            r[1 + x * 4] = s[2], r[2 + x * 4] = s[1], r[3 + x * 4] = s[0], r[4 + x * 4] = s[3];
        }
    }
    uLongf zn = compressBound(uLong(raw.size()));
    std::vector<uint8_t> z(zn);
    if (compress2(z.data(), &zn, raw.data(), uLong(raw.size()), 1) != Z_OK) return;
    std::ofstream f(path, std::ios::binary);
    auto be = [&](uint32_t v) {
        uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
        f.write(reinterpret_cast<const char*>(b), 4);
    };
    auto chunk = [&](const char* tag, const uint8_t* data, uint32_t n) {
        be(n);
        uLong crc = crc32(0, reinterpret_cast<const Bytef*>(tag), 4);
        f.write(tag, 4);
        if (n) f.write(reinterpret_cast<const char*>(data), n), crc = crc32(crc, data, n);
        be(uint32_t(crc));
    };
    f.write("\x89PNG\r\n\x1a\n", 8);
    uint8_t ihdr[13] = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
                        uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 6, 0, 0, 0};
    chunk("IHDR", ihdr, 13);
    chunk("IDAT", z.data(), uint32_t(zn));
    chunk("IEND", nullptr, 0);
}

// the hashes already in textures_dump/index.csv (an earlier run's, the level-by-level run's relaunches): not again
void SeedDumped() {
    static std::once_flag seeded;
    std::call_once(seeded, [] {
        std::ifstream idx(app::ExeDir() / "textures_dump" / "index.csv");
        std::string line;
        std::lock_guard lock(g_dump_lock);
        while (std::getline(idx, line))
            if (line.size() >= 16) g_dumped.insert(std::strtoull(line.substr(0, 16).c_str(), nullptr, 16));
    });
}

void DumpTexture(DumpJob job, bool wait = false) {
    static std::once_flag started;
    std::call_once(started, [] {
        std::thread([] {
            const auto dir = app::ExeDir() / "textures_dump";
            std::filesystem::create_directories(dir);
            for (;;) {
                DumpJob j;
                {
                    std::unique_lock lock(g_dump_lock);
                    g_dump_cv.wait(lock, [] { return !g_dump_jobs.empty(); });
                    j = std::move(g_dump_jobs.front());
                    g_dump_jobs.pop_front();
                }
                char name[32];
                std::snprintf(name, sizeof(name), "%016llx", (unsigned long long)j.hash);
                WritePng(dir / (std::string(name) + ".png"), j.bgra.data(), j.w, j.h);
                std::ofstream idx(dir / "index.csv", std::ios::app);
                idx << name << ',' << Format(j.format).name << ',' << j.w << ',' << j.h << ',' << j.levels << ','
                    << j.from << '\n';
            }
        }).detach();
    });
    for (;;) {
        {
            std::lock_guard lock(g_dump_lock);
            if (g_dump_jobs.size() < 64) {
                g_dump_jobs.push_back(std::move(job));
                g_dump_cv.notify_one();
                return;
            }
            if (!wait) {
                g_dumped.erase(job.hash);  // (the writer's behind: this one again at its next upload)
                return;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));  // (the scan's thread can wait for the writer)
    }
}

// decoded to RGBA and queued, once per hash; false if already dumped
bool DumpOnce(const TextureDesc& d, const FormatInfo& fi, const uint8_t* p, const uint32_t* colours, uint64_t hash,
              char from) {
    SeedDumped();
    {
        std::lock_guard lock(g_dump_lock);
        if (!g_dumped.insert(hash).second) return false;
    }
    std::vector<uint8_t> lv, rgba;
    uint32_t pitch = 0;
    DecodeLevel(d, 0, 0, p, colours, lv, pitch);
    if (fi.compressed) {
        DecodeDxt(fi.host, lv, pitch, d.width, d.height, 1, rgba);
    } else if (fi.host != Host::R32F && pitch >= d.width * 4) {
        rgba.resize(size_t(d.width) * d.height * 4);
        for (uint32_t y = 0; y < d.height && size_t(y + 1) * pitch <= lv.size(); y++)
            std::memcpy(&rgba[size_t(y) * d.width * 4], &lv[size_t(y) * pitch], d.width * 4);
    }
    if (rgba.size() < size_t(d.width) * d.height * 4) return false;
    DumpTexture({hash, d.width, d.height, d.format, d.levels, std::move(rgba), from}, from == 'l');
    return true;
}

// Phase 8, the load-time dump: the game builds its texture headers itself (no CreateTexture to hook), so every 8 s
// guest memory is scanned for them (Common's type a texture, a 2D format of a known kind, data in physical memory)
// and each found in two scans running with the same bytes is dumped: what a level loads, drawn yet or not. A P8
// texture's palette is only known when drawn (SetPalette), so those stay the draw-time dump's
bool ScanHeader(uint32_t va, TextureDesc& d) {
    using namespace kernel;
    uint32_t common = R32(va);
    if ((common & 0x70000) != 0x40000 || (common & 0xFFFF) == 0 || (common & 0xFC000000)) return false;
    uint32_t data = R32(va + 4), lock = R32(va + 8), fmt = R32(va + 12), size = R32(va + 16);
    if (lock || !data || data >= mem::kPhysicalSize || (data & 0x7F)) return false;
    if ((fmt & 3) == 0 || (fmt & 3) == 3 || ((fmt >> 4) & 0xF) != 2 || (fmt & 4)) return false;
    if (!size && (((fmt >> 20) & 0xF) > 11 || ((fmt >> 24) & 0xF) > 11 || (fmt >> 28))) return false;
    if (!Describe(va, d) || d.cube || d.depth != 1 || d.width < 4 || d.height < 4 || d.width > 2048 || d.height > 2048)
        return false;
    const FormatInfo& fi = Format(d.format);
    if (fi.host == Host::None || fi.depth || d.format == 0x0B) return false;
    return uint64_t(data) + TotalSize(d) <= mem::kPhysicalSize;
}

void ScanTextures(std::unordered_map<uint32_t, uint64_t>& seen) {
    std::unordered_map<uint32_t, uint64_t> now;
    int found = 0, dumped = 0;
    auto range = [&](uint32_t lo, uint32_t hi, bool check) {
        for (uint32_t page = lo; page < hi; page += mem::kPage) {
            if (check && !mem::IsCommitted(page)) continue;
            for (uint32_t va = page; va < page + mem::kPage - 20; va += 4) {
                TextureDesc d;
                if (!ScanHeader(va, d)) continue;
                const auto* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
                uint64_t hash = TextureHash(d, p, 0);
                found++;
                now[va] = hash;
                auto it = seen.find(va);
                if (it != seen.end() && it->second == hash && DumpOnce(d, Format(d.format), p, nullptr, hash, 'l'))
                    dumped++;
            }
        }
    };
    range(mem::kUserLow, mem::kUserHigh, true);
    range(mem::kContiguousBase + mem::kKernelDataSize, mem::kContiguousBase + mem::kPhysicalSize, false);
    range(mem::kSystemBase, mem::kSystemBase + mem::kSystemSize, true);
    seen.swap(now);
    if (dumped) LOGR("texture dump: scan found %d texture headers, %d new dumped", found, dumped);
}

void StartTextureScan() {
    static std::once_flag started;
    std::call_once(started, [] {
        std::thread([] {
            std::unordered_map<uint32_t, uint64_t> seen;
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(8));
                ScanTextures(seen);
            }
        }).detach();
    });
}

// Phase 8, the HD pack's loader (VR SETTINGS, TEXTURES: HD PACK). textures_hd/<16 hex>.ktx2 in the app's folder,
// named by the TextureHash of the texture it replaces (the dump's key). KTX2 without supercompression, any format the
// GPU samples (the converter writes ASTC; UNORM, as the game's shaders expect gamma-space texels), its own mips. The
// folder is listed once; a file is read on a loader thread the first time its texture is bound, then uploaded on the
// render thread (at most kHdFrameBytes a frame) and bound in its place from then on. Past kHdBudget the least
// recently bound go (and can load again); a texture without a file, or a linear, cube or volume one, stays the game's
namespace hd {
// TEXTURES on HD PACK (dev_hdtex turns it on for tests)
bool On() {
    static const bool dev = std::filesystem::exists(app::ExeDir() / "dev_hdtex");
    return dev || game::Opt().vr_hd_textures;
}
constexpr size_t kBudget = size_t(1024) << 20;
constexpr size_t kFrameBytes = size_t(8) << 20;
constexpr size_t kReadyMax = 4;  // read and waiting for the render thread (the loader waits past this)

struct Data {
    uint64_t hash = 0;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    uint32_t w = 0, h = 0;
    std::vector<std::vector<uint8_t>> levels;  // level 0 first
    size_t bytes = 0;
};
std::mutex g_lock;
std::condition_variable g_cv;
std::deque<uint64_t> g_want;
std::deque<Data> g_ready;
std::set<uint64_t> g_avail, g_asked;

void List() {
    static std::once_flag listed;
    std::call_once(listed, [] {
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(app::ExeDir() / "textures_hd", ec)) {
            std::string stem = e.path().stem().string();
            if (e.path().extension() != ".ktx2" || stem.size() != 16) continue;
            g_avail.insert(std::strtoull(stem.c_str(), nullptr, 16));
        }
        LOGR("hd textures: %zu in textures_hd", g_avail.size());  // (the device's ASTC: logged at start)
    });
}

// KTX2: the 80-byte header (vkFormat at 12, width 20, height 24, depth 28, layers 32, faces 36, levels 40,
// supercompression 44), then per level an offset, a length and an uncompressed length, 64-bit each
bool Read(uint64_t hash, Data& out) {
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.ktx2", (unsigned long long)hash);
    std::ifstream f(app::ExeDir() / "textures_hd" / name, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    static const uint8_t kId[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
    if (b.size() < 80 || std::memcmp(b.data(), kId, 12)) return false;
    auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, &b[o], 4); return v; };
    auto u64 = [&](size_t o) { uint64_t v; std::memcpy(&v, &b[o], 8); return v; };
    uint32_t levels = std::max(u32(40), 1u);
    if (u32(28) > 1 || u32(32) > 1 || u32(36) != 1 || u32(44) != 0 || levels > 16 || b.size() < 80 + levels * 24)
        return false;
    out.hash = hash;
    out.fmt = VkFormat(u32(12));
    out.w = u32(20), out.h = u32(24);
    if (!out.w || !out.h || out.w > 4096 || out.h > 4096) return false;
    for (uint32_t l = 0; l < levels; l++) {
        uint64_t off = u64(80 + l * 24), len = u64(88 + l * 24);
        if (!len || off + len > b.size()) return false;
        out.levels.emplace_back(b.begin() + off, b.begin() + off + len);
        out.bytes += len;
    }
    return true;
}

// the first time a hash is wanted: read on the loader's thread
void Request(uint64_t hash) {
    static std::once_flag started;
    std::call_once(started, [] {
        std::thread([] {
            for (;;) {
                uint64_t hash;
                {
                    std::unique_lock lock(g_lock);
                    g_cv.wait(lock, [] { return !g_want.empty() && g_ready.size() < kReadyMax; });
                    hash = g_want.front();
                    g_want.pop_front();
                }
                Data d;
                if (!Read(hash, d)) {
                    LOGR("hd textures: %016llx.ktx2 unreadable (KTX2, no supercompression, 2D)", (unsigned long long)hash);
                    continue;  // (stays asked: not tried again)
                }
                std::lock_guard lock(g_lock);
                g_ready.push_back(std::move(d));
            }
        }).detach();
    });
    std::lock_guard lock(g_lock);
    if (!g_asked.insert(hash).second) return;
    g_want.push_back(hash);
    g_cv.notify_one();
}
}  // namespace hd

const Image* VulkanBackend::HdFor(uint64_t hash) {
    if (hd_frame_ != presents_) hd_frame_ = presents_, hd_frame_bytes_ = 0;
    // what the loader has read, uploaded while this frame's share lasts
    for (;;) {
        hd::Data d;
        {
            std::lock_guard lock(hd::g_lock);
            if (hd::g_ready.empty() || hd_frame_bytes_ >= hd::kFrameBytes) break;
            d = std::move(hd::g_ready.front());
            hd::g_ready.pop_front();
            hd::g_cv.notify_one();
        }
        hd_frame_bytes_ += d.bytes;
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(pd_, d.fmt, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
            LOGR("hd textures: %016llx's format %d isn't sampled here", (unsigned long long)d.hash, int(d.fmt));
            continue;
        }
        // over the budget: the least recently bound go first (asked again if bound later)
        while (hd_bytes_ + d.bytes > hd::kBudget && !hd_.empty()) {
            auto old = hd_.begin();
            for (auto it = hd_.begin(); it != hd_.end(); ++it)
                if (it->second.used < old->second.used) old = it;
            if (old->second.used + 2 >= presents_) break;  // (all in use now: the new one waits)
            hd_bytes_ -= old->second.bytes;
            Retire(old->second.img);
            {
                std::lock_guard lock(hd::g_lock);
                hd::g_asked.erase(old->first);
            }
            hd_.erase(old);
        }
        if (hd_bytes_ + d.bytes > hd::kBudget) {
            std::lock_guard lock(hd::g_lock);
            hd::g_asked.erase(d.hash);
            continue;
        }
        HdImage& e = hd_[d.hash];
        uint32_t levels = uint32_t(d.levels.size());
        if (!CreateImage(e.img, d.fmt, d.w, d.h, 1, 1, levels, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         VK_IMAGE_VIEW_TYPE_2D)) {
            hd_.erase(d.hash);
            continue;
        }
        Transition(e.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        for (uint32_t l = 0; l < levels; l++) {
            const auto& px = d.levels[l];
            Alloc a = Upload(px.size(), 16);
            if (!a.cpu) continue;
            std::memcpy(a.cpu, px.data(), px.size());
            VkBufferImageCopy c{a.off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1}, {0, 0, 0},
                                {std::max(d.w >> l, 1u), std::max(d.h >> l, 1u), 1}};
            vkCmdCopyBufferToImage(Cb(), a.buf, e.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        }
        Transition(e.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        e.bytes = d.bytes;
        e.used = presents_;
        hd_bytes_ += d.bytes;
    }
    auto it = hd_.find(hash);
    if (it != hd_.end()) {
        it->second.used = presents_;
        return &it->second.img;
    }
    if (hd::g_avail.count(hash) && hd_bytes_ < hd::kBudget) hd::Request(hash);
    return nullptr;
}

void VulkanBackend::UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p,
                                  const uint32_t* colours) {
    tx.uploads++;
    static const bool dev_dump = std::filesystem::exists(app::ExeDir() / "dev_texdump");  // (the setting, for tests)
    if ((game::Opt().vr_dump_textures || dev_dump) && !d.cube && d.depth == 1 && !fi.depth && tx.uploads <= 2 && d.width >= 4 &&
        d.height >= 4) {
        StartTextureScan();  // (the load-time dump, from the first upload with the setting on)
        DumpOnce(d, fi, p, colours, TextureHash(d, p, tx.palette), 'd');
    }
    tx.hash = 0;
    if (hd::On() && !d.cube && d.depth == 1 && !d.linear) {
        hd::List();
        if (!hd::g_avail.empty()) tx.hash = TextureHash(d, p, tx.palette);
    }
    Transition(tx.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    uint32_t faces = d.cube ? 6 : 1;
    std::vector<uint8_t> px, decoded;
    bool bc = fi.compressed && !tx.decode;
    uint32_t block = fi.host == Host::BC1 ? 8 : 16;
    for (uint32_t f = 0; f < faces; f++)
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch;
            DecodeLevel(d, l, f, p, colours, px, pitch);
            uint32_t mw = std::max(tx.img.w >> l, 1u), mh = std::max(tx.img.h >> l, 1u), md = std::max(tx.img.d >> l, 1u);
            const std::vector<uint8_t>* src = &px;
            if (tx.decode) {
                DecodeDxt(fi.host, px, pitch, mw, mh, md, decoded);
                pitch = mw * 4;
                src = &decoded;
            }
            // rows as Vulkan reads them packed: block rows for DXT, texel rows else
            uint32_t row = bc ? std::max(1u, (mw + 3) / 4) * block : mw * 4;
            uint32_t rows = bc ? std::max(1u, (mh + 3) / 4) : mh;
            size_t src_rows = pitch ? src->size() / pitch / md : 0;
            Alloc a = Upload(VkDeviceSize(row) * rows * md, 16);
            if (!a.cpu) continue;
            for (uint32_t z = 0; z < md; z++)
                for (uint32_t y = 0; y < rows; y++) {
                    uint8_t* dst = a.cpu + (size_t(z) * rows + y) * row;
                    if (y < src_rows) std::memcpy(dst, &(*src)[(size_t(z) * src_rows + y) * pitch], std::min(pitch, row));
                    else std::memset(dst, 0, row);
                }
            VkBufferImageCopy c{a.off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, l, f, 1}, {0, 0, 0}, {mw, mh, md}};
            vkCmdCopyBufferToImage(Cb(), a.buf, tx.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        }
    Transition(tx.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

VkImageView VulkanBackend::BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                            DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    StagePalette palette;
    ReadStagePalette(s, stage, d, palette);
    const uint64_t key = uint64_t(palette.va) << 32 | d.data;
    Tex& tx = textures_[key];
    bool same = SameTexture(tx, d);
    if (!same && !CreateTexture(tx, d, fi, key)) return VK_NULL_HANDLE;
    Tex& t = textures_[key];
    if (t.frame != presents_ + 1 || !same || t.palette != palette.hash) {
        t.frame = presents_ + 1;
        t.palette = palette.hash;
        uint64_t sig = MemSig(d.data, TotalSize(d)) ^ DirtyEpoch(d.data) ^ palette.hash;
        if (sig != t.sig || !same) {
            t.sig = sig;
            UploadTexture(t, d, fi, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), palette.va ? palette.colours : nullptr);
        }
    }
    Transition(t.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (fi.depth) info.shadow = ShadowDepthKind(d.format);
    if (t.hash && hd::On()) {
        if (const Image* hd = HdFor(t.hash)) {
            stages.fmt[stage] = hd->fmt;
            stages.levels[stage] = hd->levels;
            stages.hd[stage] = true;
            return hd->view;
        }
    }
    stages.fmt[stage] = t.fmt;
    stages.levels[stage] = t.levels;
    return t.img.view;
}

VkImageView VulkanBackend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    info = {};
    stages.rt[stage] = false;
    stages.gw[stage] = 0;
    stages.k[stage] = 1;
    stages.wide[stage] = 0;
    stages.fmt[stage] = kBgra;
    stages.levels[stage] = 1;
    stages.hd[stage] = false;
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return VK_NULL_HANDLE;
    const FormatInfo& fi = Format(d.format);
    info.kind = TextureKind(d);
    info.rect = d.linear;
    auto tit = targets_.find(TargetMapKey(d.data, fi.depth));
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed)
        return BindTargetTexture(s, stage, d, tit->second, true, stages);
    return BindGuestTexture(s, stage, d, fi, stages);
}

bool VulkanBackend::BindStageTextures(const State& s, DrawTextures& stages) {
    for (int i = 0; i < 4; i++) stages.view[i] = BindTexture(s, i, stages);
    // a stage without a texture reads a black one of its kind
    for (int i = 0; i < 4; i++) {
        if (stages.view[i]) continue;
        uint8_t kind = stages.info[i].kind;
        stages.view[i] = dummy_[kind == kTex3D ? 1 : kind == kTexCube ? 2 : 0].view;
        if (stages.info[i].shadow) stages.info[i].shadow = 0;
    }
    return true;
}

constexpr float kDepth24Max = 16777215.0f;

void VulkanBackend::KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k) {
    k.zen = s.rs[kRsZEnable] != 0;
    if (k.zen) {
        k.zwrite = SimpleState(s, kMZWriteEnable) != 0;
        k.zfunc = SimpleState(s, kMZFunc);
    }
    k.sten = s.rs[kRsStencilEnable] != 0;
    if (k.sten) {
        k.sfunc = SimpleState(s, kMStencilFunc);
        k.smask = SimpleState(s, kMStencilMask) & 0xFF;
        k.swmask = SimpleState(s, kMStencilWriteMask) & 0xFF;
        k.sfail = s.rs[kRsStencilFail];
        k.szfail = SimpleState(s, kMStencilZFail);
        k.spass = SimpleState(s, kMStencilPass);
    }
    if (a.topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable)) {
        k.zbias = int32_t(std::lround(Bits(SimpleState(s, kMDepthOffset)) * kDepth24Max / s.zscale));
        k.zslope = Bits(SimpleState(s, kMSlopeScale)) * (draw_ds_ ? float(draw_ds_->w) / draw_ds_->gw : 1.0f);
    }
}

void VulkanBackend::BuildPipelineKey(const State& s, const Assembled& a, const DrawShaders& sh, PsoKey& k) {
    std::memset(&k, 0, sizeof(k));
    k.vs = sh.vh;
    k.ps = sh.ph;
    k.regs = a.regs;
    for (int r = 0; r < 16; r++)
        if ((a.regs >> r) & 1) {
            const VertexElement& e = a.elements[size_t(r)];
            k.elements[r] = e.format | uint32_t(e.buffer) << 8 | uint32_t(e.offset) << 16;
        }
    k.buffers = a.buffer_count;
    for (uint32_t i = 0; i < a.buffer_count; i++) k.strides[i] = a.buffers[i].stride;
    k.topo = uint32_t(a.topology) | (a.strip ? 0x10 : 0);
    k.has_rt = draw_rt_ != nullptr;
    k.has_ds = draw_ds_ != nullptr;
    k.rtfmt = draw_rt_ ? uint32_t(draw_rt_->img.fmt) : 0;
    KeyBlend(s, k);
    if (k.has_ds) KeyDepthStencil(s, a, k);
    KeyRasterizer(s, a, k);
}

// the glow's last step: a render target at most half the frame's width added (blend ONE onto the frame) over a
// full size target. BLOOM off skips it, the glow passes before it then go unseen. A composite done any other way
// (one pass mixing both, alpha blended) is left alone, so the frame never goes black for it
bool VulkanBackend::GlowComposite(const State& s, const DrawTextures& stages) const {
    constexpr uint32_t kBlendOne = 1;
    if (!draw_rt_ || draw_rt_->gw < guest_w_ || !SimpleState(s, kMBlendEnable) || SimpleState(s, kMDestBlend) != kBlendOne)
        return false;
    for (int i = 0; i < 4; i++) {
        if (!stages.gw[i] || stages.gw[i] * 2 > draw_rt_->gw) continue;
        // each kind once in the log, to tell what it took
        static std::set<uint64_t> seen;
        uint64_t kind = uint64_t(stages.gw[i]) << 32 | SimpleState(s, kMSrcBlend) << 16 | draw_rt_->gw;
        if (seen.size() < 16 && seen.insert(kind).second)
            LOGR("vulkan: bloom off skips %u wide onto %u wide, src blend %u", stages.gw[i], draw_rt_->gw,
                 SimpleState(s, kMSrcBlend));
        return true;
    }
    return false;
}

void VulkanBackend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    if (eye_ == stereo::kScopeEye && (s.transforms[4][0] == 2.0f || s.transforms[4][1] == 1.0f)) return;
    if (DepthCopy(s)) return;
    Assembled& a = asm_;
    if (!Assemble(s, prim, start, count, indices, a)) return;

    // the targets, CPU writes to them first
    Target* rt = EnsureTarget(s.rt, false);
    Target* ds = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? EnsureTarget(s.zs, true) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h)) ds = nullptr;
    if (!rt && !ds) return;
    if (rt && LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
    draw_rt_ = rt;
    draw_ds_ = ds;

    DrawShaders sh = ClassifyShaders(s, a);
    if (sh.prog) EnsureProgramHash(s);
    sh.vkey = VertexKey(s, a, sh);
    EnsureVertexShader(s, a, sh);

    DrawTextures stages;
    if (!BindStageTextures(s, stages) || !draw_rt_ && !draw_ds_) return;
    if (!game::Opt().bloom && GlowComposite(s, stages)) return;
    PixelKey pk = MakePixelKey(s, stages.info);
    sh.pkey = Hash(&pk, sizeof(pk));
    EnsurePixelShader(pk, sh);
    if (!sh.vs || !sh.ps) return;

    PsoKey k;
    BuildPipelineKey(s, a, sh, k);
    VkPipeline pipe = EnsurePipeline(k, sh.vs, sh.ps);
    if (!pipe) return;

    // the M249's right side, its left mirrored (vr_gun.cpp): the draw's vertices and indices swapped for ours below,
    // after the posing (which reads the game's)
    const uint8_t* mirror_v = nullptr;
    const void* mirror_i = nullptr;
    uint32_t mirror_vbytes = 0, mirror_icount = 0;
    const bool mirror = stereo_ && s.transforms[4][0] == 2.0f && s.transforms[4][4] == 1.0f &&
                        !IsArms(a.index_count) && a.buffer_count == 1 && a.index_count &&
                        ::game::VrMirror(a.buffers[0].src, a.buffers[0].stride, a.buffers[0].bytes, a.index_src,
                                         a.index16, a.index_count, a.base_vertex, a.strip, mirror_v, mirror_vbytes, mirror_i,
                                         mirror_icount);
    // dev_dumpgun: each gun draw's texture (stage 0) saved once, decoded to BGRA (with its mesh, vr_gun.cpp)
    static const bool dump_guns = std::filesystem::exists(app::ExeDir() / "dev_dumpgun");
    if (dump_guns && stereo_ && s.transforms[4][0] == 2.0f && s.transforms[4][4] == 1.0f && !IsArms(a.index_count)) {
        // (by the gun as well: the guns' textures load at one address in turn)
        static std::set<std::pair<uint32_t, uint32_t>> saved;
        TextureDesc d;
        if (s.textures[0] && Describe(s.textures[0], d) && !d.cube && d.depth == 1 &&
            saved.insert({d.data, a.index_count}).second) {
            std::vector<uint8_t> px;
            uint32_t pitch = 0;
            DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
            const FormatInfo& fi = Format(d.format);
            if (fi.compressed) {
                std::vector<uint8_t> texels;
                DecodeDxt(fi.host, px, pitch, d.width, d.height, 1, texels);
                px.swap(texels);
                pitch = d.width * 4;
            }
            if (px.size() >= size_t(pitch) * d.height) ::game::VrDumpGunTexture(d.data, px.data(), pitch, d.width, d.height);
        }
    }
    // everything uploaded before the rendering: a full ring flushes
    VkDeviceSize need = 2 * kUboRange + 1024;
    if (mirror) need += mirror_vbytes + VkDeviceSize(mirror_icount) * 4;
    for (uint32_t i = 0; i < a.buffer_count; i++) need += a.buffers[i].bytes + 16;
    need += VkDeviceSize(a.index_count) * 4 + 16;
    if (need < kUploadSize / 4 && CurrentFrame().used + need > kUploadSize) Submit(true, false);
    GuestExtent size;
    SurfaceSize(draw_rt_ ? s.rt : s.zs, size.w, size.h);
    DrawConstants c{};
    FillSurfaceConstants(s, size, c);
    FillFogConstants(s, c);
    FillCombinerConstants(s, c);
    FillTextureConstants(s, stages, c);
    if (sh.fixed_transform) {
        FillTransformConstants(s, size, c);
        FillLightConstants(s, sh.fk, c);
    }
    // own hands: the arms' bones posed by vr_gun.cpp, the game's own left as they are elsewhere
    Alloc vc;
    std::array<std::array<float, 4>, 192> hand_mag{}, drop_mag{};  // the extra draws' constants (VrHandMag, VrDropMag)
    bool hand_mag_on = false, drop_mag_on = false, stock_on = false;
    const uint8_t* stock_v = nullptr;
    const uint16_t* stock_i = nullptr;
    uint32_t stock_vbytes = 0, stock_icount = 0;
    // dev_hidebone in the app folder holds a bone number: that bone collapsed in every gun list draw (to see what it
    // is). The constants are copied below, so the change goes through a local copy
    // (several numbers: each hidden; "lo-hi" ranges in steps of 3 too, written as two numbers with a minus: 0 -129)
    static const std::vector<int> hide_bones = [] {
        std::vector<int> v;
        if (FILE* f = std::fopen((app::ExeDir() / "dev_hidebone").string().c_str(), "r")) {
            int b, prev = -1;
            while (std::fscanf(f, "%d", &b) == 1) {
                if (b < 0 && prev >= 0)
                    for (int k = prev + 3; k <= -b; k += 3) v.push_back(k);
                else v.push_back(b);
                prev = b < 0 ? -1 : b;
            }
            std::fclose(f);
        }
        return v;
    }();
    State hidden_state;
    const State* sp = &s;
    if (!hide_bones.empty() && s.transforms[4][0] == 2.0f) {
        hidden_state = s;
        // collapsed onto bone 75's origin (the AK's body): vertices only partly weighted to it move a little, not
        // metres
        for (int hb : hide_bones)
            if (hb >= 0 && hb <= 129)
                for (int r = 0; r < 3; r++) hidden_state.vsc[60 + hb + r] = {0, 0, 0, s.vsc[60 + 75 + r][3]};
        sp = &hidden_state;
    }
    const State& sv = *sp;
    if (stereo_ && s.transforms[4][0] == 2.0f && s.transforms[4][4] == 1.0f && IsArms(a.index_count)) {
        auto posed = sv.vsc;
        uint32_t data = s.streams[0].vb ? mem::VirtualFromPhysical(kernel::R32(s.streams[0].vb + 4)) : 0;  // kResData
        const uint8_t* vb = data ? static_cast<const uint8_t*>(kernel::HostPtr(data)) : nullptr;
        ::game::VrPoseArms(s.transforms[4][2], s.transforms[4][3], &s.transforms[4][5],
                           reinterpret_cast<float (*)[4]>(posed.data()), vb, s.streams[0].stride, a.index_src,
                           a.index16, a.index_count, a.base_vertex);
        // (the gun's parts aren't posed here: the arms' mesh has none, and its bones under those numbers are fingers)
        vc = Constants(posed.data(), sizeof(posed));
    } else if (stereo_ && s.transforms[4][0] == 2.0f && s.transforms[4][4] == 1.0f) {
        // the gun: its magazine bone moved by a reload by hand
        auto posed = sv.vsc;
        bool moved = ::game::VrPoseGun(s.transforms[4][2], s.transforms[4][3], &s.transforms[4][5],
                                       reinterpret_cast<float (*)[4]>(posed.data()),
                                       a.buffer_count ? a.buffers[0].src : nullptr, a.buffer_count ? a.buffers[0].stride : 0,
                                       a.buffer_count ? a.buffers[0].bytes : 0, a.index_src, a.index16, a.index_count,
                                       a.base_vertex);
        vc = Constants(moved ? posed.data() : sv.vsc.data(), sizeof(s.vsc));
        // and the magazine in the off hand, and a dropped one: the same mesh again, everything but it collapsed
        // (from the game's own bones: the posed ones may have the magazine hidden)
        hand_mag = sv.vsc;
        drop_mag = sv.vsc;
        // and a stock the viewmodel lacks (vr_gun.cpp), with its posed bones
        if (!IsArms(a.index_count) && a.buffer_count == 1 &&
            ::game::VrStock(a.buffers[0].src, a.buffers[0].stride, a.buffers[0].bytes, a.index_src, a.index16,
                            a.index_count, a.base_vertex, a.strip, stock_v, stock_vbytes, stock_i, stock_icount))
            stock_on = true;
        if (!IsArms(a.index_count) &&
            ::game::VrHandMag(s.transforms[4][2], s.transforms[4][3], &s.transforms[4][5],
                              reinterpret_cast<float (*)[4]>(hand_mag.data())))
            hand_mag_on = true;
        if (!IsArms(a.index_count) &&
            ::game::VrDropMag(s.transforms[4][2], s.transforms[4][3], &s.transforms[4][5],
                              reinterpret_cast<float (*)[4]>(drop_mag.data())))
            drop_mag_on = true;
    } else {
        vc = Constants(s.vsc.data(), sizeof(s.vsc));
    }
    Alloc dc = Constants(&c, sizeof(c));
    if (mirror) {
        a.buffers[0].src = mirror_v, a.buffers[0].bytes = mirror_vbytes;
        a.index_src = mirror_i, a.index_count = mirror_icount;
    }
    VkBuffer vbufs[16];
    VkDeviceSize voffs[16];
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        Alloc v = Upload(std::max<uint32_t>(b.bytes, 4), 16);
        if (b.src && b.bytes) std::memcpy(v.cpu, b.src, b.bytes);
        vbufs[i] = v.buf;
        voffs[i] = v.off;
    }
    Alloc ib{};
    if (a.index_count) {
        uint32_t ibytes = a.index_count * (a.index16 ? 2 : 4);
        ib = Upload(ibytes, 4);
        std::memcpy(ib.cpu, a.index_src, ibytes);
    }
    bool clamp = AnyStageWraps(s) && UnitTexcoords(a);
    VkSampler samplers[4];
    for (int i = 0; i < 4; i++) samplers[i] = StageSampler(s, i, stages, clamp);
    VkDescriptorSet set = Set(stages.view, samplers);
    // the stock's own texture (vr_gun.cpp paints it from the gun's, on a thread): bound in stage 0's place for the
    // stock alone; till it's ready the stock isn't drawn (its uvs are for its own texture). None for this gun (a
    // shotgun's shell): drawn with the gun's
    VkDescriptorSet stock_set = VK_NULL_HANDLE;
    // our models' own textures (vr_gun.cpp), each made once: 1 the M67 (the hand's grenade, every grenade the world
    // draws), 2 the M433 (the grenade launcher's round)
    static Image own_tex[3];
    auto own_ready = [&](int id) {
        static bool tried[3] = {};
        if (!tried[id]) {
            tried[id] = true;
            std::vector<uint8_t> px;
            uint32_t w = 0, h = 0;
            if (::game::VrOwnTexture(id, px, w, h)) {
                EndRender();
                if (!PixelImage(own_tex[id], px, w * 4, w, h)) own_tex[id] = {};
            }
        }
        return own_tex[id].view != VK_NULL_HANDLE;
    };
    Image& m67 = own_tex[1];
    auto m67_ready = [&] { return own_ready(1); };
    // the game's grenade in the world (found by a throw, dev_drawdiff: stride 24, 220 vertices, 479 indices: its
    // vertices position, packed normal, uv as the guns' less their bone words): the M67 drawn instead, with its texture
    VkDescriptorSet m67_set = VK_NULL_HANDLE;
    uint32_t m67_count = 0;
    const uint16_t* m67_idx = nullptr;
    const uint8_t* m67_v = nullptr;
    uint32_t m67_vbytes = 0;
    if (a.buffer_count == 1 && a.buffers[0].stride == 24 && a.buffers[0].bytes == 5280 && a.index_count == 479 &&
        ::game::VrM67World(m67_v, m67_vbytes, m67_idx, m67_count) && m67_ready()) {
        VkImageView views[4];
        for (int i = 0; i < 4; i++) views[i] = stages.view[i];
        views[0] = m67.view;
        m67_set = Set(views, samplers);
    }
    if (int own = stock_on ? ::game::VrExtraOwn() : 0) {
        // the extra mesh with a texture of its own (the hand grenade, the grenade launcher's round)
        if (own_ready(own)) {
            VkImageView views[4];
            for (int i = 0; i < 4; i++) views[i] = stages.view[i];
            views[0] = own_tex[own].view;
            stock_set = Set(views, samplers);
        } else {
            stock_on = false;
        }
    } else if (stock_on) {
        static std::map<uint32_t, Image> stock_tex;
        static std::set<uint32_t> started;
        TextureDesc d;
        if (s.textures[0] && Describe(s.textures[0], d) && !d.cube && d.depth == 1) {
            auto it = stock_tex.find(d.data);
            if (it == stock_tex.end()) {
                if (started.insert(d.data).second) {
                    std::vector<uint8_t> px;
                    uint32_t pitch = 0;
                    DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
                    const FormatInfo& fi = Format(d.format);
                    if (fi.compressed) {  // DXT blocks (DecodeLevel leaves them): to BGRA texels
                        std::vector<uint8_t> texels;
                        DecodeDxt(fi.host, px, pitch, d.width, d.height, 1, texels);
                        px.swap(texels);
                        pitch = d.width * 4;
                    }
                    if (px.size() >= size_t(pitch) * d.height)
                        ::game::VrStockBakeStart(d.data, std::move(px), pitch, d.width, d.height);
                }
                std::vector<uint8_t> baked;
                uint32_t bw = 0, bh = 0;
                int st = ::game::VrStockBakeTake(d.data, baked, bw, bh);
                if (st != 0) {
                    Image im;
                    if (st == 1) {
                        EndRender();  // the copy goes outside a pass
                        if (!PixelImage(im, baked, bw * 4, bw, bh)) im = {};
                    }
                    it = stock_tex.emplace(d.data, im).first;  // made or not, once
                }
            }
            if (it != stock_tex.end() && it->second.view) {
                VkImageView views[4];
                for (int i = 0; i < 4; i++) views[i] = stages.view[i];
                views[0] = it->second.view;
                stock_set = Set(views, samplers);
            }
        }
        bool own_none = false;
        if (TextureDesc dd; s.textures[0] && Describe(s.textures[0], dd)) {
            auto it = stock_tex.find(dd.data);
            own_none = it != stock_tex.end() && !it->second.view;
        }
        if (!stock_set && !own_none) stock_on = false;
    }

    // the rendering
    BeginRender(draw_rt_ ? &draw_rt_->img : nullptr, draw_ds_ ? &draw_ds_->img : nullptr);
    if (draw_rt_) draw_rt_->drawn = true;
    if (draw_ds_) draw_ds_->drawn = true;
    vkCmdBindPipeline(Cb(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    BindSet(set, vc.off, dc.off);
    Target& bt = draw_rt_ ? *draw_rt_ : *draw_ds_;
    float W = float(bt.w), H = float(bt.h);
    SetViewport(0, 0, W, H);
    float sc = W / size.w;
    int32_t x0 = int32_t(s.viewport.x * sc), y0 = int32_t(s.viewport.y * sc);
    int32_t x1 = int32_t((s.viewport.x + s.viewport.w) * sc), y1 = int32_t((s.viewport.y + s.viewport.h) * sc);
    if (s.scissor_count) {
        x0 = std::max(x0, int32_t(s.scissors[0] * sc));
        y0 = std::max(y0, int32_t(s.scissors[1] * sc));
        x1 = std::min(x1, int32_t(s.scissors[2] * sc));
        y1 = std::min(y1, int32_t(s.scissors[3] * sc));
    }
    SetScissor(std::clamp(x0, 0, int32_t(W)), std::clamp(y0, 0, int32_t(H)), std::clamp(x1, 0, int32_t(W)),
               std::clamp(y1, 0, int32_t(H)));
    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    vkCmdSetBlendConstants(Cb(), bf);
    vkCmdSetStencilReference(Cb(), VK_STENCIL_FACE_FRONT_AND_BACK, SimpleState(s, kMStencilRef) & 0xFF);
    // which camera a draw is through: the world's and the gun's move to the eye, the 2d layer's goes on the hud
    // plane, the rest (post passes, shadow maps, other targets' 2d) stays where the game put it
    float eye_m[16];
    std::memcpy(eye_m, kIdentity, sizeof(eye_m));
    const float* proj = s.transforms[kTsProjection].data();
    int mark = int(s.transforms[4][0]);  // d3d::SceneDraws: 1 the world's draw lists, 2 the gun's
    bool overlay = s.transforms[4][1] == 1.0f;
    bool depth = s.rs[kRsZEnable] && CmpIndex(SimpleState(s, kMZFunc)) != 7;
    bool persp = proj[11] != 0 && proj[15] == 0;
    int pe = eye_ & 1;
    // effects, casings and light shafts are drawn outside the lists, through the lists' projection
    bool listed = mark == 1;
    bool effect = !listed && mark != 2 && scene_proj_ok_[pe] && std::memcmp(proj, scene_proj_[pe], 64) == 0;
    bool world = (listed || effect) && !a.pretransformed && depth && persp;
    bool gun = mark == 2 && !a.pretransformed && depth && persp;
    if (world && listed) {
        std::memcpy(scene_proj_[pe], proj, 64);
        scene_proj_ok_[pe] = true;
    }
    bool hud = overlay && !last_theater_ && !world && !gun;
    if (hud && stereo_) {
        // brighter (kHudGain): its constants again with the gain, bound in place of the first
        c.ext[0] = kHudGain, c.ext[1] = kHudSaturation;
        dc = Constants(&c, sizeof(c));
        BindSet(set, vc.off, dc.off);
    }
    // debug: a file named stereo_noeye in the app's folder keeps both eyes but leaves draws where the game put them
    static const bool noeye = std::filesystem::exists(app::ExeDir() / "stereo_noeye");
    if (stereo_ && !noeye && (world || gun || hud)) {
        VkExtent2D se = xr::ScreenExtent();
        bool ok = hud ? HudMatrix(eye_, float(se.width) / float(se.height), eye_m)
                      : EyeMatrix(s, size, eye_, gun, eye_m);
        if (!ok) {
            std::memcpy(eye_m, kIdentity, sizeof(eye_m));
            stat_unmoved_++;
        }
    }
    stat_draws_++;
    if (world) stat_world_++;
    if (world && !listed) stat_effect_++;
    if (gun) stat_gun_++;
    if (hud) stat_hud_++;
    if (a.pretransformed && depth && !overlay) stat_flat_z_++;
    if ((world || gun) && eye_ == 0) frame_world_++;
    // dev_drawdiff: the world draws' kinds (stride, vertex bytes, indices) each frame, to find what a throw adds
    static const bool drawdiff = std::filesystem::exists(app::ExeDir() / "dev_drawdiff");
    if (drawdiff && world && eye_ == 0 && a.buffer_count)
        dev_kinds_.insert({a.buffers[0].stride, a.buffers[0].bytes, a.index_count});
    if (stereo_ && world && listed && eye_ == 0) std::memcpy(stat_proj_, proj, sizeof(stat_proj_));
    if (a.buffer_count) vkCmdBindVertexBuffers(Cb(), 0, a.buffer_count, vbufs, voffs);
    if (a.index_count)
        vkCmdBindIndexBuffer(Cb(), ib.buf, ib.off, a.index16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    // the world's grenade swapped for the M67 (above): its vertices, its indices (a strip's as the stock's), its texture
    uint32_t m67_draw = 0;
    if (m67_set) {
        Alloc v = Upload(m67_vbytes, 16);
        std::memcpy(v.cpu, m67_v, m67_vbytes);
        VkDeviceSize voff = v.off;
        vkCmdBindVertexBuffers(Cb(), 0, 1, &v.buf, &voff);
        static std::vector<uint16_t> strip;
        const uint16_t* ind = m67_idx;
        uint32_t icount = m67_count;
        if (a.strip) {
            if (strip.empty())
                for (uint32_t i = 0; i + 2 < m67_count; i += 3)
                    strip.insert(strip.end(), {m67_idx[i], m67_idx[i], m67_idx[i + 1], m67_idx[i + 2], m67_idx[i + 2]});
            ind = strip.data(), icount = uint32_t(strip.size());
        }
        Alloc ix = Upload(icount * 2, 4);
        std::memcpy(ix.cpu, ind, icount * 2);
        vkCmdBindIndexBuffer(Cb(), ix.buf, ix.off, VK_INDEX_TYPE_UINT16);
        BindSet(m67_set, vc.off, dc.off);
        m67_draw = icount;
    }
    // the eye matrix and a clip plane (none: zero) with each draw (EyeHlsl)
    auto emit = [&](const float* m, const float* clip) {
        float pc[20] = {};
        std::memcpy(pc, m, 64);
        if (clip) std::memcpy(pc + 16, clip, 16);
        vkCmdPushConstants(Cb(), layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), pc);
        if (m67_draw) vkCmdDrawIndexed(Cb(), m67_draw, 1, 0, 0, 0);
        else if (a.index_count) vkCmdDrawIndexed(Cb(), a.index_count, 1, 0, a.base_vertex, 0);
        else vkCmdDraw(Cb(), a.draw_count, 1, 0, 0);
    };
    // dev_dumpreload (phase 5): each gun list mesh's vertices once (dumps/mesh_<indices>.bin: stride, vertex count,
    // vertices), and while vr_body records, every gun list draw's 192 constants with its index count and the
    // time (dumps/reload.bin): which bones a reload moves
    if (s.transforms[4][0] == 2.0f && s.streams[0].vb && a.index_src && a.index_count && ::game::VrRecording()) {
        static std::set<uint32_t> meshes;
        auto dir = app::ExeDir() / "dumps";
        if (meshes.insert(a.index_count).second) {
            std::filesystem::create_directories(dir);
            uint32_t data = mem::VirtualFromPhysical(kernel::R32(s.streams[0].vb + 4));
            uint32_t top = 0;
            for (uint32_t i = 0; i < a.index_count; i++) {
                uint32_t ix = a.index16 ? reinterpret_cast<const uint16_t*>(a.index_src)[i]
                                        : reinterpret_cast<const uint32_t*>(a.index_src)[i];
                top = std::max(top, uint32_t(int32_t(ix) + a.base_vertex + 1));
            }
            if (FILE* f = std::fopen((dir / ("mesh_" + std::to_string(a.index_count) + ".bin")).string().c_str(), "wb")) {
                uint32_t head[2] = {s.streams[0].stride, top};
                std::fwrite(head, 4, 2, f);
                if (data) std::fwrite(kernel::HostPtr(data), 1, size_t(top) * s.streams[0].stride, f);
                std::fclose(f);
            }
        }
        static FILE* rec = nullptr;
        if (!rec) rec = std::fopen((dir / "reload.bin").string().c_str(), "wb");
        if (rec) {
            static const auto t0 = std::chrono::steady_clock::now();
            float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
            uint32_t head[2] = {a.index_count, 0};
            std::memcpy(&head[1], &t, 4);
            std::fwrite(head, 4, 2, rec);
            std::fwrite(s.vsc.data(), 16, 192, rec);
            std::fflush(rec);
        }
    }
    emit(eye_m, nullptr);
    // the extra mesh's buffers, kept for the hand pass (a shotgun's shell is drawn in the hand too)
    Alloc stock_vb{}, stock_ib{};
    uint32_t stock_draw_count = 0;
    if (stock_on) {
        Alloc v = Upload(stock_vbytes, 16);
        stock_vb = v;
        std::memcpy(v.cpu, stock_v, stock_vbytes);
        VkDeviceSize voff = v.off;
        vkCmdBindVertexBuffers(Cb(), 0, 1, &v.buf, &voff);
        float pc[20] = {};
        std::memcpy(pc, eye_m, 64);
        vkCmdPushConstants(Cb(), layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), pc);
        if (stock_set) BindSet(stock_set, vc.off, dc.off);
        {
            static std::set<uint32_t> told;  // once a mesh: drawn, with its own texture or the gun's
            if (told.insert(stock_icount).second)
                LOGR("vulkan: extra mesh drawn, %u indices, %s texture, strip %d", stock_icount,
                     stock_set ? "its own" : "the gun's", a.strip);
        }
        // the stock is a triangle list, the gun's pipeline may draw strips (the M16's does): a list read as a strip
        // joins each triangle to the next, a sliver from the stock's last part to its first. As a strip, each
        // triangle alone between repeated indices (a a b c c: the joins have no area). Both windings still: a
        // strip's culling flips with each triangle's place in it
        // (made once per index list: the stocks' are fixed)
        static std::map<std::pair<const uint16_t*, uint32_t>, std::vector<uint16_t>> strips;
        const uint16_t* ind = stock_i;
        uint32_t icount = stock_icount;
        if (a.strip) {
            auto& strip = strips[{stock_i, stock_icount}];
            if (strip.empty())
                // (six a triangle, so each starts at an odd place: all flipped alike, and a triangle and its
                // reverse face opposite ways wherever they sit; five put every other one on the other parity)
                for (uint32_t i = 0; i + 2 < stock_icount; i += 3)
                    strip.insert(strip.end(), {stock_i[i], stock_i[i], stock_i[i + 1], stock_i[i + 2], stock_i[i + 2],
                                               stock_i[i + 2]});
            ind = strip.data(), icount = uint32_t(strip.size());
        }
        Alloc ix = Upload(icount * 2, 4);
        std::memcpy(ix.cpu, ind, icount * 2);
        stock_ib = ix, stock_draw_count = icount;
        vkCmdBindIndexBuffer(Cb(), ix.buf, ix.off, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(Cb(), icount, 1, 0, 0, 0);
        // the gun's own buffers and textures back for the magazine passes
        if (stock_set) BindSet(set, vc.off, dc.off);
        if (a.buffer_count) vkCmdBindVertexBuffers(Cb(), 0, a.buffer_count, vbufs, voffs);
        if (a.index_count)
            vkCmdBindIndexBuffer(Cb(), ib.buf, ib.off, a.index16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    }
    if (hand_mag_on) {
        Alloc vc2 = Constants(hand_mag.data(), sizeof(hand_mag));
        BindSet(set, vc2.off, dc.off);
        emit(eye_m, nullptr);
        // a shell of ours (the SPAS's own is a mesh the game draws only in its reload): in the hand with the rest
        if (stock_draw_count && ::game::VrStockShell()) {
            VkDeviceSize voff = stock_vb.off;
            if (stock_set) BindSet(stock_set, vc2.off, dc.off);  // the M203's grenade: the stock's texture
            vkCmdBindVertexBuffers(Cb(), 0, 1, &stock_vb.buf, &voff);
            vkCmdBindIndexBuffer(Cb(), stock_ib.buf, stock_ib.off, VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexed(Cb(), stock_draw_count, 1, 0, 0, 0);
            if (stock_set) BindSet(set, vc2.off, dc.off);
            if (a.buffer_count) vkCmdBindVertexBuffers(Cb(), 0, a.buffer_count, vbufs, voffs);
            if (a.index_count)
                vkCmdBindIndexBuffer(Cb(), ib.buf, ib.off, a.index16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        }
    }
    if (drop_mag_on) {
        Alloc vc3 = Constants(drop_mag.data(), sizeof(drop_mag));
        BindSet(set, vc3.off, dc.off);
        emit(eye_m, nullptr);
        // our own magazine (the M249 Para's box) dropped with the rest
        if (stock_draw_count && ::game::VrStockShell()) {
            VkDeviceSize voff = stock_vb.off;
            if (stock_set) BindSet(stock_set, vc3.off, dc.off);
            vkCmdBindVertexBuffers(Cb(), 0, 1, &stock_vb.buf, &voff);
            vkCmdBindIndexBuffer(Cb(), stock_ib.buf, stock_ib.off, VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexed(Cb(), stock_draw_count, 1, 0, 0, 0);
        }
    }
}

VkRect2D ScaledRect(const Target& t, int x0, int y0, int x1, int y1) {
    float sc = float(t.w) / t.gw;
    int32_t l = std::clamp(int32_t(x0 * sc), 0, int32_t(t.w)), tp = std::clamp(int32_t(y0 * sc), 0, int32_t(t.h));
    int32_t r = std::clamp(int32_t(x1 * sc), 0, int32_t(t.w)), b = std::clamp(int32_t(y1 * sc), 0, int32_t(t.h));
    return {{l, tp}, {uint32_t(std::max(r - l, 0)), uint32_t(std::max(b - tp, 0))}};
}

void VulkanBackend::ClearTarget(Target& t, const State& s, uint32_t count, const uint8_t* rects, uint32_t flags,
                                uint32_t color, float z, uint32_t stencil) {
    std::vector<VkClearRect> rs;
    auto add = [&](const VkRect2D& r) {
        if (r.extent.width && r.extent.height) rs.push_back({r, 0, 1});
    };
    if (count && rects) {
        for (uint32_t i = 0; i < count; i++) {
            int32_t r[4];
            std::memcpy(r, rects + size_t(i) * sizeof(r), sizeof(r));
            add(ScaledRect(t, r[0], r[1], r[2], r[3]));
        }
    } else {
        const auto& v = s.viewport;
        add(ScaledRect(t, int(v.x), int(v.y), int(v.x + v.w), int(v.y + v.h)));
    }
    t.drawn = true;
    if (rs.empty()) return;
    VkClearAttachment ca{};
    if (t.depth) {
        ca.aspectMask = ((flags & kClearZ) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                        ((flags & kClearStencil) && t.img.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        if (!ca.aspectMask) return;
        ca.clearValue.depthStencil = {std::clamp(z, 0.0f, 1.0f), stencil & 0xFF};
        BeginRender(nullptr, &t.img);
    } else {
        // pixels the CPU wrote before this clear go in first, a later upload would paint over the clear
        if (LatchCpuWrite(t)) UploadTarget(t, s.rt);
        ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        Color(color, ca.clearValue.color.float32);
        BeginRender(&t.img, nullptr);
    }
    vkCmdClearAttachments(Cb(), 1, &ca, uint32_t(rs.size()), rs.data());
}

void VulkanBackend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color,
                          float z, uint32_t stencil) {
    Target* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    Target* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    for (Target* t : {rt, ds})
        if (t) ClearTarget(*t, s, count, rects, flags, color, z, stencil);
}

void VulkanBackend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects,
                         uint32_t dst_points) {
    Target* src_t = FindTarget(src, false);
    // pixels the CPU made: the copy happens in memory, textures and targets pick it up from there
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "vulkan: CopyRects %08X to %08X skipped", src.data, dst.data);
        } else if (Target* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }
    Target* dst_t = EnsureTarget(dst, false);
    src_t = FindTarget(src, false);
    if (!dst_t || !src_t || src_t == dst_t) return;
    Transition(src_t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(dst_t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    float dst_scale = float(dst_t->w) / dst_t->gw;
    bool listed = count && src_rects;
    uint32_t n = listed ? count : 1;
    std::vector<VkImageCopy> regions;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t l = 0, t = 0, r = std::min(src_t->w, dst_t->w), b = std::min(src_t->h, dst_t->h), dx = 0, dy = 0;
        if (listed) {
            GuestRect g = ReadGuestRect(src_rects, i);
            l = ScaleUnsigned(g.left, dst_scale);
            t = ScaleUnsigned(g.top, dst_scale);
            r = std::min(ScaleUnsigned(g.right, dst_scale), src_t->w);
            b = std::min(ScaleUnsigned(g.bottom, dst_scale), src_t->h);
            dx = l;
            dy = t;
            if (dst_points) {
                int32_t px, py;
                ReadGuestPoint(dst_points, i, px, py);
                dx = ScaleUnsigned(px, dst_scale);
                dy = ScaleUnsigned(py, dst_scale);
            }
        }
        if (r <= l || b <= t || dx >= dst_t->w || dy >= dst_t->h) continue;
        r = std::min(r, l + dst_t->w - dx);
        b = std::min(b, t + dst_t->h - dy);
        regions.push_back({{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           {int32_t(l), int32_t(t), 0},
                           {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           {int32_t(dx), int32_t(dy), 0},
                           {r - l, b - t, 1}});
    }
    if (!regions.empty())
        vkCmdCopyImage(Cb(), src_t->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_t->img.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(regions.size()), regions.data());
    dst_t->drawn = true;
}

// ---- read-backs

void VulkanBackend::Sync(uint32_t data, uint32_t) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || !it->second.drawn) return;
    Target& t = it->second;
    Image down;
    Image* src = CopySource(t, down);
    std::vector<uint8_t> px;
    bool ok = src && ReadBack(*src, t.gw, t.gh, px);
    Destroy(down);  // ReadBack waited for the GPU
    if (!ok) return;
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    for (uint32_t y = 0; y < t.gh; y++) std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.gw * 4], t.gw * 4);
    t.read = std::move(px);
    t.drawn = false;
    t.sig = MemSig(t.data, t.bytes);
    // the other eye's copy matches guest memory as this read-back left it: not a cpu write to upload
    if (auto o = other_targets_.find(it->first); o != other_targets_.end() && o->second.gw == t.gw && o->second.gh == t.gh) {
        o->second.read = t.read;
        o->second.drawn = false;
        o->second.sig = t.sig;
    }
}

void VulkanBackend::SyncLate(uint32_t data) {
    if (!DeliverLate(data)) Sync(data, 0);
    RecordLate(data);
}

bool VulkanBackend::DeliverLate(uint32_t data) {
    std::lock_guard lock(late_lock_);
    auto it = late_.find(data);
    if (it == late_.end()) return false;
    Late& l = it->second;
    uint64_t done = Completed(), now = presents_seen_.load();
    int best = -1;
    for (int i = 0; i < kFrames; i++) {
        bool finished = l.fence[i] && l.fence[i] <= done;
        bool recent = now - l.frame[i] <= uint64_t(kFrames) + 1;
        if (finished && recent && (best < 0 || l.fence[i] > l.fence[best])) best = i;
    }
    if (best < 0) return false;
    if (l.fence[best] > l.delivered) {
        const uint8_t* p = l.rb[best].mem.map;
        uint32_t pitch = GuestPitch(l.gw);
        uint8_t* dst = static_cast<uint8_t*>(kernel::HostPtr(data));
        for (uint32_t y = 0; y < l.gh; y++) std::memcpy(dst + size_t(y) * pitch, p + size_t(y) * l.gw * 4, l.gw * 4);
        l.delivered = l.fence[best];
    }
    return true;
}

void VulkanBackend::RecordLate(uint32_t data) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end()) return;
    Target& t = it->second;
    t.sig = MemSig(t.data, t.bytes);
    t.read.clear();
    std::lock_guard lock(late_lock_);
    Late& l = late_[data];
    if (l.gw != t.gw || l.gh != t.gh) {
        for (Buffer& b : l.rb) Retire(b);
        Retire(l.down);
        l = Late{};
        l.gw = t.gw;
        l.gh = t.gh;
    }
    uint32_t slot = l.next;
    if (l.fence[slot] > Completed()) return;
    Image* src = CopySource(t, l.down);
    if (!src) return;
    if (!l.rb[slot].buf && !CreateBuffer(l.rb[slot], VkDeviceSize(t.gw) * t.gh * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true))
        return;
    EndRender();
    VkBufferImageCopy c{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {t.gw, t.gh, 1}};
    vkCmdCopyImageToBuffer(Cb(), src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, l.rb[slot].buf, 1, &c);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(Cb(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    l.fence[slot] = value_ + 1;  // the next Submit carries it
    l.frame[slot] = presents_;
    l.next = (slot + 1) % kFrames;
}

// ---- present

// the swap chain made again when Android gave a new window, the size or vsync changed; the surface too when the
// window is new, lost, or could not be made last time. false: nothing to show on, the frame is dropped
bool VulkanBackend::AcquireImage(uint32_t& index) {
    if (!host::Foreground()) return false;
    if (!surface_ || surface_lost_ || host::SurfaceEpoch() != surface_epoch_) {
        if (surface_) LOGI("vulkan: new surface (%s)", surface_lost_ ? "lost" : "new window");
        DestroySwapchain();
        if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
        if (!CreateSurface()) return false;
        swap_stale_ = true;
    }
    int w, h;
    host::WindowPixels(w, h);
    if (!swap_ || swap_stale_ || uint32_t(w) != swap_extent_.width || uint32_t(h) != swap_extent_.height ||
        app::Get().vsync != vsync_) {
        if (!CreateSwapchain()) return false;
    }
    // a bounded wait: a window Android is taking away may never hand an image back
    constexpr uint64_t kAcquireTimeout = 500'000'000;
    for (int tries = 0; tries < 2; tries++) {
        VkResult r = vkAcquireNextImageKHR(dev_, swap_, kAcquireTimeout, CurrentFrame().acquire, VK_NULL_HANDLE, &index);
        // suboptimal is left alone: Android says so every frame on a turned screen, where only the compositor's
        // rotation differs, and making the chain again each frame freed images the GPU was still drawing (Mali)
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) return true;
        if (r == VK_TIMEOUT || r == VK_NOT_READY) return false;
        if (r == VK_ERROR_SURFACE_LOST_KHR) {
            surface_lost_ = true;
            return false;
        }
        if (r != VK_ERROR_OUT_OF_DATE_KHR || !CreateSwapchain()) return false;
    }
    return false;
}

void VulkanBackend::DrawFrame(Target* t, uint32_t index) {
    // openxr's images differ in size (eyes, theater screen): this one's for the draw, the screen's again after
    VkExtent2D keep = swap_extent_;
    if (xr_) swap_extent_ = xr::ImageExtent(index);
    struct Restore {
        VkExtent2D& e;
        VkExtent2D v;
        ~Restore() { e = v; }
    } restore{swap_extent_, keep};
    bool eye_image = stereo_ && xr::ImageEye(index) < xr::kTheater;
    EndRender();
    VkCommandBuffer cb = Cb();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = swap_images_[index];
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
    // the frame letterboxed to the game's aspect over black
    float aspect = game::Aspect();
    float w = float(swap_extent_.width), h = float(swap_extent_.height);
    float vw = std::min(w, h * aspect), vh = vw / aspect;
    // an eye: the frame is drawn for that eye's whole field of view
    if (eye_image) vw = w, vh = h;
    uint32_t filter = std::min(game::Opt().screen_filter, 8u);
    uint32_t crt = filter <= 2 ? filter : 0;
    bool fxaa = game::Opt().anti_aliasing == 1, ssaa = game::Opt().anti_aliasing == 2;
    // dev_bright (a gamma, 2 if empty) lifts the frame's shadows, for looking at models in test shots
    static const float bright = [] {
        std::ifstream f(app::ExeDir() / "dev_bright");
        if (!f) return 0.0f;
        float g = 0;
        return f >> g && g > 0 ? g : 2.0f;
    }();
    float vignette = eye_image ? ::game::VrVignette() : 0.0f;
    // the scope's glass, its picture from the last frame or two
    float lens[8] = {};
    bool scoped = eye_image && scope_img_ && scope_frame_ + 2 >= presents_ &&
                  ::game::VrScopeLens(xr::ImageEye(index), lens);
    // the barrel's reticle (the game's crosshair hidden: hud.cpp)
    float ret[4] = {};
    bool aimed = eye_image && !last_theater_ && ::game::VrReticle(xr::ImageEye(index), ret);
    // our silencer (in the eye's space) and its icon under the hud's ammunition
    float sil[16] = {}, icon[4] = {}, icon_bright = 0;
    bool silenced = false, iconed = false;
    if (eye_image && !last_theater_) {
        xr::EyeView ev;
        float rear[3], front[3], radius;
        int e = xr::ImageEye(index);
        if (xr::RenderEye(e, ev) && ::game::VrSilencerPose(rear, front, &radius)) {
            float R[9];
            QuatMatrix(ev.quat, R);
            auto toEye = [&](const float* p, float* o) {
                float d[3] = {p[0] - ev.pos[0], p[1] - ev.pos[1], p[2] - ev.pos[2]};
                for (int i = 0; i < 3; i++) o[i] = R[0 * 3 + i] * d[0] + R[1 * 3 + i] * d[1] + R[2 * 3 + i] * d[2];
            };
            toEye(rear, sil);
            sil[3] = radius;
            toEye(front, sil + 4);
            sil[7] = 1;
            sil[8] = ev.tan[0], sil[9] = ev.tan[1], sil[10] = ev.tan[2], sil[11] = ev.tan[3];
            silenced = true;
        }
        int icon_state = ::game::VrSilencerIcon();
        icon[0] = 0;
        if (icon_state >= 0 && !::game::VrMenuOverPlay()) {
            // under the ammunition and grenades (the hud's 640x480: about x 588, y 94), through the hud's own plane
            VkExtent2D se = xr::ScreenExtent();
            float m[16];
            if (HudMatrix(e, float(se.width) / float(se.height), m)) {
                auto toUv = [&](float px, float py, float* o) {
                    float x = px / 320.0f - 1, y = 1 - py / 240.0f;
                    float cx = m[0] * x + m[1] * y + m[3], cy = m[4] * x + m[5] * y + m[7], cw = m[12] * x + m[13] * y + m[15];
                    o[0] = 0.5f + 0.5f * cx / cw;
                    o[1] = 0.5f - 0.5f * cy / cw;
                };
                float c0[2], c1[2];
                toUv(588, 96, c0);  // (y 78 overlapped the grenade count's digits: headset shot 2026-11-24)
                toUv(612, 104, c1);  // (half size 24 x 8: the icon is long)  // (its half size: 20 x 8 of the hud's pixels)
                icon[0] = c0[0], icon[1] = c0[1], icon[2] = std::fabs(c1[0] - c0[0]), icon[3] = std::fabs(c1[1] - c0[1]);
                iconed = icon[2] > 0 && icon[3] > 0;
                icon_bright = icon_state > 0 ? 1.0f : 0.0f;
            }
        }
    }
    bool post = game::Opt().hdr || fxaa || bright > 0 || vignette > 0.01f || scoped || aimed || silenced || iconed;
    // opts.z: the taps a side over each screen pixel, 2 averaging SSAA's samples
    float c[56] = {0, 0, 1, 1, gamma_ ? 1.0f : 0.0f, 0, ssaa ? 2.0f : 1.0f, 1};
    c[52] = icon_bright;
    if (silenced) std::memcpy(c + 36, sil, 12 * sizeof(float));
    if (iconed) std::memcpy(c + 48, icon, sizeof(icon));
    if (scoped) std::memcpy(c + 24, lens, sizeof(lens));
    if (aimed) std::memcpy(c + 32, ret, sizeof(ret));
    c[20] = game::Opt().hdr ? 1.0f : 0.0f;
    c[21] = fxaa ? 1.0f : 0.0f;
    c[22] = bright;
    c[23] = vignette > 0.01f ? vignette : 0.0f;
    if (t) {
        // a texel of the frame, seconds that wrap before a float loses them, the colour look (4 vibrant on)
        using clock = std::chrono::steady_clock;
        float secs = std::fmod(std::chrono::duration<float>(clock::now().time_since_epoch()).count(), 1000.0f);
        float src[4] = {1.0f / float(t->img.w), 1.0f / float(t->img.h), secs, float(filter >= 4 ? filter - 4 : 0)};
        std::memcpy(c + 16, src, sizeof(src));
    }
    if (crt) {
        // light, then strong: the gaps' darkness, the curve, the mask's dimming, the corners' shade
        constexpr float kGap[] = {0.35f, 0.6f};
        constexpr float kCurve[] = {0.02f, 0.045f};
        constexpr float kMask[] = {0.15f, 0.3f};
        constexpr float kCorner[] = {0.1f, 0.2f};
        int k = int(crt) - 1;
        // the game's lines, as many screen pixels each as come nearest
        float per = std::max(1.0f, std::round(vh / float(guest_h_)));
        float lines = std::floor(vh / per);
        // on average a line keeps 1 - gap / 2 and the mask 1 - 2 mask / 3: that much taken back, most of it
        float kept = (1 - kGap[k] / 2) * (1 - 2 * kMask[k] / 3);
        float rest[8] = {lines, kGap[k], kCurve[k], kMask[k], kCorner[k], 1 + (1 / kept - 1) * 0.8f, 0, 0};
        std::memcpy(c + 8, rest, sizeof(rest));
    }
    Alloc cbuf = Constants(c, sizeof(c));
    const char* hlsl = filter || post ? kFilterHlsl : kBlitHlsl;
    const char* entry = crt ? "pscrt" : filter == 3 ? "pssharpen" : filter ? "pscolor" : post ? "psplain" : "psmain";
    VkPipeline pipe = t ? FullScreenPipeline(hlsl, entry, swap_fmt_, VK_FORMAT_UNDEFINED, false) : VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (pipe) {
        Transition(t->img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(lut_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (scoped) Transition(scope_img_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        const VkImageView views[4] = {t->img.view, lut_.view, scoped ? scope_img_.view : dummy_[0].view, VK_NULL_HANDLE};
        const VkSampler samplers[4] = {linear_, point_, point_, point_};
        set = Set(views, samplers);
    }
    VkRenderingAttachmentInfoKHR ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
    ca.imageView = swap_views_[index];
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color = {{0, 0, 0, panel_ ? 0.0f : 1.0f}};  // the panel's quad is blended over the eyes
    VkRenderingInfoKHR ri{VK_STRUCTURE_TYPE_RENDERING_INFO_KHR};
    ri.renderArea = {{0, 0}, swap_extent_};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    if (dynamic_) {
        begin_rendering_(cb, &ri);
    } else {
        VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        bi.renderPass = Pass(swap_fmt_, VK_FORMAT_UNDEFINED, true);
        bi.framebuffer = Framebuffer(swap_fmt_, VK_FORMAT_UNDEFINED, swap_views_[index], VK_NULL_HANDLE,
                                     swap_extent_.width, swap_extent_.height);
        bi.renderArea = ri.renderArea;
        bi.clearValueCount = 1;
        bi.pClearValues = &ca.clearValue;
        vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    }
    if (pipe) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        BindSet(set, cbuf.off, cbuf.off);
        SetViewport((w - vw) / 2, (h - vh) / 2, vw, vh);
        SetScissor(0, 0, int32_t(w), int32_t(h));
        vkCmdDraw(cb, 3, 1, 0, 0);
    }
    // imgui makes one frame per call: on the theater screen, or over play on the panel, never in an eye
    // (the settings panel shown, imgui runs in its pass alone: another pass would take its clicks with no window)
    if (!eye_image && (panel_ || !xr_ || !app::MenuOpen())) RenderUi();
    if (dynamic_)
        end_rendering_(cb);
    else
        vkCmdEndRenderPass(cb);
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    // openxr takes its images back in the attachment layout
    b.newLayout = xr_ ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
}

void VulkanBackend::RenderUi() {
    if (!ui_) return;
    std::lock_guard lock(app::UiLock());
    ImGui_ImplVulkan_NewFrame();
    ui::NewFrame(float(swap_extent_.width), float(swap_extent_.height));
    ImGui::NewFrame();
    if (app::kGame.ui) app::kGame.ui(fps_);
    if (!xr_) touch::Draw();  // no touch screen in the headset
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), Cb());
}

void VulkanBackend::UploadGammaRamp() {
    ramp_dirty_ = false;
    Alloc a = Upload(256 * 4, 16);
    bool identity = true;
    for (int i = 0; i < 256; i++) {
        uint8_t* p = a.cpu + i * 4;
        p[0] = ramp_[512 + i];
        p[1] = ramp_[256 + i];
        p[2] = ramp_[i];
        p[3] = 255;
        identity &= ramp_[i] == i && ramp_[256 + i] == i && ramp_[512 + i] == i;
    }
    gamma_ = !identity;
    Transition(lut_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c{a.off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {256, 1, 1}};
    vkCmdCopyBufferToImage(Cb(), a.buf, lut_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    Transition(lut_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// the touch controls' skins as an ImGui texture; without them the controls keep their drawn icons
void VulkanBackend::UploadSkin() {
    skin_tried_ = true;
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    if (!ui_ || !touch::LoadSkin(rgba, w, h)) return;
    constexpr VkImageUsageFlags kSampled = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!CreateImage(skin_, VK_FORMAT_R8G8B8A8_UNORM, w, h, 1, 1, 1, kSampled, VK_IMAGE_VIEW_TYPE_2D)) return;
    Alloc a = Upload(rgba.size(), 16);
    if (!a.cpu) {
        Destroy(skin_);
        return;
    }
    std::memcpy(a.cpu, rgba.data(), rgba.size());
    Transition(skin_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c{a.off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyBufferToImage(Cb(), a.buf, skin_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    Transition(skin_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    std::lock_guard lock(app::UiLock());
    VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(linear_, skin_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    touch::SetSkinTexture(uint64_t(reinterpret_cast<uintptr_t>(set)));
}

void VulkanBackend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;
    constexpr size_t kTexturesKept = 1024;  // a phone has less memory than a PC: fewer kept
    constexpr uint64_t kIdleFrames = 600;
    if (presents_ % kSweepEvery != 0 || textures_.size() <= kTexturesKept) return;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.frame + kIdleFrames < presents_) {
            Retire(it->second.img);
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }
}

void VulkanBackend::ApplySettings() {
    float want = app::RenderScale(guest_h_);
    if (std::fabs(want - scale_) <= 1e-4f) return;
    Submit(true, false);
    for (auto* map : {&targets_, &other_targets_, &scope_targets_}) {
        for (auto& [key, t] : *map) {
            Retire(t.img);
            Retire(t.fb);
        }
        map->clear();
    }
    draw_rt_ = draw_ds_ = nullptr;
    scale_ = want;
    LOGI("vulkan: render scale %.3f", scale_);
}

void VulkanBackend::SavePipelineCache() {
    size_t size = 0;
    if (vkGetPipelineCacheData(dev_, cache_, &size, nullptr) != VK_SUCCESS || size == cache_saved_) return;
    std::vector<char> blob(size);
    if (vkGetPipelineCacheData(dev_, cache_, &size, blob.data()) != VK_SUCCESS) return;
    std::filesystem::path p = app::ExeDir() / "shadercache" / "pipelines.bin", tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(blob.data(), std::streamsize(size));
    }
    std::error_code ec;
    std::filesystem::rename(tmp, p, ec);
    cache_saved_ = size;
}

void VulkanBackend::Present(const Surface& front) {
    if (stereo_ && eye_ == stereo::kScopeEye) {
        if (Target* t = EnsureTarget(front, false)) KeepScope(*t);
        return;
    }
    Target* t = EnsureTarget(front, false);
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    if (ramp_dirty_) UploadGammaRamp();
    if (!skin_tried_) UploadSkin();
    uint32_t index = 0;
    if (stereo_) {
        // both eyes' fronts: this eye's t, the other's from its own targets
        Target* eyes[2] = {};
        eyes[eye_] = t;
        std::swap(targets_, other_targets_);
        eyes[1 - eye_] = FindTarget(front, false);
        std::swap(targets_, other_targets_);
        bool begun = xr::BeginFrame();
        // no world this frame (menus, films, loading): the left eye's frame on the theater screen instead
        bool theater = frame_world_ == 0;
        frame_world_ = 0;
        {
            // dev_drawdiff: the kinds a throw added (seen within 2 s after it, not in the frame before it), once each
            static std::set<std::array<uint32_t, 3>> before, told;
            static int64_t thrown_seen = 0;
            int64_t thrown = ::game::VrThrownMs();
            int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch()).count();
            if (thrown != thrown_seen && now - thrown < 2000) {
                for (const auto& k : dev_kinds_)
                    if (!before.count(k) && told.insert(k).second)
                        LOGR("dev_drawdiff: after the throw: stride %u, %u vertex bytes (%u vertices), %u indices", k[0],
                             k[1], k[0] ? k[1] / k[0] : 0, k[2]);
            } else {
                before = dev_kinds_;
            }
            if (thrown != thrown_seen && now - thrown >= 2000) thrown_seen = thrown;
            dev_kinds_.clear();
        }
        last_theater_ = theater;
        if (theater) scene_proj_ok_[0] = scene_proj_ok_[1] = false;
        bool got[2] = {};
        uint32_t idx[2] = {};
        if (theater) {
            got[0] = got[1] = begun && xr::AcquireScreen(xr::kTheater, idx[0]);
            if (got[0]) DrawFrame(eyes[0], idx[0]);
        } else {
            for (int e = 0; e < 2; e++) {
                got[e] = begun && xr::AcquireScreen(e, idx[e]);
                if (got[e]) DrawFrame(eyes[e], idx[e]);
            }
        }
        // the settings panel beside the game's menus: imgui on its own swapchain (xr.cpp places its quad)
        uint32_t pidx = 0, cidx = 0;
        bool panel = begun && app::MenuOpen() && xr::AcquireScreen(xr::kPanel, pidx);
        if (panel) {
            panel_ = true;
            app::SetPanelPass(true);
            DrawFrame(nullptr, pidx);
            app::SetPanelPass(false);
            panel_ = false;
        }
        // the test checklist's card (the screen's swapchain in play, its corner on a quad at the left wrist)
        bool want_card = false, want_tip = false;
        if (!theater && begun && !app::MenuOpen()) ::game::VrHudWanted(want_card, want_tip);
        bool card = (want_card || want_tip) && xr::AcquireScreen(xr::kTheater, cidx);
        if (card) {
            panel_ = true;
            ::game::VrTotestCardPass(true);
            DrawFrame(nullptr, cidx);
            ::game::VrTotestCardPass(false);
            panel_ = false;
        }
        Submit(false, false);
        if ((theater && got[0]) || card) xr::ReleaseScreen(xr::kTheater);
        if (panel) xr::ReleaseScreen(xr::kPanel);
        for (int e = 0; e < 2 && !theater; e++)
            if (got[e]) xr::ReleaseScreen(e);
        xr::EndFrame(got[0] && got[1], theater, panel, card && want_card, card && want_tip);
        if (presents_ % 360 == 0) {
            const float* p = stat_proj_;
            // flat z: pretransformed depth-tested draws off the 2d layer (cpu-projected effects would show here)
            LOGR("stereo: %u draws, %u world (%u effects), %u gun, %u hud, %u flat z, %u unmoved; front %ux%u; "
                 "proj %.3f %.3f %.3f %.3f | %.3f %.3f",
                 stat_draws_, stat_world_, stat_effect_, stat_gun_, stat_hud_, stat_flat_z_, stat_unmoved_,
                 eyes[0] ? eyes[0]->w : 0, eyes[0] ? eyes[0]->h : 0, p[0], p[5], p[10], p[11], p[14], p[15]);
            stat_draws_ = stat_world_ = stat_unmoved_ = stat_effect_ = stat_gun_ = stat_hud_ = stat_flat_z_ = 0;
        }
    } else if (xr_) {
        // the headset's frame: waits for its slot (72 hz), the game's frame on the screen quad, handed back after
        // the submit so the compositor reads finished pixels
        bool shown = xr::BeginFrame() && xr::AcquireScreen(0, index);
        if (shown) DrawFrame(t, index);
        Submit(false, false);
        if (shown) xr::ReleaseScreen(0);
        xr::EndFrame(shown);
    }
    bool shown = !xr_ && AcquireImage(index);
    if (shown) {
        present_index_ = index;
        DrawFrame(t, index);
    }
    if (!xr_) Submit(false, shown);
    present_value_[presents_ % (kFrames + 1)].store(value_);
    if (shown) {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &swap_done_[index];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swap_;
        pi.pImageIndices = &index;
        VkResult r = vkQueuePresentKHR(queue_, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) swap_stale_ = true;
        if (r == VK_ERROR_SURFACE_LOST_KHR) surface_lost_ = true;
    }
    presents_++;
    presents_seen_.store(presents_);
    frame_++;
    Frame& next = CurrentFrame();
    ResetFrame(next);
    vkResetCommandPool(dev_, next.pool, 0);
    Begin();
    ReleaseOldTextures();
    fps_n_++;
    auto now = std::chrono::steady_clock::now();
    double el = std::chrono::duration<double>(now - fps_t_).count();
    if (el >= 0.5) {
        fps_ = fps_n_ / el;
        fps_n_ = 0;
        fps_t_ = now;
    }
    if (presents_ % 600 == 0) SavePipelineCache();
    dynres::Frame();
    ApplySettings();
}

// the slot is not reused before the caller's wait: the title runs at most one present ahead
void VulkanBackend::WaitGpuFrame(uint64_t n) { WaitValue(present_value_[n % (kFrames + 1)].load()); }

void VulkanBackend::BeginEye(int eye) {
    if (!stereo_ || eye == eye_) return;
    EndRender();
    draw_rt_ = draw_ds_ = nullptr;
    // the scope's view comes after the right eye, its targets swapped in for the right's and back
    if (eye_ == stereo::kScopeEye) {
        std::swap(targets_, scope_targets_);
        eye_ = 1;
        if (eye == 1) return;
    }
    if (eye == stereo::kScopeEye) {
        if (eye_ == 0) std::swap(targets_, other_targets_);
        std::swap(targets_, scope_targets_);
        eye_ = eye;
        return;
    }
    std::swap(targets_, other_targets_);
    eye_ = eye;
}

// the scope's view's front, copied for the eyes to draw on its glass
void VulkanBackend::KeepScope(Target& t) {
    if (!scope_img_ || scope_img_.w != t.w || scope_img_.h != t.h) {
        Retire(scope_img_);
        if (!CreateImage(scope_img_, kBgra, t.w, t.h, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         VK_IMAGE_VIEW_TYPE_2D))
            return;
    }
    EndRender();
    Transition(t.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(scope_img_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy c{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                  {t.w, t.h, 1}};
    vkCmdCopyImage(Cb(), t.img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scope_img_.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    Transition(scope_img_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    scope_frame_ = presents_;
}

VulkanBackend* g_vulkan = nullptr;

}  // namespace

std::unique_ptr<Backend> CreateBackend() {
    auto backend = std::make_unique<VulkanBackend>();
    g_vulkan = backend.get();
    return backend;
}

void WaitGpuFrame(uint64_t n) {
    if (g_vulkan) g_vulkan->WaitGpuFrame(n);
}

}  // namespace gpu

namespace stereo {
int Eyes() { return gpu::g_vulkan && gpu::g_vulkan->Stereo() ? 2 : 1; }
bool Scope() { return Eyes() == 2 && ::game::VrScopeWanted(); }
void BeginEye(int eye) {
    if (gpu::g_vulkan) gpu::g_vulkan->BeginEye(eye);
}
}  // namespace stereo
