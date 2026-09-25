// Small Win32/COM helpers shared across the host.
#pragma once

#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace dm {

using Microsoft::WRL::ComPtr;

std::string to_utf8(std::wstring_view w);
std::wstring to_wide(std::string_view s);
std::string hr_string(HRESULT hr);

// Monotonic microseconds (QueryPerformanceCounter).
uint64_t now_us();

// Sleep with ~0.5 ms accuracy (plain Sleep() rounds up to the 15.6 ms timer tick).
void precise_sleep_us(uint64_t us);

// RAII for Win32 HANDLEs that use CloseHandle.
class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE h) : h_(h) {}
    ~UniqueHandle() { reset(); }
    UniqueHandle(UniqueHandle&& o) noexcept : h_(o.release()) {}
    UniqueHandle& operator=(UniqueHandle&& o) noexcept {
        if (this != &o) reset(o.release());
        return *this;
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    HANDLE get() const { return h_; }
    bool valid() const { return h_ && h_ != INVALID_HANDLE_VALUE; }
    HANDLE release() {
        HANDLE h = h_;
        h_ = nullptr;
        return h;
    }
    void reset(HANDLE h = nullptr) {
        if (valid()) CloseHandle(h_);
        h_ = h;
    }

private:
    HANDLE h_ = nullptr;
};

}  // namespace dm
