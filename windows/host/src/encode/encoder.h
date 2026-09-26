// Hardware video encoders fed with NV12 D3D11 textures.
#pragma once

#include <d3d11.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dm/protocol.h"

namespace dm {

struct EncoderConfig {
    proto::Codec codec = proto::Codec::HEVC;
    uint32_t width = 0, height = 0;
    uint32_t fps = 60;
    uint32_t bitrate_kbps = 50000;
};

struct EncodedPacket {
    std::vector<uint8_t> data;  // Annex B (H.264/HEVC) or low-overhead OBUs (AV1)
    bool keyframe = false;
    bool recovery = false;  // first frame after invalidate(): refers only to frames before the loss
};

class IEncoder {
public:
    virtual ~IEncoder() = default;
    virtual const char* name() const = 0;
    virtual bool init(ID3D11Device* device, const EncoderConfig& cfg) = 0;
    // Synchronous: returns once the packet for this frame is available. `frame_id`: the
    // session's frame number (what invalidate() refers to).
    virtual bool encode(ID3D11Texture2D* nv12, bool force_keyframe, uint64_t frame_id, EncodedPacket& out) = 0;
    virtual bool set_bitrate(uint32_t kbps) = 0;
    // Reference frame invalidation: frames first..last never reached the device, so the next
    // frame must not refer to them (it becomes a recovery frame). False if unsupported or the
    // frames are too old to still be referenced - send a keyframe instead.
    virtual bool invalidate(uint64_t first, uint64_t last) {
        (void)first;
        (void)last;
        return false;
    }
};

enum class EncoderBackend { Auto, Nvenc, Amf };

std::unique_ptr<IEncoder> create_nvenc_encoder();
std::unique_ptr<IEncoder> create_amf_encoder();

// Picks the backend matching the device's GPU vendor (NVENC on NVIDIA, AMF on
// AMD), falling back to the other if init fails. Returns null if none works.
std::unique_ptr<IEncoder> create_encoder(ID3D11Device* device, const EncoderConfig& cfg, EncoderBackend pref);

// Which codecs can the device's GPU encode? (probed by trial init at 1280x720)
uint32_t probe_encodable_codecs(ID3D11Device* device, EncoderBackend pref);

const char* codec_name(proto::Codec c);

}  // namespace dm
