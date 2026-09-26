// Video over UDP for Wi-Fi: a lost packet no longer stalls everything behind it (TCP
// head-of-line blocking). Each frame is cut into shards; every block of up to
// kMaxDataShards data shards gets Reed-Solomon parity shards, so any k of a block's
// k + m packets rebuild it. Frames that still can't be rebuilt are reported lost; the
// device then asks for a keyframe (over the TCP control connection, like everything
// else except video).
//
// Every datagram is sealed with XChaCha20-Poly1305 under a per-session key the PC sends
// inside the encrypted control channel (MsgType::VideoTransport):
//   u64 seq (little-endian, also the nonce) | ciphertext(shard header + shard) | tag(16)
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "dm/protocol.h"

namespace dm::udp {

constexpr size_t kShardSize = 1100;     // payload bytes per packet: fits a 1280-byte path MTU
constexpr int kMaxDataShards = 32;      // per Reed-Solomon block
constexpr size_t kSealOverhead = 8 + 16;  // seq + tag
using Key = std::array<uint8_t, 32>;

// ---------------------------------------------------------------- Reed-Solomon (GF(2^8))
// Systematic Cauchy code: shards 0..k-1 are the data, k..k+m-1 parity. k + m <= 256.
namespace fec {
void encode(int k, int m, size_t size, const uint8_t* const* data, uint8_t* const* parity);
// `shards` holds k + m buffers of `size` bytes; missing data shards (present[i] false,
// i < k) are rebuilt in place. False if fewer than k shards are present.
bool reconstruct(int k, int m, size_t size, uint8_t* const* shards, const bool* present);
}  // namespace fec

// ---------------------------------------------------------------- packets

struct ShardHeader {
    uint32_t frame_id = 0;
    uint8_t frame_flags = 0;
    uint64_t capture_time_us = 0;
    uint32_t frame_size = 0;
    uint16_t block = 0, blocks = 1;
    uint8_t shard = 0, k = 1, m = 0;

    static constexpr size_t kSize = 4 + 1 + 8 + 4 + 2 + 2 + 3;
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);
};

// Cuts a frame into plaintext packets (header + one shard), data and parity interleaved per block.
// `fec_percent`: parity shards per data shard, in percent (at least one per block).
std::vector<std::vector<uint8_t>> packetize(const proto::VideoFrame& frame, int fec_percent);

// ---------------------------------------------------------------- sealing

class Cipher {
public:
    explicit Cipher(const Key& key) : key_(key) {}
    ~Cipher();
    std::vector<uint8_t> seal(uint64_t seq, std::span<const uint8_t> plain) const;
    // False if forged / corrupted.
    bool open(std::span<const uint8_t> datagram, std::vector<uint8_t>& plain, uint64_t& seq) const;

private:
    Key key_;
};

// ---------------------------------------------------------------- reassembly

class FrameAssembler {
public:
    // One opened packet. `now_us`: a monotonic clock, for deciding when a frame is lost.
    void add(std::span<const uint8_t> packet, uint64_t now_us);
    // The next frame in order, once complete. Frames that can no longer be rebuilt are
    // skipped and counted in lost().
    bool pop(proto::VideoFrame& out, uint64_t now_us);

    uint32_t take_lost();       // frames skipped since the last call
    uint32_t take_recovered();  // data shards rebuilt from parity since the last call

    // A frame counts as lost once a newer one is complete and it stayed incomplete this long.
    static constexpr uint64_t kReorderWaitUs = 30000;

private:
    struct Block {
        uint8_t k = 0, m = 0;
        std::vector<std::vector<uint8_t>> shards;
        std::vector<bool> have;
        int count = 0;
        bool done = false;
    };
    struct Frame {
        ShardHeader info;
        std::vector<Block> blocks;
        int blocks_done = 0;
        uint64_t completed_us = 0;  // 0 while incomplete
    };
    bool try_finish(Frame& f, Block& b, uint64_t now_us);

    std::map<uint32_t, Frame> frames_;
    std::optional<uint32_t> next_;
    uint32_t lost_ = 0, recovered_ = 0;
};

}  // namespace dm::udp
