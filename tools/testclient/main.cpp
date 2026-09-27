// Headless DisplayMaster client for integration tests: connects N simulated devices to a
// running engine, checks the handshake and video stream, measures throughput and RTT.
//
//   dm_testclient [--host 127.0.0.1] [--port 47800] [--clients N] [--seconds S]
//                 [--size 1280x800] [--codec h264|hevc|av1] [--mode extend|mirror]
//                 [--secure] [--key <64 hex digits>] [--udp [--loss N] [--udp-blocked]]
//   --secure: encrypted like Wi-Fi (Noise handshake); --key fixes the device key so a
//   second run is recognized as an already paired device.
//   --udp (with --secure): video over UDP like Wi-Fi; --loss drops N % of the datagrams at
//   random (parity must repair most), --udp-blocked drops all (must fall back to TCP).
//   --windows: also asks for the PC's window list and to pull the last used window here
//   (run the engine with --no-input: it answers without moving anything).
//
// Exit code 0 when every client passed.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "dm/noise.h"
#include "dm/protocol.h"
#include "dm/udp_video.h"

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
    bool secure = false;
    std::optional<noise::Key> key;
    bool udp = false;
    int loss_percent = 0;
    bool udp_blocked = false;
    bool rfi = true;  // announce recovery-frame support (like the app); --no-rfi: keyframes only
    bool windows = false;
};

struct Result {
    bool ok = false;
    std::string error;
    proto::VideoConfig config;
    uint64_t frames = 0, keyframes = 0, bytes = 0;
    double rtt_ms = 0;
    bool first_is_key = false, annexb_ok = true, keyframe_on_request = false;
    // --windows
    bool windows_feature = false;
    int window_count = -1, window_icons = 0, windows_here = 0;
    std::vector<std::string> window_apps;  // program names only: titles can be private
    int move_result = -1;
    std::string pairing_code;  // secure connections
    bool approval_requested = false;
    // UDP video
    bool udp_video = false;   // the host switched video to UDP
    uint64_t udp_frames = 0, udp_lost = 0, udp_recovered = 0, udp_dropped_by_test = 0;
    bool fell_back = false;   // no UDP arrived; video moved back to TCP
    uint64_t recoveries = 0, key_repairs = 0;  // how losses were repaired
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

    proto::FrameParser parser;
    proto::RawMessage msg;
    std::vector<uint8_t> buf(256 * 1024);

    // Wi-Fi style: Noise XX handshake first, then every frame sealed.
    std::unique_ptr<noise::SecureChannel> channel;
    if (o.secure) {
        const auto identity = o.key ? noise::KeyPair::from_private(*o.key) : noise::KeyPair::generate();
        noise::HandshakeXX hs(noise::HandshakeXX::Role::Initiator, identity, noise::prologue());
        std::vector<uint8_t> out, payload;
        hs.write_message({}, out);
        send_all(s, noise::handshake_frame(out));
        while (!parser.next(msg)) {
            const int n = recv(s, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0);
            if (n <= 0) break;
            parser.feed(buf.data(), static_cast<size_t>(n));
        }
        if (msg.header.type != proto::MsgType::Handshake || !hs.read_message(msg.payload, payload)) {
            r.error = "handshake failed";
            closesocket(s);
            return r;
        }
        out.clear();
        hs.write_message({}, out);
        send_all(s, noise::handshake_frame(out));
        r.pairing_code = noise::pairing_code(hs.handshake_hash());
        channel = std::make_unique<noise::SecureChannel>(hs);
    }
    auto send_msg = [&](const std::vector<uint8_t>& frame) { return send_all(s, channel ? channel->seal(frame) : frame); };

