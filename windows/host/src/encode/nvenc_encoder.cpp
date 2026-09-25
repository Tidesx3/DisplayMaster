// NVIDIA NVENC via nvEncodeAPI64.dll (loaded at runtime; ships with the driver).
#include <windows.h>

#include <unordered_map>

#include "core/log.h"
#include "core/win.h"
#include "encode/encoder.h"
#include "nvEncodeAPI.h"

namespace dm {
namespace {

class NvencEncoder final : public IEncoder {
public:
    ~NvencEncoder() override { shutdown(); }
    const char* name() const override { return "NVENC"; }

    bool init(ID3D11Device* device, const EncoderConfig& cfg) override {
        cfg_ = cfg;
        lib_ = LoadLibraryW(L"nvEncodeAPI64.dll");
        if (!lib_) return false;
        using CreateFn = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
        using MaxVerFn = NVENCSTATUS(NVENCAPI*)(uint32_t*);
        auto create = reinterpret_cast<CreateFn>(GetProcAddress(lib_, "NvEncodeAPICreateInstance"));
        auto max_ver = reinterpret_cast<MaxVerFn>(GetProcAddress(lib_, "NvEncodeAPIGetMaxSupportedVersion"));
        if (!create || !max_ver) return false;

        uint32_t driver_ver = 0;
        max_ver(&driver_ver);
        const uint32_t needed = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
        if (driver_ver < needed) {
            DM_LOGW("NVENC: driver supports API %u.%u, need %u.%u - update the NVIDIA driver", driver_ver >> 4,
                    driver_ver & 0xF, NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
            return false;
        }
        nv_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
        if (create(&nv_) != NV_ENC_SUCCESS) return false;

        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sp{};
        sp.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        sp.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        sp.device = device;
        sp.apiVersion = NVENCAPI_VERSION;
        if (nv_.nvEncOpenEncodeSessionEx(&sp, &enc_) != NV_ENC_SUCCESS) {
            enc_ = nullptr;  // not an NVIDIA device
            return false;
        }

        GUID codec_guid;
        switch (cfg.codec) {
            case proto::Codec::H264: codec_guid = NV_ENC_CODEC_H264_GUID; break;
            case proto::Codec::HEVC: codec_guid = NV_ENC_CODEC_HEVC_GUID; break;
            case proto::Codec::AV1: codec_guid = NV_ENC_CODEC_AV1_GUID; break;
            default: return false;
        }

        NV_ENC_PRESET_CONFIG preset{};
        preset.version = NV_ENC_PRESET_CONFIG_VER;
        preset.presetCfg.version = NV_ENC_CONFIG_VER;
        if (nv_.nvEncGetEncodePresetConfigEx(enc_, codec_guid, NV_ENC_PRESET_P1_GUID,
                                             NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset) != NV_ENC_SUCCESS) {
            DM_LOGW("NVENC: %s not supported", codec_name(cfg.codec));
            return false;
        }
        config_ = preset.presetCfg;
        config_.gopLength = NVENC_INFINITE_GOPLENGTH;  // keyframes only on request
        config_.frameIntervalP = 1;                    // no B-frames
        apply_rate_control(cfg.bitrate_kbps);

        switch (cfg.codec) {
            case proto::Codec::H264: {
                auto& h = config_.encodeCodecConfig.h264Config;
                h.idrPeriod = NVENC_INFINITE_GOPLENGTH;
                h.repeatSPSPPS = 1;
                set_vui(h.h264VUIParameters);
                break;
            }
            case proto::Codec::HEVC: {
                auto& h = config_.encodeCodecConfig.hevcConfig;
                h.idrPeriod = NVENC_INFINITE_GOPLENGTH;
                h.repeatSPSPPS = 1;
                set_vui(h.hevcVUIParameters);
                break;
            }
            case proto::Codec::AV1: {
                auto& a = config_.encodeCodecConfig.av1Config;
                a.idrPeriod = NVENC_INFINITE_GOPLENGTH;
                a.repeatSeqHdr = 1;
                a.chromaFormatIDC = 1;
                a.colorPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
                a.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
                a.matrixCoefficients = NV_ENC_VUI_MATRIX_COEFFS_BT709;
                a.colorRange = 0;
                break;
            }
        }

        init_params_ = {};
        init_params_.version = NV_ENC_INITIALIZE_PARAMS_VER;
        init_params_.encodeGUID = codec_guid;
        init_params_.presetGUID = NV_ENC_PRESET_P1_GUID;
        init_params_.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
        init_params_.encodeWidth = init_params_.darWidth = init_params_.maxEncodeWidth = cfg.width;
        init_params_.encodeHeight = init_params_.darHeight = init_params_.maxEncodeHeight = cfg.height;
        init_params_.frameRateNum = cfg.fps;
        init_params_.frameRateDen = 1;
        init_params_.enablePTD = 1;
        init_params_.encodeConfig = &config_;
        NVENCSTATUS st = nv_.nvEncInitializeEncoder(enc_, &init_params_);
        if (st != NV_ENC_SUCCESS) {
            DM_LOGW("NVENC: nvEncInitializeEncoder(%s %ux%u) failed %d", codec_name(cfg.codec), cfg.width,
                    cfg.height, st);
            return false;
        }

        NV_ENC_CREATE_BITSTREAM_BUFFER bb{};
        bb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        if (nv_.nvEncCreateBitstreamBuffer(enc_, &bb) != NV_ENC_SUCCESS) return false;
        bitstream_ = bb.bitstreamBuffer;
        return true;
    }

    bool encode(ID3D11Texture2D* nv12, bool force_keyframe, EncodedPacket& out) override {
        NV_ENC_REGISTERED_PTR reg = registered(nv12);
        if (!reg) return false;
        NV_ENC_MAP_INPUT_RESOURCE map{};
        map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        map.registeredResource = reg;
        if (nv_.nvEncMapInputResource(enc_, &map) != NV_ENC_SUCCESS) return false;

        NV_ENC_PIC_PARAMS pic{};
        pic.version = NV_ENC_PIC_PARAMS_VER;
        pic.inputWidth = cfg_.width;
        pic.inputHeight = cfg_.height;
        pic.inputBuffer = map.mappedResource;
        pic.bufferFmt = map.mappedBufferFmt;
        pic.outputBitstream = bitstream_;
        pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        pic.inputTimeStamp = frame_++;
        if (force_keyframe) pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;

        NVENCSTATUS st = nv_.nvEncEncodePicture(enc_, &pic);
        bool ok = st == NV_ENC_SUCCESS;
        if (ok) {
            NV_ENC_LOCK_BITSTREAM lock{};
            lock.version = NV_ENC_LOCK_BITSTREAM_VER;
            lock.outputBitstream = bitstream_;
            ok = nv_.nvEncLockBitstream(enc_, &lock) == NV_ENC_SUCCESS;
            if (ok) {
                const auto* p = static_cast<const uint8_t*>(lock.bitstreamBufferPtr);
                out.data.assign(p, p + lock.bitstreamSizeInBytes);
                out.keyframe = lock.pictureType == NV_ENC_PIC_TYPE_IDR || lock.pictureType == NV_ENC_PIC_TYPE_I;
                nv_.nvEncUnlockBitstream(enc_, bitstream_);
            }
        } else {
            DM_LOGE("NVENC: encode failed %d", st);
        }
        nv_.nvEncUnmapInputResource(enc_, map.mappedResource);
        return ok;
    }

    bool set_bitrate(uint32_t kbps) override {
        apply_rate_control(kbps);
        NV_ENC_RECONFIGURE_PARAMS rp{};
        rp.version = NV_ENC_RECONFIGURE_PARAMS_VER;
        rp.reInitEncodeParams = init_params_;
        rp.reInitEncodeParams.encodeConfig = &config_;
        return nv_.nvEncReconfigureEncoder(enc_, &rp) == NV_ENC_SUCCESS;
    }

private:
    void apply_rate_control(uint32_t kbps) {
        auto& rc = config_.rcParams;
        rc.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        rc.averageBitRate = kbps * 1000;
        rc.maxBitRate = kbps * 1000;
        // One frame of VBV: keeps every frame close to the average size, so no
        // frame takes much longer than a frame interval to transmit.
        rc.vbvBufferSize = rc.vbvInitialDelay = kbps * 1000 / std::max(1u, cfg_.fps);
        rc.enableAQ = 1;
    }

    static void set_vui(NV_ENC_CONFIG_H264_VUI_PARAMETERS& v) {
        v.videoSignalTypePresentFlag = 1;
        v.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
        v.videoFullRangeFlag = 0;
        v.colourDescriptionPresentFlag = 1;
        v.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
        v.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
        v.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
    }

    NV_ENC_REGISTERED_PTR registered(ID3D11Texture2D* tex) {
        auto it = regs_.find(tex);
        if (it != regs_.end()) return it->second;
        NV_ENC_REGISTER_RESOURCE r{};
        r.version = NV_ENC_REGISTER_RESOURCE_VER;
        r.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        r.width = cfg_.width;
        r.height = cfg_.height;
        r.resourceToRegister = tex;
        r.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
        r.bufferUsage = NV_ENC_INPUT_IMAGE;
        if (nv_.nvEncRegisterResource(enc_, &r) != NV_ENC_SUCCESS) {
            DM_LOGE("NVENC: register resource failed");
            return nullptr;
        }
        regs_[tex] = r.registeredResource;
        return r.registeredResource;
    }

    void shutdown() {
        if (enc_) {
            for (auto& [tex, reg] : regs_) nv_.nvEncUnregisterResource(enc_, reg);
            if (bitstream_) nv_.nvEncDestroyBitstreamBuffer(enc_, bitstream_);
            nv_.nvEncDestroyEncoder(enc_);
            enc_ = nullptr;
        }
        if (lib_) FreeLibrary(lib_);
        lib_ = nullptr;
    }

    HMODULE lib_ = nullptr;
    NV_ENCODE_API_FUNCTION_LIST nv_{};
    void* enc_ = nullptr;
    NV_ENC_OUTPUT_PTR bitstream_ = nullptr;
    NV_ENC_CONFIG config_{};
    NV_ENC_INITIALIZE_PARAMS init_params_{};
    EncoderConfig cfg_;
    std::unordered_map<ID3D11Texture2D*, NV_ENC_REGISTERED_PTR> regs_;
    uint64_t frame_ = 0;
};

}  // namespace

std::unique_ptr<IEncoder> create_nvenc_encoder() { return std::make_unique<NvencEncoder>(); }

}  // namespace dm
