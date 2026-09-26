#include <gtest/gtest.h>

#include "dm/protocol.h"

using namespace dm;
using namespace dm::proto;

namespace {

// Encode `msg`, run it through a FrameParser, and decode it back.
template <typename T>
T roundtrip(const T& msg) {
    const auto bytes = encode(msg);
    FrameParser p;
    p.feed(bytes.data(), bytes.size());
    RawMessage raw;
    EXPECT_TRUE(p.next(raw));
    EXPECT_EQ(raw.header.type, T::kType);
    EXPECT_EQ(raw.header.length, bytes.size() - kHeaderSize);
    auto out = decode<T>(raw.payload);
    EXPECT_TRUE(out.has_value());
    return out.value_or(T{});
}

}  // namespace

TEST(Bytes, LittleEndianLayout) {
    ByteWriter w;
    w.u16(0x1234);
    w.u32(0xAABBCCDD);
    const auto b = w.take();
    ASSERT_EQ(b.size(), 6u);
    EXPECT_EQ(b[0], 0x34);
    EXPECT_EQ(b[1], 0x12);
    EXPECT_EQ(b[2], 0xDD);
    EXPECT_EQ(b[5], 0xAA);
}

TEST(Bytes, ReaderLatchesOnOverrun) {
    const uint8_t data[3] = {1, 2, 3};
    ByteReader r(data);
    EXPECT_EQ(r.u16(), 0x0201);
    EXPECT_EQ(r.u32(), 0u);  // only 1 byte left
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.u8(), 0u);  // stays failed
}

TEST(Bytes, StringLengthBeyondBufferFails) {
    ByteWriter w;
    w.u16(100);  // claims 100 bytes
    w.raw("abc", 3);
    const auto b = w.take();
    ByteReader r(b);
    EXPECT_TRUE(r.str().empty());
    EXPECT_FALSE(r.ok());
}

TEST(Protocol, HelloRoundtrip) {
    Hello h;
    h.device_id = "5f0c-uuid";
    h.device_name = "Galaxy Tab S7+";
    h.model = "SM-T970";
    h.sdk_int = 33;
    h.geometry = {2800, 1752, 266, 120000, 1, Posture::Flat};
    h.decode_codecs = codec_bit(Codec::H264) | codec_bit(Codec::HEVC);
    h.input_caps = kCapTouch | kCapPen | kCapPenTilt | kCapPenHover;
    h.max_bitrate_kbps = 150000;
    h.transport = Transport::UsbAdb;
    h.settings.mode = DisplayMode::Mirror;
    h.settings.touch_mode = TouchMode::Trackpad;
    h.settings.max_fps = 90;
    h.settings.preferred_codec = Codec::AV1;

    const auto o = roundtrip(h);
    EXPECT_EQ(o.settings, h.settings);
    EXPECT_EQ(o.device_id, h.device_id);
    EXPECT_EQ(o.device_name, h.device_name);
    EXPECT_EQ(o.model, h.model);
    EXPECT_EQ(o.sdk_int, 33);
    EXPECT_EQ(o.geometry.width_px, 2800u);
    EXPECT_EQ(o.geometry.height_px, 1752u);
    EXPECT_EQ(o.geometry.refresh_mhz, 120000u);
    EXPECT_EQ(o.geometry.posture, Posture::Flat);
    EXPECT_EQ(o.decode_codecs, h.decode_codecs);
    EXPECT_EQ(o.input_caps, h.input_caps);
    EXPECT_EQ(o.transport, Transport::UsbAdb);
}

TEST(Protocol, HelloRejectsBadMagic) {
    Hello h;
    h.magic = 0xDEADBEEF;
    const auto bytes = encode(h);
    auto out = decode<Hello>(std::span<const uint8_t>(bytes).subspan(kHeaderSize));
    EXPECT_FALSE(out.has_value());
}

