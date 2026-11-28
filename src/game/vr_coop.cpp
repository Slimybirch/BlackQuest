// Phase 9: the VR settings panel's CO-OP tab (menu.cpp draws it), over black-seamless-coop's session (coop.h).
// Host: this headset's internet address and port shown, COPY puts "ip:port" on the clipboard (the user: shared in
// the headset's messaging, pasted by the others). Join: PASTE, or typed on a number pad, or a game found on this
// network. Every player's name listed, the host marked. A name is picked on a letter pad (no text keyboard in VR).
// The internet address is asked of api.ipify.org (plain HTTP, its one line) when hosting starts, and only then
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <SDL3/SDL_clipboard.h>
#include <imgui.h>

#include "app/app.h"
#include "common/options.h"
#include "core/log.h"
#include "game/coop.h"

namespace game {
namespace {

std::mutex g_lock;
std::string g_public;            // this headset's internet address, "" not known yet
std::atomic<bool> g_asking{false};
std::string g_address;           // the JOIN row's ip:port
int g_pad = 0;                   // 0 none, 1 the address's number pad, 2 the name's letter pad
std::string g_name_edit;

// the internet address as the world sees it: a plain HTTP GET of api.ipify.org on a thread
void AskPublicAddress() {
    if (g_asking.exchange(true)) return;
    std::thread([] {
        std::string ip;
        addrinfo hints{}, *found = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo("api.ipify.org", "80", &hints, &found) == 0 && found) {
            int s = ::socket(found->ai_family, found->ai_socktype, found->ai_protocol);
            timeval tv{5, 0};
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            if (s >= 0 && ::connect(s, found->ai_addr, found->ai_addrlen) == 0) {
                const char req[] = "GET / HTTP/1.0\r\nHost: api.ipify.org\r\nUser-Agent: BlackQuest\r\n\r\n";
                ::send(s, req, sizeof req - 1, 0);
                std::string reply;
                char buf[512];
                for (ssize_t n; (n = ::recv(s, buf, sizeof buf, 0)) > 0;) reply.append(buf, size_t(n));
                size_t body = reply.find("\r\n\r\n");
                if (body != std::string::npos) {
                    std::string b = reply.substr(body + 4);
                    for (char c : b)
                        if ((c >= '0' && c <= '9') || c == '.') ip += c;
                }
            }
            if (s >= 0) ::close(s);
            freeaddrinfo(found);
        }
        LOGR("coop: internet address %s", ip.empty() ? "not found" : ip.c_str());
        std::lock_guard lock(g_lock);
        g_public = ip;
        g_asking = false;
    }).detach();
}

// this headset's address on its own network (Wi-Fi), for players in the same house
std::string LocalAddress() {
    std::string out;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK)) continue;
        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr, ip, sizeof ip);
        out = ip;
        break;
    }
    freeifaddrs(list);
    return out;
}

// "host:port" or "host" (the port's default)
bool SplitAddress(const std::string& text, std::string& host, uint16_t& port) {
    std::string t;
    for (char c : text)
        if (c != ' ' && c != '\n' && c != '\r' && c != '\t') t += c;
    if (t.empty()) return false;
    port = uint16_t(game::Opt().coop_port);
    size_t colon = t.rfind(':');
    if (colon != std::string::npos) {
        int p = std::atoi(t.c_str() + colon + 1);
        if (p <= 0 || p > 65535) return false;
        port = uint16_t(p);
        t = t.substr(0, colon);
    }
    host = t;
    return !host.empty();
}

bool Wide(const char* label, float w) { return ImGui::Button(label, ImVec2(w, 0)); }

// a pad of keys in rows; returns the key pressed, 0 none
char Pad(const char* const* rows, int count, float em) {
    char hit = 0;
    for (int r = 0; r < count; r++) {
        for (const char* k = rows[r]; *k; k++) {
            char label[8];
            std::snprintf(label, sizeof label, "%c##k%d", *k == ' ' ? '_' : *k, r * 64 + int(k - rows[r]));
            if (k != rows[r]) ImGui::SameLine();
            if (ImGui::Button(label, ImVec2(em * 2.6f, em * 2.2f))) hit = *k;
        }
    }
    return hit;
}

void DrawPlayers(const coop::Session& s) {
    ImGui::Text("PLAYERS  %d / %d", s.players, s.capacity);
    for (const auto& m : s.members) {
        std::string line = m.nickname;
        if (m.host) line += "  (HOST)";
        if (m.local) line += "  (YOU)";
        ImGui::BulletText("%s", line.c_str());
    }
    // (user 2026-11-27): every player's log kept as it is now, for a problem to look at later
    if (ImGui::Button("SAVE LOGS (ALL PLAYERS)")) coop::SaveAllLogs();
}

}  // namespace

