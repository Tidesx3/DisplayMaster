#include "transport/udp_sender.h"

#include <ws2tcpip.h>

#include "core/log.h"
#include "core/win.h"

namespace dm {

UdpSender::~UdpSender() {
    if (s_ != INVALID_SOCKET) closesocket(s_);
}

bool UdpSender::open(const std::string& ip, uint16_t port, const udp::Key& key) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) return false;
    s_ = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (s_ == INVALID_SOCKET) return false;
    int sndbuf = 4 * 1024 * 1024;
    setsockopt(s_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndbuf), sizeof sndbuf);
    // A "connected" UDP socket: plain send(), and ICMP errors don't disturb anything else.
    if (connect(s_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        closesocket(s_);
        s_ = INVALID_SOCKET;
        return false;
    }
    cipher_ = std::make_unique<udp::Cipher>(key);
    return true;
}

void UdpSender::send_frame(const proto::VideoFrame& frame, int fec_percent) {
    if (s_ == INVALID_SOCKET) return;
    const auto packets = udp::packetize(frame, fec_percent);
    // Pacing: the first packets go out at once (small frames stay low-latency); beyond that,
    // hold the average send rate at pace_kbps_.
    constexpr size_t kBurst = 24;
    const uint64_t start = now_us();
    const double bytes_per_us = pace_kbps_ / 8000.0;
    size_t sent_bytes = 0;
    for (size_t i = 0; i < packets.size(); ++i) {
        const auto sealed = cipher_->seal(seq_++, packets[i]);
        send(s_, reinterpret_cast<const char*>(sealed.data()), static_cast<int>(sealed.size()), 0);
        sent_bytes += sealed.size();
        if (i >= kBurst && bytes_per_us > 0) {
            const uint64_t due = start + static_cast<uint64_t>(sent_bytes / bytes_per_us);
            const uint64_t now = now_us();
            if (due > now + 500) precise_sleep_us(due - now);
        }
    }
}

}  // namespace dm
