// capture (shared DDA hub) -> convert/scale (D3D11 VP) -> encode (NVENC/AMF), all on
// the GPU that owns the captured monitor.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "capture/shared_capture.h"
#include "capture/video_converter.h"
#include "encode/encoder.h"

namespace dm {

struct PipelineParams {
    std::wstring gdi_name;             // monitor to capture
    uint32_t video_w = 0, video_h = 0;  // encoded size (0 = same as monitor)
    bool letterbox = false;            // fit monitor into video preserving aspect
    std::vector<proto::Codec> codecs;  // in preference order; first that works wins
    uint32_t fps = 60;
    uint32_t bitrate_kbps = 0;         // 0 = automatic
    bool usb = true;                   // affects automatic bitrate
    EncoderBackend backend = EncoderBackend::Auto;
};

class VideoPipeline {
public:
    enum class Step { Frame, Idle, Lost, Error };

    bool init(const PipelineParams& p);

    // Waits up to timeout_ms for a desktop or pointer change and encodes it. With
    // force_keyframe and no change, re-encodes the last frame as a keyframe.
    Step step(uint32_t timeout_ms, bool force_keyframe, EncodedPacket& out);

    bool set_bitrate(uint32_t kbps);

    proto::Codec codec() const { return codec_; }
    uint32_t video_width() const { return video_w_; }
    uint32_t video_height() const { return video_h_; }
    uint32_t fps() const { return fps_; }
    uint32_t bitrate_kbps() const { return bitrate_kbps_; }
    RectF content() const { return content_; }
    const char* encoder_name() const { return encoder_ ? encoder_->name() : "none"; }
    std::string adapter_name() const { return capture_ ? capture_->adapter_name() : std::string(); }

    static uint32_t auto_bitrate_kbps(uint32_t w, uint32_t h, uint32_t fps, proto::Codec c, bool usb);

private:
    std::shared_ptr<SharedCapture> capture_;
    uint64_t seen_version_ = 0;
    VideoConverter converter_;
    std::unique_ptr<IEncoder> encoder_;
    ComPtr<ID3D11Texture2D> frame_;  // this session's copy of the composed desktop
    ComPtr<ID3D11Texture2D> nv12_;
    bool have_frame_ = false;
    proto::Codec codec_ = proto::Codec::HEVC;
    uint32_t video_w_ = 0, video_h_ = 0, fps_ = 60, bitrate_kbps_ = 0;
    RectF content_;
};

}  // namespace dm
