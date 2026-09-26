#include <gtest/gtest.h>

#include "dm/noise.h"

using namespace dm;
using namespace dm::noise;

namespace {

std::vector<uint8_t> hex(const std::string& s) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(static_cast<uint8_t>(std::stoi(s.substr(i, 2), nullptr, 16)));
    return out;
}

KeyPair key(const std::string& priv_hex) {
    Key k;
    const auto b = hex(priv_hex);
    std::copy(b.begin(), b.end(), k.begin());
    return KeyPair::from_private(k);
}

// Noise_XX_25519_ChaChaPoly_BLAKE2b from the cacophony test vectors
// (https://github.com/haskell-cryptography/cacophony, vectors/cacophony.txt).
struct Message {
    const char* payload;
    const char* ciphertext;
};
constexpr Message kMessages[] = {
    {"4c756477696720766f6e204d69736573",
     "ca35def5ae56cec33dc2036731ab14896bc4c75dbb07a61f879f8e3afa4c79444c756477696720766f6e204d69736573"},
    {"4d757272617920526f746862617264",
     "95ebc60d2b1fa672c1f46a8aa265ef51bfe38e7ccb39ec5be34069f1448088430505b6745ce64a5f33f0e8e3b83f11ce8802bca507f4f2"
     "d8b564dbe277e1966116e132faa2dfd70b8b077b9f94b913df5056ae1319469b824a98d54bbaa82c325595587064f978c4b6d104f7596e"
     "6f"},
    {"462e20412e20486179656b",
     "99579e1c1ee15e422a57ddd6b16d37087b17558e8369c18991b4b2ca3a824abf904cdcf5458b5431a75af034ca9e9b982de039eaaf15677"
     "5e2d580cd4e5ebae89c3f8cb2594b556d8a8169"},
    // Transport messages (after the handshake), still alternating: responder, initiator, responder.
    {"4361726c204d656e676572", "fc56eea290b3f3a21aac0c70cd5787b5ee99be37d2f4d751329b55"},
    {"4a65616e2d426170746973746520536179", "bb31c9da10d5639a4cdb88a12f5c61de41bbc7df09bf75d94f8184fe4157f5c68f"},
    {"457567656e2042f6686d20766f6e2042617765726b",
     "f6199cadb152fb27f82be0a0891ec76a33598ae92a46cab2fb5a8ed5bf48b7f267f8370af7"},
};

}  // namespace

TEST(Noise, MatchesCacophonyVector) {
    const auto prologue = hex("4a6f686e2047616c74");
    const KeyPair is = key("e61ef9919cde45dd5f82166404bd08e38bceb5dfdfded0a34c8df7ed542214d1");
    const KeyPair ie = key("893e28b9dc6ca8d611ab664754b8ceb7bac5117349a4439a6b0569da977c464a");
    const KeyPair rs = key("4a3acbfdb163dec651dfa3194dece676d437029c62a408b4c5ea9114246e4893");
    const KeyPair re = key("bbdb4cdbd309f1a1f2e1456967fe288cadd6f712d65dc7b7793d5e63da6b375b");
    HandshakeXX init(HandshakeXX::Role::Initiator, is, prologue, ie);
    HandshakeXX resp(HandshakeXX::Role::Responder, rs, prologue, re);

    // Handshake: messages 0 and 2 from the initiator, 1 from the responder.
    for (int i = 0; i < 3; ++i) {
        HandshakeXX& writer = i % 2 == 0 ? init : resp;
        HandshakeXX& reader = i % 2 == 0 ? resp : init;
        std::vector<uint8_t> msg, payload;
        ASSERT_TRUE(writer.write_message(hex(kMessages[i].payload), msg)) << i;
        EXPECT_EQ(to_hex(msg), kMessages[i].ciphertext) << "message " << i;
        ASSERT_TRUE(reader.read_message(msg, payload)) << i;
        EXPECT_EQ(payload, hex(kMessages[i].payload)) << i;
    }
    ASSERT_TRUE(init.complete());
    ASSERT_TRUE(resp.complete());
    EXPECT_EQ(to_hex(init.handshake_hash()),
              "8cf47d7b3cb5804c0109d48e8bcdbee2cbb65687d8ea2c92994ca361fb86151ad93627b98936cbb32de56e8abb21def3925011ac"
              "3e35db9cbeea73ab9a4392c2");
    EXPECT_EQ(init.handshake_hash(), resp.handshake_hash());
    EXPECT_EQ(init.remote_static(), rs.pub);
    EXPECT_EQ(resp.remote_static(), is.pub);

    CipherState i_send, i_recv, r_send, r_recv;
    init.split(i_send, i_recv);
    resp.split(r_send, r_recv);
    for (int i = 3; i < 6; ++i) {
        const bool from_initiator = i % 2 == 0;  // the vector keeps alternating: 3 is the responder's
        CipherState& send = from_initiator ? i_send : r_send;
        CipherState& recv = from_initiator ? r_recv : i_recv;
        std::vector<uint8_t> ct, pt;
        send.encrypt({}, hex(kMessages[i].payload), ct);
        EXPECT_EQ(to_hex(ct), kMessages[i].ciphertext) << "message " << i;
        ASSERT_TRUE(recv.decrypt({}, ct, pt));
        EXPECT_EQ(pt, hex(kMessages[i].payload));
    }
}