TEST(Protocol, VideoFrameRoundtrip) {
    VideoFrame f;
    f.frame_id = 123456789012ull;
    f.capture_time_us = 42;
    f.flags = kFrameKey;
    f.data = {0, 0, 0, 1, 0x40, 0x01, 0xFF};
    const auto o = roundtrip(f);
    EXPECT_EQ(o.frame_id, f.frame_id);
    EXPECT_EQ(o.capture_time_us, 42u);
    EXPECT_EQ(o.flags, kFrameKey);
    EXPECT_EQ(o.data, f.data);
}

TEST(Protocol, TouchRoundtrip) {
    Touch t;
    t.action = PointerAction::Down;
    t.action_id = 7;
    t.event_time_us = 99;
    t.points = {{7, 0.25f, 0.75f, 0.5f, 0.01f}, {9, 1.0f, 0.0f, 1.0f, 0.02f}};
    const auto o = roundtrip(t);
    EXPECT_EQ(o.action, PointerAction::Down);
    EXPECT_EQ(o.action_id, 7u);
    ASSERT_EQ(o.points.size(), 2u);
    EXPECT_FLOAT_EQ(o.points[0].x, 0.25f);
    EXPECT_FLOAT_EQ(o.points[1].major, 0.02f);
}

TEST(Protocol, TouchRejectsTooManyPoints) {
    ByteWriter w;
    w.u8(0);
    w.u32(0);
    w.u64(0);
    w.u8(200);  // absurd count
    const auto b = w.take();
    EXPECT_FALSE(decode<Touch>(b).has_value());
}

TEST(Protocol, PenRoundtrip) {
    Pen p;
    p.flags = kPenInRange | kPenContact | kPenBarrel;
    p.x = 0.5f;
    p.y = 0.125f;
    p.pressure = 0.66f;
    p.tilt_x = -30;
    p.tilt_y = 45;
    const auto o = roundtrip(p);
    EXPECT_EQ(o.flags, p.flags);
    EXPECT_FLOAT_EQ(o.pressure, 0.66f);
    EXPECT_FLOAT_EQ(o.tilt_x, -30.0f);
    EXPECT_FLOAT_EQ(o.tilt_y, 45.0f);
}

TEST(Protocol, VideoConfigAndKeyRoundtrip) {
    VideoConfig c;
    c.codec = Codec::AV1;
    c.width = 2800;
    c.height = 1752;
    c.fps = 120;
    c.content_y = 0.1f;
    c.content_h = 0.8f;
    c.mode = DisplayMode::Mirror;
    const auto oc = roundtrip(c);
    EXPECT_EQ(oc.codec, Codec::AV1);
    EXPECT_EQ(oc.fps, 120u);
    EXPECT_FLOAT_EQ(oc.content_h, 0.8f);
    EXPECT_EQ(oc.mode, DisplayMode::Mirror);

    Key k;
    k.scancode = 0x1D;
    k.flags = kKeyDown | kKeyExtended;
    const auto ok = roundtrip(k);
    EXPECT_EQ(ok.scancode, 0x1D);
    EXPECT_EQ(ok.flags, k.flags);
}

TEST(Protocol, TrailingBytesAreIgnoredForForwardCompat) {
    Ping p;
    p.sender_time_us = 5;
    ByteWriter w;
    p.write(w);
    w.u32(0xFFFFFFFF);  // field from a future version
    const auto b = w.take();
    auto o = decode<Ping>(b);
    ASSERT_TRUE(o.has_value());
    EXPECT_EQ(o->sender_time_us, 5u);
}

