#include "session/session.h"

#include <algorithm>
#include <tuple>
#include <utility>

#include "core/log.h"
#include "core/win.h"
#include "session/video_pipeline.h"
#include "transport/apk_server.h"

namespace dm {

Session::Session(uint32_t id, std::unique_ptr<Connection> conn, const HostOptions& opts, VirtualDisplayManager& vdm,
                 ApprovalBroker& approvals, const noise::KeyPair& identity)
    : id_(id),
      conn_(std::move(conn)),
      opts_(opts),
      usb_(conn_->loopback() && !opts.test_mode),
      vdm_(vdm),
      approvals_(approvals),
      identity_(identity) {
    input_.set_pressure_curve(opts.pen_curve);
}

Session::~Session() {
    stop();
}

std::string Session::device_name() const {
    std::lock_guard lock(mu_);
    return hello_.device_name;
}

void Session::kick(const std::string& reason) {
    send(proto::Bye{reason});
    conn_->close();
}

SessionStatus Session::status() const {
    std::lock_guard lock(status_mu_);
    return status_;
}

void Session::start() {
    running_ = true;
    recv_thread_ = std::thread([this] { receive_loop(); });
}

void Session::stop() {
    running_ = false;
    conn_->close();
    // The receive thread owns (starts and joins) the video thread.
    if (recv_thread_.joinable() && recv_thread_.get_id() != std::this_thread::get_id()) recv_thread_.join();
}

bool Session::send_frame(const std::vector<uint8_t>& frame) {
    std::lock_guard lock(send_mu_);
    return conn_->send(channel_ ? channel_->seal(frame) : frame);
}

bool Session::handshake_step(const proto::RawMessage& m) {
    if (m.header.type == proto::MsgType::Hello) {
        // An app from before encryption: tell it why instead of just closing.
        proto::Welcome w;
        w.reason = "Update the DisplayMaster app on this device to connect over Wi-Fi";
        send(w);
        DM_LOGW("Session %u: device app too old for encrypted Wi-Fi", id_);
        return false;
    }
    std::vector<uint8_t> payload;
    if (m.header.type != proto::MsgType::Handshake || !handshake_->read_message(m.payload, payload)) {
        DM_LOGW("Session %u: handshake failed", id_);
        return false;
    }
    if (!handshake_->complete()) {
        std::vector<uint8_t> reply;
        return handshake_->write_message({}, reply) && send_frame(noise::handshake_frame(reply));
    }
    device_key_ = noise::to_hex(handshake_->remote_static());
    pairing_code_ = noise::pairing_code(handshake_->handshake_hash());
    {
        std::lock_guard lock(send_mu_);
        channel_ = std::make_unique<noise::SecureChannel>(*handshake_);
    }
    handshake_.reset();
    DM_LOGI("Session %u: encrypted connection established", id_);
    return true;
}

void Session::receive_loop() {
    proto::FrameParser parser;
    proto::RawMessage outer, inner;
    std::vector<uint8_t> buf(64 * 1024);
    bool greeted = false;
    bool first = true;
    // Wi-Fi traffic is encrypted and authenticated. USB (adb, loopback) stays plain unless
    // the device starts with a handshake anyway.
    auto begin_handshake = [this] {
        handshake_ = std::make_unique<noise::HandshakeXX>(noise::HandshakeXX::Role::Responder, identity_,
                                                          noise::prologue());
    };
    if (!usb_) begin_handshake();
    while (running_) {
        const int n = conn_->recv(buf.data(), static_cast<int>(buf.size()));
        if (n <= 0) break;
        // A phone's browser fetching the Android app (QR code in the PC app), not a device.
        if (std::exchange(first, false) && looks_like_http(buf.data(), static_cast<size_t>(n))) {
            serve_http(*conn_, buf.data(), static_cast<size_t>(n));
            running_ = false;
            finished_ = true;
            return;
        }
        parser.feed(buf.data(), static_cast<size_t>(n));
        while (parser.next(outer)) {
            if (usb_ && !greeted && !handshake_ && !channel_ && outer.header.type == proto::MsgType::Handshake)
                begin_handshake();
            if (handshake_) {
                if (!handshake_step(outer)) {
                    running_ = false;
                    break;
                }
                continue;
            }
            if (channel_ && (outer.header.type != proto::MsgType::Encrypted || !channel_->open(outer.payload, inner))) {
                // Forged, corrupted or replayed: the stream can't be trusted any more.
                DM_LOGW("Session %u: message failed authentication", id_);
                running_ = false;
                break;
            }
            const proto::RawMessage& msg = channel_ ? inner : outer;
            if (!greeted) {
                auto hello = msg.header.type == proto::MsgType::Hello ? proto::decode<proto::Hello>(msg.payload)
                                                                      : std::nullopt;
                if (!hello || !handle_hello(*hello)) {
                    running_ = false;
                    break;
                }
                greeted = true;
                continue;
            }
            handle(msg);
        }
        if (parser.error()) {
            DM_LOGW("Session %u: corrupt stream", id_);
            break;
        }
    }
    running_ = false;
    if (video_thread_.joinable()) video_thread_.join();
    input_.release_all();
    if (have_monitor_) vdm_.release(id_);
    DM_LOGI("Session %u (%s) ended", id_, hello_.device_name.c_str());
    finished_ = true;
}

void Session::start_udp(uint16_t port) {
    const std::string& peer = conn_->peer();
    const std::string ip = peer.substr(0, peer.rfind(':'));
    proto::VideoTransport t;
    t.udp = true;
    t.fec_percent = kFecPercent;
    noise::random_bytes(t.key.data(), t.key.size());
    auto sender = std::make_unique<UdpSender>();
    if (!sender->open(ip, port, t.key)) {
        DM_LOGW("Session %u: UDP video to %s:%u unavailable - staying on TCP", id_, ip.c_str(), port);
        return;
    }
    udp_ = std::move(sender);
    send(t);  // sealed: carries the packet key
    udp_active_ = true;
    DM_LOGI("Session %u: video over UDP to %s:%u (%d%% parity)", id_, ip.c_str(), port, kFecPercent);
}

bool Session::handle_hello(const proto::Hello& h) {
    proto::Welcome w;
    wchar_t host[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD len = ARRAYSIZE(host);
    GetComputerNameW(host, &len);
    w.host_name = to_utf8(host);
    w.session_id = id_;

    if (h.version != proto::kVersion) {
        w.reason = "Protocol version mismatch - update the app on both devices";
    } else if (!h.geometry.width_px || !h.geometry.height_px) {
        w.reason = "Invalid display size";
    } else if (!usb_ && opts_.test_mode &&
               (!approvals_.is_trusted(device_key_) || (h.flags & proto::kHelloConfirmPairing))) {
        // Tests can't click Allow; the log line lets them check both sides computed the same code.
        send(proto::AwaitingApproval{});
        DM_LOGI("Session %u: test mode approved pairing code %s", id_, pairing_code_.c_str());
        approvals_.trust(device_key_, h.device_name);
    } else if (!usb_ &&
               (!approvals_.is_trusted(device_key_) || (h.flags & proto::kHelloConfirmPairing))) {
        // Unknown device on the network (or one that doesn't know this PC yet): the user
        // compares the pairing code on both screens and approves it in the DisplayMaster app.
        send(proto::AwaitingApproval{});
        const bool allowed =
            approvals_.request({id_, device_key_, h.device_name, h.model, conn_->peer(), pairing_code_}, 60000,
                               [this] { return running_ && !conn_->peer_closed(); });
        if (!allowed) w.reason = "Not approved on the PC";
    }
    w.accepted = w.reason.empty();
    send(w);
    if (!w.accepted) {
        DM_LOGW("Session %u rejected: %s", id_, w.reason.c_str());
        return false;
    }

    {
        std::lock_guard lock(mu_);
        hello_ = h;
        settings_ = h.settings;
        settings_.mode = effective_mode(settings_.mode);
    }
    DM_LOGI("Session %u: \"%s\" (%s, Android API %u) %ux%u@%.0fHz via %s", id_, h.device_name.c_str(), h.model.c_str(),
            h.sdk_int, h.geometry.width_px, h.geometry.height_px, h.geometry.refresh_mhz / 1000.0,
            usb_ ? "USB" : channel_ ? "Wi-Fi (encrypted)" : "Wi-Fi");
    {
        std::lock_guard lock(status_mu_);
        status_.id = id_;
        status_.device_name = h.device_name;
        status_.model = h.model;
        status_.peer = conn_->peer();
        status_.usb = usb_;
        status_.has_pen = h.input_caps & proto::kCapPen;
    }
    // Only over the encrypted channel: the transport message carries the UDP packet key.
    if (!usb_ && channel_ && h.udp_port) start_udp(h.udp_port);
    {
        std::lock_guard lock(status_mu_);
        status_.udp = udp_active_;
    }
    video_thread_ = std::thread([this] { video_loop(); });
    return true;
}

void Session::handle(const proto::RawMessage& m) {
    using proto::MsgType;
    if (!opts_.inject_input && log_input(m)) return;
    switch (m.header.type) {
        case MsgType::Pen:
            if (auto p = proto::decode<proto::Pen>(m.payload)) input_.pen(*p);
            break;
        case MsgType::Touch:
            if (auto t = proto::decode<proto::Touch>(m.payload)) input_.touch(*t);
            break;
        case MsgType::Mouse:
            if (auto x = proto::decode<proto::Mouse>(m.payload)) input_.mouse(*x);
            break;
        case MsgType::Key:
            if (auto k = proto::decode<proto::Key>(m.payload)) input_.key(*k);
            break;
        case MsgType::RequestKeyframe: force_keyframe_ = true; break;
        case MsgType::Ping:
            if (auto p = proto::decode<proto::Ping>(m.payload)) send(proto::Pong{p->sender_time_us, now_us()});
            break;
        case MsgType::DisplayGeometry:
            if (auto g = proto::decode<proto::DisplayGeometry>(m.payload)) {
                std::lock_guard lock(mu_);
                if (g->width_px && g->height_px &&
                    (g->width_px != hello_.geometry.width_px || g->height_px != hello_.geometry.height_px)) {
                    DM_LOGI("Session %u: display now %ux%u", id_, g->width_px, g->height_px);
                    hello_.geometry = *g;
                    reconfigure_ = true;
                }
            }
            break;
        case MsgType::ClientSettings:
            if (auto s = proto::decode<proto::ClientSettings>(m.payload)) {
                std::lock_guard lock(mu_);
                s->mode = effective_mode(s->mode);
                // Touch mode is interpreted on the device; only stream-affecting fields restart video.
                auto stream_key = [](const proto::ClientSettings& c) {
                    return std::tuple(c.mode, c.max_fps, c.bitrate_kbps, c.preferred_codec);
                };
                if (stream_key(*s) != stream_key(settings_)) reconfigure_ = true;
                settings_ = *s;
            }
            break;
        case MsgType::ClientStats:
            if (auto s = proto::decode<proto::ClientStats>(m.payload)) {
                DM_LOGD("Session %u: client decoded %u dropped %u avg decode %u us", id_, s->frames_decoded,
                        s->frames_dropped, s->avg_decode_us);
                std::lock_guard lock(status_mu_);
                status_.client_decode_ms = s->avg_decode_us / 1000.0;
                status_.client_dropped = s->frames_dropped;
                status_.udp_lost_frames = s->udp_lost_frames;
                status_.udp_recovered_shards = s->udp_recovered_shards;
                if (udp_active_) {
                    const int fec = fec_percent_;
                    int next = fec;
                    if (s->udp_lost_frames) {
                        next = std::min(kFecMax, fec + std::max(10, fec / 2));
                        clean_stats_ = 0;
                    } else if (s->udp_recovered_shards) {
                        clean_stats_ = 0;
                    } else if (++clean_stats_ >= 5 && fec > kFecPercent) {
                        next = std::max(kFecPercent, fec - 5);
                        clean_stats_ = 0;
                    }
                    if (next != fec) {
                        fec_percent_ = next;
                        DM_LOGI("Session %u: UDP lost %u frames, rebuilt %u shards - parity %d%% -> %d%%", id_,
                                s->udp_lost_frames, s->udp_recovered_shards, fec, next);
                    }
                }
            }
            break;
        case MsgType::UdpFallback:
            if (udp_active_.exchange(false)) {
                // UDP doesn't get through (router, firewall): back to this connection.
                DM_LOGW("Session %u: device receives no UDP video - using TCP", id_);
                send(proto::VideoTransport{});
                force_keyframe_ = true;
                std::lock_guard lock(status_mu_);
                status_.udp = false;
            }
            break;
        case MsgType::Bye: running_ = false; conn_->close(); break;
        default: break;
    }
}

// The host can force a mode; extend needs the virtual display driver.
proto::DisplayMode Session::effective_mode(proto::DisplayMode requested) const {
    const auto mode = opts_.force_mode.value_or(requested);
    return mode == proto::DisplayMode::Extend && !vdm_.available() ? proto::DisplayMode::Mirror : mode;
}

// --no-input: describe input messages instead of injecting them. Returns true if `m` was input.
bool Session::log_input(const proto::RawMessage& m) {
    using proto::MsgType;
    switch (m.header.type) {
        case MsgType::Pen:
            if (auto p = proto::decode<proto::Pen>(m.payload))
                DM_LOGI("input: pen flags=%02x (%.3f, %.3f) pressure %.2f tilt (%.0f, %.0f)", p->flags, p->x, p->y,
                        p->pressure, p->tilt_x, p->tilt_y);
            return true;
        case MsgType::Touch:
            if (auto t = proto::decode<proto::Touch>(m.payload))
                DM_LOGI("input: touch action=%u id=%u contacts=%zu first=(%.3f, %.3f)", static_cast<unsigned>(t->action),
                        t->action_id, t->points.size(), t->points.empty() ? 0.f : t->points[0].x,
                        t->points.empty() ? 0.f : t->points[0].y);
            return true;
        case MsgType::Mouse:
            if (auto x = proto::decode<proto::Mouse>(m.payload))
                DM_LOGI("input: mouse kind=%u (%.3f, %.3f) button=%u down=%d", static_cast<unsigned>(x->kind), x->x, x->y,
                        static_cast<unsigned>(x->button), x->down);
            return true;
        case MsgType::Key:
            if (auto k = proto::decode<proto::Key>(m.payload))
                DM_LOGI("input: key scancode=0x%02X flags=%u unicode=%u", k->scancode, k->flags, k->unicode);
            return true;
        default: return false;
    }
}

void Session::set_stream_options(const HostOptions& o) {
    std::lock_guard lock(mu_);
    opts_.codec = o.codec;
    opts_.bitrate_kbps = o.bitrate_kbps;
    opts_.max_fps = o.max_fps;
    opts_.resolution_scale = o.resolution_scale;
    reconfigure_ = true;
}

std::vector<proto::Codec> Session::codec_order() const {
    std::vector<proto::Codec> order;
    auto add = [&](proto::Codec c) {
        if ((hello_.decode_codecs & proto::codec_bit(c)) && std::find(order.begin(), order.end(), c) == order.end())
            order.push_back(c);
    };
    if (opts_.codec) add(*opts_.codec);
    add(settings_.preferred_codec);
    for (auto c : {proto::Codec::HEVC, proto::Codec::H264, proto::Codec::AV1}) add(c);
    return order;
}

bool Session::setup_pipeline(VideoPipeline& pipe) {
    proto::DisplayGeometry geo;
    proto::ClientSettings settings;
    std::vector<proto::Codec> codecs;
    uint32_t max_fps, bitrate_kbps;
    float scale;
    {
        std::lock_guard lock(mu_);  // the app can change the stream options meanwhile
        geo = hello_.geometry;
        settings = settings_;
        codecs = codec_order();
        max_fps = opts_.max_fps;
        bitrate_kbps = opts_.bitrate_kbps;
        scale = opts_.resolution_scale;
    }
    const uint32_t device_hz = std::max(30u, (geo.refresh_mhz + 500) / 1000);
    const uint32_t fps = std::min({device_hz, settings.max_fps ? settings.max_fps : 120u, max_fps});
    const Size panel = virtual_mode_for_panel(geo.width_px, geo.height_px, scale);

    PipelineParams p;
    p.codecs = codecs;
    p.fps = fps;
    p.bitrate_kbps = settings.bitrate_kbps ? settings.bitrate_kbps : bitrate_kbps;
    p.usb = usb_;
    p.backend = opts_.backend;

    std::optional<MonitorInfo> mon;
    if (settings.mode == proto::DisplayMode::Extend) {
        const DisplayModeSpec mode{panel.w, panel.h, fps};
        mon = have_monitor_ ? vdm_.reconfigure(id_, mode) : vdm_.acquire(id_, mode);
        if (mon) have_monitor_ = true;
    } else {
        if (have_monitor_) {
            vdm_.release(id_);
            have_monitor_ = false;
        }
        mon = find_primary_monitor();
        // Mirror / tablet: encode at the device's size and letterbox the desktop into it.
        p.video_w = panel.w;
        p.video_h = panel.h;
        p.letterbox = true;
    }
    if (!mon || mon->gdi_name.empty()) {
        DM_LOGE("Session %u: no monitor to capture", id_);
        return false;
    }
    p.gdi_name = mon->gdi_name;
    if (!pipe.init(p)) return false;

    input_.set_target(mon->rect, pipe.content());
    proto::VideoConfig vc;
    vc.codec = pipe.codec();
    vc.width = pipe.video_width();
    vc.height = pipe.video_height();
    vc.fps = pipe.fps();
    vc.bitrate_kbps = pipe.bitrate_kbps();
    if (udp_) udp_->set_pace_kbps(std::max(3 * vc.bitrate_kbps, 60000u));  // headroom for keyframes
    vc.content_x = pipe.content().x;
    vc.content_y = pipe.content().y;
    vc.content_w = pipe.content().w;
    vc.content_h = pipe.content().h;
    vc.mode = settings.mode;
    DM_LOGI("Session %u: streaming %s %ux%u@%u %u kbps via %s on %s", id_, codec_name(vc.codec), vc.width, vc.height,
            vc.fps, vc.bitrate_kbps, pipe.encoder_name(), pipe.adapter_name().c_str());
    {
        std::lock_guard lock(status_mu_);
        status_.streaming = true;
        status_.mode = vc.mode;
        status_.codec = vc.codec;
        status_.width = vc.width;
        status_.height = vc.height;
        status_.fps = vc.fps;
        status_.bitrate_kbps = vc.bitrate_kbps;
        status_.encoder = pipe.encoder_name();
        status_.adapter = pipe.adapter_name();
        status_.monitor = to_utf8(mon->gdi_name);
        status_.monitor_rect = mon->rect;
    }
    force_keyframe_ = true;
    return send(vc);
}

void Session::video_loop() {
    VideoPipeline pipe;
    proto::VideoFrame frame;
    EncodedPacket pkt;
    uint64_t frame_id = 0;
    bool ready = false;
    uint64_t min_interval_us = 0, last_frame_us = 0;
    uint32_t failures = 0;
    // Per-second host stats.
    uint64_t stats_start_us = now_us(), stats_bytes = 0, stats_work_us = 0;
    uint32_t stats_frames = 0;

    while (running_) {
        if (!ready || reconfigure_.exchange(false)) {
            ready = setup_pipeline(pipe);
            if (!ready) {
                if (++failures > 20) {
                    send(proto::Bye{"Could not start video on this PC - see host log"});
                    conn_->close();
                    break;
                }
                Sleep(500);  // secure desktop, mode switch in progress, ...
                continue;
            }
            failures = 0;
            min_interval_us = 1'000'000 / std::max(1u, pipe.fps());
        }

        // Frame pacing, start-to-start: don't exceed the negotiated fps. DDA
        // coalesces changes meanwhile, so we always encode the newest image.
        const uint64_t since = now_us() - last_frame_us;
        if (since < min_interval_us) precise_sleep_us(min_interval_us - since);

        const bool key = force_keyframe_.exchange(false);
        const uint64_t start = now_us();
        const auto step = pipe.step(100, key, pkt);
        switch (step) {
            case VideoPipeline::Step::Frame:
                last_frame_us = start;
                frame.frame_id = frame_id++;
                frame.capture_time_us = start;
                frame.flags = pkt.keyframe ? proto::kFrameKey : 0;
                frame.data = std::move(pkt.data);
                stats_bytes += frame.data.size();
                if (udp_active_)
                    udp_->send_frame(frame, fec_percent_);
                else if (!send(frame))
                    running_ = false;
                stats_work_us += pipe.last_work_us();
                ++stats_frames;
                break;
            case VideoPipeline::Step::Idle:
                if (key) force_keyframe_ = true;  // keep the request for the next frame
                break;
            case VideoPipeline::Step::Lost:
            case VideoPipeline::Step::Error:
                if (key) force_keyframe_ = true;
                ready = false;
                break;
        }

        const uint64_t elapsed = now_us() - stats_start_us;
        if (elapsed >= 1'000'000) {
            const double fps = stats_frames * 1e6 / elapsed, mbps = stats_bytes * 8.0 / elapsed;
            const double work_ms = stats_frames ? stats_work_us / 1000.0 / stats_frames : 0.0;
            DM_LOGD("Session %u: sent %.1f fps, %.1f Mbps, convert+encode avg %.2f ms", id_, fps, mbps, work_ms);
            {
                std::lock_guard lock(status_mu_);
                status_.sent_fps = fps;
                status_.mbps = mbps;
                status_.work_ms = work_ms;
            }
            stats_start_us = now_us();
            stats_frames = 0;
            stats_bytes = stats_work_us = 0;
        }
    }
}

}  // namespace dm
