// Headless DisplayMaster client for integration tests: connects N simulated devices to a
// running engine, checks the handshake and video stream, measures throughput and RTT.
//
//   dm_testclient [--host 127.0.0.1] [--port 47800] [--clients N] [--seconds S]
//                 [--size 1280x800] [--codec h264|hevc|av1] [--mode extend|mirror]
//
// Exit code 0 when every client passed.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "dm/protocol.h"

using namespace dm;
using Clock = std::chrono::steady_clock;

namespace {

struct Options {
    std::string host = "127.0.0.1";
    uint16_t port = proto::kDefaultPort;
    int clients = 1;
    int seconds = 5;
    uint32_t width = 1280, height = 800;
    proto::Codec codec = proto::Codec::HEVC;
    proto::DisplayMode mode = proto::DisplayMode::Mirror;
};

struct Result {
    bool ok = false;
    std::string error;
    proto::VideoConfig config;
    uint64_t frames = 0, keyframes = 0, bytes = 0;
    double rtt_ms = 0;
    bool first_is_key = false, annexb_ok = true, keyframe_on_request = false;
};

uint64_t mono_us() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
}

bool send_all(SOCKET s, const std::vector<uint8_t>& d) {
    size_t off = 0;
    while (off < d.size()) {
        const int n = send(s, reinterpret_cast<const char*>(d.data() + off), static_cast<int>(d.size() - off), 0);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

bool starts_with_start_code(const std::vector<uint8_t>& d) {
    return (d.size() > 4 && d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 1) ||
           (d.size() > 3 && d[0] == 0 && d[1] == 0 && d[2] == 1);
}

Result run_client(const Options& o, int index) {
    Result r;
    addrinfo hints{}, *ai = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(o.host.c_str(), std::to_string(o.port).c_str(), &hints, &ai) != 0) {
        r.error = "resolve failed";
        return r;
    }
    SOCKET s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    const bool connected = connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0;
    freeaddrinfo(ai);
    if (!connected) {
        r.error = "connect failed";
        closesocket(s);
        return r;
    }
    DWORD timeout_ms = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof timeout_ms);

    proto::Hello hello;
    hello.device_id = "testclient-" + std::to_string(index);
    hello.device_name = "Test client " + std::to_string(index + 1);
    hello.model = "dm_testclient";
    hello.sdk_int = 34;
    hello.geometry = {o.width, o.height, 320, 60000, 0, proto::Posture::Unknown};
    hello.decode_codecs = proto::codec_bit(proto::Codec::H264) | proto::codec_bit(o.codec);
    hello.input_caps = proto::kCapTouch;
    hello.transport = proto::Transport::UsbAdb;
    hello.settings.mode = o.mode;
    hello.settings.max_fps = 60;
    hello.settings.preferred_codec = o.codec;
    send_all(s, proto::encode(hello));

    proto::FrameParser parser;
    proto::RawMessage msg;
    std::vector<uint8_t> buf(256 * 1024);
    bool welcomed = false, configured = false, requested_key = false;
    uint64_t key_requested_at_frame = 0;
    const auto start = Clock::now();
    const auto end = start + std::chrono::seconds(o.seconds);
    auto last_ping = Clock::now() - std::chrono::seconds(1);

    while (Clock::now() < end) {
        if (Clock::now() - last_ping > std::chrono::milliseconds(500)) {
            last_ping = Clock::now();
            send_all(s, proto::encode(proto::Ping{mono_us()}));
        }
        // Halfway through, ask for a keyframe like a decoder that lost sync.
        if (configured && !requested_key && Clock::now() - start > std::chrono::seconds(o.seconds) / 2) {
            requested_key = true;
            key_requested_at_frame = r.frames;
            send_all(s, proto::encode(proto::RequestKeyframe{}));
        }
        const int n = recv(s, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0);
        if (n <= 0) {
            if (WSAGetLastError() == WSAETIMEDOUT && welcomed) continue;  // static desktop
            r.error = welcomed ? "connection closed" : "no Welcome";
            break;
        }
        parser.feed(buf.data(), static_cast<size_t>(n));
        while (parser.next(msg)) {
            switch (msg.header.type) {
                case proto::MsgType::Welcome: {
                    auto w = proto::decode<proto::Welcome>(msg.payload);
                    if (!w || !w->accepted) {
                        r.error = "rejected: " + (w ? w->reason : std::string("malformed Welcome"));
                        closesocket(s);
                        return r;
                    }
                    welcomed = true;
                    break;
                }
                case proto::MsgType::VideoConfig:
                    if (auto c = proto::decode<proto::VideoConfig>(msg.payload)) {
                        r.config = *c;
                        configured = true;
                    }
                    break;
                case proto::MsgType::VideoFrame:
                    if (auto f = proto::decode<proto::VideoFrame>(msg.payload)) {
                        const bool key = f->flags & proto::kFrameKey;
                        if (r.frames == 0) r.first_is_key = key;
                        if (key && requested_key && r.frames >= key_requested_at_frame) r.keyframe_on_request = true;
                        if (r.config.codec != proto::Codec::AV1 && !starts_with_start_code(f->data)) r.annexb_ok = false;
                        ++r.frames;
                        r.keyframes += key;
                        r.bytes += f->data.size();
                    }
                    break;
                case proto::MsgType::Pong:
                    if (auto p = proto::decode<proto::Pong>(msg.payload))
                        r.rtt_ms = (mono_us() - p->echo_time_us) / 1000.0;
                    break;
                case proto::MsgType::Bye:
                    r.error = "host said bye";
                    break;
                default: break;
            }
        }
    }
    send_all(s, proto::encode(proto::Bye{"test finished"}));
    closesocket(s);

    if (r.error.empty()) {
        if (!configured) r.error = "no VideoConfig";
        else if (r.frames == 0) r.error = "no frames";
        else if (!r.first_is_key) r.error = "first frame is not a keyframe";
        else if (!r.annexb_ok) r.error = "bitstream is not Annex B";
        else if (requested_key && !r.keyframe_on_request) r.error = "RequestKeyframe was not answered";
    }
    r.ok = r.error.empty();
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--host") o.host = next();
        else if (a == "--port") o.port = static_cast<uint16_t>(std::stoi(next()));
        else if (a == "--clients") o.clients = std::stoi(next());
        else if (a == "--seconds") o.seconds = std::stoi(next());
        else if (a == "--size") {
            const auto v = next();
            o.width = static_cast<uint32_t>(std::stoul(v.substr(0, v.find('x'))));
            o.height = static_cast<uint32_t>(std::stoul(v.substr(v.find('x') + 1)));
        } else if (a == "--codec") {
            const auto v = next();
            o.codec = v == "h264" ? proto::Codec::H264 : v == "av1" ? proto::Codec::AV1 : proto::Codec::HEVC;
        } else if (a == "--mode") {
            o.mode = next() == "extend" ? proto::DisplayMode::Extend : proto::DisplayMode::Mirror;
        }
    }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    std::vector<Result> results(static_cast<size_t>(o.clients));
    std::vector<std::thread> threads;
    for (int i = 0; i < o.clients; ++i) threads.emplace_back([&, i] { results[static_cast<size_t>(i)] = run_client(o, i); });
    for (auto& t : threads) t.join();

    int failed = 0;
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        const char* codec = r.config.codec == proto::Codec::H264 ? "H.264" : r.config.codec == proto::Codec::AV1 ? "AV1" : "HEVC";
        printf("client %zu: %s  %s %ux%u  %llu frames (%llu key)  %.1f fps  %.1f Mbps  RTT %.2f ms%s%s\n", i + 1,
               r.ok ? "PASS" : "FAIL", codec, r.config.width, r.config.height, r.frames, r.keyframes,
               r.frames / static_cast<double>(o.seconds), r.bytes * 8.0 / o.seconds / 1e6, r.rtt_ms,
               r.ok ? "" : "  -> ", r.error.c_str());
        failed += !r.ok;
    }
    WSACleanup();
    return failed ? 1 : 0;
}
