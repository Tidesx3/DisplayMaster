#include "capture/video_converter.h"

#include "core/log.h"

namespace dm {

bool VideoConverter::init(ID3D11Device* device, uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h,
                          RectI dest_rect) {
    device_ = device;
    in_w_ = in_w;
    in_h_ = in_h;
    out_w_ = out_w;
    out_h_ = out_h;
    in_views_.clear();
    out_views_.clear();

    ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&vdev_))) || FAILED(ctx.As(&vctx_))) {
        DM_LOGE("Converter: device has no video support");
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = in_w;
    cd.InputHeight = in_h;
    cd.OutputWidth = out_w;
    cd.OutputHeight = out_h;
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    HRESULT hr = vdev_->CreateVideoProcessorEnumerator(&cd, &enum_);
    if (SUCCEEDED(hr)) hr = vdev_->CreateVideoProcessor(enum_.Get(), 0, &vp_);
    if (FAILED(hr)) {
        DM_LOGE("Converter: CreateVideoProcessor failed %s", hr_string(hr).c_str());
        return false;
    }

    // Desktop is full-range sRGB-ish RGB; encoders expect BT.709 studio range.
    vctx_->VideoProcessorSetStreamColorSpace1(vp_.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    vctx_->VideoProcessorSetOutputColorSpace1(vp_.Get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    vctx_->VideoProcessorSetStreamAutoProcessingMode(vp_.Get(), 0, FALSE);
    vctx_->VideoProcessorSetStreamFrameFormat(vp_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);

    const RECT src{0, 0, static_cast<LONG>(in_w), static_cast<LONG>(in_h)};
    const RECT dst{dest_rect.x, dest_rect.y, dest_rect.x + dest_rect.w, dest_rect.y + dest_rect.h};
    const RECT target{0, 0, static_cast<LONG>(out_w), static_cast<LONG>(out_h)};
    vctx_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, &src);
    vctx_->VideoProcessorSetStreamDestRect(vp_.Get(), 0, TRUE, &dst);
    vctx_->VideoProcessorSetOutputTargetRect(vp_.Get(), TRUE, &target);
    // Letterbox bars: black in YCbCr.
    D3D11_VIDEO_COLOR black{};
    black.YCbCr = {0.0625f, 0.5f, 0.5f, 1.0f};
    vctx_->VideoProcessorSetOutputBackgroundColor(vp_.Get(), TRUE, &black);
    return true;
}

ComPtr<ID3D11Texture2D> VideoConverter::create_input_texture() const {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = in_w_;
    d.Height = in_h_;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> t;
    device_->CreateTexture2D(&d, nullptr, &t);
    return t;
}

ComPtr<ID3D11Texture2D> VideoConverter::create_output_texture() const {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = out_w_;
    d.Height = out_h_;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_NV12;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> t;
    device_->CreateTexture2D(&d, nullptr, &t);
    return t;
}

ID3D11VideoProcessorOutputView* VideoConverter::output_view(ID3D11Texture2D* tex) {
    auto it = out_views_.find(tex);
    if (it != out_views_.end()) return it->second.Get();
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od{};
    od.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorOutputView> v;
    if (FAILED(vdev_->CreateVideoProcessorOutputView(tex, enum_.Get(), &od, &v))) return nullptr;
    return out_views_.emplace(tex, v).first->second.Get();
}

bool VideoConverter::convert(ID3D11Texture2D* in_bgra, ID3D11Texture2D* out_nv12) {
    auto it = in_views_.find(in_bgra);
    if (it == in_views_.end()) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC id{};
        id.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11VideoProcessorInputView> v;
        HRESULT hr = vdev_->CreateVideoProcessorInputView(in_bgra, enum_.Get(), &id, &v);
        if (FAILED(hr)) {
            DM_LOGE("Converter: input view failed %s", hr_string(hr).c_str());
            return false;
        }
        it = in_views_.emplace(in_bgra, v).first;
    }
    auto* ov = output_view(out_nv12);
    if (!ov) return false;

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = it->second.Get();
    HRESULT hr = vctx_->VideoProcessorBlt(vp_.Get(), ov, 0, 1, &stream);
    if (FAILED(hr)) {
        DM_LOGE("Converter: VideoProcessorBlt failed %s", hr_string(hr).c_str());
        return false;
    }
    return true;
}

}  // namespace dm
