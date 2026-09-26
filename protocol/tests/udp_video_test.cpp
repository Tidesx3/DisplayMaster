#include <gtest/gtest.h>

#include <algorithm>
#include <random>

#include "dm/udp_video.h"

using namespace dm;
using namespace dm::udp;

namespace {

proto::VideoFrame make_frame(uint64_t id, size_t size, uint32_t seed) {
    proto::VideoFrame f;
    f.frame_id = id;
    f.capture_time_us = 1000 + id;
    f.flags = id == 0 ? proto::kFrameKey : 0;
    f.data.resize(size);
    std::mt19937 rng(seed);
    for (auto& b : f.data) b = static_cast<uint8_t>(rng());
    return f;
}

// Shards of one block, by header.
ShardHeader header_of(const std::vector<uint8_t>& p) {
    ByteReader r(p);
    ShardHeader h;
    h.read(r);
    return h;
}

}  // namespace

TEST(Fec, RebuildsAnyKOfKPlusM) {
    const int k = 6, m = 3;
    const size_t n = 64;
    std::mt19937 rng(1);
    std::vector<std::vector<uint8_t>> shards(k + m, std::vector<uint8_t>(n));
    for (int i = 0; i < k; ++i)
        for (auto& b : shards[i]) b = static_cast<uint8_t>(rng());
    std::vector<const uint8_t*> data;
    std::vector<uint8_t*> parity;
    for (int i = 0; i < k; ++i) data.push_back(shards[i].data());
    for (int j = 0; j < m; ++j) parity.push_back(shards[k + j].data());
    fec::encode(k, m, n, data.data(), parity.data());

    // Every way of losing up to m shards (2^9 patterns) must be recoverable.
    for (int mask = 0; mask < (1 << (k + m)); ++mask) {
        int lost = 0;
        for (int b = mask; b; b &= b - 1) ++lost;
        auto copy = shards;
        bool present[9];
        std::vector<uint8_t*> ptrs;
        for (int i = 0; i < k + m; ++i) {
            present[i] = !(mask & (1 << i));
            if (!present[i]) std::fill(copy[i].begin(), copy[i].end(), 0xEE);
            ptrs.push_back(copy[i].data());
        }
        const bool ok = fec::reconstruct(k, m, n, ptrs.data(), present);
        if (lost > m) {
            EXPECT_FALSE(ok) << mask;
            continue;
        }
        ASSERT_TRUE(ok) << mask;
        for (int i = 0; i < k; ++i) ASSERT_EQ(copy[i], shards[i]) << "mask " << mask << " shard " << i;
    }
}

TEST(UdpVideo, RoundTripsFramesOutOfOrder) {
    FrameAssembler a;
    std::vector<proto::VideoFrame> frames;
    std::vector<std::vector<uint8_t>> packets;
    for (uint64_t id = 0; id < 5; ++id) {
        frames.push_back(make_frame(id, id == 0 ? 90000 : 1000 + 7000 * id, static_cast<uint32_t>(id)));
        for (auto& p : packetize(frames.back(), 20)) packets.push_back(std::move(p));
    }
    std::shuffle(packets.begin(), packets.end(), std::mt19937(7));
    for (auto& p : packets) a.add(p, 1);
    proto::VideoFrame out;
    for (const auto& want : frames) {
        ASSERT_TRUE(a.pop(out, 2));
        EXPECT_EQ(out.frame_id, want.frame_id);
        EXPECT_EQ(out.flags, want.flags);
        EXPECT_EQ(out.capture_time_us, want.capture_time_us);
        EXPECT_EQ(out.data, want.data);
    }
    EXPECT_FALSE(a.pop(out, 3));
    EXPECT_EQ(a.take_lost(), 0u);
}

TEST(UdpVideo, ParityRepairsLossesWithinBudget) {
    const auto frame = make_frame(0, 100000, 3);  // 91 shards -> 3 blocks
    auto packets = packetize(frame, 20);
    // Drop m shards from every block: still complete.
    std::vector<std::vector<uint8_t>> kept;
    std::map<uint16_t, int> dropped;
    for (auto& p : packets) {
        const auto h = header_of(p);
        if (dropped[h.block] < h.m) {
            ++dropped[h.block];
            continue;
        }
        kept.push_back(p);
    }
    FrameAssembler a;
    for (auto& p : kept) a.add(p, 1);
    proto::VideoFrame out;
    ASSERT_TRUE(a.pop(out, 2));
    EXPECT_EQ(out.data, frame.data);
    EXPECT_GT(a.take_recovered(), 0u);
}

TEST(UdpVideo, UnrepairableFrameIsSkipped) {
    FrameAssembler a;
    const auto f0 = make_frame(0, 5000, 1), f1 = make_frame(1, 5000, 2), f2 = make_frame(2, 5000, 3);
    for (auto& p : packetize(f0, 20)) a.add(p, 1);
    auto p1 = packetize(f1, 20);
    for (size_t i = 0; i + 3 < p1.size(); ++i) a.add(p1[i], 1);  // 3 of 6 lost, only 1 parity
    for (auto& p : packetize(f2, 20)) a.add(p, 1000);

    proto::VideoFrame out;
    ASSERT_TRUE(a.pop(out, 1000));
    EXPECT_EQ(out.frame_id, 0u);
    // Frame 2 just completed: wait a little for frame 1's stragglers first.
    EXPECT_FALSE(a.pop(out, 1000 + FrameAssembler::kReorderWaitUs / 2));
    ASSERT_TRUE(a.pop(out, 1000 + FrameAssembler::kReorderWaitUs));
    EXPECT_EQ(out.frame_id, 2u);
    EXPECT_EQ(out.data, f2.data);
    EXPECT_EQ(a.take_lost(), 1u);
    for (auto& p : p1) a.add(p, 2000);  // late packets for the skipped frame are ignored
    EXPECT_FALSE(a.pop(out, 3000));
}

TEST(UdpVideo, EmptyAndTinyFrames) {
    FrameAssembler a;
    const auto empty = make_frame(0, 0, 1), tiny = make_frame(1, 10, 2);
    for (auto& p : packetize(empty, 20)) a.add(p, 1);
    for (auto& p : packetize(tiny, 20)) a.add(p, 1);
    proto::VideoFrame out;
    ASSERT_TRUE(a.pop(out, 2));
    EXPECT_TRUE(out.data.empty());
    ASSERT_TRUE(a.pop(out, 2));
    EXPECT_EQ(out.data, tiny.data);
}

TEST(UdpVideo, CipherRejectsTampering) {
    Key key{};
    key[0] = 42;
    Cipher c(key);
    const std::vector<uint8_t> plain = {1, 2, 3, 4, 5};
    auto sealed = c.seal(77, plain);
    std::vector<uint8_t> out;
    uint64_t seq = 0;
    ASSERT_TRUE(c.open(sealed, out, seq));
    EXPECT_EQ(out, plain);
    EXPECT_EQ(seq, 77u);
    sealed[9] ^= 1;
    EXPECT_FALSE(c.open(sealed, out, seq));
    sealed[9] ^= 1;
    sealed[0] ^= 1;  // the sequence number is authenticated too
    EXPECT_FALSE(c.open(sealed, out, seq));
    Key other{};
    EXPECT_FALSE(Cipher(other).open(c.seal(1, plain), out, seq));
}
