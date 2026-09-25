// AMD AMF encoder via amfrt64.dll (ships with the Radeon driver). Used for the
// Radeon 780M iGPU on the Zephyrus G14 and any other AMD GPU.
#include <windows.h>

#include <chrono>
#include <thread>

#include "components/VideoEncoderAV1.h"
#include "components/VideoEncoderHEVC.h"
#include "components/VideoEncoderVCE.h"
#include "core/Factory.h"
#include "core/log.h"
#include "core/win.h"
#include "encode/encoder.h"

namespace dm {
namespace {

// Per-codec property names (AMF prefixes every property with the codec).
struct AmfProps {
    const wchar_t* component;
    const wchar_t* usage;
    amf_int64 usage_ull;
    const wchar_t* target_bitrate;
    const wchar_t* peak_bitrate;
    const wchar_t* vbv;
    const wchar_t* rc_method;
    amf_int64 rc_cbr;
    const wchar_t* preset;
    amf_int64 preset_speed;
    const wchar_t* framesize;
    const wchar_t* framerate;
    const wchar_t* color_profile;
    const wchar_t* transfer;
    const wchar_t* primaries;
    const wchar_t* query_timeout;
};

const AmfProps kAvc{AMFVideoEncoderVCE_AVC,
                    AMF_VIDEO_ENCODER_USAGE,
                    AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY,
                    AMF_VIDEO_ENCODER_TARGET_BITRATE,
                    AMF_VIDEO_ENCODER_PEAK_BITRATE,
                    AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE,
                    AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
                    AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR,
                    AMF_VIDEO_ENCODER_QUALITY_PRESET,
                    AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED,
                    AMF_VIDEO_ENCODER_FRAMESIZE,
                    AMF_VIDEO_ENCODER_FRAMERATE,
                    AMF_VIDEO_ENCODER_OUTPUT_COLOR_PROFILE,
                    AMF_VIDEO_ENCODER_OUTPUT_TRANSFER_CHARACTERISTIC,
                    AMF_VIDEO_ENCODER_OUTPUT_COLOR_PRIMARIES,
                    AMF_VIDEO_ENCODER_QUERY_TIMEOUT};

const AmfProps kHevc{AMFVideoEncoder_HEVC,
                     AMF_VIDEO_ENCODER_HEVC_USAGE,
                     AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY,
                     AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE,
                     AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE,
                     AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE,
                     AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD,
                     AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR,
                     AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET,
                     AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED,
                     AMF_VIDEO_ENCODER_HEVC_FRAMESIZE,
                     AMF_VIDEO_ENCODER_HEVC_FRAMERATE,
                     AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PROFILE,
                     AMF_VIDEO_ENCODER_HEVC_OUTPUT_TRANSFER_CHARACTERISTIC,
                     AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PRIMARIES,
                     AMF_VIDEO_ENCODER_HEVC_QUERY_TIMEOUT};

const AmfProps kAv1{AMFVideoEncoder_AV1,
                    AMF_VIDEO_ENCODER_AV1_USAGE,
                    AMF_VIDEO_ENCODER_AV1_USAGE_ULTRA_LOW_LATENCY,
                    AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE,
                    AMF_VIDEO_ENCODER_AV1_PEAK_BITRATE,
                    AMF_VIDEO_ENCODER_AV1_VBV_BUFFER_SIZE,
                    AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD,
                    AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CBR,
                    AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET,
                    AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_SPEED,
                    AMF_VIDEO_ENCODER_AV1_FRAMESIZE,
                    AMF_VIDEO_ENCODER_AV1_FRAMERATE,
                    AMF_VIDEO_ENCODER_AV1_OUTPUT_COLOR_PROFILE,
                    AMF_VIDEO_ENCODER_AV1_OUTPUT_TRANSFER_CHARACTERISTIC,
                    AMF_VIDEO_ENCODER_AV1_OUTPUT_COLOR_PRIMARIES,
                    AMF_VIDEO_ENCODER_AV1_QUERY_TIMEOUT};

class AmfEncoder final : public IEncoder {
public:
    ~AmfEncoder() override {
        if (enc_) enc_->Terminate();
        enc_ = nullptr;
        if (ctx_) ctx_->Terminate();
        ctx_ = nullptr;
        if (lib_) FreeLibrary(lib_);
    }
    const char* name() const override { return "AMF"; }

