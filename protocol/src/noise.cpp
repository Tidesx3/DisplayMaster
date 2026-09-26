#include "dm/noise.h"

#include <cstdio>
#include <cstring>

#include "monocypher.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <stdlib.h>  // arc4random_buf (bionic)
#endif

namespace dm::noise {
namespace {

constexpr char kProtocolName[] = "Noise_XX_25519_ChaChaPoly_BLAKE2b";
constexpr size_t kBlockSize = 128;  // BLAKE2b
constexpr char kPrologue[] = "DisplayMaster/2";

Hash hash(std::span<const uint8_t> a, std::span<const uint8_t> b = {}) {
    Hash out;
    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, kHashSize);
    crypto_blake2b_update(&ctx, a.data(), a.size());
    crypto_blake2b_update(&ctx, b.data(), b.size());
    crypto_blake2b_final(&ctx, out.data());
    return out;
}

// HMAC (RFC 2104) over BLAKE2b, as Noise specifies (not BLAKE2b's keyed mode).
Hash hmac(std::span<const uint8_t> key, std::span<const uint8_t> data) {
    uint8_t k[kBlockSize] = {};
    if (key.size() > kBlockSize) {
        const Hash hk = hash(key);
        std::memcpy(k, hk.data(), hk.size());
    } else {
        std::memcpy(k, key.data(), key.size());
    }
    uint8_t ipad[kBlockSize], opad[kBlockSize];
    for (size_t i = 0; i < kBlockSize; ++i) {
        ipad[i] = static_cast<uint8_t>(k[i] ^ 0x36);
        opad[i] = static_cast<uint8_t>(k[i] ^ 0x5c);
    }
    const Hash inner = hash(ipad, data);
    Hash out = hash(opad, inner);
    crypto_wipe(k, sizeof k);
    crypto_wipe(ipad, sizeof ipad);
    crypto_wipe(opad, sizeof opad);
    return out;
}

// HKDF with two outputs (all Noise needs here).
void hkdf2(const Hash& ck, std::span<const uint8_t> ikm, Hash& out1, Hash& out2) {
    Hash temp = hmac(ck, ikm);
    const uint8_t one = 1;
    out1 = hmac(temp, {&one, 1});
    uint8_t in2[kHashSize + 1];
    std::memcpy(in2, out1.data(), kHashSize);
    in2[kHashSize] = 2;
    out2 = hmac(temp, in2);
    crypto_wipe(temp.data(), temp.size());
}

void nonce_bytes(uint64_t n, uint8_t out[12]) {
    std::memset(out, 0, 4);
    for (int i = 0; i < 8; ++i) out[4 + i] = static_cast<uint8_t>(n >> (8 * i));  // little-endian
}

}  // namespace

void random_bytes(uint8_t* out, size_t n) {
#ifdef _WIN32
    BCryptGenRandom(nullptr, out, static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#else
    arc4random_buf(out, n);
#endif
}

KeyPair KeyPair::generate() {
    Key priv;
    random_bytes(priv.data(), priv.size());
    KeyPair kp = from_private(priv);
    crypto_wipe(priv.data(), priv.size());
    return kp;
}

KeyPair KeyPair::from_private(const Key& priv) {
    KeyPair kp;
    kp.priv = priv;
    crypto_x25519_public_key(kp.pub.data(), kp.priv.data());
    return kp;
}

// ---------------------------------------------------------------- CipherState

CipherState::~CipherState() {
    crypto_wipe(key_, sizeof key_);
}

void CipherState::init(const uint8_t key[kKeySize]) {
    std::memcpy(key_, key, kKeySize);
    nonce_ = 0;
    has_key_ = true;
}

void CipherState::encrypt(std::span<const uint8_t> ad, std::span<const uint8_t> plain, std::vector<uint8_t>& out) {
    const size_t at = out.size();
    if (!has_key_) {
        out.insert(out.end(), plain.begin(), plain.end());
        return;
    }
    out.resize(at + plain.size() + kTagSize);
    uint8_t nonce[12];
    nonce_bytes(nonce_++, nonce);
    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, key_, nonce);
    crypto_aead_write(&ctx, out.data() + at, out.data() + at + plain.size(), ad.data(), ad.size(), plain.data(),
                      plain.size());
    crypto_wipe(&ctx, sizeof ctx);
}

