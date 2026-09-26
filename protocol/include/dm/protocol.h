// DisplayMaster wire protocol (shared by the Windows host and the Android client).
//
// Every message on a stream transport (TCP / ADB reverse / AOA bulk) is framed as:
//   u32 payload_length | u8 type | u8 flags | u16 reserved | payload...
// All integers little-endian. Coordinates are normalized [0,1] relative to the
// video surface the client displays, so the host owns all pixel mapping.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "dm/bytes.h"

namespace dm::proto {

constexpr uint32_t kMagic = 0x31504D44;  // "DMP1"
constexpr uint16_t kVersion = 1;
constexpr uint16_t kDefaultPort = 47800;
constexpr size_t kHeaderSize = 8;
constexpr uint32_t kMaxPayload = 32u * 1024u * 1024u;

enum class MsgType : uint8_t {
    Hello = 1,       // client -> host, first message
    Welcome = 2,     // host -> client, reply to Hello
    Bye = 3,         // either direction
    Ping = 4,        // either direction
    Pong = 5,
    AwaitingApproval = 6,  // host -> client: unknown Wi-Fi device, waiting for the user on the PC

    DisplayGeometry = 10,  // client -> host: screen size/rotation/posture changed
    VideoConfig = 11,      // host -> client: (re)configure decoder
    VideoFrame = 12,       // host -> client: one encoded access unit (Annex B / OBU)
    RequestKeyframe = 13,  // client -> host: decoder lost sync
    ClientSettings = 14,   // client -> host: user changed mode/quality on the device
    VideoTransport = 15,   // host -> client: video moves to UDP (Wi-Fi) or back to this connection
    UdpFallback = 16,      // client -> host: no UDP video arrives here, keep it on this connection
    InvalidateFrames = 17, // client -> host: these frames were lost; repair without a keyframe if possible

    Touch = 20,  // client -> host
    Pen = 21,
    Mouse = 22,
    Key = 23,

    ClientStats = 30,  // client -> host, periodic

    // Wi-Fi only (see dm/noise.h): three Noise handshake messages, then every frame
    // travels sealed inside an Encrypted frame. USB (adb) stays plain.
    Handshake = 40,
    Encrypted = 41,
};

enum class Codec : uint8_t { H264 = 1, HEVC = 2, AV1 = 3 };
constexpr uint32_t codec_bit(Codec c) { return 1u << static_cast<uint8_t>(c); }

enum class Transport : uint8_t { Unknown = 0, UsbAdb = 1, UsbAoa = 2, WiFi = 3 };

enum class DisplayMode : uint8_t { Extend = 0, Mirror = 1, Tablet = 2 };

enum class TouchMode : uint8_t { Touch = 0, Mouse = 1, Trackpad = 2 };

enum class Posture : uint8_t { Unknown = 0, Flat = 1, HalfOpened = 2, Folded = 3 };

// Input capability bits advertised in Hello.
enum InputCaps : uint32_t {
    kCapTouch = 1u << 0,
    kCapPen = 1u << 1,
    kCapPenTilt = 1u << 2,
    kCapPenHover = 1u << 3,
    kCapPenEraser = 1u << 4,
    kCapKeyboard = 1u << 5,
};

struct Header {
    uint32_t length = 0;
    MsgType type{};
    uint8_t flags = 0;
};

// ---------------------------------------------------------------- messages

struct DisplayGeometry {
    static constexpr MsgType kType = MsgType::DisplayGeometry;
    uint32_t width_px = 0;       // physical pixels of the area the video fills
    uint32_t height_px = 0;
    uint16_t dpi = 160;
    uint32_t refresh_mhz = 60000;  // 120 Hz == 120000
    uint8_t rotation = 0;          // 0..3 quarter turns (informational)
    Posture posture = Posture::Unknown;

    void write(ByteWriter& w) const;
    bool read(ByteReader& r);
};

// User preferences from the device. Sent inside Hello, then again whenever they change.
struct ClientSettings {
    static constexpr MsgType kType = MsgType::ClientSettings;
    DisplayMode mode = DisplayMode::Extend;
    TouchMode touch_mode = TouchMode::Touch;
    uint32_t max_fps = 120;
    uint32_t bitrate_kbps = 0;  // 0 == automatic
    Codec preferred_codec = Codec::HEVC;

    bool operator==(const ClientSettings&) const = default;
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);
};

enum HelloFlags : uint8_t {
    // Wi-Fi: the device doesn't know this PC's key yet, so the PC must show the pairing
    // code for comparison even if it already trusts the device.
    kHelloConfirmPairing = 1u << 0,
    // The device handles recovery frames (kFrameRecovery) and sends InvalidateFrames on loss.
    kHelloRecoveryFrames = 1u << 1,
};

