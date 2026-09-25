// Desktop Duplication delivers the desktop image *without* the mouse pointer; the
// pointer shape and position arrive separately. This draws the pointer back onto
// the captured frame on the GPU, including inverting (XOR) monochrome cursors like
// the text I-beam.
#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdint>
#include <vector>

#include "core/win.h"

namespace dm {

class CursorCompositor {
public:
    bool init(ID3D11Device* device);

    void set_shape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info, const std::vector<uint8_t>& buffer);
    void set_position(bool visible, POINT top_left);
    bool visible() const { return visible_ && has_shape_; }

    // Draw onto `target` (BGRA render target of the output's size).
    void draw(ID3D11Texture2D* target, uint32_t target_w, uint32_t target_h);

    // Exposed for tests: convert a DXGI pointer shape into premultiplied BGRA
    // color + an inversion mask (white where destination pixels get inverted).
    static void convert_shape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info, const std::vector<uint8_t>& buffer,
                              std::vector<uint32_t>& color, std::vector<uint32_t>& invert, uint32_t& w, uint32_t& h);

private:
    ComPtr<ID3D11ShaderResourceView> upload(const std::vector<uint32_t>& pixels, uint32_t w, uint32_t h);
    ID3D11RenderTargetView* rtv(ID3D11Texture2D* target);

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> cb_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11BlendState> blend_alpha_, blend_invert_;
    ComPtr<ID3D11ShaderResourceView> color_srv_, invert_srv_;
    ID3D11Texture2D* rtv_target_ = nullptr;
    ComPtr<ID3D11RenderTargetView> rtv_;
    uint32_t w_ = 0, h_ = 0;
    bool has_shape_ = false, has_invert_ = false, visible_ = false;
    POINT pos_{};
};

}  // namespace dm
