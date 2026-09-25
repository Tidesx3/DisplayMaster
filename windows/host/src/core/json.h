// Minimal JSON for the host <-> UI control channel: a streaming writer and
// flat-object field readers. The UI's messages are small and flat, so a full
// parser isn't warranted.
#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace dm::json {

class Writer {
public:
    Writer& begin_object() { return open('{'); }
    Writer& end_object() { return close('}'); }
    Writer& begin_array() { return open('['); }
    Writer& end_array() { return close(']'); }

    Writer& key(std::string_view k) {
        comma();
        string(k);
        out_ += ':';
        after_key_ = true;
        return *this;
    }
    Writer& value(std::string_view v) {
        comma();
        string(v);
        return *this;
    }
    Writer& value(const char* v) { return value(std::string_view(v)); }
    Writer& value(bool v) {
        comma();
        out_ += v ? "true" : "false";
        return *this;
    }
    Writer& value(int64_t v) {
        comma();
        out_ += std::to_string(v);
        return *this;
    }
    Writer& value(uint32_t v) { return value(static_cast<int64_t>(v)); }
    Writer& value(int v) { return value(static_cast<int64_t>(v)); }
    Writer& value(double v) {
        comma();
        char buf[32];
        snprintf(buf, sizeof buf, "%.2f", v);
        out_ += buf;
        return *this;
    }
    template <typename T>
    Writer& field(std::string_view k, T v) {
        key(k);
        return value(v);
    }

    const std::string& str() const { return out_; }

private:
    Writer& open(char c) {
        comma();
        out_ += c;
        first_ = true;
        return *this;
    }
    Writer& close(char c) {
        out_ += c;
        first_ = false;
        return *this;
    }
    void comma() {
        if (after_key_) {
            after_key_ = false;
            return;
        }
        if (!first_ && !out_.empty()) out_ += ',';
        first_ = false;
    }
    void string(std::string_view s) {
        out_ += '"';
        for (char c : s) {
            switch (c) {
                case '"': out_ += "\\\""; break;
                case '\\': out_ += "\\\\"; break;
                case '\n': out_ += "\\n"; break;
                case '\r': out_ += "\\r"; break;
                case '\t': out_ += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        snprintf(buf, sizeof buf, "\\u%04x", c);
                        out_ += buf;
                    } else {
                        out_ += c;
                    }
            }
        }
        out_ += '"';
    }

    std::string out_;
    bool first_ = true;
    bool after_key_ = false;
};

namespace detail {
// Position just after `"key":` (skipping whitespace), or npos.
inline size_t value_pos(std::string_view doc, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    size_t p = doc.find(needle);
    while (p != std::string_view::npos) {
        size_t q = p + needle.size();
        while (q < doc.size() && (doc[q] == ' ' || doc[q] == '\t')) ++q;
        if (q < doc.size() && doc[q] == ':') {
            ++q;
            while (q < doc.size() && (doc[q] == ' ' || doc[q] == '\t')) ++q;
            return q;
        }
        p = doc.find(needle, p + 1);
    }
    return std::string_view::npos;
}
}  // namespace detail

inline std::optional<std::string> get_string(std::string_view doc, std::string_view key) {
    size_t p = detail::value_pos(doc, key);
    if (p == std::string_view::npos || p >= doc.size() || doc[p] != '"') return std::nullopt;
    std::string out;
    for (++p; p < doc.size() && doc[p] != '"'; ++p) {
        if (doc[p] == '\\' && p + 1 < doc.size()) {
            ++p;
            switch (doc[p]) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                default: out += doc[p];
            }
        } else {
            out += doc[p];
        }
    }
    return out;
}

inline std::optional<int64_t> get_int(std::string_view doc, std::string_view key) {
    size_t p = detail::value_pos(doc, key);
    if (p == std::string_view::npos) return std::nullopt;
    bool neg = p < doc.size() && doc[p] == '-';
    if (neg) ++p;
    if (p >= doc.size() || doc[p] < '0' || doc[p] > '9') return std::nullopt;
    int64_t v = 0;
    for (; p < doc.size() && doc[p] >= '0' && doc[p] <= '9'; ++p) v = v * 10 + (doc[p] - '0');
    return neg ? -v : v;
}

inline std::optional<double> get_number(std::string_view doc, std::string_view key) {
    size_t p = detail::value_pos(doc, key);
    if (p == std::string_view::npos) return std::nullopt;
    size_t end = p;
    while (end < doc.size() && (std::isdigit(static_cast<unsigned char>(doc[end])) || doc[end] == '-' ||
                                doc[end] == '.' || doc[end] == 'e' || doc[end] == 'E' || doc[end] == '+'))
        ++end;
    if (end == p) return std::nullopt;
    try {
        return std::stod(std::string(doc.substr(p, end - p)));
    } catch (...) {
        return std::nullopt;
    }
}

inline std::optional<bool> get_bool(std::string_view doc, std::string_view key) {
    size_t p = detail::value_pos(doc, key);
    if (p == std::string_view::npos) return std::nullopt;
    if (doc.substr(p, 4) == "true") return true;
    if (doc.substr(p, 5) == "false") return false;
    return std::nullopt;
}

}  // namespace dm::json
