#include "dm/protocol.h"

#include <algorithm>

namespace dm::proto {

// A single touch frame can't reasonably exceed this; guards allocations on bad input.
constexpr uint8_t kMaxTouchPoints = 20;

void DisplayGeometry::write(ByteWriter& w) const {
    w.u32(width_px);
    w.u32(height_px);
    w.u16(dpi);
    w.u32(refresh_mhz);
    w.u8(rotation);
    w.u8(static_cast<uint8_t>(posture));
}
bool DisplayGeometry::read(ByteReader& r) {
    width_px = r.u32();
    height_px = r.u32();
    dpi = r.u16();
    refresh_mhz = r.u32();
    rotation = r.u8();
    posture = static_cast<Posture>(r.u8());
    return r.ok();
}

void Hello::write(ByteWriter& w) const {
    w.u32(magic);
    w.u16(version);
    w.str(device_id);
    w.str(device_name);
    w.str(model);
    w.u16(sdk_int);
    geometry.write(w);
    w.u32(decode_codecs);
    w.u32(input_caps);
    w.u32(max_bitrate_kbps);
    w.u8(static_cast<uint8_t>(transport));
    settings.write(w);
    w.u8(flags);
    w.u16(udp_port);
}
bool Hello::read(ByteReader& r) {
    magic = r.u32();
    version = r.u16();
    device_id = r.str();
    device_name = r.str();
    model = r.str();
    sdk_int = r.u16();
    geometry.read(r);
    decode_codecs = r.u32();
    input_caps = r.u32();
    max_bitrate_kbps = r.u32();
    transport = static_cast<Transport>(r.u8());
    settings.read(r);
    flags = r.remaining() ? r.u8() : 0;
    udp_port = r.remaining() >= 2 ? r.u16() : 0;
    return r.ok() && magic == kMagic;
}

void Welcome::write(ByteWriter& w) const {
    w.u16(version);
    w.boolean(accepted);
    w.str(reason);
    w.str(host_name);
    w.u32(session_id);
}
bool Welcome::read(ByteReader& r) {
    version = r.u16();
    accepted = r.boolean();
    reason = r.str();
    host_name = r.str();
    session_id = r.u32();
    return r.ok();
}

void Bye::write(ByteWriter& w) const { w.str(reason); }
bool Bye::read(ByteReader& r) {
    reason = r.str();
    return r.ok();
}

void Ping::write(ByteWriter& w) const { w.u64(sender_time_us); }
bool Ping::read(ByteReader& r) {
    sender_time_us = r.u64();
    return r.ok();
}

void Pong::write(ByteWriter& w) const {
    w.u64(echo_time_us);
    w.u64(responder_time_us);
}
bool Pong::read(ByteReader& r) {
    echo_time_us = r.u64();
    responder_time_us = r.u64();
    return r.ok();
}

void VideoConfig::write(ByteWriter& w) const {
    w.u8(static_cast<uint8_t>(codec));
    w.u32(width);
    w.u32(height);
    w.u32(fps);
    w.u32(bitrate_kbps);
    w.f32(content_x);
    w.f32(content_y);
    w.f32(content_w);
    w.f32(content_h);
    w.u8(static_cast<uint8_t>(mode));
}
bool VideoConfig::read(ByteReader& r) {
    codec = static_cast<Codec>(r.u8());
    width = r.u32();
    height = r.u32();
    fps = r.u32();
    bitrate_kbps = r.u32();
    content_x = r.f32();
    content_y = r.f32();
    content_w = r.f32();
    content_h = r.f32();
    mode = static_cast<DisplayMode>(r.u8());
    return r.ok();
}

void VideoFrame::write(ByteWriter& w) const {
    w.u64(frame_id);
    w.u64(capture_time_us);
    w.u8(flags);
    w.blob(data);
}
bool VideoFrame::read(ByteReader& r) {
    frame_id = r.u64();
    capture_time_us = r.u64();
    flags = r.u8();
    data = r.blob();
    return r.ok();
}

void ClientSettings::write(ByteWriter& w) const {
    w.u8(static_cast<uint8_t>(mode));
    w.u8(static_cast<uint8_t>(touch_mode));
    w.u32(max_fps);
    w.u32(bitrate_kbps);
    w.u8(static_cast<uint8_t>(preferred_codec));
}
bool ClientSettings::read(ByteReader& r) {
    mode = static_cast<DisplayMode>(r.u8());
    touch_mode = static_cast<TouchMode>(r.u8());
    max_fps = r.u32();
    bitrate_kbps = r.u32();
    preferred_codec = static_cast<Codec>(r.u8());
    return r.ok();
}

void Touch::write(ByteWriter& w) const {
    w.u8(static_cast<uint8_t>(action));
    w.u32(action_id);
    w.u64(event_time_us);
    const auto n = static_cast<uint8_t>(std::min<size_t>(points.size(), kMaxTouchPoints));
    w.u8(n);
    for (uint8_t i = 0; i < n; ++i) {
        const auto& p = points[i];
        w.u32(p.id);
        w.f32(p.x);
        w.f32(p.y);
        w.f32(p.pressure);
        w.f32(p.major);
    }
}
bool Touch::read(ByteReader& r) {
    action = static_cast<PointerAction>(r.u8());
    action_id = r.u32();
    event_time_us = r.u64();
    const uint8_t n = r.u8();
    if (n > kMaxTouchPoints) return false;
    points.resize(n);
    for (auto& p : points) {
        p.id = r.u32();
        p.x = r.f32();
        p.y = r.f32();
        p.pressure = r.f32();
        p.major = r.f32();
    }
    return r.ok();
}

void Pen::write(ByteWriter& w) const {
    w.u8(flags);
    w.f32(x);
    w.f32(y);
    w.f32(pressure);
    w.f32(tilt_x);
    w.f32(tilt_y);
    w.f32(rotation);
    w.u64(event_time_us);
}
bool Pen::read(ByteReader& r) {
    flags = r.u8();
    x = r.f32();
    y = r.f32();
    pressure = r.f32();
    tilt_x = r.f32();
    tilt_y = r.f32();
    rotation = r.f32();
    event_time_us = r.u64();
    return r.ok();
}

void Mouse::write(ByteWriter& w) const {
    w.u8(static_cast<uint8_t>(kind));
    w.f32(x);
    w.f32(y);
    w.u8(static_cast<uint8_t>(button));
    w.boolean(down);
}
bool Mouse::read(ByteReader& r) {
    kind = static_cast<MouseKind>(r.u8());
    x = r.f32();
    y = r.f32();
    button = static_cast<MouseButton>(r.u8());
    down = r.boolean();
    return r.ok();
}

void Key::write(ByteWriter& w) const {
    w.u16(scancode);
    w.u8(flags);
    w.u32(unicode);
}
bool Key::read(ByteReader& r) {
    scancode = r.u16();
    flags = r.u8();
    unicode = r.u32();
    return r.ok();
}

void ClientStats::write(ByteWriter& w) const {
    w.u32(frames_decoded);
    w.u32(frames_dropped);
    w.u32(avg_decode_us);
    w.u32(interval_ms);
    w.u32(udp_lost_frames);
    w.u32(udp_recovered_shards);
}
bool ClientStats::read(ByteReader& r) {
    frames_decoded = r.u32();
    frames_dropped = r.u32();
    avg_decode_us = r.u32();
    interval_ms = r.u32();
    if (r.remaining() >= 8) {
        udp_lost_frames = r.u32();
        udp_recovered_shards = r.u32();
    }
    return r.ok();
}

void VideoTransport::write(ByteWriter& w) const {
    w.boolean(udp);
    w.raw(key.data(), key.size());
    w.u8(fec_percent);
}
bool VideoTransport::read(ByteReader& r) {
    udp = r.boolean();
    for (auto& b : key) b = r.u8();
    fec_percent = r.u8();
    return r.ok();
}

// ---------------------------------------------------------------- FrameParser

void FrameParser::feed(const uint8_t* data, size_t n) {
    // Compact once the consumed prefix dominates, keeping appends amortized O(1).
    if (start_ > 0 && start_ >= buf_.size() / 2) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<ptrdiff_t>(start_));
        start_ = 0;
    }
    buf_.insert(buf_.end(), data, data + n);
}

