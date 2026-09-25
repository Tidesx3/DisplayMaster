#include "decoder.h"

#include <android/log.h>
#include <android/native_window.h>
#include <media/NdkMediaFormat.h>
#include <time.h>

#include <cstring>

#define LOG_TAG "DMDecoder"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace dm {
namespace {

uint64_t mono_us() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

const char* mime_for(proto::Codec c) {
    switch (c) {
        case proto::Codec::H264: return "video/avc";
        case proto::Codec::HEVC: return "video/hevc";
        case proto::Codec::AV1: return "video/av01";
    }
    return nullptr;
}

}  // namespace

bool Decoder::configure(ANativeWindow* window, const proto::VideoConfig& cfg, uint32_t sdk_int) {
    release();
    const char* mime = mime_for(cfg.codec);
    if (!mime || !window) return false;
    codec_ = AMediaCodec_createDecoderByType(mime);
    if (!codec_) {
        LOGE("no decoder for %s", mime);
        return false;
    }
    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, static_cast<int32_t>(cfg.width));
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, static_cast<int32_t>(cfg.height));
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, static_cast<int32_t>(cfg.width * cfg.height));
    // Latency knobs. Unknown keys are ignored by codecs that don't support them.
    if (sdk_int >= 30) AMediaFormat_setInt32(fmt, "low-latency", 1);
    AMediaFormat_setInt32(fmt, "priority", 0);                           // realtime
    AMediaFormat_setInt32(fmt, "vendor.qti-ext-dec-low-latency.enable", 1);  // Qualcomm (Snapdragon)
    AMediaFormat_setInt32(fmt, "vendor.qti-ext-dec-picture-order.enable", 1);
    AMediaFormat_setInt32(fmt, "vendor.low-latency.enable", 1);          // Exynos / MediaTek variants

    media_status_t st = AMediaCodec_configure(codec_, fmt, window, nullptr, 0);
    AMediaFormat_delete(fmt);
    if (st != AMEDIA_OK || AMediaCodec_start(codec_) != AMEDIA_OK) {
        LOGE("configure/start failed (%d) for %s %ux%u", st, mime, cfg.width, cfg.height);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
        return false;
    }
    char* name = nullptr;
    if (__builtin_available(android 28, *)) {
        if (AMediaCodec_getName(codec_, &name) == AMEDIA_OK && name) {
            LOGI("decoder %s for %s %ux%u@%u", name, mime, cfg.width, cfg.height, cfg.fps);
            AMediaCodec_releaseName(codec_, name);
        }
    } else {
        LOGI("decoder for %s %ux%u@%u", mime, cfg.width, cfg.height, cfg.fps);
    }
    waiting_for_key_ = true;
    running_ = true;
    out_thread_ = std::thread([this] { output_loop(); });
    return true;
}

void Decoder::release() {
    running_ = false;
    if (out_thread_.joinable()) out_thread_.join();
    if (codec_) {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
}

Decoder::Feed Decoder::feed(const proto::VideoFrame& f) {
    if (!codec_) return Feed::Error;
    const bool key = f.flags & proto::kFrameKey;
    if (waiting_for_key_ && !key) return Feed::NeedKeyframe;

    // Wait briefly for an input buffer; if the decoder is backed up, drop and resync.
    const ssize_t idx = AMediaCodec_dequeueInputBuffer(codec_, 20000);
    if (idx < 0) {
        std::lock_guard lock(stats_mu_);
        ++stats_.dropped;
        waiting_for_key_ = true;
        return Feed::NeedKeyframe;
    }
    size_t cap = 0;
    uint8_t* buf = AMediaCodec_getInputBuffer(codec_, static_cast<size_t>(idx), &cap);
    if (!buf || cap < f.data.size()) {
        AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(idx), 0, 0, 0, 0);
        waiting_for_key_ = true;
        return Feed::NeedKeyframe;
    }
    std::memcpy(buf, f.data.data(), f.data.size());
    // PTS carries our local receive time so the output thread can measure decode latency.
    const uint64_t pts = mono_us();
    if (AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(idx), 0, f.data.size(), pts, 0) != AMEDIA_OK)
        return Feed::Error;
    waiting_for_key_ = false;
    return Feed::Ok;
}

void Decoder::output_loop() {
    AMediaCodecBufferInfo info;
    while (running_) {
        const ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec_, &info, 10000);
        if (idx >= 0) {
            // Render immediately: no presentation-time scheduling, newest frame wins.
            AMediaCodec_releaseOutputBuffer(codec_, static_cast<size_t>(idx), info.size > 0);
            std::lock_guard lock(stats_mu_);
            ++stats_.decoded;
            decode_us_sum_ += mono_us() - static_cast<uint64_t>(info.presentationTimeUs);
        } else if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* f = AMediaCodec_getOutputFormat(codec_);
            LOGI("output format: %s", AMediaFormat_toString(f));
            AMediaFormat_delete(f);
        }
    }
}

Decoder::Stats Decoder::take_stats() {
    std::lock_guard lock(stats_mu_);
    Stats s = stats_;
    s.avg_decode_us = s.decoded ? static_cast<uint32_t>(decode_us_sum_ / s.decoded) : 0;
    stats_ = {};
    decode_us_sum_ = 0;
    return s;
}

}  // namespace dm
