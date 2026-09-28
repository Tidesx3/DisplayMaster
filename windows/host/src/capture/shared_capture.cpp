#include "capture/shared_capture.h"

#include <chrono>
#include <map>

#include "core/log.h"

namespace dm {
namespace {

std::mutex g_registry_mu;
std::map<std::wstring, std::weak_ptr<SharedCapture>> g_registry;

ComPtr<ID3D11Texture2D> make_bgra(ID3D11Device* device, uint32_t w, uint32_t h) {
    D3D11_TEXTURE2D_DESC d{w, h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                           D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ComPtr<ID3D11Texture2D> t;
    device->CreateTexture2D(&d, nullptr, &t);
    return t;
}

}  // namespace

std::shared_ptr<SharedCapture> SharedCapture::get(const std::wstring& gdi_name) {
    std::lock_guard lock(g_registry_mu);
    if (auto existing = g_registry[gdi_name].lock()) {
        std::lock_guard l(existing->mu_);
        // A GPU reset (TDR) removes the device: everything created on it fails from then
        // on, so the capture needs a new device, not just a new duplication.
        const HRESULT removed = existing->device()->GetDeviceRemovedReason();
        if (FAILED(removed))
            DM_LOGW("GPU device of %s was removed (%s) - recreating it", to_utf8(gdi_name).c_str(),
                    hr_string(removed).c_str());
        else if (!existing->lost_)
            return existing;  // a lost one is replaced by a fresh duplication
    }
    auto sc = std::shared_ptr<SharedCapture>(new SharedCapture());
    if (!sc->init(gdi_name)) return nullptr;
    g_registry[gdi_name] = sc;
    return sc;
}

bool SharedCapture::init(const std::wstring& gdi_name) {
    gdi_name_ = gdi_name;
    if (!capture_.init(gdi_name)) return false;
    desktop_ = make_bgra(capture_.device(), capture_.width(), capture_.height());
    composed_ = make_bgra(capture_.device(), capture_.width(), capture_.height());
    return desktop_ && composed_;
}

std::string SharedCapture::adapter_name() const {
    return to_utf8(capture_.adapter_desc().Description);
}

SharedCapture::Result SharedCapture::next(uint64_t& seen, uint32_t timeout_ms, ID3D11Texture2D* dst) {
    std::unique_lock lock(mu_);
    auto deliver = [&] {
        // Queued on the same immediate context as later captures, so the GPU finishes
        // this copy before composed_ is overwritten.
        capture_.context()->CopyResource(dst, composed_.Get());
        seen = version_;
        return Result::Frame;
    };
    if (lost_) return Result::Lost;
    if (version_ > seen) return deliver();

    if (capturing_) {
        // Another session is capturing: wait for its frame.
        cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return version_ > seen || !capturing_ || lost_; });
    } else {
        capturing_ = true;
        lock.unlock();
        const auto r = capture_.capture(timeout_ms, desktop_.Get());
        bool fresh = false;
        if (r == DdaCapture::Result::Frame) have_desktop_ = true;
        if ((r == DdaCapture::Result::Frame || r == DdaCapture::Result::Pointer) && have_desktop_) {
            capture_.context()->CopyResource(composed_.Get(), desktop_.Get());
            capture_.cursor().draw(composed_.Get(), capture_.width(), capture_.height());
            fresh = true;
        }
        lock.lock();
        capturing_ = false;
        if (fresh) ++version_;
        if (r == DdaCapture::Result::Lost || r == DdaCapture::Result::Error) lost_ = true;
        cv_.notify_all();
    }
    if (lost_) return Result::Lost;
    return version_ > seen ? deliver() : Result::Timeout;
}

}  // namespace dm
