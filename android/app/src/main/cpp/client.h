// Connection to a DisplayMaster host: handshake, video receive + decode, input send.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "decoder.h"
#include "dm/noise.h"
#include "dm/protocol.h"

struct ANativeWindow;

namespace dm {

// on_state(kConnecting, "approval:<code>"): the PC is asking its user to allow this device
// (<code>: the pairing code, empty over USB).
inline constexpr const char* kApprovalPending = "approval:";

class ClientListener {
public:
    virtual ~ClientListener() = default;
    enum State { kConnecting = 0, kConnected = 1, kStreaming = 2, kDisconnected = 3, kError = 4 };
    virtual void on_state(State s, const std::string& message) = 0;
    virtual void on_video_config(const proto::VideoConfig& cfg) = 0;
    struct Stats {
        float fps = 0, mbps = 0, rtt_ms = 0, decode_ms = 0;
        uint32_t dropped = 0;
    };
    virtual void on_stats(const Stats& s) = 0;
    // Wi-Fi, after the encrypted handshake: true if this device paired with the PC whose
    // public key (hex) this is. Called on the connection thread.
    virtual bool is_known_pc(const std::string& pc_key) = 0;
    // A PC this device doesn't know: show `code`; the user answers with confirm_pairing().
    virtual void on_pairing(const std::string& code, const std::string& pc_key) = 0;
};

class Client {
public:
    explicit Client(ClientListener* listener) : listener_(listener) {}
    ~Client() { disconnect(); }

    // Non-blocking: connects on a background thread, reporting via the listener.
    // `identity` (this device's long-term key) turns on encryption: required over Wi-Fi.
    void connect(const std::string& host, uint16_t port, const proto::Hello& hello,
                 std::optional<noise::Key> identity);
    void disconnect();
    // The user compared the pairing code with the PC's (after on_pairing).
    void confirm_pairing(bool codes_match);

    // Surface lifecycle from the UI (nullptr when destroyed). Takes ownership of one reference.
    void set_surface(ANativeWindow* window);

    template <typename T>
    void send(const T& msg) {
        send_raw(proto::encode(msg));
    }

private:
    void run(std::string host, uint16_t port, proto::Hello hello, std::optional<noise::Key> identity);
    bool handshake(int fd, const noise::Key& identity, proto::FrameParser& parser, std::string& error);
    // After Welcome from a new PC: blocks until the user confirmed the code. False: cancelled.
    bool wait_for_pairing_confirmation();
    void handle(const proto::RawMessage& m);
    void on_frame(const proto::VideoFrame& f);
    void reconfigure_decoder_locked();
    void send_raw(const std::vector<uint8_t>& data);
    void tick_stats();

    ClientListener* listener_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<int> sock_{-1};
    std::mutex send_mu_;  // also guards channel_ for sealing (nonce order == byte order)
    std::unique_ptr<noise::SecureChannel> channel_;
    std::string pairing_code_;

    std::mutex pair_mu_;
    std::condition_variable pair_cv_;
    bool need_confirmation_ = false;
    int pair_decision_ = -1;  // -1 pending, 0 cancelled, 1 codes match

    std::mutex dec_mu_;  // guards decoder_, window_, config_
    Decoder decoder_;
    ANativeWindow* window_ = nullptr;
    proto::VideoConfig config_;
    bool have_config_ = false;
    uint16_t sdk_int_ = 0;

    // Stats (receive thread only).
    uint64_t stats_start_us_ = 0, last_ping_us_ = 0, bytes_ = 0;
    std::atomic<float> rtt_ms_{0};
    uint64_t last_key_request_us_ = 0;
};

}  // namespace dm
