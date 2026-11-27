#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <string>

// the game's own settings, in the settings file through app::kGame.options
namespace game {

// the languages, saved by these codes: English, French, German, Italian, Spanish, Indonesian. Indonesian is the
// recomp's own text only: the game files have none, so the game itself stays in English
inline constexpr const char* kLanguages[] = {"int", "fra", "deu", "ita", "esp", "ind"};

struct Options {
    bool skip_intros = false;  // logos at boot and the film after pressing start
    bool fps_counter = false;
    uint32_t fmv_scale = 0;  // 4:3 films on a wider screen: 0 fill it, top and bottom cut; 1 kept 4:3
    // a look over the frame: 0 none, 1 and 2 a CRT (light, strong), 3 sharpen, 4 vibrant, 5 noir, 6 sepia,
    // 7 night vision, 8 VHS
    uint32_t screen_filter = 0;
    bool reload_blur = true;
    bool bloom = true;  // the glow added over bright parts of the frame (native/vulkan.cpp)
    bool fog = true;
    uint32_t anti_aliasing = 0;  // 0 off, 1 FXAA as the frame goes onto the screen, 2 SSAA: twice the resolution
    bool hdr = false;  // a tone map that brings back the colours the console's overbright look burns out
    uint32_t draw_distance = 1;  // times the game's LOD and draw distances
    bool vr_grip_toggle = false;
    bool vr_totest = false;  // (off by default, user 2026-11-26; its key renamed so saved settings start off) vr: TOTEST.md's unchecked items one at a time on a card over the left wrist (user)
    bool vr_manual_reload = true;  // vr: magazines out and in by hand where the gun's magazine is known (phase 5)  // vr: the off hand's grip takes the gun with a press and lets go with the next (user)
    // vr: a physical gunstock (user): the barrel aims from the gun hand to the off hand whenever it's ahead, no grip
    bool vr_gunstock = false;
    uint32_t vr_gun_angle = 45;     // vr: the gun's pitch on the controller, degrees + 45 (0..90: -45 down..+45 up)
    uint32_t vr_stock_height = 10;  // vr: the off controller over the barrel's line on a stock, cm + 10 (-10..+10)
    uint32_t vr_snap_turn = 0;
    uint32_t vr_turn_speed = 100;   // vr: the smooth turn's speed, percent of the game's (50..200)
    uint32_t vr_view_height = 20;   // vr: the view (and hands) raised by this many cm less 20 (-20..+20)
    // vr: the HD texture pack (Phase 8) or the game's own textures (user: the choice, for an authentic look)
    bool vr_hd_textures = false;
    // phase 8: each texture the game uploads saved once, by its hash, as a PNG (textures_dump/ in the app's folder)
    bool vr_dump_textures = false;
    // co-op (black-seamless-coop, MIT; phase 9): 0 off, 1 host, 2 join the host at coop_host; the host's UDP port
    bool vr_launcher_tips_done = false;  // vr: the launchers' hints shown till the first round went in (user)
    bool vr_setup_done = false;  // vr: the first launch's setup (stance, height, turning) done or skipped (user)
    bool vr_wall_pull = true;  // vr: the gun pulled back along its barrel where it would go into a wall (user)
    bool vr_reticle = true;  // vr: a reticle where the barrel points, the game's crosshair hidden (user)
    uint32_t coop = 0;
    uint32_t coop_port = 45410;
    std::string coop_nickname = "Player";
    std::string coop_host;  // the last host joined (ip:port), the JOIN row's
    uint32_t coop_max_players = 4;  // the host's cap, 2 to 8
    uint32_t vr_vignette = 0;   // vr: the view's edges darkened while moving or turning: 0 off, 1 low, 2 medium, 3 high
    bool vr_seated = false;     // vr: played sitting: the view at standing height, no real crouch
    uint32_t vr_refresh = 0;    // vr: the display's refresh rate in Hz (0 the headset's own)  // vr: degrees a flick of the right stick turns the body; 0 turns smoothly (user)
    // the render resolution follows the frame rate (native/dynamic_resolution.cpp): down when it falls under the
    // target, up again once it holds the target this many seconds; within min and max, percent of the screen's
    bool dynamic_resolution = false;
    uint32_t dynamic_target_fps = 60;
    uint32_t dynamic_min_percent = 40;
    uint32_t dynamic_max_percent = 100;
    uint32_t dynamic_hold = 5;
    float mouse_sensitivity = 1.0f;
    float touch_sensitivity = 1.0f;  // the touch screen's look, apart from the mouse's
    bool invert_mouse = false;
    bool hold_aim = true;  // scopes too, keyboard and mouse
    uint32_t fov = 70;  // horizontal degrees on a 4:3 frame, 70 is the game's
    uint32_t viewmodel_fov = 70;  // the gun and arms, the same measure
    uint32_t walk_speed = 50;  // percent of running, while the walk key is held
    // a controller's sticks: the radial dead zone as percent of the travel, 0 the game's own, and what the travel
    // past it is scaled by; the left stick moves, the right looks
    uint32_t left_deadzone = 0;
    uint32_t right_deadzone = 0;
    float left_sensitivity = 1.0f;
    float right_sensitivity = 1.0f;
    uint32_t audio_output = 0;  // audio::kOutputAuto, kOutputStereo, kOutputSurround or kOutputMono
    std::string language = "int";  // one of kLanguages
    bool infinite_ammo = false;  // cheat: the player's magazine and reserve never go down
    uint32_t rapid_fire = 1;  // cheat: times the player's rate of fire, 1 the game's
    bool no_recoil = false;  // cheat: the player's shots do not kick the view
    uint32_t game_speed = 100;  // cheat: percent of the game's speed, menus and films keep theirs
    bool god_mode = false;  // cheat: no hit takes the player's health
    bool one_hit_kill = false;  // cheat: any hit of the player's bullets kills
    bool infinite_grenades = false;  // cheat: throwing a grenade keeps the count
    bool unlock_levels = false;  // cheat: every mission open on every difficulty, the save is left as it is
    bool unlock_black_ops = false;  // cheat: Black Ops open on the missions open on another difficulty, the same
    bool esp = false;  // cheat: a line from the bottom of the screen and a box to each living enemy
    bool aimbot = false;  // cheat: aiming in turns the view to the body of the enemy nearest the crosshair
    std::map<std::string, std::string> keys;  // action id to key names, absent is the default
};
Options& Opt();
// the language setting as this run started, the language the game boots in
const std::string& BootLanguage();
bool HasLanguages();  // language.cpp
bool LanguageAvailable(const std::string& code);  // language.cpp: the LANGUAGE row offers it
inline std::atomic<float> g_fps{0};  // from the host, for the game's own counter
inline std::atomic<bool> g_hide_hud{false};  // the toggle HUD key: the gameplay HUD is not drawn
// the view's zoom as the look update scales the turn by (src/game/mouse.cpp): 1 none, about 0.74 a rifle's sights,
// a scope's levels under that (a half at X2, a quarter at X5)
inline std::atomic<float> g_zoom{1.0f};
inline std::atomic<bool> g_focus_paused{false};  // the game thread waits while the app is in the background
inline std::atomic<uint32_t> g_focus_pauses{0};  // focus pauses ended so far
void WaitWhileUnfocused();  // native/game_host.cpp: game thread, before the present
bool HudShown();  // src/game/hud.cpp: the player's HUD drew lately, a hint's pause too

}  // namespace game
