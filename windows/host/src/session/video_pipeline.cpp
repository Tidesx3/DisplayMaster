#include "session/video_pipeline.h"

#include <algorithm>

#include "core/log.h"

namespace dm {

uint32_t VideoPipeline::auto_bitrate_kbps(uint32_t w, uint32_t h, uint32_t fps, proto::Codec c, bool usb) {
    // Bits per pixel per frame; desktop content compresses well, text needs headroom.
    double bpp = usb ? 0.12 : 0.06;
    if (c == proto::Codec::H264) bpp *= 1.5;
    if (c == proto::Codec::AV1) bpp *= 0.85;
    const double kbps = static_cast<double>(w) * h * fps * bpp / 1000.0;
    return static_cast<uint32_t>(std::clamp(kbps, 5000.0, usb ? 150000.0 : 60000.0));
}

bool VideoPipeline::init(const PipelineParams& p) {
    have_frame_ = false;
    encoder_.reset();
    capture_ = SharedCapture::get(p.gdi_name);
    if (!capture_) return false;
    seen_version_ = 0;  // take the newest frame the hub has
    ID3D11Device* device = capture_->device();

    const uint32_t mon_w = capture_->width(), mon_h = capture_->height();
    video_w_ = (p.video_w ? p.video_w : mon_w) & ~1u;
    video_h_ = (p.video_h ? p.video_h : mon_h) & ~1u;
    fps_ = p.fps;

    RectI dest{0, 0, static_cast<int32_t>(video_w_), static_cast<int32_t>(video_h_)};
    content_ = {};
    if (p.letterbox) {
        content_ = letterbox(mon_w, mon_h, video_w_, video_h_);
        dest = {static_cast<int32_t>(content_.x * video_w_), static_cast<int32_t>(content_.y * video_h_),
                static_cast<int32_t>(content_.w * video_w_), static_cast<int32_t>(content_.h * video_h_)};
    }
    if (!converter_.init(device, mon_w, mon_h, video_w_, video_h_, dest)) return false;
    frame_ = converter_.create_input_texture();
    nv12_ = converter_.create_output_texture();
    if (!frame_ || !nv12_) return false;
    have_frame_ = p.blank_start;  // new textures are zero-filled: a valid, dark picture

    for (auto c : p.codecs) {
        EncoderConfig cfg;
        cfg.codec = c;
        cfg.width = video_w_;
        cfg.height = video_h_;
        cfg.fps = fps_;
        cfg.bitrate_kbps = p.bitrate_kbps ? p.bitrate_kbps : auto_bitrate_kbps(video_w_, video_h_, fps_, c, p.usb);
        encoder_ = create_encoder(device, cfg, p.backend);
        if (encoder_) {
            codec_ = c;
            bitrate_kbps_ = cfg.bitrate_kbps;
            return true;
        }
    }
    return false;
}

VideoPipeline::Step VideoPipeline::step(uint32_t timeout_ms, bool force_keyframe, bool force_frame, uint64_t frame_id,
                                        EncodedPacket& out) {
    const auto result = capture_->next(seen_version_, timeout_ms, frame_.Get());
    const uint64_t work_start = now_us();
    switch (result) {
        case SharedCapture::Result::Frame:
            if (!converter_.convert(frame_.Get(), nv12_.Get())) return Step::Error;
            have_frame_ = true;
            break;
        case SharedCapture::Result::Timeout:
            if (!((force_keyframe || force_frame) && have_frame_)) return Step::Idle;
            break;  // re-encode the last frame (as a keyframe, or as the recovery frame)
        case SharedCapture::Result::Lost: return Step::Lost;
    }
    const bool ok = encoder_->encode(nv12_.Get(), force_keyframe, frame_id, out);
    last_work_us_ = now_us() - work_start;
    return ok ? Step::Frame : Step::Error;
}

bool VideoPipeline::set_bitrate(uint32_t kbps) {
    if (!encoder_ || !encoder_->set_bitrate(kbps)) return false;
    bitrate_kbps_ = kbps;
    return true;
}

}  // namespace dm
