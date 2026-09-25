// One connected device: handshake, its virtual monitor, the video stream and
// its input. Runs a receive thread and a video thread.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "display/virtual_display.h"
#include "session/approval.h"
#include "dm/protocol.h"
#include "encode/encoder.h"
#include "input/input_injector.h"
#include "transport/connection.h"

namespace dm {

struct HostOptions {
    uint16_t port = proto::kDefaultPort;
    std::optional<bool> allow_wifi;     // unset: use the saved setting (default off)
    bool adb = true;
    bool adb_auto_launch = true;
    std::optional<proto::DisplayMode> force_mode;  // overrides the device preference
    std::optional<proto::Codec> codec;  // force a codec
    EncoderBackend backend = EncoderBackend::Auto;
    uint32_t bitrate_kbps = 0;          // 0 = auto
    uint32_t max_fps = 120;
    float resolution_scale = 1.0f;      // virtual monitor size relative to the device panel
    bool inject_input = true;           // false: log input instead of injecting (testing)
    PressureCurve pen_curve;            // shaping applied to pen pressure
};

// Snapshot for the control API / UI.
struct SessionStatus {
    uint32_t id = 0;
    std::string device_name, model, peer;
    bool usb = false;
    bool streaming = false;
    proto::DisplayMode mode = proto::DisplayMode::Extend;
    proto::Codec codec = proto::Codec::HEVC;
    uint32_t width = 0, height = 0, fps = 0, bitrate_kbps = 0;
    std::string encoder, adapter, monitor;  // monitor: GDI name
    RectI monitor_rect;
    bool has_pen = false;
    // Live, updated once per second.
    double sent_fps = 0, mbps = 0, work_ms = 0, client_decode_ms = 0;
    uint32_t client_dropped = 0;
};

class Session {
public:
    Session(uint32_t id, std::unique_ptr<Connection> conn, const HostOptions& opts, VirtualDisplayManager& vdm,
            ApprovalBroker& approvals);
    ~Session();

    void start();
    void stop();
    bool finished() const { return finished_; }
    uint32_t id() const { return id_; }
    std::string device_name() const;
    SessionStatus status() const;
    void set_pressure_curve(const PressureCurve& c) { input_.set_pressure_curve(c); }
    // Ends the session from the PC side, telling the device why.
    void kick(const std::string& reason);

private:
    void receive_loop();
    void video_loop();
    bool handle_hello(const proto::Hello& h);
    void handle(const proto::RawMessage& m);
    bool log_input(const proto::RawMessage& m);
    bool setup_pipeline(class VideoPipeline& pipe);
    std::vector<proto::Codec> codec_order() const;
    proto::DisplayMode effective_mode(proto::DisplayMode requested) const;
    template <typename T>
    bool send(const T& msg) {
        return conn_->send(proto::encode(msg));
    }

    const uint32_t id_;
    std::unique_ptr<Connection> conn_;
    HostOptions opts_;
    VirtualDisplayManager& vdm_;
    ApprovalBroker& approvals_;
    InputInjector input_;

    mutable std::mutex mu_;
    proto::Hello hello_;
    proto::ClientSettings settings_;
    bool have_monitor_ = false;

    std::thread recv_thread_, video_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> finished_{false};
    std::atomic<bool> force_keyframe_{true};
    std::atomic<bool> reconfigure_{false};

    mutable std::mutex status_mu_;
    SessionStatus status_;
};

}  // namespace dm
