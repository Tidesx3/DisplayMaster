#include "client.h"

#include <android/log.h>
#include <android/native_window.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define LOG_TAG "DMClient"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace dm {
namespace {

uint64_t mono_us() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

int connect_tcp(const std::string& host, uint16_t port, std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        error = "Can't resolve " + host;
        return -1;
    }
    int fd = -1;
    for (addrinfo* a = res; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        timeval tv{5, 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (::connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        error = "Can't reach " + host + ":" + std::to_string(port) + " - is DisplayMaster running on the PC?";
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    timeval none{0, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &none, sizeof none);
    // Receive timeout so pings/stats keep ticking while the desktop is static.
    timeval tick{0, 250000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tick, sizeof tick);
    return fd;
}

}  // namespace

void Client::connect(const std::string& host, uint16_t port, const proto::Hello& hello,
                     std::optional<noise::Key> identity) {
    disconnect();
    running_ = true;
    {
        std::lock_guard lock(pair_mu_);
        need_confirmation_ = false;
        pair_decision_ = -1;
    }
    thread_ = std::thread([this, host, port, hello, identity] { run(host, port, hello, identity); });
}

void Client::disconnect() {
    running_ = false;
    pair_cv_.notify_all();  // a pending pairing confirmation gives up
    if (sock_ >= 0) send(proto::Bye{"disconnected on the device"});  // polite goodbye (sealed on Wi-Fi)
    const int fd = sock_.exchange(-1);
    if (fd >= 0) {
        // Unblocks the receive thread.
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
    std::lock_guard lock(dec_mu_);
    decoder_.release();
    have_config_ = false;
}

void Client::confirm_pairing(bool codes_match) {
    std::lock_guard lock(pair_mu_);
    pair_decision_ = codes_match ? 1 : 0;
    pair_cv_.notify_all();
}

bool Client::wait_for_pairing_confirmation() {
    std::unique_lock lock(pair_mu_);
    if (!need_confirmation_) return true;
    pair_cv_.wait(lock, [this] { return pair_decision_ >= 0 || !running_; });
    return pair_decision_ == 1 && running_;
}

void Client::set_surface(ANativeWindow* window) {
    std::lock_guard lock(dec_mu_);
    if (window_) ANativeWindow_release(window_);
    window_ = window;
    if (!window_) {
        decoder_.release();
        return;
    }
    reconfigure_decoder_locked();
}

void Client::reconfigure_decoder_locked() {
    if (!window_ || !have_config_) return;
    if (!decoder_.configure(window_, config_, sdk_int_)) {
        listener_->on_state(ClientListener::kError, "This device can't decode the video format");
        return;
    }
    send(proto::RequestKeyframe{});
}

void Client::send_raw(const std::vector<uint8_t>& data) {
    const int fd = sock_;
    if (fd < 0) return;
    std::lock_guard lock(send_mu_);
    const std::vector<uint8_t> sealed = channel_ ? channel_->seal(data) : std::vector<uint8_t>();
    const std::vector<uint8_t>& out = channel_ ? sealed : data;
    size_t off = 0;
    while (off < out.size()) {
        const ssize_t n = ::send(fd, out.data() + off, out.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return;
        off += static_cast<size_t>(n);
    }
}

// Noise XX as the initiator (see dm/noise.h). Afterwards channel_ seals and opens frames.
bool Client::handshake(int fd, const noise::Key& identity, proto::FrameParser& parser, std::string& error) {
    noise::HandshakeXX hs(noise::HandshakeXX::Role::Initiator, noise::KeyPair::from_private(identity),
                          noise::prologue());
    std::vector<uint8_t> out, payload;
    hs.write_message({}, out);
    send_raw(noise::handshake_frame(out));

    proto::RawMessage msg;
    std::vector<uint8_t> buf(64 * 1024);
    const uint64_t deadline = mono_us() + 10000000;
    while (!parser.next(msg)) {
        if (!running_) return false;
        if (mono_us() > deadline) {
            error = "The PC didn't answer - update DisplayMaster on the PC";
            return false;
        }
        const ssize_t n = recv(fd, buf.data(), buf.size(), 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        if (n <= 0) {
            // A PC from before encryption drops the unknown first message.
            error = "The PC closed the connection - update DisplayMaster on the PC";
            return false;
        }
        parser.feed(buf.data(), static_cast<size_t>(n));
    }
    if (msg.header.type != proto::MsgType::Handshake || !hs.read_message(msg.payload, payload)) {
        error = "Secure connection to the PC failed";
        return false;
    }
    out.clear();
    hs.write_message({}, out);
    send_raw(noise::handshake_frame(out));

    pairing_code_ = noise::pairing_code(hs.handshake_hash());
    const std::string pc_key = noise::to_hex(hs.remote_static());
    {
        std::lock_guard lock(send_mu_);
        channel_ = std::make_unique<noise::SecureChannel>(hs);
    }
    if (!listener_->is_known_pc(pc_key)) {
        // First time with this PC: both screens show the code, the user confirms it here too.
        {
            std::lock_guard lock(pair_mu_);
            need_confirmation_ = true;
        }
        listener_->on_pairing(pairing_code_, pc_key);
    }
    LOGI("encrypted connection established");
    return true;
}

void Client::run(std::string host, uint16_t port, proto::Hello hello, std::optional<noise::Key> identity) {
    sdk_int_ = hello.sdk_int;
    pairing_code_.clear();  // USB has none; don't show a previous Wi-Fi connection's code
    listener_->on_state(ClientListener::kConnecting, host);
    std::string error;
    const int fd = connect_tcp(host, port, error);
    if (fd < 0) {
        running_ = false;
        listener_->on_state(ClientListener::kError, error);
        return;
    }
    sock_ = fd;

    proto::FrameParser parser;
    proto::RawMessage outer, msg;
    std::vector<uint8_t> buf(256 * 1024);
    bool welcomed = false;
    std::string end_reason = "The PC closed the connection";

    if (identity && !handshake(fd, *identity, parser, end_reason)) running_ = false;
    {
        std::lock_guard lock(pair_mu_);
        if (need_confirmation_) hello.flags |= proto::kHelloConfirmPairing;  // the PC shows its code too
    }
    if (running_) send(hello);
    stats_start_us_ = last_ping_us_ = mono_us();
    bytes_ = 0;

    while (running_) {
        const ssize_t n = recv(fd, buf.data(), buf.size(), 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            tick_stats();
            continue;
        }
        if (n <= 0) break;
        bytes_ += static_cast<uint64_t>(n);
        parser.feed(buf.data(), static_cast<size_t>(n));
        while (running_ && parser.next(outer)) {
            if (channel_) {
                if (outer.header.type != proto::MsgType::Encrypted || !channel_->open(outer.payload, msg)) {
                    end_reason = "Data from the PC failed verification";
                    running_ = false;
                    break;
                }
            } else {
                msg = std::move(outer);
            }
            if (!welcomed && msg.header.type == proto::MsgType::AwaitingApproval) {
                listener_->on_state(ClientListener::kConnecting, std::string(kApprovalPending) + pairing_code_);
                continue;
            }
            if (!welcomed) {
                auto w = msg.header.type == proto::MsgType::Welcome ? proto::decode<proto::Welcome>(msg.payload)
                                                                    : std::nullopt;
                if (!w || !w->accepted) {
                    end_reason = w ? w->reason : "Unexpected reply from the PC";
                    running_ = false;
                    break;
                }
                // New PC: no streaming (or input) until the user confirmed the code here too.
                if (!wait_for_pairing_confirmation()) {
                    if (running_) end_reason = "Pairing cancelled";
                    running_ = false;
                    break;
                }
                welcomed = true;
                LOGI("connected to %s", w->host_name.c_str());
                listener_->on_state(ClientListener::kConnected, w->host_name);
                continue;
            }
            if (msg.header.type == proto::MsgType::Bye) {
                if (auto b = proto::decode<proto::Bye>(msg.payload)) end_reason = b->reason;
                running_ = false;
                break;
            }
            handle(msg);
        }
        if (parser.error()) {
            end_reason = "Corrupt data from the PC";
            break;
        }
        tick_stats();
    }

    // sock_ already -1 means disconnect() closed it (user action).
    const bool user_initiated = sock_.exchange(-1) < 0;
    if (!user_initiated) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
    {
        std::lock_guard lock(send_mu_);
        channel_.reset();
    }
    {
        std::lock_guard lock(dec_mu_);
        decoder_.release();
        have_config_ = false;
    }
    listener_->on_state(user_initiated ? ClientListener::kDisconnected : ClientListener::kError,
                        user_initiated ? std::string() : end_reason);
}

void Client::handle(const proto::RawMessage& m) {
    switch (m.header.type) {
        case proto::MsgType::VideoFrame:
            if (auto f = proto::decode<proto::VideoFrame>(m.payload)) on_frame(*f);
            break;
        case proto::MsgType::VideoConfig:
            if (auto c = proto::decode<proto::VideoConfig>(m.payload)) {
                {
                    std::lock_guard lock(dec_mu_);
                    config_ = *c;
                    have_config_ = true;
                    reconfigure_decoder_locked();
                }
                listener_->on_video_config(*c);
                listener_->on_state(ClientListener::kStreaming, "");
            }
            break;
        case proto::MsgType::Pong:
            if (auto p = proto::decode<proto::Pong>(m.payload))
                rtt_ms_ = static_cast<float>(mono_us() - p->echo_time_us) / 1000.0f;
            break;
        case proto::MsgType::Ping:
            if (auto p = proto::decode<proto::Ping>(m.payload)) send(proto::Pong{p->sender_time_us, mono_us()});
            break;
        default: break;
    }
}

void Client::on_frame(const proto::VideoFrame& f) {
    Decoder::Feed r;
    {
        std::lock_guard lock(dec_mu_);
        if (!decoder_.ready()) return;  // no surface yet; a keyframe is requested once it exists
        r = decoder_.feed(f);
    }
    if (r == Decoder::Feed::NeedKeyframe) {
        // Rate-limited: the host answers within a frame or two.
        const uint64_t now = mono_us();
        if (now - last_key_request_us_ > 200000) {
            last_key_request_us_ = now;
            send(proto::RequestKeyframe{});
        }
    }
}

void Client::tick_stats() {
    const uint64_t now = mono_us();
    if (now - last_ping_us_ >= 1000000) {
        last_ping_us_ = now;
        send(proto::Ping{now});
    }
    const uint64_t elapsed = now - stats_start_us_;
    if (elapsed < 1000000) return;
    Decoder::Stats ds;
    {
        std::lock_guard lock(dec_mu_);
        ds = decoder_.take_stats();
    }
    ClientListener::Stats s;
    s.fps = static_cast<float>(ds.decoded) * 1e6f / static_cast<float>(elapsed);
    s.mbps = static_cast<float>(bytes_) * 8.0f / static_cast<float>(elapsed);
    s.rtt_ms = rtt_ms_;
    s.decode_ms = static_cast<float>(ds.avg_decode_us) / 1000.0f;
    s.dropped = ds.dropped;
    listener_->on_stats(s);
    send(proto::ClientStats{ds.decoded, ds.dropped, ds.avg_decode_us, static_cast<uint32_t>(elapsed / 1000)});
    stats_start_us_ = now;
    bytes_ = 0;
}

}  // namespace dm