    bool init(ID3D11Device* device, const EncoderConfig& cfg) override {
        cfg_ = cfg;
        switch (cfg.codec) {
            case proto::Codec::H264: p_ = &kAvc; break;
            case proto::Codec::HEVC: p_ = &kHevc; break;
            case proto::Codec::AV1: p_ = &kAv1; break;
            default: return false;
        }
        lib_ = LoadLibraryW(AMF_DLL_NAME);
        if (!lib_) return false;
        auto init_fn = reinterpret_cast<AMFInit_Fn>(GetProcAddress(lib_, AMF_INIT_FUNCTION_NAME));
        if (!init_fn || init_fn(AMF_FULL_VERSION, &factory_) != AMF_OK) return false;
        if (factory_->CreateContext(&ctx_) != AMF_OK) return false;
        if (ctx_->InitDX11(device, amf::AMF_DX11_1) != AMF_OK) return false;  // fails on non-AMD devices
        if (factory_->CreateComponent(ctx_, p_->component, &enc_) != AMF_OK) {
            DM_LOGW("AMF: %s not supported on this GPU", codec_name(cfg.codec));
            return false;
        }

        // Static properties (before Init).
        enc_->SetProperty(p_->usage, p_->usage_ull);
        enc_->SetProperty(p_->framesize, ::AMFConstructSize(cfg.width, cfg.height));
        enc_->SetProperty(p_->framerate, ::AMFConstructRate(cfg.fps, 1));
        enc_->SetProperty(p_->preset, p_->preset_speed);
        enc_->SetProperty(p_->rc_method, p_->rc_cbr);
        enc_->SetProperty(p_->color_profile, static_cast<amf_int64>(AMF_VIDEO_CONVERTER_COLOR_PROFILE_709));
        enc_->SetProperty(p_->transfer, static_cast<amf_int64>(AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709));
        enc_->SetProperty(p_->primaries, static_cast<amf_int64>(AMF_COLOR_PRIMARIES_BT709));
        // QueryOutput blocks until the frame is done (woken by the driver) instead of us polling.
        enc_->SetProperty(p_->query_timeout, static_cast<amf_int64>(50));
        switch (cfg.codec) {
            case proto::Codec::H264:
                enc_->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, static_cast<amf_int64>(0));
                enc_->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, static_cast<amf_int64>(0));  // first frame only
                enc_->SetProperty(AMF_VIDEO_ENCODER_LOWLATENCY_MODE, true);
                break;
            case proto::Codec::HEVC:
                enc_->SetProperty(AMF_VIDEO_ENCODER_HEVC_NOMINAL_RANGE,
                                  static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_NOMINAL_RANGE_STUDIO));
                enc_->SetProperty(AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, static_cast<amf_int64>(0));
                enc_->SetProperty(AMF_VIDEO_ENCODER_HEVC_NUM_GOPS_PER_IDR, static_cast<amf_int64>(0));
                enc_->SetProperty(AMF_VIDEO_ENCODER_HEVC_LOWLATENCY_MODE, true);
                break;
            case proto::Codec::AV1:
                enc_->SetProperty(AMF_VIDEO_ENCODER_AV1_GOP_SIZE, static_cast<amf_int64>(0));
                enc_->SetProperty(AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE,
                                  static_cast<amf_int64>(AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE_LOWEST_LATENCY));
                // Encode exactly the requested size (AV1 otherwise pads to 64x16).
                enc_->SetProperty(AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE,
                                  static_cast<amf_int64>(AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE_NO_RESTRICTIONS));
                break;
        }
        set_bitrate_props(cfg.bitrate_kbps);

