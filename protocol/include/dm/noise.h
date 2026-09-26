// Encrypted, mutually authenticated Wi-Fi connections: the Noise XX handshake
// (Noise_XX_25519_ChaChaPoly_BLAKE2b, https://noiseprotocol.org/noise.html, revision 34)
// on Monocypher, then every protocol frame sealed with ChaCha20-Poly1305.
//
//   device (initiator)          PC (responder)
//   -> e
//                               <- e, ee, s, es
//   -> s, se
//
// Both sides end up with each other's long-term (static) X25519 key. A new pairing is
// confirmed by the user comparing pairing_code() on both screens (defeats a man in the
// middle); afterwards each side recognizes the other by its pinned static key.
//
// Framing: handshake messages travel as MsgType::Handshake frames; afterwards every frame
// is sent as MsgType::Encrypted whose payload is the sealed *inner* frame (header +
// payload). Unlike Noise transport messages, sealed frames may exceed 64 KiB (video).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "dm/protocol.h"

namespace dm::noise {

constexpr size_t kKeySize = 32;
constexpr size_t kTagSize = 16;
constexpr size_t kHashSize = 64;
using Key = std::array<uint8_t, kKeySize>;
using Hash = std::array<uint8_t, kHashSize>;

// Cryptographically secure randomness from the OS.
void random_bytes(uint8_t* out, size_t n);

struct KeyPair {
    Key priv{}, pub{};
    static KeyPair generate();
    static KeyPair from_private(const Key& priv);
};

// One direction of the transport: ChaCha20-Poly1305 with a 64-bit counter nonce.
class CipherState {
public:
    ~CipherState();
    void init(const uint8_t key[kKeySize]);
    bool has_key() const { return has_key_; }
    // Appends ciphertext + tag to `out` (plaintext as is while there is no key).
    void encrypt(std::span<const uint8_t> ad, std::span<const uint8_t> plain, std::vector<uint8_t>& out);
    // Appends the plaintext to `out`; false if the message was forged or corrupted.
    bool decrypt(std::span<const uint8_t> ad, std::span<const uint8_t> cipher, std::vector<uint8_t>& out);

private:
    uint8_t key_[kKeySize]{};
    uint64_t nonce_ = 0;
    bool has_key_ = false;
};

class HandshakeXX {
public:
    enum class Role { Initiator, Responder };
    // `ephemeral` is for test vectors only; normally a fresh key is generated.
    HandshakeXX(Role role, const KeyPair& static_key, std::span<const uint8_t> prologue,
                std::optional<KeyPair> ephemeral = std::nullopt);
    ~HandshakeXX();

    // Next message of the pattern, carrying `payload`. False if it's the peer's turn.
    bool write_message(std::span<const uint8_t> payload, std::vector<uint8_t>& out);
    // Peer's message; appends its payload. False if malformed, forged or out of turn.
    bool read_message(std::span<const uint8_t> message, std::vector<uint8_t>& payload);
    bool complete() const { return step_ == 3; }
    bool my_turn() const { return (step_ % 2 == 0) == (role_ == Role::Initiator) && !complete(); }

    const Key& remote_static() const { return rs_; }
    const Hash& handshake_hash() const { return h_; }
    // After complete(): the ciphers for sending and receiving.
    void split(CipherState& send, CipherState& recv);

private:
    void mix_hash(std::span<const uint8_t> data);
    void mix_key(std::span<const uint8_t> ikm);
    bool dh_mix(const Key& priv, const Key& pub);
    void encrypt_and_hash(std::span<const uint8_t> plain, std::vector<uint8_t>& out);
    bool decrypt_and_hash(std::span<const uint8_t> cipher, std::vector<uint8_t>& out);

    Role role_;
    int step_ = 0;  // messages processed so far
    KeyPair s_, e_;
    Key re_{}, rs_{};
    Hash h_{}, ck_{};
    CipherState cs_;
};

// Six digits both screens show for a new pairing, e.g. "482 913".
std::string pairing_code(const Hash& handshake_hash);
std::string to_hex(std::span<const uint8_t> bytes);
std::optional<Key> key_from_hex(const std::string& hex);

// The transport after the handshake: seals outgoing frames, opens incoming ones.
// Not thread-safe; callers serialize sending (the nonce order must match the byte order).
class SecureChannel {
public:
    explicit SecureChannel(HandshakeXX& finished_handshake);
    // A complete framed message in, an Encrypted frame out.
    std::vector<uint8_t> seal(std::span<const uint8_t> frame);
    // The payload of an Encrypted frame in, the inner message out.
    bool open(std::span<const uint8_t> payload, proto::RawMessage& inner);

private:
    CipherState send_, recv_;
    std::vector<uint8_t> scratch_;
};

// Protocol prologue: binds the handshake to this protocol and version.
std::span<const uint8_t> prologue();

// A MsgType::Handshake frame carrying one handshake message (the frame payload as is).
std::vector<uint8_t> handshake_frame(std::span<const uint8_t> message);

}  // namespace dm::noise
