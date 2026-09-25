#include "capture/cursor_compositor.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

#include "core/log.h"

namespace dm {
namespace {

// Full-screen-triangle-strip style quad generated from SV_VertexID; rect is in NDC.
constexpr char kShader[] = R"(
cbuffer Rect : register(b0) { float4 rect; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_main(uint id : SV_VertexID) {
    float2 uv = float2(id & 1, id >> 1);
    VSOut o;
    o.pos = float4(lerp(rect.x, rect.z, uv.x), lerp(rect.y, rect.w, uv.y), 0, 1);
    o.uv = uv;
    return o;
}
Texture2D tex : register(t0);
SamplerState smp : register(s0);
float4 ps_main(VSOut i) : SV_Target { return tex.Sample(smp, i.uv); }
)";

ComPtr<ID3DBlob> compile(const char* entry, const char* target) {
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(kShader, sizeof kShader - 1, "cursor", nullptr, nullptr, entry, target,
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
        DM_LOGE("Cursor shader: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return nullptr;
    }
    return code;
}

constexpr uint32_t kOpaqueBlack = 0xFF000000u;
constexpr uint32_t kOpaqueWhite = 0xFFFFFFFFu;

}  // namespace

bool CursorCompositor::init(ID3D11Device* device) {
    device_ = device;
    device->GetImmediateContext(&ctx_);
    auto vs = compile("vs_main", "vs_4_0");
    auto ps = compile("ps_main", "ps_4_0");
    if (!vs || !ps) return false;
    device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_);
    device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &ps_);

    D3D11_BUFFER_DESC bd{16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    device->CreateBuffer(&bd, nullptr, &cb_);

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;  // pixel-exact cursor
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device->CreateSamplerState(&sd, &sampler_);

    D3D11_BLEND_DESC blend{};
    auto& rt = blend.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    // Premultiplied alpha over the desktop.
    rt.SrcBlend = D3D11_BLEND_ONE;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    device->CreateBlendState(&blend, &blend_alpha_);
    // XOR-style inversion: src white -> 1 - dst, src black -> dst.
    rt.SrcBlend = D3D11_BLEND_INV_DEST_COLOR;
    rt.DestBlend = D3D11_BLEND_INV_SRC_COLOR;
    rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
    rt.DestBlendAlpha = D3D11_BLEND_ONE;
    device->CreateBlendState(&blend, &blend_invert_);
    return vs_ && ps_ && cb_ && sampler_ && blend_alpha_ && blend_invert_;
}

void CursorCompositor::convert_shape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info, const std::vector<uint8_t>& buf,
                                     std::vector<uint32_t>& color, std::vector<uint32_t>& invert, uint32_t& w,
                                     uint32_t& h) {
    w = info.Width;
    // Monochrome shapes stack the AND mask on top of the XOR mask: real height is half.
    h = info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME ? info.Height / 2 : info.Height;
    color.assign(static_cast<size_t>(w) * h, 0);
    invert.assign(static_cast<size_t>(w) * h, kOpaqueBlack);

    auto px32 = [&](uint32_t x, uint32_t y) {
        uint32_t v;
        std::memcpy(&v, buf.data() + y * info.Pitch + x * 4, 4);
        return v;
    };
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            switch (info.Type) {
                case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME: {
                    const uint8_t bit = static_cast<uint8_t>(0x80 >> (x % 8));
                    const bool and_bit = buf[y * info.Pitch + x / 8] & bit;
                    const bool xor_bit = buf[(y + h) * info.Pitch + x / 8] & bit;
                    if (!and_bit) color[i] = xor_bit ? kOpaqueWhite : kOpaqueBlack;
                    else if (xor_bit) invert[i] = kOpaqueWhite;  // screen-inverting pixel
                    break;
                }
                case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR: {
                    // Straight alpha BGRA -> premultiplied.
                    const uint32_t v = px32(x, y);
                    const uint32_t a = v >> 24;
                    auto pm = [&](uint32_t c) { return (c * a + 127) / 255; };
                    color[i] = (a << 24) | (pm((v >> 16) & 0xFF) << 16) | (pm((v >> 8) & 0xFF) << 8) | pm(v & 0xFF);
                    break;
                }
                case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR: {
                    // Alpha 0: replace with RGB. Alpha 0xFF: XOR RGB onto the screen
                    // (approximated: black = no-op, anything else = invert).
                    const uint32_t v = px32(x, y);
                    if ((v >> 24) == 0) color[i] = 0xFF000000u | (v & 0xFFFFFF);
                    else if (v & 0xFFFFFF) invert[i] = kOpaqueWhite;
                    break;
                }
                default: break;
            }
        }
    }
}