        const AMF_RESULT r = enc_->Init(amf::AMF_SURFACE_NV12, cfg.width, cfg.height);
        if (r != AMF_OK) {
            DM_LOGW("AMF: Init(%s %ux%u) failed %d", codec_name(cfg.codec), cfg.width, cfg.height, r);
            return false;
        }
        return true;
    }

    bool encode(ID3D11Texture2D* nv12, bool force_keyframe, EncodedPacket& out) override {
        amf::AMFSurfacePtr surf;
        if (ctx_->CreateSurfaceFromDX11Native(nv12, &surf, nullptr) != AMF_OK) return false;
        if (force_keyframe) {
            switch (cfg_.codec) {
                case proto::Codec::H264:
                    surf->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE,
                                      static_cast<amf_int64>(AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR));
                    surf->SetProperty(AMF_VIDEO_ENCODER_INSERT_SPS, true);
                    surf->SetProperty(AMF_VIDEO_ENCODER_INSERT_PPS, true);
                    break;
                case proto::Codec::HEVC:
                    surf->SetProperty(AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE,
                                      static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR));
                    surf->SetProperty(AMF_VIDEO_ENCODER_HEVC_INSERT_HEADER, true);
                    break;
                case proto::Codec::AV1:
                    surf->SetProperty(AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE,
                                      static_cast<amf_int64>(AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE_KEY));
                    surf->SetProperty(AMF_VIDEO_ENCODER_AV1_FORCE_INSERT_SEQUENCE_HEADER, true);
                    break;
            }
        }
        AMF_RESULT r = enc_->SubmitInput(surf);
        if (r != AMF_OK) {
            DM_LOGE("AMF: SubmitInput failed %d", r);
            return false;
        }
        // With QueryTimeout set this normally returns on the first call; the loop
        // covers older drivers that ignore it.
        amf::AMFDataPtr data;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while ((r = enc_->QueryOutput(&data)) == AMF_REPEAT || (r == AMF_OK && !data)) {
            if (std::chrono::steady_clock::now() > deadline) {
                DM_LOGE("AMF: timed out waiting for output");
                return false;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        if (r != AMF_OK) return false;

        amf::AMFBufferPtr buf(data);
        const auto* p = static_cast<const uint8_t*>(buf->GetNative());
        out.data.assign(p, p + buf->GetSize());
        amf_int64 type = -1;
        switch (cfg_.codec) {
            case proto::Codec::H264:
                buf->GetProperty(AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &type);
                out.keyframe = type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR;
                break;
            case proto::Codec::HEVC:
                buf->GetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE, &type);
                out.keyframe = type == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR;
                break;
            case proto::Codec::AV1:
                buf->GetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE, &type);
                out.keyframe = type == AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE_KEY;
                break;
        }
        return true;
    }

    bool set_bitrate(uint32_t kbps) override {
        set_bitrate_props(kbps);
        return true;
    }

private:
    void set_bitrate_props(uint32_t kbps) {
        const amf_int64 bps = static_cast<amf_int64>(kbps) * 1000;
        enc_->SetProperty(p_->target_bitrate, bps);
        enc_->SetProperty(p_->peak_bitrate, bps);
        enc_->SetProperty(p_->vbv, bps / std::max(1u, cfg_.fps));  // one-frame VBV, see NVENC
    }

    HMODULE lib_ = nullptr;
    amf::AMFFactory* factory_ = nullptr;
    amf::AMFContextPtr ctx_;
    amf::AMFComponentPtr enc_;
    const AmfProps* p_ = nullptr;
    EncoderConfig cfg_;
};

}  // namespace

std::unique_ptr<IEncoder> create_amf_encoder() { return std::make_unique<AmfEncoder>(); }

}  // namespace dm
