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
    // Tests: encode even before the screen delivered a frame (a blank picture), so tests run
    // with the monitors off or locked, when Desktop Duplication delivers nothing.
    bool blank_start = false;
};

class VideoPipeline {
public:
    enum class Step { Frame, Idle, Lost, Error };

    bool init(const PipelineParams& p);

    // Waits up to timeout_ms for a desktop or pointer change and encodes it as frame
    // `frame_id`. With force_keyframe or force_frame and no change, re-encodes the last frame.
    Step step(uint32_t timeout_ms, bool force_keyframe, bool force_frame, uint64_t frame_id, EncodedPacket& out);
    bool invalidate(uint64_t first, uint64_t last) { return encoder_ && encoder_->invalidate(first, last); }

    bool set_bitrate(uint32_t kbps);
    // GPU work of the last Frame step (copy, convert, encode), excluding the wait for a change.
    uint64_t last_work_us() const { return last_work_us_; }

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
    uint64_t last_work_us_ = 0;
    proto::Codec codec_ = proto::Codec::HEVC;
    uint32_t video_w_ = 0, video_h_ = 0, fps_ = 60, bitrate_kbps_ = 0;
    RectF content_;
};

}  // namespace dm
