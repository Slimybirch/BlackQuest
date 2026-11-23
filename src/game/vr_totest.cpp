// the in-game test checklist (user): TOTEST.md's unchecked items one at a time on a card over the left wrist, white
// while waiting and green once done, then the next. an item is done by the game's own events (TestEvent, called
// where the thing happens) and/or the user's PASS, chosen by holding the gun hand's ray on it a second (no game
// button is free for it). FAIL (red) keeps it for review, off the card until it's fixed (its line in TOTEST.md is
// changed, e.g. "retest 2" added to its parenthesis). SKIP puts it at the back, and it comes up again next run.
// passes and fails are kept in totest_done.txt (by the item's line), so a run carries on where the last left off
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "app/app.h"
#include "common/options.h"
#include "core/log.h"
#include "game/vr_totest_md.h"  // generated from TOTEST.md (tools/embed.py)
#include "xr.h"

namespace game {
namespace {

using Clock = std::chrono::steady_clock;

struct Item {
    std::string section, text, line;  // line: the text with its completion, the key in the done file
    std::vector<std::string> events;  // all of them, while it's the current item
    bool confirm = false;             // the user's PASS
};

std::mutex g_lock;
bool g_loaded = false;
std::vector<Item> g_items;
std::vector<size_t> g_queue;  // the items left, the current one first
std::set<std::string> g_seen;  // the current item's events so far
bool g_passed = false, g_failed = false;
size_t g_fails = 0;  // failed, before and this run
Clock::time_point g_done_at{};
bool g_card_pass = false;  // render thread: the imgui frame being drawn is the card's
size_t g_total = 0;

std::string Trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// "- [ ] text (auto: a, b; confirm)": the completion is the last parenthesis, if it starts with auto: or confirm
bool ParseItem(const std::string& raw, const std::string& section, Item& it) {
    if (raw.rfind("- [ ] ", 0) != 0) return false;
    std::string s = Trim(raw.substr(6));
    it.section = section;
    it.line = s;
    it.text = s;
    size_t open = s.rfind(" (");
    if (open != std::string::npos && s.back() == ')') {
        std::string spec = s.substr(open + 2, s.size() - open - 3);
        if (spec.rfind("auto:", 0) == 0 || spec.rfind("confirm", 0) == 0) {
            it.text = Trim(s.substr(0, open));
            for (size_t a = 0; a <= spec.size();) {
                size_t b = spec.find(';', a);
                if (b == std::string::npos) b = spec.size();
                std::string part = Trim(spec.substr(a, b - a));
                if (part == "confirm") {
                    it.confirm = true;
                } else if (part.rfind("auto:", 0) == 0) {
                    std::string list = part.substr(5);
                    for (size_t c = 0; c <= list.size();) {
                        size_t d = list.find(',', c);
                        if (d == std::string::npos) d = list.size();
                        std::string e = Trim(list.substr(c, d - c));
                        if (!e.empty()) it.events.push_back(e);
                        c = d + 1;
                    }
                }
                a = b + 1;
            }
        }
    }
    if (it.events.empty()) it.confirm = true;  // nothing to see it by: the user says
    return true;
}

std::filesystem::path DoneFile() { return app::ExeDir() / "totest_done.txt"; }

void Load() {
    if (g_loaded) return;
    g_loaded = true;
    std::set<std::string> done, failed;
    {
        std::ifstream f(DoneFile());
        for (std::string l; std::getline(f, l);) {
            size_t tab = l.find('\t');
            if (tab == std::string::npos) continue;
            std::string line = Trim(l.substr(tab + 1));
            done.insert(line);
            if (l.rfind("fail", 0) == 0) failed.insert(line);
        }
    }
    std::string text = kTotestMd, section;
    size_t skipped = 0;
    for (size_t a = 0; a < text.size();) {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        std::string l = Trim(text.substr(a, b - a));
        a = b + 1;
        if (l.rfind("## ", 0) == 0) {
            section = l.substr(3);
            continue;
        }
        Item it;
        if (!ParseItem(l, section, it)) continue;
        if (done.count(it.line)) {
            skipped++;
            g_fails += failed.count(it.line);
            continue;
        }
        g_queue.push_back(g_items.size());
        g_items.push_back(std::move(it));
    }
    g_total = g_items.size() + skipped;
    LOGR("vr totest: %zu items, %zu passed or failed before (%zu failed), %zu to go", g_total, skipped, g_fails,
         g_queue.size());
}

bool AutoDone(const Item& it) {
    for (const std::string& e : it.events)
        if (!g_seen.count(e)) return false;
    return true;
}
bool Done(const Item& it) { return g_passed || g_failed || (!it.confirm && AutoDone(it)); }

void Next() {
    if (!g_queue.empty()) g_queue.erase(g_queue.begin());
    g_seen.clear();
    g_passed = g_failed = false;
    g_done_at = {};
}

void Save(const Item& it) {
    std::ofstream f(DoneFile(), std::ios::app);
    f << (g_failed ? "fail" : "pass") << "\t" << it.line << "\n";
}

// the gun codes' other versions (the levels' variants) by their first's name, as TOTEST.md names them
std::string Canon(std::string e) {
    static const char* const kSame[][2] = {{"AK5", "AK1"}, {"PS5", "PST"}, {"GK5", "GK1"}, {"AS5", "ASR"},
                                           {"M45", "M16"}, {"MP1", "MP5"}, {"P95", "P90"}, {"SM5", "SMG"},
                                           {"MC5", "SM3"}, {"SN5", "SNR"}, {"HV5", "HVY"}, {"MG5", "MGN"},
                                           {"SH5", "SHG"}, {"RM5", "RM1"}};
    size_t colon = e.find(':');
    if (colon == std::string::npos) return e;
    std::string code = e.substr(colon + 1);
    for (const auto& s : kSame)
        if (code == s[0]) return e.substr(0, colon + 1) + s[1];
    return e;
}

}  // namespace

// any thread: something the checklist may be waiting for happened
void TestEvent(const std::string& name) {
    if (!Opt().vr_totest) return;
    std::string e = Canon(name);
    std::lock_guard lock(g_lock);
    Load();
    if (g_queue.empty()) return;
    const Item& it = g_items[g_queue.front()];
    if (std::find(it.events.begin(), it.events.end(), e) == it.events.end() || g_seen.count(e)) return;
    g_seen.insert(e);
    LOGR("vr totest: %s (for \"%s\")", e.c_str(), it.text.c_str());
}

std::string VrTip();  // vr_reload.cpp: the launchers' hint over the off hand

void VrHudWanted(bool& card, bool& tip) {
    card = tip = false;
    // dev hands: the real controllers lie on the desk by the headset, so anything on them would fill the view
    static const bool dev_hands = std::filesystem::exists(app::ExeDir() / "dev_fakehands");
    if (!xr::Stereo() || dev_hands) return;
    tip = !VrTip().empty();
    if (!Opt().vr_totest) return;
    std::lock_guard lock(g_lock);
    Load();
    card = !g_queue.empty();
}

namespace {
// the hint's strip under the card, white on dark, the text as big as fits
void DrawTip() {
    std::string t = VrTip();
    if (t.empty()) return;
    const float W = float(xr::kCardW), H = float(xr::kTipH);
    ImGui::SetNextWindowPos(ImVec2(0, float(xr::kTipY)));
    ImGui::SetNextWindowSize(ImVec2(W, H));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 20.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 10));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(16, 18, 22, 225));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(240, 190, 80, 255));
    ImGui::Begin("##tip", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale(1.0f);
    float em = ImGui::GetFontSize(), w = ImGui::CalcTextSize(t.c_str()).x;
    float scale = std::min(44.0f / em, (W - 40) / std::max(w, 1.0f));
    ImGui::SetWindowFontScale(scale);
    ImVec2 ts = ImGui::CalcTextSize(t.c_str());
    ImGui::SetCursorPos(ImVec2((W - ts.x) / 2, (H - ts.y) / 2));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(245, 245, 245, 255));
    ImGui::TextUnformatted(t.c_str());
    ImGui::PopStyleColor();
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}
}  // namespace
void VrTotestCardPass(bool on) { g_card_pass = on; }