bool CipherState::decrypt(std::span<const uint8_t> ad, std::span<const uint8_t> cipher, std::vector<uint8_t>& out) {
    if (!has_key_) {
        out.insert(out.end(), cipher.begin(), cipher.end());
        return true;
    }
    if (cipher.size() < kTagSize) return false;
    const size_t n = cipher.size() - kTagSize;
    const size_t at = out.size();
    out.resize(at + n);
    uint8_t nonce[12];
    nonce_bytes(nonce_, nonce);
    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, key_, nonce);
    const int r = crypto_aead_read(&ctx, out.data() + at, cipher.data() + n, ad.data(), ad.size(), cipher.data(), n);
    crypto_wipe(&ctx, sizeof ctx);
    if (r != 0) {
        out.resize(at);
        return false;
    }
    ++nonce_;  // only advances on success, so a forged message can't desync the stream
    return true;
}

// ---------------------------------------------------------------- HandshakeXX

HandshakeXX::HandshakeXX(Role role, const KeyPair& static_key, std::span<const uint8_t> prologue,
                         std::optional<KeyPair> ephemeral)
    : role_(role), s_(static_key), e_(ephemeral ? *ephemeral : KeyPair::generate()) {
    // Protocol name fits in HASHLEN: zero-padded, not hashed.
    std::memcpy(h_.data(), kProtocolName, sizeof kProtocolName - 1);
    ck_ = h_;
    mix_hash(prologue);
}

HandshakeXX::~HandshakeXX() {
    crypto_wipe(s_.priv.data(), s_.priv.size());
    crypto_wipe(e_.priv.data(), e_.priv.size());
    crypto_wipe(ck_.data(), ck_.size());
}

void HandshakeXX::mix_hash(std::span<const uint8_t> data) {
    h_ = hash(h_, data);
}

void HandshakeXX::mix_key(std::span<const uint8_t> ikm) {
    Hash k;
    hkdf2(ck_, ikm, ck_, k);
    cs_.init(k.data());  // truncated to 32 bytes (HASHLEN is 64)
    crypto_wipe(k.data(), k.size());
}

bool HandshakeXX::dh_mix(const Key& priv, const Key& pub) {
    uint8_t shared[32];
    crypto_x25519(shared, priv.data(), pub.data());
    // All-zero output means a low-order public key: refuse instead of mixing in nothing.
    uint8_t any = 0;
    for (uint8_t b : shared) any |= b;
    mix_key(shared);
    crypto_wipe(shared, sizeof shared);
    return any != 0;
}

void HandshakeXX::encrypt_and_hash(std::span<const uint8_t> plain, std::vector<uint8_t>& out) {
    const size_t at = out.size();
    cs_.encrypt(h_, plain, out);
    mix_hash(std::span<const uint8_t>(out.data() + at, out.size() - at));
}

bool HandshakeXX::decrypt_and_hash(std::span<const uint8_t> cipher, std::vector<uint8_t>& out) {
    const Hash h = h_;
    if (!cs_.decrypt(h, cipher, out)) return false;
    mix_hash(cipher);
    return true;
}

bool HandshakeXX::write_message(std::span<const uint8_t> payload, std::vector<uint8_t>& out) {
    if (!my_turn()) return false;
    switch (step_) {
        case 0:  // -> e
            out.insert(out.end(), e_.pub.begin(), e_.pub.end());
            mix_hash(e_.pub);
            break;
        case 1:  // <- e, ee, s, es
            out.insert(out.end(), e_.pub.begin(), e_.pub.end());
            mix_hash(e_.pub);
            if (!dh_mix(e_.priv, re_)) return false;
            encrypt_and_hash(s_.pub, out);
            if (!dh_mix(s_.priv, re_)) return false;
            break;
        case 2:  // -> s, se
            encrypt_and_hash(s_.pub, out);
            if (!dh_mix(s_.priv, re_)) return false;
            break;
    }
    encrypt_and_hash(payload, out);
    ++step_;
    return true;
}

