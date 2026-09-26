#include "dm/udp_video.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

#include "monocypher.h"

namespace dm::udp {

// ---------------------------------------------------------------- GF(2^8) arithmetic

namespace {

struct Gf {
    uint8_t exp[512];
    uint8_t log[256];
    uint8_t mul[256][256];  // full product table: the inner loops are one lookup per byte

    Gf() {
        int x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[i] = static_cast<uint8_t>(x);
            log[x] = static_cast<uint8_t>(i);
            x <<= 1;
            if (x & 0x100) x ^= 0x11D;  // x^8 + x^4 + x^3 + x^2 + 1
        }
        for (int i = 255; i < 512; ++i) exp[i] = exp[i - 255];
        log[0] = 0;
        for (int a = 0; a < 256; ++a)
            for (int b = 0; b < 256; ++b)
                mul[a][b] = (a && b) ? exp[log[a] + log[b]] : 0;
    }
    uint8_t inv(uint8_t a) const { return exp[255 - log[a]]; }
};

const Gf& gf() {
    static const Gf g;
    return g;
}

// Cauchy matrix entry for parity row j (of m) and data column i (of k): 1 / (x_j + y_i)
// with x_j = k + j and y_i = i, all distinct, so every square submatrix of [I; C] is invertible.
uint8_t cauchy(int k, int j, int i) {
    return gf().inv(static_cast<uint8_t>((k + j) ^ i));
}

// out ^= c * in
void mul_add(uint8_t* out, const uint8_t* in, uint8_t c, size_t n) {
    if (!c) return;
    const uint8_t* row = gf().mul[c];
    for (size_t i = 0; i < n; ++i) out[i] ^= row[in[i]];
}

}  // namespace

namespace fec {

void encode(int k, int m, size_t size, const uint8_t* const* data, uint8_t* const* parity) {
    for (int j = 0; j < m; ++j) {
        std::memset(parity[j], 0, size);
        for (int i = 0; i < k; ++i) mul_add(parity[j], data[i], cauchy(k, j, i), size);
    }
}

bool reconstruct(int k, int m, size_t size, uint8_t* const* shards, const bool* present) {
    std::vector<int> missing, rows;
    for (int i = 0; i < k; ++i)
        if (!present[i]) missing.push_back(i);
    if (missing.empty()) return true;
    for (int i = 0; i < k + m && static_cast<int>(rows.size()) < k; ++i)
        if (present[i]) rows.push_back(i);
    if (static_cast<int>(rows.size()) < k) return false;

    // A (k x k): row r expresses received shard rows[r] in terms of the data shards.
    std::vector<uint8_t> a(static_cast<size_t>(k) * k, 0), inv(static_cast<size_t>(k) * k, 0);
    for (int r = 0; r < k; ++r) {
        const int s = rows[r];
        for (int c = 0; c < k; ++c) a[r * k + c] = s < k ? (s == c) : cauchy(k, s - k, c);
        inv[r * k + r] = 1;
    }
    // Gauss-Jordan inversion over GF(2^8).
    const Gf& g = gf();
    for (int col = 0; col < k; ++col) {
        int pivot = col;
        while (pivot < k && !a[pivot * k + col]) ++pivot;
        if (pivot == k) return false;  // cannot happen for a Cauchy code
        if (pivot != col)
            for (int c = 0; c < k; ++c) {
                std::swap(a[pivot * k + c], a[col * k + c]);
                std::swap(inv[pivot * k + c], inv[col * k + c]);
            }
        const uint8_t scale = g.inv(a[col * k + col]);
        for (int c = 0; c < k; ++c) {
            a[col * k + c] = g.mul[scale][a[col * k + c]];
            inv[col * k + c] = g.mul[scale][inv[col * k + c]];
        }
        for (int r = 0; r < k; ++r) {
            const uint8_t f = a[r * k + col];
            if (r == col || !f) continue;
            for (int c = 0; c < k; ++c) {
                a[r * k + c] ^= g.mul[f][a[col * k + c]];
                inv[r * k + c] ^= g.mul[f][inv[col * k + c]];
            }
        }
    }
    // data_i = sum_r inv[i][r] * received_r, only for the missing ones.
    std::vector<std::vector<uint8_t>> rebuilt(missing.size(), std::vector<uint8_t>(size, 0));
    for (size_t n = 0; n < missing.size(); ++n)
        for (int r = 0; r < k; ++r) mul_add(rebuilt[n].data(), shards[rows[r]], inv[missing[n] * k + r], size);
    for (size_t n = 0; n < missing.size(); ++n) std::memcpy(shards[missing[n]], rebuilt[n].data(), size);
    return true;
}

}  // namespace fec

