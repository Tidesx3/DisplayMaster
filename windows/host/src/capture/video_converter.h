// GPU color conversion + scaling (BGRA desktop -> NV12 BT.709 limited range) using
// the D3D11 video processor, with optional letterboxing for mirror mode.
#pragma once

#include <d3d11_1.h>

#include <cstdint>
#include <unordered_map>

#include "core/win.h"
#include "dm/input_math.h"

namespace dm {

class VideoConverter {
public:
    // dest_rect: where the input lands inside the output (pixels); the rest is black.
    bool init(ID3D11Device* device, uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h, RectI dest_rect);
    bool convert(ID3D11Texture2D* in_bgra, ID3D11Texture2D* out_nv12);

    // Helpers to create matching textures on the same device.
    ComPtr<ID3D11Texture2D> create_input_texture() const;
    ComPtr<ID3D11Texture2D> create_output_texture() const;

private:
    ID3D11VideoProcessorOutputView* output_view(ID3D11Texture2D* tex);

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11VideoDevice> vdev_;
    ComPtr<ID3D11VideoContext1> vctx_;
    ComPtr<ID3D11VideoProcessorEnumerator> enum_;
    ComPtr<ID3D11VideoProcessor> vp_;
    std::unordered_map<ID3D11Texture2D*, ComPtr<ID3D11VideoProcessorInputView>> in_views_;
    std::unordered_map<ID3D11Texture2D*, ComPtr<ID3D11VideoProcessorOutputView>> out_views_;
    uint32_t in_w_ = 0, in_h_ = 0, out_w_ = 0, out_h_ = 0;
};

}  // namespace dm
