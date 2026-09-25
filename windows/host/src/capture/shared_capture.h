// One Desktop Duplication per monitor, shared by every session that shows it.
//
// DXGI allows a single duplication of an output per process, so several devices
// mirroring the same screen must share it. Whichever session asks first performs
// the capture (and composites the cursor once); the others wait for that frame.
// Each session then copies the composed image into its own texture on the shared
// D3D device and scales/encodes it independently.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "capture/dda_capture.h"

namespace dm {

class SharedCapture {
public:
    enum class Result { Frame, Timeout, Lost };

    // The capture for a monitor, created on first use; nullptr if it can't be duplicated.
    static std::shared_ptr<SharedCapture> get(const std::wstring& gdi_name);

    // Waits up to timeout_ms for a frame newer than `seen` and copies it into `dst`
    // (a BGRA texture of width() x height() on device()). Updates `seen`.
    Result next(uint64_t& seen, uint32_t timeout_ms, ID3D11Texture2D* dst);

    ID3D11Device* device() const { return capture_.device(); }
    uint32_t width() const { return capture_.width(); }
    uint32_t height() const { return capture_.height(); }
    std::string adapter_name() const;

private:
    bool init(const std::wstring& gdi_name);

    DdaCapture capture_;
    ComPtr<ID3D11Texture2D> desktop_;   // last desktop image
    ComPtr<ID3D11Texture2D> composed_;  // desktop + pointer, what sessions copy
    std::mutex mu_;
    std::condition_variable cv_;
    uint64_t version_ = 0;
    bool capturing_ = false;
    bool have_desktop_ = false;
    bool lost_ = false;
    std::wstring gdi_name_;
};

}  // namespace dm