struct Hello {
    static constexpr MsgType kType = MsgType::Hello;
    uint32_t magic = kMagic;
    uint16_t version = kVersion;
    std::string device_id;    // stable per install (UUID)
    std::string device_name;  // user-visible, e.g. "Galaxy Tab S7+"
    std::string model;        // Build.MODEL
    uint16_t sdk_int = 0;
    DisplayGeometry geometry;
    uint32_t decode_codecs = 0;  // codec_bit() mask
    uint32_t input_caps = 0;     // InputCaps mask
    uint32_t max_bitrate_kbps = 0;
    Transport transport = Transport::Unknown;
    ClientSettings settings;  // initial preferences, so the first stream is already right
    uint8_t flags = 0;        // HelloFlags; optional trailing field (older apps omit it)
    uint16_t udp_port = 0;    // Wi-Fi: the device's UDP port for video (0: none); trailing too

    void write(ByteWriter& w) const;
    bool read(ByteReader& r);
};

struct Welcome {
    static constexpr MsgType kType = MsgType::Welcome;
    uint16_t version = kVersion;
    bool accepted = false;
    std::string reason;  // set when !accepted
    std::string host_name;
    uint32_t session_id = 0;

    void write(ByteWriter& w) const;
    bool read(ByteReader& r);
};

struct Bye {
    static constexpr MsgType kType = MsgType::Bye;
    std::string reason;
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);
};

struct AwaitingApproval {
    static constexpr MsgType kType = MsgType::AwaitingApproval;
    void write(ByteWriter&) const {}
    bool read(ByteReader&) { return true; }
};

struct Ping {
    static constexpr MsgType kType = MsgType::Ping;
    uint64_t sender_time_us = 0;
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);
};

struct Pong {
    static constexpr MsgType kType = MsgType::Pong;
    uint64_t echo_time_us = 0;      // Ping.sender_time_us
    uint64_t responder_time_us = 0;
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);
};

struct VideoConfig {
    static constexpr MsgType kType = MsgType::VideoConfig;
    Codec codec = Codec::HEVC;
    uint32_t width = 0;  // coded video size
    uint32_t height = 0;
    uint32_t fps = 60;
    uint32_t bitrate_kbps = 0;
    // Area of the video that holds desktop content (letterboxing in mirror mode),
    // normalized to the video size. Input outside it is ignored by the host.
    float content_x = 0, content_y = 0, content_w = 1, content_h = 1;
    DisplayMode mode = DisplayMode::Extend;

    void write(ByteWriter& w) const;
    bool read(ByteReader& r);
};

enum VideoFrameFlags : uint8_t {
    kFrameKey = 1u << 0,
    // First frame encoded after InvalidateFrames: it refers only to frames from before the
    // loss, so a decoder that skipped the frames in between can continue from here.
    kFrameRecovery = 1u << 1,
};

struct VideoFrame {
    static constexpr MsgType kType = MsgType::VideoFrame;
    uint64_t frame_id = 0;
    uint64_t capture_time_us = 0;  // host clock, for latency stats
    uint8_t flags = 0;
    std::vector<uint8_t> data;

    void write(ByteWriter& w) const;
    bool read(ByteReader& r);
};

struct RequestKeyframe {
    static constexpr MsgType kType = MsgType::RequestKeyframe;
    void write(ByteWriter&) const {}
    bool read(ByteReader&) { return true; }
};

enum class PointerAction : uint8_t { Down = 0, Move = 1, Up = 2, Cancel = 3 };

struct TouchPoint {
    uint32_t id = 0;
    float x = 0, y = 0;  // normalized to video surface
    float pressure = 1;  // 0..1
    float major = 0;     // contact size, normalized to video width
};

// A touch frame carries all active contacts (Windows injection wants the full set).
// action/action_id describe which contact changed.
struct Touch {
    static constexpr MsgType kType = MsgType::Touch;
    PointerAction action = PointerAction::Move;
    uint32_t action_id = 0;
    uint64_t event_time_us = 0;  // client clock
    std::vector<TouchPoint> points;

    void write(ByteWriter& w) const;
    bool read(ByteReader& r);
};

enum PenFlags : uint8_t {
    kPenInRange = 1u << 0,   // hovering or touching
    kPenContact = 1u << 1,   // tip touching the screen
    kPenBarrel = 1u << 2,    // primary side button held
    kPenEraser = 1u << 3,    // eraser end / eraser tool
    kPenInverted = 1u << 4,  // pen flipped (eraser end in range)
    kPenBarrel2 = 1u << 5,   // secondary side button held
};

struct Pen {
    static constexpr MsgType kType = MsgType::Pen;
    uint8_t flags = 0;
    float x = 0, y = 0;     // normalized
    float pressure = 0;     // 0..1
    float tilt_x = 0;       // degrees -90..90, +X = pen top leans right
    float tilt_y = 0;       // degrees -90..90, +Y = pen top leans toward user (down)
    float rotation = 0;     // degrees 0..359 (barrel twist, rarely available)
    uint64_t event_time_us = 0;

    void write(ByteWriter& w) const;
    bool read(ByteReader& r);
};

