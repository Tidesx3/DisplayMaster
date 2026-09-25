// DXGI Desktop Duplication capture of one monitor. The D3D11 device is created
// on the adapter that owns the output, so frames never cross GPUs (important on
// hybrid laptops like the G14: capturing an iGPU-driven screen must not wake the dGPU).
#pragma once

#include <d3d11.h>
#include <dxgi1_6.h>

#include <cstdint>
#include <string>
#include <vector>

#include "capture/cursor_compositor.h"
#include "core/win.h"

namespace dm {

class DdaCapture {
public:
    // Frame: new desktop image. Pointer: only the mouse pointer moved/changed.
    enum class Result { Frame, Pointer, Timeout, Lost, Error };

    // gdi_name: "\\.\DISPLAYn". Creates its own device on the owning adapter.
    bool init(const std::wstring& gdi_name);

    // Waits up to timeout_ms for a desktop or pointer update. On Frame, `copy_to`
    // (a BGRA texture of the output's size on this device) receives the image;
    // pointer updates go to cursor().
    Result capture(uint32_t timeout_ms, ID3D11Texture2D* copy_to);

    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return context_.Get(); }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    DXGI_ADAPTER_DESC1 adapter_desc() const { return adapter_desc_; }
    DXGI_MODE_ROTATION rotation() const { return rotation_; }
    CursorCompositor& cursor() { return cursor_; }

private:
    bool duplicate();

    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<IDXGIOutput5> output_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutputDuplication> dup_;
    DXGI_ADAPTER_DESC1 adapter_desc_{};
    DXGI_MODE_ROTATION rotation_ = DXGI_MODE_ROTATION_IDENTITY;
    uint32_t width_ = 0, height_ = 0;
    bool holding_frame_ = false;
    bool size_changed_ = false;
    HRESULT last_dup_error_ = S_OK;  // log each distinct failure once
    CursorCompositor cursor_;
    std::vector<uint8_t> shape_buf_;
};

}  // namespace dm