TEST(FrameParser, HandlesByteAtATimeAndMultipleMessages) {
    std::vector<uint8_t> stream;
    for (uint64_t i = 0; i < 5; ++i) {
        Ping p;
        p.sender_time_us = i;
        const auto b = encode(p);
        stream.insert(stream.end(), b.begin(), b.end());
    }
    FrameParser parser;
    std::vector<uint64_t> got;
    RawMessage raw;
    for (uint8_t byte : stream) {
        parser.feed(&byte, 1);
        while (parser.next(raw)) {
            auto p = decode<Ping>(raw.payload);
            ASSERT_TRUE(p.has_value());
            got.push_back(p->sender_time_us);
        }
    }
    EXPECT_EQ(got, (std::vector<uint64_t>{0, 1, 2, 3, 4}));
    EXPECT_EQ(parser.buffered(), 0u);
}

TEST(FrameParser, LargeFrameSplitAcrossChunks) {
    VideoFrame f;
    f.data.resize(300000);
    for (size_t i = 0; i < f.data.size(); ++i) f.data[i] = static_cast<uint8_t>(i * 31);
    const auto b = encode(f);
    FrameParser parser;
    RawMessage raw;
    size_t off = 0;
    int messages = 0;
    while (off < b.size()) {
        const size_t n = std::min<size_t>(16384, b.size() - off);
        parser.feed(b.data() + off, n);
        off += n;
        while (parser.next(raw)) {
            ++messages;
            auto o = decode<VideoFrame>(raw.payload);
            ASSERT_TRUE(o.has_value());
            EXPECT_EQ(o->data, f.data);
        }
    }
    EXPECT_EQ(messages, 1);
}

TEST(FrameParser, OversizedLengthIsAnError) {
    const uint8_t hdr[8] = {0xFF, 0xFF, 0xFF, 0x7F, 1, 0, 0, 0};
    FrameParser parser;
    parser.feed(hdr, sizeof hdr);
    RawMessage raw;
    EXPECT_FALSE(parser.next(raw));
    EXPECT_TRUE(parser.error());
}

TEST(Protocol, HelloTrailingFieldsAreOptional) {
    Hello h;
    h.device_name = "Fold";
    h.flags = kHelloConfirmPairing;
    h.udp_port = 50123;
    auto frame = encode(h);
    auto got = decode<Hello>(std::span<const uint8_t>(frame).subspan(kHeaderSize));
    ASSERT_TRUE(got);
    EXPECT_EQ(got->flags, kHelloConfirmPairing);
    EXPECT_EQ(got->udp_port, 50123);

    // An app from before these fields ends right after the settings.
    const auto old = std::span<const uint8_t>(frame).subspan(kHeaderSize, frame.size() - kHeaderSize - 3);
    got = decode<Hello>(old);
    ASSERT_TRUE(got);
    EXPECT_EQ(got->flags, 0);
    EXPECT_EQ(got->udp_port, 0);
    EXPECT_EQ(got->device_name, "Fold");
}

TEST(Protocol, VideoTransportRoundtrip) {
    VideoTransport t;
    t.udp = true;
    for (size_t i = 0; i < t.key.size(); ++i) t.key[i] = static_cast<uint8_t>(i * 7);
    t.fec_percent = 35;
    auto frame = encode(t);
    auto got = decode<VideoTransport>(std::span<const uint8_t>(frame).subspan(kHeaderSize));
    ASSERT_TRUE(got);
    EXPECT_TRUE(got->udp);
    EXPECT_EQ(got->key, t.key);
    EXPECT_EQ(got->fec_percent, 35);
}

TEST(Protocol, ClientStatsUdpFieldsAreOptional) {
    ClientStats s{60, 1, 4000, 1000, 2, 17};
    auto frame = encode(s);
    auto got = decode<ClientStats>(std::span<const uint8_t>(frame).subspan(kHeaderSize));
    ASSERT_TRUE(got);
    EXPECT_EQ(got->udp_lost_frames, 2u);
    EXPECT_EQ(got->udp_recovered_shards, 17u);
    got = decode<ClientStats>(std::span<const uint8_t>(frame).subspan(kHeaderSize, 16));
    ASSERT_TRUE(got);
    EXPECT_EQ(got->frames_decoded, 60u);
    EXPECT_EQ(got->udp_lost_frames, 0u);
}