enum class MouseKind : uint8_t { MoveAbs = 0, MoveRel = 1, Button = 2, Wheel = 3 };
enum class MouseButton : uint8_t { Left = 0, Right = 1, Middle = 2 };

struct Mouse {
    static constexpr MsgType kType = MsgType::Mouse;
    MouseKind kind = MouseKind::MoveAbs;
    float x = 0, y = 0;  // MoveAbs: normalized; MoveRel: device pixels; Wheel: notches (y+ = scroll up)
    MouseButton button = MouseButton::Left;
    bool down = false;

    void write(ByteWriter& w) const;
    bool read(ByteReader& r);
};

enum KeyFlags : uint8_t {
    kKeyDown = 1u << 0,
    kKeyExtended = 1u << 1,  // E0-prefixed scancode
    // Shortcut keys (tablet shortcut bar): `scancode` holds a Windows virtual-key code, or is
    // 0 and `unicode` names the key by the character it types in the PC's current layout.
    // Unlike scancodes this means "the Z key" on QWERTZ too, so Ctrl+Z stays undo.
    kKeyVirtual = 1u << 2,
};

// Keys travel as PC set-1 scancodes so the Windows keyboard layout decides the
// character (avoids AZERTY/QWERTZ shortcut mismatches). `unicode` is a fallback
// for characters with no scancode (IME / on-screen keyboard text).
struct Key {
    static constexpr MsgType kType = MsgType::Key;
    uint16_t scancode = 0;
    uint8_t flags = 0;
    uint32_t unicode = 0;

    void write(ByteWriter& w) const;
    bool read(ByteReader& r);
};

// Video over UDP (dm/udp_video.h). Sent inside the encrypted control connection, since it
// carries the packet key.
struct VideoTransport {
    static constexpr MsgType kType = MsgType::VideoTransport;
    bool udp = false;           // false: frames come on this connection again
    std::array<uint8_t, 32> key{};
    uint8_t fec_percent = 20;   // parity shards per data shard

    void write(ByteWriter& w) const;
    bool read(ByteReader& r);
};

// Frames the device lost for good (frame ids, low 32 bits, inclusive). The PC encodes the
// next frame from an older reference (kFrameRecovery) or, if it can't, sends a keyframe.
struct InvalidateFrames {
    static constexpr MsgType kType = MsgType::InvalidateFrames;
    uint32_t first = 0, last = 0;
    void write(ByteWriter& w) const {
        w.u32(first);
        w.u32(last);
    }
    bool read(ByteReader& r) {
        first = r.u32();
        last = r.u32();
        return r.ok();
    }
};

struct UdpFallback {
    static constexpr MsgType kType = MsgType::UdpFallback;
    void write(ByteWriter&) const {}
    bool read(ByteReader&) { return true; }
};

struct ClientStats {
    static constexpr MsgType kType = MsgType::ClientStats;
    uint32_t frames_decoded = 0;  // since last stats message
    uint32_t frames_dropped = 0;
    uint32_t avg_decode_us = 0;
    uint32_t interval_ms = 0;
    // UDP video (trailing, optional): frames lost for good, data shards rebuilt from parity.
    uint32_t udp_lost_frames = 0;
    uint32_t udp_recovered_shards = 0;

    void write(ByteWriter& w) const;
    bool read(ByteReader& r);
};

// ---------------------------------------------------------------- framing

// Serialize a full framed message (header + payload).
template <typename T>
std::vector<uint8_t> encode(const T& msg, uint8_t flags = 0) {
    std::vector<uint8_t> out;
    out.reserve(64);
    ByteWriter w(out);
    w.u32(0);  // length, patched below
    w.u8(static_cast<uint8_t>(T::kType));
    w.u8(flags);
    w.u16(0);
    msg.write(w);
    w.patch_u32(0, static_cast<uint32_t>(out.size() - kHeaderSize));
    return out;
}

// Decode a payload into a message struct. Returns nullopt if malformed.
// Trailing bytes are allowed so newer peers can append fields.
template <typename T>
std::optional<T> decode(std::span<const uint8_t> payload) {
    ByteReader r(payload);
    T msg;
    if (!msg.read(r) || !r.ok()) return std::nullopt;
    return msg;
}

struct RawMessage {
    Header header;
    std::vector<uint8_t> payload;
};

// Incremental stream de-framer. Feed arbitrary chunks, then pull whole messages.
class FrameParser {
public:
    void feed(const uint8_t* data, size_t n);
    // Returns true and fills `out` when a complete message is available.
    bool next(RawMessage& out);
    // Set when the stream is corrupt (oversized length). The connection must be dropped.
    bool error() const { return error_; }
    size_t buffered() const { return buf_.size() - start_; }

private:
    std::vector<uint8_t> buf_;
    size_t start_ = 0;
    bool error_ = false;
};

const char* to_string(MsgType t);

}  // namespace dm::proto