bool HandshakeXX::read_message(std::span<const uint8_t> message, std::vector<uint8_t>& payload) {
    if (my_turn() || complete()) return false;
    size_t pos = 0;
    auto take_key = [&](Key& k) {
        if (message.size() - pos < kKeySize) return false;
        std::memcpy(k.data(), message.data() + pos, kKeySize);
        pos += kKeySize;
        return true;
    };
    auto take_encrypted_key = [&](Key& k) {
        const size_t n = kKeySize + (cs_.has_key() ? kTagSize : 0);
        if (message.size() - pos < n) return false;
        std::vector<uint8_t> plain;
        if (!decrypt_and_hash(message.subspan(pos, n), plain)) return false;
        std::memcpy(k.data(), plain.data(), kKeySize);
        pos += n;
        return true;
    };
    switch (step_) {
        case 0:  // -> e (we are the responder)
            if (!take_key(re_)) return false;
            mix_hash(re_);
            break;
        case 1:  // <- e, ee, s, es (we are the initiator)
            if (!take_key(re_)) return false;
            mix_hash(re_);
            if (!dh_mix(e_.priv, re_)) return false;
            if (!take_encrypted_key(rs_)) return false;
            if (!dh_mix(e_.priv, rs_)) return false;
            break;
        case 2:  // -> s, se (we are the responder)
            if (!take_encrypted_key(rs_)) return false;
            if (!dh_mix(e_.priv, rs_)) return false;
            break;
    }
    if (!decrypt_and_hash(message.subspan(pos), payload)) return false;
    ++step_;
    return true;
}

void HandshakeXX::split(CipherState& send, CipherState& recv) {
    Hash k1, k2;
    hkdf2(ck_, {}, k1, k2);
    // k1 encrypts initiator -> responder, k2 the other way.
    if (role_ == Role::Initiator) {
        send.init(k1.data());
        recv.init(k2.data());
    } else {
        send.init(k2.data());
        recv.init(k1.data());
    }
    crypto_wipe(k1.data(), k1.size());
    crypto_wipe(k2.data(), k2.size());
}

// ---------------------------------------------------------------- helpers

std::string pairing_code(const Hash& h) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(h[i]) << (8 * i);
    char buf[8];
    const uint32_t n = v % 1000000;
    std::snprintf(buf, sizeof buf, "%03u %03u", n / 1000, n % 1000);
    return buf;
}

std::string to_hex(std::span<const uint8_t> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string s;
    s.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        s.push_back(kDigits[b >> 4]);
        s.push_back(kDigits[b & 15]);
    }
    return s;
}

std::optional<Key> key_from_hex(const std::string& hex) {
    if (hex.size() != kKeySize * 2) return std::nullopt;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    Key k;
    for (size_t i = 0; i < kKeySize; ++i) {
        const int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        k[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    return k;
}

std::span<const uint8_t> prologue() {
    return {reinterpret_cast<const uint8_t*>(kPrologue), sizeof kPrologue - 1};
}

std::vector<uint8_t> handshake_frame(std::span<const uint8_t> message) {
    std::vector<uint8_t> out;
    ByteWriter w(out);
    w.u32(static_cast<uint32_t>(message.size()));
    w.u8(static_cast<uint8_t>(proto::MsgType::Handshake));
    w.u8(0);
    w.u16(0);
    w.raw(message.data(), message.size());
    return out;
}

// ---------------------------------------------------------------- SecureChannel

SecureChannel::SecureChannel(HandshakeXX& hs) {
    hs.split(send_, recv_);
}

std::vector<uint8_t> SecureChannel::seal(std::span<const uint8_t> frame) {
    std::vector<uint8_t> out;
    out.reserve(proto::kHeaderSize + frame.size() + kTagSize);
    ByteWriter w(out);
    w.u32(static_cast<uint32_t>(frame.size() + kTagSize));
    w.u8(static_cast<uint8_t>(proto::MsgType::Encrypted));
    w.u8(0);
    w.u16(0);
    send_.encrypt({}, frame, out);
    return out;
}

bool SecureChannel::open(std::span<const uint8_t> payload, proto::RawMessage& inner) {
    scratch_.clear();
    if (!recv_.decrypt({}, payload, scratch_)) return false;
    proto::FrameParser parser;
    parser.feed(scratch_.data(), scratch_.size());
    // Exactly one whole frame per sealed message.
    return parser.next(inner) && parser.buffered() == 0;
}

}  // namespace dm::noise
