#include <dxgi.h>

#include "core/log.h"
#include "core/win.h"
#include "encode/encoder.h"

namespace dm {
namespace {

constexpr UINT kVendorNvidia = 0x10DE;
constexpr UINT kVendorAmd = 0x1002;

UINT vendor_of(ID3D11Device* device) {
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter)))
        adapter->GetDesc(&desc);
    return desc.VendorId;
}

}  // namespace

const char* codec_name(proto::Codec c) {
    switch (c) {
        case proto::Codec::H264: return "H.264";
        case proto::Codec::HEVC: return "HEVC";
        case proto::Codec::AV1: return "AV1";
    }
    return "?";
}

std::unique_ptr<IEncoder> create_encoder(ID3D11Device* device, const EncoderConfig& cfg, EncoderBackend pref) {
    using Factory = std::unique_ptr<IEncoder> (*)();
    std::vector<Factory> order;
    switch (pref) {
        case EncoderBackend::Nvenc: order = {create_nvenc_encoder}; break;
        case EncoderBackend::Amf: order = {create_amf_encoder}; break;
        case EncoderBackend::Auto:
            // Encoders must run on the GPU that owns the captured texture.
            if (vendor_of(device) == kVendorAmd)
                order = {create_amf_encoder, create_nvenc_encoder};
            else
                order = {create_nvenc_encoder, create_amf_encoder};
            break;
    }
    for (auto make : order) {
        auto enc = make();
        if (enc->init(device, cfg)) {
            DM_LOGI("Encoder: %s %s %ux%u@%u %u kbps", enc->name(), codec_name(cfg.codec), cfg.width, cfg.height,
                    cfg.fps, cfg.bitrate_kbps);
            return enc;
        }
    }
    DM_LOGE("No hardware encoder for %s %ux%u on this GPU", codec_name(cfg.codec), cfg.width, cfg.height);
    return nullptr;
}

uint32_t probe_encodable_codecs(ID3D11Device* device, EncoderBackend pref) {
    uint32_t mask = 0;
    for (auto c : {proto::Codec::H264, proto::Codec::HEVC, proto::Codec::AV1}) {
        EncoderConfig cfg{c, 1280, 720, 60, 5000};
        auto enc = create_encoder(device, cfg, pref);
        if (enc) mask |= proto::codec_bit(c);
    }
    return mask;
}

}  // namespace dm
