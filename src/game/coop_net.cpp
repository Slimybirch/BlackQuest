// The co-op's socket side for Android (BSD sockets). From black-seamless-coop (MIT, src/game/COOP_LICENSE), its
// Win32 file rewritten for the headset: no WSA start-up, errno for the would-blocks, no command line (dev files)
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <fstream>

#include "app/app.h"
#include "core/log.h"
#include "game/coop_net.h"
#include "game/coop_online.h"

namespace coop::net {

namespace {

int g_socket = -1;
int g_control = -1;

int OpenUdp(uint16_t port, uint32_t bind_addr, bool broadcast) {
    int s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return -1;
    ::fcntl(s, F_SETFL, ::fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    int on = 1;
    if (broadcast) setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(bind_addr);
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        LOGE("coop: bind of UDP %u failed (%d)", port, errno);
        ::close(s);
        return -1;
    }
    return s;
}

int ReceiveOn(int s, Peer& from, void* p, int n) {
    if (s < 0) return -1;
    sockaddr_in a{};
    socklen_t len = sizeof a;
    ssize_t r = ::recvfrom(s, p, size_t(n), 0, reinterpret_cast<sockaddr*>(&a), &len);
    if (r < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? -1 : 0;  // (a refused port and the like: lost)
    from = {a.sin_addr.s_addr, ntohs(a.sin_port)};
    return int(r);
}

bool SendOn(int s, const Peer& to, const void* p, int n) {
    if (s < 0) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(to.port);
    a.sin_addr.s_addr = to.addr;
    return ::sendto(s, p, size_t(n), 0, reinterpret_cast<sockaddr*>(&a), sizeof a) == n;
}

}  // namespace

bool Open(uint16_t port) {
    Close();
    g_socket = OpenUdp(port, INADDR_ANY, true);
    return g_socket >= 0;
}

void Close() {
    online::Stop();
    if (g_socket >= 0) ::close(g_socket);
    g_socket = -1;
}

std::string HostName() {
    char name[64]{};
    return gethostname(name, sizeof name - 1) == 0 && name[0] ? name : "BLACK host";
}

uint32_t Resolve(const std::string& host) {
    size_t from = host.find_first_not_of(" \t");
    size_t to = host.find_last_not_of(" \t");
    if (from == std::string::npos) return 0;
    std::string name = host.substr(from, to - from + 1);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(name.c_str(), nullptr, &hints, &found) != 0 || !found) return 0;
    uint32_t addr = reinterpret_cast<sockaddr_in*>(found->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(found);
    return addr;
}

bool Send(const Peer& to, const void* p, int n) {
    if (to.online) return online::Send(to.online, p, n);
    return SendOn(g_socket, to, p, n);
}

int Receive(Peer& from, void* p, int n) {
    if (online::Active()) return online::Receive(from, p, n);
    return ReceiveOn(g_socket, from, p, n);
}

std::string Text(const Peer& p) {
    if (p.online) return online::PeerName(p.online);
    in_addr a{};
    a.s_addr = p.addr;
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &a, ip, sizeof ip);
    return std::string(ip) + ":" + std::to_string(p.port);
}

// no command line on Android: dev_coop_args in the app's folder holds one ("--coop host" and the like), for tests
bool Arg(const char* name, std::string& value) {
    static const std::string cmd = [] {
        std::ifstream f(app::ExeDir() / "dev_coop_args");
        std::string s, w;
        while (f >> w) s += " " + w;
        return s + " ";
    }();
    std::string n = std::string(" ") + name;
    for (size_t at = cmd.find(n); at != std::string::npos; at = cmd.find(n, at + 1)) {
        size_t end = at + n.size();
        if (end < cmd.size() && cmd[end] != ' ' && cmd[end] != '=') continue;  // --coop is not --coop-port
        value.clear();
        size_t from = cmd.find_first_not_of(" =", end);
        if (from != std::string::npos && cmd[from] != '-')
            for (size_t i = from; i < cmd.size() && cmd[i] != ' '; i++) value += cmd[i];
        return true;
    }
    return false;
}

bool Foreground() { return true; }  // (one copy a headset: always the one in front)

bool OpenControl(uint16_t port) {
    if (g_control >= 0) ::close(g_control);
    g_control = OpenUdp(port, INADDR_LOOPBACK, false);
    return g_control >= 0;
}

int ReceiveControl(Peer& from, void* p, int n) { return ReceiveOn(g_control, from, p, n); }

bool SendControl(const Peer& to, const void* p, int n) { return SendOn(g_control, to, p, n); }

}  // namespace coop::net