namespace {

// Runs a full handshake between fresh keys and returns both ends.
struct Pair {
    KeyPair device = KeyPair::generate(), pc = KeyPair::generate();
    HandshakeXX init{HandshakeXX::Role::Initiator, device, prologue()};
    HandshakeXX resp{HandshakeXX::Role::Responder, pc, prologue()};
    bool run() {
        std::vector<uint8_t> m, p;
        return init.write_message({}, m) && resp.read_message(m, p) && (m.clear(), resp.write_message({}, m)) &&
               init.read_message(m, p) && (m.clear(), init.write_message({}, m)) && resp.read_message(m, p);
    }
};

}  // namespace

TEST(Noise, SecureChannelRoundTripsFrames) {
    Pair p;
    ASSERT_TRUE(p.run());
    EXPECT_EQ(pairing_code(p.init.handshake_hash()), pairing_code(p.resp.handshake_hash()));
    EXPECT_EQ(pairing_code(p.init.handshake_hash()).size(), 7u);  // "123 456"

    SecureChannel device(p.init), pc(p.resp);
    proto::VideoFrame f;
    f.frame_id = 42;
    f.data.assign(200000, 0xAB);  // larger than a Noise transport message may be
    const auto sealed = pc.seal(proto::encode(f));

    proto::FrameParser outer;
    outer.feed(sealed.data(), sealed.size());
    proto::RawMessage msg, inner;
    ASSERT_TRUE(outer.next(msg));
    EXPECT_EQ(msg.header.type, proto::MsgType::Encrypted);
    ASSERT_TRUE(device.open(msg.payload, inner));
    EXPECT_EQ(inner.header.type, proto::MsgType::VideoFrame);
    auto got = proto::decode<proto::VideoFrame>(inner.payload);
    ASSERT_TRUE(got);
    EXPECT_EQ(got->frame_id, 42u);
    EXPECT_EQ(got->data.size(), 200000u);
}

TEST(Noise, RejectsTamperingAndReplay) {
    Pair p;
    ASSERT_TRUE(p.run());
    SecureChannel device(p.init), pc(p.resp);
    auto sealed = device.seal(proto::encode(proto::Ping{7}));
    std::vector<uint8_t> payload(sealed.begin() + proto::kHeaderSize, sealed.end());

    proto::RawMessage inner;
    auto flipped = payload;
    flipped[3] ^= 1;
    EXPECT_FALSE(pc.open(flipped, inner));  // forged: rejected, stream stays in sync
    ASSERT_TRUE(pc.open(payload, inner));
    EXPECT_FALSE(pc.open(payload, inner));  // replay: nonce already used
}

TEST(Noise, ManInTheMiddleShowsDifferentCodes) {
    // An attacker relaying between device and PC runs two separate handshakes.
    Pair device_side, pc_side;
    ASSERT_TRUE(device_side.run());
    ASSERT_TRUE(pc_side.run());
    EXPECT_NE(device_side.init.handshake_hash(), pc_side.resp.handshake_hash());
}

TEST(Noise, RejectsWrongPrologue) {
    const auto a = KeyPair::generate(), b = KeyPair::generate();
    const uint8_t other[] = {'x'};
    HandshakeXX init(HandshakeXX::Role::Initiator, a, prologue());
    HandshakeXX resp(HandshakeXX::Role::Responder, b, other);
    std::vector<uint8_t> m, p;
    ASSERT_TRUE(init.write_message({}, m));
    ASSERT_TRUE(resp.read_message(m, p));  // message 1 is unauthenticated
    m.clear();
    ASSERT_TRUE(resp.write_message({}, m));
    EXPECT_FALSE(init.read_message(m, p));  // different transcript: decryption fails
}

TEST(Noise, HexRoundTrip) {
    const auto k = KeyPair::generate().pub;
    EXPECT_EQ(key_from_hex(to_hex(k)), k);
    EXPECT_FALSE(key_from_hex("zz"));
}