void VrCoopTab(float em) {
    coop::Session s = coop::GetSession();
    const float w = ImGui::GetContentRegionAvail().x;
    if (g_address.empty()) g_address = Opt().coop_host;
    ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "%s", s.status.c_str());
    if (!s.error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", s.error.c_str());
    ImGui::Separator();

    // the name
    ImGui::Text("YOUR NAME: %s", s.nickname.c_str());
    ImGui::SameLine();
    if (ImGui::Button(g_pad == 2 ? "DONE##name" : "CHANGE##name")) {
        if (g_pad == 2) {
            coop::RequestNickname(g_name_edit);
            g_pad = 0;
        } else {
            g_name_edit = s.nickname == "Player" ? "" : s.nickname;
            g_pad = 2;
        }
    }
    if (g_pad == 2) {
        ImGui::Text("> %s_", g_name_edit.c_str());
        static const char* const kLetters[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM <"};
        char k = Pad(kLetters, 4, em);
        if (k == '<') {
            if (!g_name_edit.empty()) g_name_edit.pop_back();
        } else if (k && g_name_edit.size() < size_t(coop::kNicknameCharacters)) {
            g_name_edit += k;
        }
        return;
    }
    ImGui::Separator();

    if (s.running && s.role == coop::Role::Host) {
        // hosting: the address to share
        std::string pub;
        {
            std::lock_guard lock(g_lock);
            pub = g_public;
        }
        if (pub.empty() && !g_asking) AskPublicAddress();
        std::string share = pub.empty() ? "" : pub + ":" + std::to_string(s.port);
        ImGui::TextUnformatted("SHARE THIS WITH YOUR PARTNERS:");
        ImGui::SetWindowFontScale(1.5f);
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "%s", share.empty() ? "(finding your address...)" : share.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::BeginDisabled(share.empty());
        if (Wide("COPY ADDRESS", w * 0.48f)) SDL_SetClipboardText(share.c_str());
        ImGui::EndDisabled();
        ImGui::SameLine();
        std::string lan = LocalAddress();
        ImGui::TextDisabled("same Wi-Fi: %s:%u", lan.c_str(), unsigned(s.port));
        ImGui::TextDisabled("(over the internet: UDP port %u forwarded to this headset)", unsigned(s.port));
        ImGui::Separator();
        DrawPlayers(s);
        ImGui::Separator();
        if (Wide("STOP HOSTING", w)) coop::RequestStop();
        return;
    }
    if (s.running || s.pending) {
        // joined, or on the way
        DrawPlayers(s);
        ImGui::Separator();
        if (Wide("LEAVE", w)) coop::RequestStop();
        return;
    }

    // not in a game: host, or join
    if (Wide("HOST A GAME", w)) {
        {
            std::lock_guard lock(g_lock);
            g_public.clear();
        }
        coop::RequestStart(coop::Role::Host, {}, uint16_t(Opt().coop_port));
        AskPublicAddress();
    }
    ImGui::Separator();
    ImGui::TextUnformatted("JOIN A GAME:");
    ImGui::Text("> %s%s", g_address.c_str(), g_pad == 1 ? "_" : "");
    if (Wide("PASTE", w * 0.32f)) {
        if (char* clip = SDL_GetClipboardText()) {
            g_address.clear();
            for (const char* c = clip; *c && g_address.size() < 40; c++)
                if ((*c >= '0' && *c <= '9') || *c == '.' || *c == ':' || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '-')
                    g_address += *c;
            SDL_free(clip);
        }
    }
    ImGui::SameLine();
    if (Wide(g_pad == 1 ? "HIDE KEYS" : "TYPE IT", w * 0.32f)) g_pad = g_pad == 1 ? 0 : 1;
    ImGui::SameLine();
    std::string host;
    uint16_t port = 0;
    bool ok = SplitAddress(g_address, host, port);
    ImGui::BeginDisabled(!ok);
    if (Wide("CONNECT", w * 0.32f)) {
        {
            std::lock_guard<std::recursive_mutex> ui(app::UiLock());
            Opt().coop_host = g_address;
            app::Save();
        }
        g_pad = 0;
        coop::RequestStart(coop::Role::Join, host, port);
    }
    ImGui::EndDisabled();
    if (g_pad == 1) {
        static const char* const kDigits[] = {"123", "456", "789", ".0:", "<"};
        char k = Pad(kDigits, 5, em);
        if (k == '<') {
            if (!g_address.empty()) g_address.pop_back();
        } else if (k && g_address.size() < 40) {
            g_address += k;
        }
    }
    ImGui::Separator();
    ImGui::BeginDisabled(s.searching);
    if (Wide(s.searching ? "SEARCHING..." : "FIND GAMES ON THIS WI-FI", w)) coop::RequestSearch(uint16_t(Opt().coop_port));
    ImGui::EndDisabled();
    for (size_t i = 0; i < s.servers.size(); i++) {
        const auto& sv = s.servers[i];
        char label[160];
        std::snprintf(label, sizeof label, "JOIN %s  (%s)%s##sv%zu", sv.name.c_str(), sv.address.c_str(),
                      sv.busy ? "  FULL" : "", i);
        ImGui::BeginDisabled(sv.busy);
        if (Wide(label, w)) coop::RequestStart(coop::Role::Join, sv.address, sv.port);
        ImGui::EndDisabled();
    }
    ImGui::Separator();
    // the options that make sense here: the port (the host's, forwarded on its router)
    ImGui::Text("PORT: %u", unsigned(Opt().coop_port));
    ImGui::SameLine();
    if (ImGui::Button("-##port")) Opt().coop_port = Opt().coop_port > 1024 ? Opt().coop_port - 1 : 1024;
    ImGui::SameLine();
    if (ImGui::Button("+##port")) Opt().coop_port = Opt().coop_port < 65535 ? Opt().coop_port + 1 : 65535;
    ImGui::SameLine();
    if (ImGui::Button("DEFAULT##port")) Opt().coop_port = 45410;
}

}  // namespace game