// render thread, in the card's imgui frame: the current item, its events, PASS, FAIL and SKIP
void VrTotestUi() {
    if (!g_card_pass) return;
    DrawTip();
    if (!Opt().vr_totest) return;
    std::lock_guard lock(g_lock);
    if (g_queue.empty()) return;
    auto now = Clock::now();
    Item& it = g_items[g_queue.front()];
    bool done = Done(it);
    if (done && g_done_at == Clock::time_point{}) {
        g_done_at = now;
        Save(it);
        xr::Pulse(0, 0.25f, 0.03f);
        LOGR("vr totest: %s: %s", g_failed ? "failed" : "done", it.text.c_str());
    }
    if (done && now - g_done_at > std::chrono::milliseconds(1500)) {
        Next();
        if (g_queue.empty()) return;
    }
    const Item& cur = g_items[g_queue.front()];
    done = Done(cur);

    const float W = float(xr::kCardW), H = float(xr::kCardH);
    const ImU32 kWhite = IM_COL32(245, 245, 245, 255), kGreen = IM_COL32(90, 230, 110, 255),
                kGrey = IM_COL32(150, 155, 165, 255), kAmber = IM_COL32(240, 190, 80, 255),
                kRed = IM_COL32(240, 85, 75, 255);
    const ImU32 kDone = g_failed ? kRed : kGreen;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(W, H));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 22.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26, 20));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(16, 18, 22, 235));
    ImGui::PushStyleColor(ImGuiCol_Border, done ? kDone : IM_COL32(70, 75, 85, 255));
    ImGui::Begin("##totest", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale(1.0f);
    float em = ImGui::GetFontSize();
    float scale = 30.0f / em;  // about 30 px letters on the card: 7 mm on the wrist
    ImGui::SetWindowFontScale(scale);
    size_t left = g_queue.size(), number = g_total - left + 1;
    ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
    if (g_fails) ImGui::Text("TEST %zu / %zu  -  %s  -  %zu failed", number, g_total, cur.section.c_str(), g_fails);
    else ImGui::Text("TEST %zu / %zu  -  %s", number, g_total, cur.section.c_str());
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::SetWindowFontScale(scale * 1.25f);
    ImGui::PushStyleColor(ImGuiCol_Text, done ? kDone : kWhite);
    ImGui::PushTextWrapPos(W - 26);
    ImGui::TextUnformatted(cur.text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(scale * 0.85f);
    // the events it waits for, each green once seen
    if (!cur.events.empty()) {
        ImGui::Dummy(ImVec2(0, 2));
        for (size_t i = 0; i < cur.events.size(); i++) {
            bool seen = g_seen.count(cur.events[i]) > 0;
            if (i) ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, seen ? kGreen : kGrey);
            ImGui::Text("%s %s", seen ? "[x]" : "[ ]", cur.events[i].c_str());
            ImGui::PopStyleColor();
        }
    }
    if (done) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDone);
        ImGui::TextUnformatted(g_failed ? "FAILED: kept for review" : "DONE");
        ImGui::PopStyleColor();
    } else if (cur.confirm && AutoDone(cur)) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextUnformatted(cur.events.empty() ? "Check it, then hold the gun hand's aim on PASS or FAIL"
                                                  : "Seen: PASS if it looked right, else FAIL");
        ImGui::PopStyleColor();
    }

    // PASS, FAIL and SKIP along the bottom, chosen by holding the gun hand's ray on one for a second
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float bh = 64, by = H - bh - 18, gap = 20, bw = (W - 26 * 2 - gap * 2) / 3;
    const ImVec2 pass0(26, by), pass1(26 + bw, by + bh), fail0(26 + bw + gap, by), fail1(26 + bw * 2 + gap, by + bh),
        skip0(W - 26 - bw, by), skip1(W - 26, by + bh);
    float u = 0, v = 0;
    bool pointing = xr::CardPointer(u, v);
    ImVec2 pt(u * W, v * H);
    auto inside = [&](ImVec2 a, ImVec2 b) { return pointing && pt.x >= a.x && pt.x <= b.x && pt.y >= a.y && pt.y <= b.y; };
    static int hover = 0;  // 1 pass, 2 skip, 3 fail
    static Clock::time_point hover_since;
    static bool need_leave = false;  // after a choice, the ray leaves the buttons before the next
    int h = done ? 0 : inside(pass0, pass1) ? 1 : inside(skip0, skip1) ? 2 : inside(fail0, fail1) ? 3 : 0;
    if (h == 0) need_leave = false;
    if (need_leave) h = 0;
    if (h != hover) hover = h, hover_since = now;
    float held = hover ? std::chrono::duration<float>(now - hover_since).count() : 0.0f;
    auto button = [&](ImVec2 a, ImVec2 b, const char* label, ImU32 tint, bool on) {
        dl->AddRectFilled(a, b, IM_COL32(40, 44, 52, 255), 14);
        if (on) dl->AddRectFilled(a, ImVec2(a.x + (b.x - a.x) * std::min(held, 1.0f), b.y), tint, 14);
        dl->AddRect(a, b, tint, 14, 0, 3);
        ImGui::SetWindowFontScale(scale);
        ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2((a.x + b.x - ts.x) / 2, (a.y + b.y - ts.y) / 2),
                    kWhite, label);
    };
    if (!done) {
        button(pass0, pass1, "PASS", IM_COL32(60, 170, 80, 255), hover == 1);
        button(fail0, fail1, "FAIL", IM_COL32(200, 70, 60, 255), hover == 3);
        button(skip0, skip1, "SKIP", IM_COL32(150, 150, 160, 255), hover == 2);
    }
    if (pointing) dl->AddCircleFilled(pt, 9, IM_COL32(255, 255, 255, 220));
    if (hover && held >= 1.0f) {
        xr::Pulse(1, 0.3f, 0.03f);
        need_leave = true;
        if (hover == 1) {
            g_passed = true;
            LOGR("vr totest: passed by hand: %s", cur.text.c_str());
        } else if (hover == 3) {
            g_failed = true;
            g_fails++;
            LOGR("vr totest: failed by hand: %s", cur.text.c_str());
        } else {
            LOGR("vr totest: skipped: %s", cur.text.c_str());
            size_t i = g_queue.front();
            Next();
            g_queue.push_back(i);
        }
        hover = 0;
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

}  // namespace game