    // Like the app over Wi-Fi: a UDP port for video, offered in Hello.
    SOCKET us = INVALID_SOCKET;
    if (o.udp) {
        us = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        inet_pton(AF_INET, "0.0.0.0", &a.sin_addr);
        bind(us, reinterpret_cast<sockaddr*>(&a), sizeof a);
        int rcvbuf = 8 * 1024 * 1024;
        setsockopt(us, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof rcvbuf);
        u_long nonblocking = 1;
        ioctlsocket(us, FIONBIO, &nonblocking);
    }

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
    if (us != INVALID_SOCKET && o.rfi) hello.flags |= proto::kHelloRecoveryFrames;
    if (us != INVALID_SOCKET) {
        sockaddr_in a{};
        int len = sizeof a;
        getsockname(us, reinterpret_cast<sockaddr*>(&a), &len);
        hello.udp_port = ntohs(a.sin_port);
    }
    send_msg(proto::encode(hello));

    proto::RawMessage outer;
    bool welcomed = false, configured = false, requested_key = false;
    uint64_t key_requested_at_frame = 0;
    const auto start = Clock::now();
    const auto end = start + std::chrono::seconds(o.seconds);
    auto last_ping = Clock::now() - std::chrono::seconds(1);

    // Like the device's decoder: after a lost frame, skip until a keyframe and ask for one.
    // Over UDP the first keyframe itself can be lost, so start out waiting for one.
    bool waiting_for_key = o.udp;
    uint32_t stats_lost = 0, stats_recovered = 0;
    auto last_stats = Clock::now();
    auto last_key_request = Clock::now() - std::chrono::seconds(1);
    auto ask_keyframe = [&] {
        if (Clock::now() - last_key_request < std::chrono::milliseconds(200)) return;
        last_key_request = Clock::now();
        send_msg(proto::encode(proto::RequestKeyframe{}));
    };
    // After a loss (with RFI): skip frames until a recovery frame newer than the loss or a key.
    std::optional<uint32_t> recover_after;
    auto lost_at = Clock::now();
    auto on_frame = [&](const proto::VideoFrame& f) {
        const bool key = f.flags & proto::kFrameKey;
        const bool recovery = (f.flags & proto::kFrameRecovery) && recover_after &&
                              static_cast<int32_t>(static_cast<uint32_t>(f.frame_id) - *recover_after) > 0;
        if (waiting_for_key && !key && !recovery) {
            // With RFI the PC repairs by itself; ask for a keyframe only if nothing comes.
            if (!recover_after || Clock::now() - lost_at > std::chrono::milliseconds(300)) ask_keyframe();
            return;
        }
        if (waiting_for_key) (key ? r.key_repairs : r.recoveries) += 1;
        waiting_for_key = false;
        recover_after.reset();
        if (r.frames == 0) r.first_is_key = key;
        if (key && requested_key && r.frames >= key_requested_at_frame) r.keyframe_on_request = true;
        if (r.config.codec != proto::Codec::AV1 && !starts_with_start_code(f.data)) r.annexb_ok = false;
        ++r.frames;
        r.keyframes += key;
        r.bytes += f.data.size();
    };

    std::unique_ptr<udp::Cipher> udp_cipher;
    udp::FrameAssembler assembler;
    auto udp_since = Clock::now();    // the switch to UDP
    auto config_at = Clock::now();    // the latest VideoConfig
    bool udp_got = false;             // any valid datagram since the switch
    std::mt19937 rng(1234 + index);
    std::vector<uint8_t> dgram(2048), plain;

