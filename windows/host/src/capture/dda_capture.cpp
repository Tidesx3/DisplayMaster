#include "capture/dda_capture.h"

#include <d3d11_4.h>

#include <algorithm>

#include "core/log.h"

namespace dm {

bool DdaCapture::init(const std::wstring& gdi_name) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> out;
        for (UINT o = 0; adapter->EnumOutputs(o, &out) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc;
            out->GetDesc(&desc);
            if (gdi_name == desc.DeviceName) {
                adapter_ = adapter;
                out.As(&output_);
                width_ = static_cast<uint32_t>(desc.DesktopCoordinates.right - desc.DesktopCoordinates.left);
                height_ = static_cast<uint32_t>(desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top);
                rotation_ = desc.Rotation;
                break;
            }
        }
        if (output_) break;
    }
    if (!output_) {
        DM_LOGE("DDA: no DXGI output for %s", to_utf8(gdi_name).c_str());
        return false;
    }
    adapter_->GetDesc1(&adapter_desc_);

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    // VIDEO_SUPPORT: needed for the video processor (BGRA -> NV12) on this device.
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, nullptr, &context_);
    if (FAILED(hr)) {
        DM_LOGE("DDA: D3D11CreateDevice failed %s", hr_string(hr).c_str());
        return false;
    }
    // Encoders may use the device from another thread.
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(context_.As(&mt))) mt->SetMultithreadProtected(TRUE);

    DM_LOGI("DDA: %s on \"%s\" %ux%u", to_utf8(gdi_name).c_str(), to_utf8(adapter_desc_.Description).c_str(),
            width_, height_);
    if (!cursor_.init(device_.Get())) DM_LOGW("DDA: cursor compositing unavailable");
    // A failure here (e.g. the UAC secure desktop is up) is retried by capture().
    duplicate();
    return true;
}

bool DdaCapture::duplicate() {
    dup_.Reset();
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
    HRESULT hr = output_->DuplicateOutput1(device_.Get(), 0, ARRAYSIZE(formats), formats, &dup_);
    if (FAILED(hr)) {
        // E_ACCESSDENIED: secure desktop (UAC / lock screen) is showing.
        if (hr != last_dup_error_) DM_LOGW("DDA: DuplicateOutput1 failed %s", hr_string(hr).c_str());
        last_dup_error_ = hr;
        return false;
    }
    last_dup_error_ = S_OK;
    // After a mode change the output has a new size: callers must rebuild their textures.
    DXGI_OUTPUT_DESC desc;
    output_->GetDesc(&desc);
    size_changed_ = static_cast<uint32_t>(desc.DesktopCoordinates.right - desc.DesktopCoordinates.left) != width_ ||
                    static_cast<uint32_t>(desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top) != height_;
    return true;
}

DdaCapture::Result DdaCapture::capture(uint32_t timeout_ms, ID3D11Texture2D* copy_to) {
    if (!dup_) {
        if (!duplicate()) {
            // Temporarily unavailable (secure desktop): no new frames, try again later.
            Sleep(std::min<uint32_t>(timeout_ms, 100));
            return Result::Timeout;
        }
    }
    if (size_changed_) return Result::Lost;
    if (holding_frame_) {
        dup_->ReleaseFrame();
        holding_frame_ = false;
    }

    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> res;
    HRESULT hr = dup_->AcquireNextFrame(timeout_ms, &info, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return Result::Timeout;
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
        dup_.Reset();
        return Result::Lost;
    }
    if (FAILED(hr)) {
        DM_LOGE("DDA: AcquireNextFrame failed %s", hr_string(hr).c_str());
        dup_.Reset();
        return Result::Error;
    }
    holding_frame_ = true;

    bool pointer_changed = false;
    if (info.LastMouseUpdateTime.QuadPart != 0) {
        cursor_.set_position(info.PointerPosition.Visible, info.PointerPosition.Position);
        pointer_changed = true;
    }
    if (info.PointerShapeBufferSize > 0) {
        shape_buf_.resize(info.PointerShapeBufferSize);
        DXGI_OUTDUPL_POINTER_SHAPE_INFO shape{};
        UINT needed = 0;
        if (SUCCEEDED(dup_->GetFramePointerShape(static_cast<UINT>(shape_buf_.size()), shape_buf_.data(), &needed,
                                                 &shape))) {
            cursor_.set_shape(shape, shape_buf_);
            pointer_changed = true;
        }
    }

    // Pointer-only update: the desktop image didn't change.
    if (info.LastPresentTime.QuadPart == 0) return pointer_changed ? Result::Pointer : Result::Timeout;

    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(res.As(&tex))) return Result::Error;
    context_->CopyResource(copy_to, tex.Get());
    // Release right away so DWM can keep composing while we encode.
    dup_->ReleaseFrame();
    holding_frame_ = false;
    return Result::Frame;
}

}  // namespace dm