bool FrameParser::next(RawMessage& out) {
    if (error_ || buffered() < kHeaderSize) return false;
    ByteReader hr(std::span<const uint8_t>(buf_.data() + start_, kHeaderSize));
    const uint32_t len = hr.u32();
    const auto type = static_cast<MsgType>(hr.u8());
    const uint8_t flags = hr.u8();
    if (len > kMaxPayload) {
        error_ = true;
        return false;
    }
    if (buffered() < kHeaderSize + len) return false;
    out.header = Header{len, type, flags};
    const auto* p = buf_.data() + start_ + kHeaderSize;
    out.payload.assign(p, p + len);
    start_ += kHeaderSize + len;
    if (start_ == buf_.size()) {
        buf_.clear();
        start_ = 0;
    }
    return true;
}

const char* to_string(MsgType t) {
    switch (t) {
        case MsgType::Hello: return "Hello";
        case MsgType::Welcome: return "Welcome";
        case MsgType::Bye: return "Bye";
        case MsgType::Ping: return "Ping";
        case MsgType::Pong: return "Pong";
        case MsgType::AwaitingApproval: return "AwaitingApproval";
        case MsgType::DisplayGeometry: return "DisplayGeometry";
        case MsgType::VideoConfig: return "VideoConfig";
        case MsgType::VideoFrame: return "VideoFrame";
        case MsgType::RequestKeyframe: return "RequestKeyframe";
        case MsgType::ClientSettings: return "ClientSettings";
        case MsgType::Touch: return "Touch";
        case MsgType::Pen: return "Pen";
        case MsgType::Mouse: return "Mouse";
        case MsgType::Key: return "Key";
        case MsgType::ClientStats: return "ClientStats";
        case MsgType::Handshake: return "Handshake";
        case MsgType::Encrypted: return "Encrypted";
        case MsgType::VideoTransport: return "VideoTransport";
        case MsgType::UdpFallback: return "UdpFallback";
        case MsgType::InvalidateFrames: return "InvalidateFrames";
    }
    return "Unknown";
}

}  // namespace dm::proto
