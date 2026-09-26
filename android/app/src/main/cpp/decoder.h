// Low-latency hardware video decoding straight to a Surface via AMediaCodec.
#pragma once

#include <media/NdkMediaCodec.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

#include "dm/protocol.h"

struct ANativeWindow;

namespace dm {

class Decoder {
public:
    ~Decoder() { release(); }

    // (Re)creates the codec for `cfg`, rendering into `window`.
    bool configure(ANativeWindow* window, const proto::VideoConfig& cfg, uint32_t sdk_int);
    void release();
    bool ready() const { return codec_ != nullptr; }

    enum class Feed { Ok, NeedKeyframe, Dropped, Error };
    Feed feed(const proto::VideoFrame& f);
    // A frame went missing (UDP): later frames refer to it, so wait for the next keyframe.
    void resync() { waiting_for_key_ = true; }

    // Stats since last call.
    struct Stats {
        uint32_t decoded = 0;
        uint32_t dropped = 0;
        uint32_t avg_decode_us = 0;
    };
    Stats take_stats();

private:
    void output_loop();

    AMediaCodec* codec_ = nullptr;
    std::thread out_thread_;
    std::atomic<bool> running_{false};
    bool waiting_for_key_ = true;
    std::mutex stats_mu_;
    Stats stats_;
    uint64_t decode_us_sum_ = 0;
};

}  // namespace dm
