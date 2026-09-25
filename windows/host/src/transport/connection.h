// Stream connections and the TCP listener. USB via `adb reverse` arrives as a
// loopback TCP connection, Wi-Fi as a LAN one; both speak the same framed protocol.
#pragma once

#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dm {

class Connection {
public:
    explicit Connection(SOCKET s, std::string peer, bool loopback);
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // Thread-safe; sends the whole buffer or fails (and closes).
    bool send(const std::vector<uint8_t>& data);
    // Blocking read of up to n bytes. Returns 0 on close/error.
    int recv(uint8_t* buf, int n);
    void close();

    const std::string& peer() const { return peer_; }
    bool loopback() const { return loopback_; }
    bool closed() const { return closed_; }
    // Non-blocking check whether the peer hung up (without consuming data).
    bool peer_closed();

private:
    SOCKET s_;
    std::string peer_;
    bool loopback_;
    std::mutex send_mu_;
    std::atomic<bool> closed_{false};
};

class TcpServer {
public:
    using AcceptFn = std::function<void(std::unique_ptr<Connection>)>;
    ~TcpServer() { stop(); }
    bool start(uint16_t port, bool allow_lan, AcceptFn on_accept);
    void stop();

private:
    void accept_loop();

    SOCKET listen_ = INVALID_SOCKET;
    bool allow_lan_ = false;
    AcceptFn on_accept_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

bool winsock_init();

}  // namespace dm