    while (Clock::now() < end) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        if (us != INVALID_SOCKET) FD_SET(us, &rd);
        timeval tick{0, 20000};
        select(0, &rd, nullptr, nullptr, &tick);
        if (us != INVALID_SOCKET && FD_ISSET(us, &rd)) {
            int n;
            while ((n = recv(us, reinterpret_cast<char*>(dgram.data()), static_cast<int>(dgram.size()), 0)) > 0) {
                if (!udp_cipher) continue;
                if (o.udp_blocked || static_cast<int>(rng() % 100) < o.loss_percent) {
                    ++r.udp_dropped_by_test;
                    continue;
                }
                uint64_t seq = 0;
                if (!udp_cipher->open(std::span<const uint8_t>(dgram.data(), static_cast<size_t>(n)), plain, seq)) continue;
                udp_got = true;
                assembler.add(plain, mono_us());
            }
        }
        if (udp_cipher) {
            proto::VideoFrame f;
            while (assembler.pop(f, mono_us())) {
                ++r.udp_frames;
                on_frame(f);
            }
            uint32_t lost_first = 0, lost_last = 0;
            if (const uint32_t lost = assembler.take_lost(lost_first, lost_last)) {
                r.udp_lost += lost;
                stats_lost += lost;
                waiting_for_key = true;
                lost_at = Clock::now();
                if (o.rfi) {
                    recover_after = recover_after ? std::max(*recover_after, lost_last) : lost_last;
                    send_msg(proto::encode(proto::InvalidateFrames{lost_first, lost_last}));
                } else {
                    ask_keyframe();
                }
            }
            const uint32_t recovered = assembler.take_recovered();
            r.udp_recovered += recovered;
            stats_recovered += recovered;
            // Once a second, like the app: the host adapts the parity to these.
            if (Clock::now() - last_stats > std::chrono::seconds(1)) {
                last_stats = Clock::now();
                proto::ClientStats st;
                st.interval_ms = 1000;
                st.udp_lost_frames = std::exchange(stats_lost, 0);
                st.udp_recovered_shards = std::exchange(stats_recovered, 0);
                send_msg(proto::encode(st));
            }
            // Nothing gets through: tell the host to keep video on TCP. Like the app, only
            // silence from the start counts (a still desktop sends no frames).
            if (!udp_got && configured && Clock::now() - std::max(udp_since, config_at) > std::chrono::seconds(3)) {
                send_msg(proto::encode(proto::UdpFallback{}));
                udp_cipher.reset();
                r.fell_back = true;
            }
        }
        if (!FD_ISSET(s, &rd)) {
            if (Clock::now() - last_ping > std::chrono::milliseconds(500)) {
                last_ping = Clock::now();
                send_msg(proto::encode(proto::Ping{mono_us()}));
            }
            continue;
        }
        if (Clock::now() - last_ping > std::chrono::milliseconds(500)) {
            last_ping = Clock::now();
            send_msg(proto::encode(proto::Ping{mono_us()}));
        }
        // Halfway through, ask for a keyframe like a decoder that lost sync.
        if (configured && !requested_key && Clock::now() - start > std::chrono::seconds(o.seconds) / 2) {
            requested_key = true;
            key_requested_at_frame = r.frames;
            send_msg(proto::encode(proto::RequestKeyframe{}));
        }
        const int n = recv(s, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0);
        if (n <= 0) {
            if (WSAGetLastError() == WSAETIMEDOUT && welcomed) continue;  // static desktop
            r.error = welcomed ? "connection closed" : "no Welcome";
            break;
        }
        parser.feed(buf.data(), static_cast<size_t>(n));
        while (parser.next(outer)) {
            if (channel && (outer.header.type != proto::MsgType::Encrypted || !channel->open(outer.payload, msg))) {
                r.error = "message failed authentication";
                break;
            }
            if (!channel) msg = outer;
            switch (msg.header.type) {
                case proto::MsgType::AwaitingApproval:
                    r.approval_requested = true;
                    break;
                case proto::MsgType::Welcome: {
                    auto w = proto::decode<proto::Welcome>(msg.payload);
                    if (!w || !w->accepted) {
                        r.error = "rejected: " + (w ? w->reason : std::string("malformed Welcome"));
                        closesocket(s);
                        return r;
                    }
                    welcomed = true;
                    r.windows_feature = (w->features & proto::kFeatureWindows) != 0;
                    break;
                }
                case proto::MsgType::VideoConfig:
                    if (auto c = proto::decode<proto::VideoConfig>(msg.payload)) {
                        r.config = *c;
                        if (o.windows && !configured) send_msg(proto::encode(proto::WindowListRequest{}));
                        configured = true;
                        config_at = Clock::now();
                    }
                    break;
                case proto::MsgType::WindowList:
                    if (auto l = proto::decode<proto::WindowList>(msg.payload)) {
                        r.window_count = static_cast<int>(l->windows.size());
                        for (const auto& x : l->windows) {
                            r.window_icons += !x.icon.empty();
                            r.windows_here += (x.flags & proto::kWindowHere) != 0;
                            if (std::find(r.window_apps.begin(), r.window_apps.end(), x.app) == r.window_apps.end())
                                r.window_apps.push_back(x.app);
                        }
                        send_msg(proto::encode(proto::MoveWindow{0, proto::WindowTarget::Here}));
                    }
                    break;
                case proto::MsgType::MoveWindowResult:
                    if (auto m = proto::decode<proto::MoveWindowResult>(msg.payload))
                        r.move_result = static_cast<int>(m->result);
                    break;
                case proto::MsgType::VideoFrame:
                    if (auto f = proto::decode<proto::VideoFrame>(msg.payload)) on_frame(*f);
                    break;
                case proto::MsgType::VideoTransport:
                    if (auto t = proto::decode<proto::VideoTransport>(msg.payload)) {
                        if (t->udp && us != INVALID_SOCKET) {
                            udp_cipher = std::make_unique<udp::Cipher>(t->key);
                            udp_since = Clock::now();
                            udp_got = false;
                            r.udp_video = true;
                        } else {
                            udp_cipher.reset();
                        }
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
        if (!r.error.empty()) break;
    }
    send_msg(proto::encode(proto::Bye{"test finished"}));
    closesocket(s);
    if (us != INVALID_SOCKET) closesocket(us);

    if (r.error.empty()) {
        if (!configured) r.error = "no VideoConfig";
        else if (r.frames == 0) r.error = "no frames";
        else if (!r.first_is_key) r.error = "first frame is not a keyframe";
        else if (o.udp && !r.udp_video) r.error = "host didn't switch video to UDP";
        else if (o.udp && !o.udp_blocked && r.udp_frames == 0) r.error = "no frames over UDP";
        else if (o.udp_blocked && !r.fell_back) r.error = "no fallback to TCP";
        else if (!r.annexb_ok) r.error = "bitstream is not Annex B";
        else if (requested_key && !r.keyframe_on_request) r.error = "RequestKeyframe was not answered";
        else if (o.windows && !r.windows_feature) r.error = "engine doesn't offer moving windows";
        else if (o.windows && r.window_count < 0) r.error = "no WindowList";
        else if (o.windows && r.move_result < 0) r.error = "MoveWindow was not answered";
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
        } else if (a == "--secure") {
            o.secure = true;
        } else if (a == "--key") {
            o.key = noise::key_from_hex(next());
        } else if (a == "--udp") {
            o.udp = true;
        } else if (a == "--loss") {
            o.loss_percent = std::stoi(next());
        } else if (a == "--udp-blocked") {
            o.udp_blocked = true;
        } else if (a == "--no-rfi") {
            o.rfi = false;
        } else if (a == "--windows") {
            o.windows = true;
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
        if (r.udp_video)
            printf("client %zu: UDP video: %llu frames, %llu lost, %llu shards rebuilt, %llu datagrams dropped by the test%s\n"
                   "client %zu: losses repaired by %llu recovery frames, %llu keyframes\n",
                   i + 1, r.udp_frames, r.udp_lost, r.udp_recovered, r.udp_dropped_by_test,
                   r.fell_back ? ", fell back to TCP" : "", i + 1, r.recoveries, r.key_repairs);
        if (o.windows) {
            static const char* kResults[] = {"moved", "nothing to move", "gone", "denied", "not responding", "not extended", "failed"};
            std::string apps;
            for (const auto& a : r.window_apps) apps += (apps.empty() ? "" : ", ") + a;
            printf("client %zu: windows: %d listed (%d with icon, %d here), apps: %s; pull last window: %s\n", i + 1,
                   r.window_count, r.window_icons, r.windows_here, apps.c_str(),
                   r.move_result >= 0 && r.move_result < 7 ? kResults[r.move_result] : "no answer");
        }
        if (!r.pairing_code.empty())
            printf("client %zu: encrypted, pairing code %s%s\n", i + 1, r.pairing_code.c_str(),
                   r.approval_requested ? "" : ", already paired");
        failed += !r.ok;
    }
    WSACleanup();
    return failed ? 1 : 0;
}