// ---------------------------------------------------------------- packets

void ShardHeader::write(ByteWriter& w) const {
    w.u32(frame_id);
    w.u8(frame_flags);
    w.u64(capture_time_us);
    w.u32(frame_size);
    w.u16(block);
    w.u16(blocks);
    w.u8(shard);
    w.u8(k);
    w.u8(m);
}

bool ShardHeader::read(ByteReader& r) {
    frame_id = r.u32();
    frame_flags = r.u8();
    capture_time_us = r.u64();
    frame_size = r.u32();
    block = r.u16();
    blocks = r.u16();
    shard = r.u8();
    k = r.u8();
    m = r.u8();
    return r.ok() && blocks > 0 && block < blocks && k > 0 && k <= kMaxDataShards && shard < k + m &&
           frame_size <= proto::kMaxPayload;
}

std::vector<std::vector<uint8_t>> packetize(const proto::VideoFrame& frame, int fec_percent) {
    const size_t size = frame.data.size();
    const size_t total = std::max<size_t>(1, (size + kShardSize - 1) / kShardSize);
    const size_t blocks = (total + kMaxDataShards - 1) / kMaxDataShards;
    std::vector<std::vector<uint8_t>> out;
    size_t shard_start = 0;
    for (size_t b = 0; b < blocks; ++b) {
        // Spread the shards evenly so the last block isn't a tiny, weakly protected one.
        const size_t k = total / blocks + (b < total % blocks ? 1 : 0);
        const size_t m = std::max<size_t>(1, (k * static_cast<size_t>(fec_percent) + 99) / 100);
        std::vector<std::vector<uint8_t>> shards(k + m, std::vector<uint8_t>(kShardSize, 0));
        for (size_t i = 0; i < k; ++i) {
            const size_t off = (shard_start + i) * kShardSize;
            if (off < size) std::memcpy(shards[i].data(), frame.data.data() + off, std::min(kShardSize, size - off));
        }
        std::vector<const uint8_t*> data(k);
        std::vector<uint8_t*> parity(m);
        for (size_t i = 0; i < k; ++i) data[i] = shards[i].data();
        for (size_t j = 0; j < m; ++j) parity[j] = shards[k + j].data();
        fec::encode(static_cast<int>(k), static_cast<int>(m), kShardSize, data.data(), parity.data());

        ShardHeader h;
        h.frame_id = static_cast<uint32_t>(frame.frame_id);
        h.frame_flags = frame.flags;
        h.capture_time_us = frame.capture_time_us;
        h.frame_size = static_cast<uint32_t>(size);
        h.block = static_cast<uint16_t>(b);
        h.blocks = static_cast<uint16_t>(blocks);
        h.k = static_cast<uint8_t>(k);
        h.m = static_cast<uint8_t>(m);
        for (size_t s = 0; s < k + m; ++s) {
            std::vector<uint8_t> p;
            p.reserve(ShardHeader::kSize + kShardSize);
            ByteWriter w(p);
            h.shard = static_cast<uint8_t>(s);
            h.write(w);
            w.raw(shards[s].data(), kShardSize);
            out.push_back(std::move(p));
        }
        shard_start += k;
    }
    return out;
}

// ---------------------------------------------------------------- sealing

Cipher::~Cipher() {
    crypto_wipe(key_.data(), key_.size());
}

std::vector<uint8_t> Cipher::seal(uint64_t seq, std::span<const uint8_t> plain) const {
    std::vector<uint8_t> out(8 + plain.size() + 16);
    uint8_t nonce[24] = {};
    for (int i = 0; i < 8; ++i) out[i] = nonce[i] = static_cast<uint8_t>(seq >> (8 * i));
    crypto_aead_lock(out.data() + 8, out.data() + 8 + plain.size(), key_.data(), nonce, out.data(), 8, plain.data(),
                     plain.size());
    return out;
}

bool Cipher::open(std::span<const uint8_t> datagram, std::vector<uint8_t>& plain, uint64_t& seq) const {
    if (datagram.size() < kSealOverhead) return false;
    const size_t n = datagram.size() - kSealOverhead;
    uint8_t nonce[24] = {};
    seq = 0;
    for (int i = 0; i < 8; ++i) {
        nonce[i] = datagram[i];
        seq |= static_cast<uint64_t>(datagram[i]) << (8 * i);
    }
    plain.resize(n);
    return crypto_aead_unlock(plain.data(), datagram.data() + 8 + n, key_.data(), nonce, datagram.data(), 8,
                              datagram.data() + 8, n) == 0;
}

