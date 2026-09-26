#include "transport/connection.h"

#include <ws2tcpip.h>

#include "core/log.h"

namespace dm {

bool winsock_init() {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

Connection::Connection(SOCKET s, std::string peer, bool loopback)
    : s_(s), peer_(std::move(peer)), loopback_(loopback) {
    BOOL nodelay = TRUE;
    setsockopt(s_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof nodelay);
    int sndbuf = 4 * 1024 * 1024;
    setsockopt(s_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndbuf), sizeof sndbuf);
}

Connection::~Connection() {
    close();
}

bool Connection::send(const std::vector<uint8_t>& data) {
    std::lock_guard lock(send_mu_);
    size_t off = 0;
    while (off < data.size()) {
        if (closed_) return false;
        const int n = ::send(s_, reinterpret_cast<const char*>(data.data() + off),
                             static_cast<int>(std::min<size_t>(data.size() - off, 1 << 20)), 0);
        if (n <= 0) {
            close();
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

int Connection::recv(uint8_t* buf, int n) {
    const int r = ::recv(s_, reinterpret_cast<char*>(buf), n, 0);
    return r > 0 ? r : 0;
}

bool Connection::peer_closed() {
    if (closed_) return true;
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s_, &rd);
    timeval zero{0, 0};
    if (select(0, &rd, nullptr, nullptr, &zero) <= 0) return false;  // nothing pending: still open
    char c;
    return ::recv(s_, &c, 1, MSG_PEEK) <= 0;  // readable with no data = orderly close / error
}

void Connection::close() {
    if (closed_.exchange(true)) return;
    shutdown(s_, SD_BOTH);
    closesocket(s_);
}

void Connection::close_gracefully() {
    if (closed_) return;
    shutdown(s_, SD_SEND);
    // Wait (bounded) for the peer to close its side, discarding whatever it still sends.
    DWORD timeout_ms = 5000;
    setsockopt(s_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof timeout_ms);
    char sink[1024];
    while (!closed_ && ::recv(s_, sink, sizeof sink, 0) > 0) {
    }
    close();
}

bool TcpServer::start(uint16_t port, bool allow_lan, AcceptFn on_accept) {
    allow_lan_ = allow_lan;
    on_accept_ = std::move(on_accept);
    // Not inheritable: a child process holding a copy would keep the port open after stop().
    listen_ = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (listen_ == INVALID_SOCKET) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    // Loopback-only unless Wi-Fi is enabled: adb reverse connects via 127.0.0.1.
    addr.sin_addr.s_addr = htonl(allow_lan ? INADDR_ANY : INADDR_LOOPBACK);
    if (bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(listen_, 8) != 0) {
        DM_LOGE("Cannot listen on port %u (in use?)", port);
        closesocket(listen_);
        listen_ = INVALID_SOCKET;
        return false;
    }
    running_ = true;
    thread_ = std::thread([this] { accept_loop(); });
    DM_LOGI("Listening on %s:%u", allow_lan ? "0.0.0.0" : "127.0.0.1", port);
    return true;
}

void TcpServer::stop() {
    if (!running_.exchange(false)) return;
    closesocket(listen_);
    if (thread_.joinable()) thread_.join();
}

void TcpServer::accept_loop() {
    while (running_) {
        sockaddr_in peer{};
        int len = sizeof peer;
        SOCKET s = accept(listen_, reinterpret_cast<sockaddr*>(&peer), &len);
        if (s == INVALID_SOCKET) continue;
        SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);
        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        const bool loopback = (ntohl(peer.sin_addr.s_addr) >> 24) == 127;
        std::string name = std::string(ip) + ":" + std::to_string(ntohs(peer.sin_port));
        DM_LOGI("Connection from %s%s", name.c_str(), loopback ? " (USB/adb)" : "");
        on_accept_(std::make_unique<Connection>(s, std::move(name), loopback));
    }
}

}  // namespace dm
