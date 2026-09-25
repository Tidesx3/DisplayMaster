// Little-endian byte writer/reader used by the wire protocol.
// Reader is bounds-checked: any out-of-range read latches ok() == false and
// returns zero values, so decoders can read everything and check once.
#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dm {

class ByteWriter {
public:
    ByteWriter() = default;
    explicit ByteWriter(std::vector<uint8_t>& out) : ext_(&out) {}

    void u8(uint8_t v) { buf().push_back(v); }
    void u16(uint16_t v) { put_le(v); }
    void u32(uint32_t v) { put_le(v); }
    void u64(uint64_t v) { put_le(v); }
    void i32(int32_t v) { put_le(static_cast<uint32_t>(v)); }
    void f32(float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, sizeof bits);
        put_le(bits);
    }
    void boolean(bool v) { u8(v ? 1 : 0); }

    // Short string: u16 length prefix, UTF-8. Truncated at 65535 bytes.
    void str(std::string_view s) {
        const size_t n = s.size() > 0xFFFF ? 0xFFFF : s.size();
        u16(static_cast<uint16_t>(n));
        raw(s.data(), n);
    }
    // Blob: u32 length prefix.
    void blob(std::span<const uint8_t> b) {
        u32(static_cast<uint32_t>(b.size()));
        raw(b.data(), b.size());
    }
    void raw(const void* p, size_t n) {
        auto* c = static_cast<const uint8_t*>(p);
        buf().insert(buf().end(), c, c + n);
    }

    size_t size() const { return ext_ ? ext_->size() : own_.size(); }
    std::vector<uint8_t>& buf() { return ext_ ? *ext_ : own_; }
    std::vector<uint8_t> take() { return std::move(buf()); }

    // Overwrite a previously written u32 (used for length back-patching).
    void patch_u32(size_t offset, uint32_t v) {
        for (int i = 0; i < 4; ++i) buf()[offset + i] = static_cast<uint8_t>(v >> (8 * i));
    }

private:
    template <typename T>
    void put_le(T v) {
        for (size_t i = 0; i < sizeof(T); ++i) buf().push_back(static_cast<uint8_t>(v >> (8 * i)));
    }

    std::vector<uint8_t> own_;
    std::vector<uint8_t>* ext_ = nullptr;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const uint8_t> data) : data_(data) {}

    uint8_t u8() { return get_le<uint8_t>(); }
    uint16_t u16() { return get_le<uint16_t>(); }
    uint32_t u32() { return get_le<uint32_t>(); }
    uint64_t u64() { return get_le<uint64_t>(); }
    int32_t i32() { return static_cast<int32_t>(get_le<uint32_t>()); }
    float f32() {
        uint32_t bits = get_le<uint32_t>();
        float v;
        std::memcpy(&v, &bits, sizeof v);
        return v;
    }
    bool boolean() { return u8() != 0; }

    std::string str() {
        const uint16_t n = u16();
        if (!need(n)) return {};
        std::string s(reinterpret_cast<const char*>(data_.data() + pos_), n);
        pos_ += n;
        return s;
    }
    std::vector<uint8_t> blob() {
        const uint32_t n = u32();
        if (!need(n)) return {};
        std::vector<uint8_t> b(data_.begin() + pos_, data_.begin() + pos_ + n);
        pos_ += n;
        return b;
    }

    bool ok() const { return ok_; }
    size_t remaining() const { return data_.size() - pos_; }

private:
    bool need(size_t n) {
        if (!ok_ || data_.size() - pos_ < n) {
            ok_ = false;
            return false;
        }
        return true;
    }
    template <typename T>
    T get_le() {
        if (!need(sizeof(T))) return T{};
        T v{};
        for (size_t i = 0; i < sizeof(T); ++i) v |= static_cast<T>(static_cast<T>(data_[pos_ + i]) << (8 * i));
        pos_ += sizeof(T);
        return v;
    }

    std::span<const uint8_t> data_;
    size_t pos_ = 0;
    bool ok_ = true;
};

}  // namespace dm