// ---------------------------------------------------------------- reassembly

namespace {
// Serial-number comparison, so frame ids may wrap.
bool before(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) < 0;
}
}  // namespace

void FrameAssembler::add(std::span<const uint8_t> packet, uint64_t now_us) {
    ByteReader r(packet);
    ShardHeader h;
    if (!h.read(r) || r.remaining() != kShardSize) return;
    if (next_ && before(h.frame_id, *next_)) return;  // already delivered or given up
    // A stalled stream must not grow without bound: keep the newest frames.
    while (frames_.size() >= 128) frames_.erase(frames_.begin());
    auto [it, fresh] = frames_.try_emplace(h.frame_id);
    Frame& f = it->second;
    if (fresh) {
        f.info = h;
        f.blocks.resize(h.blocks);
    } else if (f.info.blocks != h.blocks || f.info.frame_size != h.frame_size) {
        return;  // inconsistent with earlier packets of this frame
    }
    if (f.completed_us) return;
    Block& b = f.blocks[h.block];
    if (b.shards.empty()) {
        b.k = h.k;
        b.m = h.m;
        b.shards.assign(h.k + h.m, {});
        b.have.assign(h.k + h.m, false);
    } else if (b.k != h.k || b.m != h.m) {
        return;
    }
    if (b.done || b.have[h.shard]) return;
    b.shards[h.shard].assign(packet.end() - kShardSize, packet.end());
    b.have[h.shard] = true;
    ++b.count;
    try_finish(f, b, now_us);
}

bool FrameAssembler::try_finish(Frame& f, Block& b, uint64_t now_us) {
    if (b.count < b.k) return false;
    std::vector<uint8_t*> ptrs(b.k + b.m);
    std::unique_ptr<bool[]> present(new bool[b.k + b.m]);
    for (int i = 0; i < b.k + b.m; ++i) {
        if (b.shards[i].empty()) b.shards[i].assign(kShardSize, 0);
        ptrs[i] = b.shards[i].data();
        present[i] = b.have[i];
    }
    int rebuilt = 0;
    for (int i = 0; i < b.k; ++i) rebuilt += !present[i];
    if (!fec::reconstruct(b.k, b.m, kShardSize, ptrs.data(), present.get())) return false;
    recovered_ += static_cast<uint32_t>(rebuilt);
    b.done = true;
    b.shards.resize(b.k);  // parity no longer needed
    if (++f.blocks_done == static_cast<int>(f.blocks.size())) f.completed_us = now_us ? now_us : 1;
    return true;
}

bool FrameAssembler::pop(proto::VideoFrame& out, uint64_t now_us) {
    if (frames_.empty()) return false;
    if (!next_) {
        // Start with the first complete frame (the stream begins with a keyframe).
        for (auto& [id, f] : frames_)
            if (f.completed_us) {
                next_ = id;
                break;
            }
        if (!next_) return false;
    }
    while (true) {
        auto it = frames_.find(*next_);
        if (it != frames_.end() && it->second.completed_us) {
            Frame& f = it->second;
            out.frame_id = f.info.frame_id;
            out.flags = f.info.frame_flags;
            out.capture_time_us = f.info.capture_time_us;
            out.data.clear();
            out.data.reserve(f.info.frame_size);
            for (auto& b : f.blocks)
                for (auto& s : b.shards) out.data.insert(out.data.end(), s.begin(), s.end());
            out.data.resize(f.info.frame_size);
            frames_.erase(it);
            ++*next_;
            return true;
        }
        // The next frame is incomplete: give up on it once a newer frame has been complete
        // for a while (its missing packets aren't coming any more).
        bool newer_done = false;
        for (auto& [id, f] : frames_)
            if (before(*next_, id) && f.completed_us && now_us - f.completed_us >= kReorderWaitUs) newer_done = true;
        if (!newer_done) return false;
        if (it != frames_.end()) frames_.erase(it);
        if (!lost_) lost_first_ = *next_;
        lost_last_ = *next_;
        ++lost_;
        ++*next_;
        // Anything older than the new position is useless now.
        while (!frames_.empty() && before(frames_.begin()->first, *next_)) frames_.erase(frames_.begin());
    }
}

uint32_t FrameAssembler::take_lost() {
    return std::exchange(lost_, 0);
}

uint32_t FrameAssembler::take_lost(uint32_t& first, uint32_t& last) {
    first = lost_first_;
    last = lost_last_;
    return std::exchange(lost_, 0);
}

uint32_t FrameAssembler::take_recovered() {
    return std::exchange(recovered_, 0);
}

}  // namespace dm::udp
