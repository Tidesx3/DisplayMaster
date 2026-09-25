#include "diagnostics.h"

#include <d3d11.h>
#include <dxgi1_6.h>

#include <cstdio>
#include <fstream>
#include <vector>

#include "core/log.h"
#include "core/win.h"
#include "display/display_config.h"
#include "session/video_pipeline.h"

namespace dm {

int list_monitors() {
    for (const auto& m : enumerate_monitors(true)) {
        printf("%-14s %-8s %-4s %-24s %5dx%-5d @ (%d,%d) %.2f Hz  target=%u\n    %s\n",
               to_utf8(m.gdi_name.empty() ? L"-" : m.gdi_name).c_str(), m.active ? "active" : "inactive",
               m.is_vdd ? "VDD" : "", to_utf8(m.friendly_name).c_str(), m.rect.w, m.rect.h, m.rect.x, m.rect.y,
               m.refresh_mhz / 1000.0, m.target_id, to_utf8(m.device_path).c_str());
    }
    return 0;
}

int selftest(int seconds, const std::string& out_path, const HostOptions& opts) {
    auto mon = find_primary_monitor();
    if (!mon) {
        DM_LOGE("No primary monitor");
        return 1;
    }
    PipelineParams p;
    p.gdi_name = mon->gdi_name;
    p.codecs = {opts.codec.value_or(proto::Codec::HEVC)};
    p.fps = opts.max_fps;
    p.bitrate_kbps = opts.bitrate_kbps;
    p.backend = opts.backend;
    VideoPipeline pipe;
    if (!pipe.init(p)) {
        DM_LOGE("Pipeline init failed");
        return 1;
    }
    DM_LOGI("Selftest: %s %ux%u via %s on %s for %d s - move a window around to generate frames",
            codec_name(pipe.codec()), pipe.video_width(), pipe.video_height(), pipe.encoder_name(),
            pipe.adapter_name().c_str(), seconds);

    std::ofstream out;
    if (!out_path.empty()) out.open(out_path, std::ios::binary);
    const uint64_t start = now_us(), end = start + static_cast<uint64_t>(seconds) * 1'000'000;
    uint64_t frames = 0, keyframes = 0, bytes = 0, encode_us_total = 0, encode_us_max = 0;
    bool first = true;
    EncodedPacket pkt;
    while (now_us() < end) {
        const uint64_t t0 = now_us();
        // Force a keyframe every 2 s so the dump is seekable.
        const bool key = first || (frames % (2 * p.fps) == 0);
        const auto r = pipe.step(50, key, pkt);
        if (r == VideoPipeline::Step::Lost || r == VideoPipeline::Step::Error) {
            DM_LOGW("Capture lost, re-initializing");
            if (!pipe.init(p)) return 1;
            continue;
        }
        if (r != VideoPipeline::Step::Frame) continue;
        const uint64_t dt = now_us() - t0;
        encode_us_total += dt;
        encode_us_max = std::max(encode_us_max, dt);
        first = false;
        ++frames;
        keyframes += pkt.keyframe;
        bytes += pkt.data.size();
        if (out) out.write(reinterpret_cast<const char*>(pkt.data.data()), static_cast<std::streamsize>(pkt.data.size()));
    }
    const double secs = (now_us() - start) / 1e6;
    printf("\nSelftest result: %llu frames (%llu key) in %.1f s = %.1f fps, %.1f Mbit/s, "
           "capture+encode avg %.2f ms max %.2f ms\n",
           frames, keyframes, secs, frames / secs, bytes * 8 / secs / 1e6,
           frames ? encode_us_total / 1000.0 / frames : 0.0, encode_us_max / 1000.0);
    return frames ? 0 : 2;
}

int probe_encoders() {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 1;
    constexpr uint32_t W = 1920, H = 1080, kFrames = 30;

    // Synthetic NV12 frame: luma gradient, neutral chroma.
    std::vector<uint8_t> nv12(W * H * 3 / 2, 128);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) nv12[y * W + x] = static_cast<uint8_t>(16 + (x + y) * 219 / (W + H));

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        printf("\n%s (vendor %04X)\n", to_utf8(desc.Description).c_str(), desc.VendorId);

        ComPtr<ID3D11Device> dev;
        if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                     nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, nullptr))) {
            printf("  cannot create D3D11 device\n");
            continue;
        }
        D3D11_TEXTURE2D_DESC td{W, H, 1, 1, DXGI_FORMAT_NV12, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
        D3D11_SUBRESOURCE_DATA init{nv12.data(), W, 0};
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(dev->CreateTexture2D(&td, &init, &tex))) {
            printf("  cannot create NV12 texture\n");
            continue;
        }
        for (auto backend : {EncoderBackend::Nvenc, EncoderBackend::Amf}) {
            for (auto codec : {proto::Codec::H264, proto::Codec::HEVC, proto::Codec::AV1}) {
                const char* bname = backend == EncoderBackend::Nvenc ? "NVENC" : "AMF";
                auto enc = backend == EncoderBackend::Nvenc ? create_nvenc_encoder() : create_amf_encoder();
                if (!enc->init(dev.Get(), EncoderConfig{codec, W, H, 120, 40000})) {
                    printf("  %-5s %-5s  -\n", bname, codec_name(codec));
                    continue;
                }
                EncodedPacket pkt;
                uint64_t total = 0, worst = 0, bytes = 0;
                bool ok = true;
                for (uint32_t i = 0; i < kFrames && ok; ++i) {
                    const uint64_t t0 = now_us();
                    ok = enc->encode(tex.Get(), i == 0, pkt);
                    const uint64_t dt = now_us() - t0;
                    total += dt;
                    worst = std::max(worst, dt);
                    bytes += pkt.data.size();
                }
                if (ok)
                    printf("  %-5s %-5s  OK  1080p encode avg %.2f ms, max %.2f ms, %llu bytes\n", bname, codec_name(codec),
                           total / 1000.0 / kFrames, worst / 1000.0, bytes);
                else
                    printf("  %-5s %-5s  init OK, encode FAILED\n", bname, codec_name(codec));
            }
        }
    }
    return 0;
}

}  // namespace dm