ComPtr<ID3D11ShaderResourceView> CursorCompositor::upload(const std::vector<uint32_t>& pixels, uint32_t w,
                                                          uint32_t h) {
    D3D11_TEXTURE2D_DESC td{w, h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE,
                            D3D11_BIND_SHADER_RESOURCE, 0, 0};
    D3D11_SUBRESOURCE_DATA init{pixels.data(), w * 4, 0};
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (SUCCEEDED(device_->CreateTexture2D(&td, &init, &tex))) device_->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    return srv;
}

void CursorCompositor::set_shape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info, const std::vector<uint8_t>& buffer) {
    std::vector<uint32_t> color, invert;
    convert_shape(info, buffer, color, invert, w_, h_);
    if (!w_ || !h_) return;
    color_srv_ = upload(color, w_, h_);
    has_invert_ = std::any_of(invert.begin(), invert.end(), [](uint32_t v) { return v == kOpaqueWhite; });
    invert_srv_ = has_invert_ ? upload(invert, w_, h_) : nullptr;
    has_shape_ = color_srv_ != nullptr;
}

void CursorCompositor::set_position(bool visible, POINT top_left) {
    visible_ = visible;
    pos_ = top_left;
}

ID3D11RenderTargetView* CursorCompositor::rtv(ID3D11Texture2D* target) {
    if (target != rtv_target_) {
        rtv_.Reset();
        device_->CreateRenderTargetView(target, nullptr, &rtv_);
        rtv_target_ = target;
    }
    return rtv_.Get();
}

void CursorCompositor::draw(ID3D11Texture2D* target, uint32_t tw, uint32_t th) {
    if (!visible() || !tw || !th) return;
    auto* view = rtv(target);
    if (!view) return;

    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    const float rect[4] = {
        static_cast<float>(pos_.x) / tw * 2 - 1,
        1 - static_cast<float>(pos_.y) / th * 2,
        static_cast<float>(pos_.x + static_cast<LONG>(w_)) / tw * 2 - 1,
        1 - static_cast<float>(pos_.y + static_cast<LONG>(h_)) / th * 2,
    };
    std::memcpy(m.pData, rect, sizeof rect);
    ctx_->Unmap(cb_.Get(), 0);

    D3D11_VIEWPORT vp{0, 0, static_cast<float>(tw), static_cast<float>(th), 0, 1};
    ctx_->RSSetViewports(1, &vp);
    ctx_->OMSetRenderTargets(1, &view, nullptr);
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->VSSetConstantBuffers(0, 1, cb_.GetAddressOf());
    ctx_->PSSetShader(ps_.Get(), nullptr, 0);
    ctx_->PSSetSamplers(0, 1, sampler_.GetAddressOf());

    const float factor[4] = {};
    ctx_->OMSetBlendState(blend_alpha_.Get(), factor, 0xFFFFFFFF);
    ctx_->PSSetShaderResources(0, 1, color_srv_.GetAddressOf());
    ctx_->Draw(4, 0);
    if (has_invert_) {
        ctx_->OMSetBlendState(blend_invert_.Get(), factor, 0xFFFFFFFF);
        ctx_->PSSetShaderResources(0, 1, invert_srv_.GetAddressOf());
        ctx_->Draw(4, 0);
    }
    ID3D11ShaderResourceView* null_srv = nullptr;
    ctx_->PSSetShaderResources(0, 1, &null_srv);
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
}

}  // namespace dm
