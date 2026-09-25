// Virtual monitors, one per connected device.
//
// IVirtualDisplayProvider abstracts the driver (today: the signed MTT
// Virtual-Display-Driver; later possibly our own IddCx driver).
// VirtualDisplayManager assigns driver monitor "slots" to sessions, attaches
// them to the desktop at the requested mode and detaches them when unused.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "display/display_config.h"

namespace dm {

struct DisplayModeSpec {
    uint32_t width = 0, height = 0, refresh_hz = 60;
    bool operator==(const DisplayModeSpec&) const = default;
};

class IVirtualDisplayProvider {
public:
    virtual ~IVirtualDisplayProvider() = default;
    virtual const char* name() const = 0;
    virtual bool available() = 0;
    virtual uint32_t monitor_count() = 0;
    // Makes `modes` selectable and sets the monitor count. May reload the driver,
    // which briefly removes and re-adds every virtual monitor.
    virtual bool configure(uint32_t monitor_count, const std::vector<DisplayModeSpec>& modes) = 0;
    // Pin rendering to a GPU (by adapter description), empty = driver default.
    virtual bool set_render_gpu(const std::wstring& adapter_name) = 0;
};

// Signed open-source driver: https://github.com/VirtualDrivers/Virtual-Display-Driver
// Controlled via \\.\pipe\MTTVirtualDisplayPipe and C:\VirtualDisplayDriver\vdd_settings.xml.
class MttVddProvider : public IVirtualDisplayProvider {
public:
    explicit MttVddProvider(std::wstring settings_path = {});  // default: driver's VDDPATH\vdd_settings.xml
    const char* name() const override { return "MTT VDD"; }
    bool available() override;
    uint32_t monitor_count() override;
    bool configure(uint32_t monitor_count, const std::vector<DisplayModeSpec>& modes) override;
    bool set_render_gpu(const std::wstring& adapter_name) override;

    // Exposed for tests: add missing <resolution> entries to settings XML text.
    static std::string add_resolutions(const std::string& xml, const std::vector<DisplayModeSpec>& modes,
                                       bool* changed);
    static uint32_t parse_count(const std::string& xml);

private:
    bool send_command(const std::wstring& cmd, std::wstring* reply = nullptr);
    std::wstring settings_path_;
};

class VirtualDisplayManager {
public:
    explicit VirtualDisplayManager(std::unique_ptr<IVirtualDisplayProvider> provider);

    bool available() const { return available_; }

    // Startup: make sure the driver has at least `min_slots` monitors (one reload, now
    // rather than mid-stream), then detach every virtual monitor nobody uses.
    void prepare(uint32_t min_slots);

    // Give `session_id` a virtual monitor with the requested mode, placed to the
    // right of the desktop. Returns the monitor as it now appears.
    std::optional<MonitorInfo> acquire(uint32_t session_id, const DisplayModeSpec& mode);
    // Change the mode of the session's monitor (rotation, fold/unfold).
    std::optional<MonitorInfo> reconfigure(uint32_t session_id, const DisplayModeSpec& mode);
    // Current state of the session's monitor (after driver reloads, GDI names can change).
    std::optional<MonitorInfo> current(uint32_t session_id);
    void release(uint32_t session_id);

private:
    struct Slot {
        uint32_t session_id = 0;
        DisplayModeSpec mode;
    };
    std::vector<MonitorInfo> vdd_monitors();  // sorted by target id == slot order
    std::optional<MonitorInfo> wait_for_slot(size_t slot, bool want_active, uint32_t timeout_ms);
    std::optional<MonitorInfo> apply_mode(size_t slot, const DisplayModeSpec& mode);
    void detach_unused_locked(size_t keep_slot);

    std::unique_ptr<IVirtualDisplayProvider> provider_;
    bool available_ = false;
    std::mutex mu_;
    std::map<size_t, Slot> slots_;  // slot index -> owner
    std::vector<DisplayModeSpec> known_modes_;
};

}  // namespace dm
