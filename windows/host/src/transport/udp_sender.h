// Sends video frames to one device over UDP (Wi-Fi): packetized with Reed-Solomon
// parity and sealed per packet (dm/udp_video.h), paced so a large keyframe doesn't
// arrive as one burst that overflows the access point's queue.
#pragma once

#include <winsock2.h>

#include <cstdint>
#include <memory>
#include <string>

#include "dm/protocol.h"
#include "dm/udp_video.h"

namespace dm {

class UdpSender {
public:
    ~UdpSender();
    // `ip`: the device's address (from its control connection); `port`: its video port.
    bool open(const std::string& ip, uint16_t port, const udp::Key& key);
    // Send rate while pacing; a few times the video bitrate leaves room for keyframes.
    void set_pace_kbps(uint32_t kbps) { pace_kbps_ = kbps; }
    void send_frame(const proto::VideoFrame& frame, int fec_percent);

private:
    SOCKET s_ = INVALID_SOCKET;
    std::unique_ptr<udp::Cipher> cipher_;
    uint64_t seq_ = 0;
    uint32_t pace_kbps_ = 100000;
};

}  // namespace dm
