#include "display/virtual_display.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <regex>
#include <sstream>
#include <thread>

#include "core/log.h"
#include "core/win.h"
#include "setup/vdd_setup.h"

namespace dm {

// ---------------------------------------------------------------- MttVddProvider

MttVddProvider::MttVddProvider(std::wstring settings_path) : settings_path_(std::move(settings_path)) {
    if (settings_path_.empty()) settings_path_ = (setup::vdd_settings_dir() / L"vdd_settings.xml").wstring();
}

bool MttVddProvider::send_command(const std::wstring& cmd, std::wstring* reply) {
    constexpr const wchar_t* kPipe = L"\\\\.\\pipe\\MTTVirtualDisplayPipe";
    if (!WaitNamedPipeW(kPipe, 2000)) return false;
    UniqueHandle pipe(CreateFileW(kPipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!pipe.valid()) return false;
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr);
    DWORD written = 0;
    // The driver reads wchar_t text (it NUL-terminates what it receives itself).
    if (!WriteFile(pipe.get(), cmd.c_str(), static_cast<DWORD>(cmd.size() * sizeof(wchar_t)), &written, nullptr))
        return false;
    if (reply) {
        wchar_t buf[512] = {};
        DWORD read = 0;
        if (ReadFile(pipe.get(), buf, sizeof buf - sizeof(wchar_t), &read, nullptr))
            *reply = std::wstring(buf, read / sizeof(wchar_t));
    }
    return true;
}

bool MttVddProvider::available() {
    std::wstring reply;
    return send_command(L"PING", &reply);
}

static bool read_file(const std::wstring& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static bool write_file(const std::wstring& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << data;
    return static_cast<bool>(f);
}

uint32_t MttVddProvider::parse_count(const std::string& xml) {
    std::smatch m;
    static const std::regex re(R"(<monitors>\s*<count>\s*(\d+)\s*</count>)");
    if (std::regex_search(xml, m, re)) return static_cast<uint32_t>(std::stoul(m[1].str()));
    return 1;
}

std::string MttVddProvider::set_count(const std::string& xml, uint32_t count) {
    static const std::regex re(R"(<monitors>\s*<count>\s*(\d+)\s*</count>)");
    std::smatch m;
    if (!std::regex_search(xml, m, re)) return xml;  // unexpected format: leave untouched
    const auto pos = static_cast<size_t>(m.position(1));
    return xml.substr(0, pos) + std::to_string(count) + xml.substr(pos + static_cast<size_t>(m.length(1)));
}

std::string MttVddProvider::add_resolutions(const std::string& xml, const std::vector<DisplayModeSpec>& modes,
                                            bool* changed) {
    *changed = false;
    std::string out = xml;
    for (const auto& m : modes) {
        // Existing entry with the same width/height/refresh?
        const std::regex existing("<resolution>\\s*<width>\\s*" + std::to_string(m.width) +
                                  "\\s*</width>\\s*<height>\\s*" + std::to_string(m.height) +
                                  "\\s*</height>\\s*<refresh_rate>\\s*" + std::to_string(m.refresh_hz) +
                                  "\\s*</refresh_rate>");
        if (std::regex_search(out, existing)) continue;
        const auto pos = out.find("</resolutions>");
        if (pos == std::string::npos) return xml;  // unexpected format: leave untouched
        const std::string entry = "    <resolution>\n            <width>" + std::to_string(m.width) +
                                  "</width>\n            <height>" + std::to_string(m.height) +
                                  "</height>\n            <refresh_rate>" + std::to_string(m.refresh_hz) +
                                  "</refresh_rate>\n        </resolution>\n    ";
        out.insert(pos, entry);
        *changed = true;
    }
    return out;
}

uint32_t MttVddProvider::monitor_count() {
    std::string xml;
    return read_file(settings_path_, xml) ? parse_count(xml) : 0;
}

bool MttVddProvider::configure(uint32_t monitor_count, const std::vector<DisplayModeSpec>& modes) {
    std::string xml;
    if (!read_file(settings_path_, xml)) {
        DM_LOGE("Cannot read %s", to_utf8(settings_path_).c_str());
        return false;
    }
    bool modes_changed = false;
    std::string updated = add_resolutions(xml, modes, &modes_changed);
    const bool count_changed = parse_count(updated) != monitor_count;
    if (!modes_changed && !count_changed) return true;
    if (!enabled()) {
        // Switched off: the driver reads the file when it starts.
        if (write_file(settings_path_, set_count(updated, monitor_count))) return true;
        DM_LOGE("Cannot write %s (host must run elevated)", to_utf8(settings_path_).c_str());
        return false;
    }
    if (modes_changed && !write_file(settings_path_, updated)) {
        DM_LOGE("Cannot write %s (host must run elevated)", to_utf8(settings_path_).c_str());
        return false;
    }
    // SETDISPLAYCOUNT rewrites the count and re-initializes the adapter, which
    // also picks up the new resolution list.
    DM_LOGI("VDD: reloading with %u monitor(s)%s", monitor_count, modes_changed ? " and new modes" : "");
    return send_command(L"SETDISPLAYCOUNT " + std::to_wstring(monitor_count));
}

bool MttVddProvider::set_render_gpu(const std::wstring& adapter_name) {
    return send_command(L"SETGPU \"" + adapter_name + L"\"");
}

bool MttVddProvider::installed() { return setup::vdd_state() != setup::VddState::NotInstalled; }

bool MttVddProvider::enabled() { return setup::vdd_state() == setup::VddState::Enabled; }

bool MttVddProvider::set_enabled(bool on) {
    // Only a driver we installed is ours to switch off; another tool may rely on it.
    if (!on && !setup::vdd_installed_by_us()) return false;
    if (!setup::set_vdd_enabled(on)) return false;
    if (!on) return true;
    // Starting takes a moment; the control pipe answers once the adapter is up.
    for (int i = 0; i < 40; ++i) {
        if (available()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    DM_LOGE("VDD: driver did not start");
    return false;
}

// ---------------------------------------------------------------- VirtualDisplayManager

VirtualDisplayManager::VirtualDisplayManager(std::unique_ptr<IVirtualDisplayProvider> provider)
    : provider_(std::move(provider)) {
    // A driver we switched off earlier counts: acquire() turns it back on.
    available_ = provider_ && (provider_->available() || (provider_->installed() && !provider_->enabled()));
    if (available_)
        DM_LOGI("Virtual display driver: %s (%s)", provider_->name(), provider_->enabled() ? "on" : "off");
    else
        DM_LOGW("Virtual display driver not found - extend mode unavailable, mirror mode only");
}

std::vector<MonitorInfo> VirtualDisplayManager::vdd_monitors() {
    auto all = enumerate_monitors(true);
    std::vector<MonitorInfo> v;
    for (auto& m : all)
        if (m.is_vdd) v.push_back(std::move(m));
    std::sort(v.begin(), v.end(), [](const MonitorInfo& a, const MonitorInfo& b) { return a.target_id < b.target_id; });
    return v;
}

std::optional<MonitorInfo> VirtualDisplayManager::wait_for_slot(size_t slot, bool want_active, uint32_t timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    do {
        auto mons = vdd_monitors();
        if (slot < mons.size() && (!want_active || mons[slot].active)) return mons[slot];
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (std::chrono::steady_clock::now() < deadline);
    return std::nullopt;
}

void VirtualDisplayManager::prepare() {
    if (!available_) return;
    std::lock_guard lock(mu_);
    idle_locked();
}

// No device connected. Every driver monitor shows up in Windows' display settings, attached
// or not, so switch the driver off; acquire() switches it on with as many monitors as needed.
// If it must stay on (not ours, not elevated), keep a single detached monitor.
void VirtualDisplayManager::idle_locked() {
    if (!slots_.empty() || !provider_->enabled()) return;
    if (provider_->set_enabled(false)) {
        DM_LOGI("VDD: switched off until a device extends the desktop");
        provider_->configure(1, known_modes_);  // start with one monitor next time
        return;
    }
    if (provider_->monitor_count() > 1) {
        provider_->configure(1, known_modes_);
        wait_for_slot(0, false, 8000);
    }
    detach_unused_locked(SIZE_MAX);
}

void VirtualDisplayManager::detach_unused_locked(size_t keep_slot) {
    auto mons = vdd_monitors();
    for (size_t i = 0; i < mons.size(); ++i) {
        if (mons[i].active && !slots_.count(i) && i != keep_slot) {
            DM_LOGI("Detaching unused virtual monitor %zu (%s)", i, to_utf8(mons[i].gdi_name).c_str());
            set_target_active(mons[i].adapter_luid, mons[i].target_id, false);
        }
    }
}

std::optional<MonitorInfo> VirtualDisplayManager::apply_mode(size_t slot, const DisplayModeSpec& mode,
                                                            Placement placement) {
    auto mon = wait_for_slot(slot, false, 5000);
    if (!mon) {
        DM_LOGE("Virtual monitor slot %zu did not appear", slot);
        return std::nullopt;
    }
    if (!mon->active && !set_target_active(mon->adapter_luid, mon->target_id, true)) return std::nullopt;
    mon = wait_for_slot(slot, true, 3000);
    if (!mon) return std::nullopt;

    // Right after a driver reload the old monitor instance can still be listed without
    // the new modes; retry briefly against the freshly enumerated monitor.
    for (int attempt = 0; attempt < 30; ++attempt) {
        const auto pos = position_for(placement, enumerate_monitors(false), mon->gdi_name,
                                      static_cast<int32_t>(mode.width), static_cast<int32_t>(mode.height));
        if (set_monitor_mode(mon->gdi_name, mode.width, mode.height, mode.refresh_hz, pos, attempt < 29) ||
            // Refresh rate might not be offered; accept the driver's choice.
            set_monitor_mode(mon->gdi_name, mode.width, mode.height, 0, pos, attempt < 29))
            return wait_for_slot(slot, true, 1000);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (auto fresh = wait_for_slot(slot, true, 500)) mon = fresh;
    }
    DM_LOGE("Virtual monitor %zu does not accept %ux%u", slot, mode.width, mode.height);
    return std::nullopt;
}

std::optional<MonitorInfo> VirtualDisplayManager::acquire(uint32_t session_id, const DisplayModeSpec& mode,
                                                         Placement placement) {
    if (!available_) return std::nullopt;
    std::lock_guard lock(mu_);

    size_t slot = 0;
    while (slots_.count(slot)) ++slot;

    // Portrait variant too, so rotating the device doesn't need a driver reload.
    for (const auto& m : {mode, DisplayModeSpec{mode.height, mode.width, mode.refresh_hz}})
        if (std::find(known_modes_.begin(), known_modes_.end(), m) == known_modes_.end()) known_modes_.push_back(m);

    const uint32_t needed = static_cast<uint32_t>(std::max<size_t>(slot + 1, provider_->monitor_count()));
    if (!provider_->enabled()) {
        // Count and modes go into the settings file first, so starting needs no reload.
        if (!provider_->configure(static_cast<uint32_t>(slot + 1), known_modes_)) return std::nullopt;
        DM_LOGI("VDD: switching on");
        if (!provider_->set_enabled(true)) return std::nullopt;
    } else if (!provider_->configure(needed, known_modes_)) {
        return std::nullopt;
    }

    auto mon = apply_mode(slot, mode, placement);
    if (!mon) return std::nullopt;
    slots_[slot] = Slot{session_id, mode, placement};
    // A driver reload re-attaches every virtual monitor; hide the ones nobody uses.
    detach_unused_locked(slot);
    mon = wait_for_slot(slot, true, 1000);
    if (!mon) return std::nullopt;
    DM_LOGI("Session %u -> virtual monitor %zu %s %ux%u@%u at (%d,%d)", session_id, slot,
            to_utf8(mon->gdi_name).c_str(), mode.width, mode.height, mode.refresh_hz, mon->rect.x, mon->rect.y);
    return mon;
}

std::optional<MonitorInfo> VirtualDisplayManager::reconfigure(uint32_t session_id, const DisplayModeSpec& mode,
                                                             Placement placement) {
    std::lock_guard lock(mu_);
    for (auto& [slot, s] : slots_) {
        if (s.session_id != session_id) continue;
        // Capture restarts (another monitor changed) must not touch the display config:
        // a mode set here would in turn restart every other session's capture.
        if (s.mode == mode && s.placement == placement) {
            auto mons = vdd_monitors();
            if (slot < mons.size() && mons[slot].active && mons[slot].rect.w == static_cast<int32_t>(mode.width) &&
                mons[slot].rect.h == static_cast<int32_t>(mode.height))
                return mons[slot];
        }
        if (std::find(known_modes_.begin(), known_modes_.end(), mode) == known_modes_.end()) {
            known_modes_.push_back(mode);
            if (!provider_->configure(provider_->monitor_count(), known_modes_)) return std::nullopt;
        }
        s.mode = mode;
        s.placement = placement;
        return apply_mode(slot, mode, placement);
    }
    return std::nullopt;
}

std::optional<MonitorInfo> VirtualDisplayManager::current(uint32_t session_id) {
    std::lock_guard lock(mu_);
    for (auto& [slot, s] : slots_) {
        if (s.session_id != session_id) continue;
        auto mons = vdd_monitors();
        if (slot < mons.size()) return mons[slot];
    }
    return std::nullopt;
}

void VirtualDisplayManager::release(uint32_t session_id) {
    std::lock_guard lock(mu_);
    for (auto it = slots_.begin(); it != slots_.end(); ++it) {
        if (it->second.session_id != session_id) continue;
        auto mons = vdd_monitors();
        if (it->first < mons.size() && mons[it->first].active)
            set_target_active(mons[it->first].adapter_luid, mons[it->first].target_id, false);
        DM_LOGI("Session %u released virtual monitor %zu", session_id, it->first);
        slots_.erase(it);
        idle_locked();
        return;
    }
}

}  // namespace dm
