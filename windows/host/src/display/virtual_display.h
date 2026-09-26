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

    // Driver device on/off. Off, Windows lists none of its monitors. Providers that can't
    // (or mustn't) switch the device return false from set_enabled and stay on.
    virtual bool installed() { return available(); }
    virtual bool enabled() { return true; }
    virtual bool set_enabled(bool) { return false; }
    // Driver crashed (Windows stopped the device) / restart it. Restarting needs elevation.
    virtual bool failed() { return false; }
    virtual bool restart() { return false; }
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
    bool installed() override;
    bool enabled() override;
    bool set_enabled(bool on) override;
    bool failed() override;
    bool restart() override;

    // The driver lists every <resolution> at its own and at each global refresh rate; with
    // around 15 entries (100+ modes) it stops creating monitors. Keep the list short.
    static constexpr size_t kMaxResolutions = 12;

    // Exposed for tests: add missing <resolution> entries to settings XML text.
    static std::string add_resolutions(const std::string& xml, const std::vector<DisplayModeSpec>& modes,
                                       bool* changed);
    // Like add_resolutions, but also drops entries that are neither in `modes` nor one of the
    // driver's stock ones (used when the driver is ours: older versions only ever added).
    static std::string set_resolutions(const std::string& xml, const std::vector<DisplayModeSpec>& modes,
                                       bool* changed);
    static size_t count_resolutions(const std::string& xml);
    static uint32_t parse_count(const std::string& xml);
    static std::string set_count(const std::string& xml, uint32_t count);

private:
    bool send_command(const std::wstring& cmd, std::wstring* reply = nullptr);
    std::wstring settings_path_;
};

class VirtualDisplayManager {
public:
    explicit VirtualDisplayManager(std::unique_ptr<IVirtualDisplayProvider> provider);

    bool available() const { return available_; }

    // Startup: switch the driver off while no device needs it (every driver monitor is
    // listed in Windows' display settings, attached or not).
    void prepare();

    // Give `session_id` a virtual monitor with the requested mode, placed to the
    // right of the desktop. Returns the monitor as it now appears.
    std::optional<MonitorInfo> acquire(uint32_t session_id, const DisplayModeSpec& mode,
                                       Placement placement = Placement::Right);
    // Change the mode or position of the session's monitor (rotation, fold/unfold, user choice).
    std::optional<MonitorInfo> reconfigure(uint32_t session_id, const DisplayModeSpec& mode,
                                           Placement placement = Placement::Right);
    // Current state of the session's monitor (after driver reloads, GDI names can change).
    std::optional<MonitorInfo> current(uint32_t session_id);
    void release(uint32_t session_id);

private:
    struct Slot {
        uint32_t session_id = 0;
        DisplayModeSpec mode;
        Placement placement = Placement::Right;
    };
    std::vector<MonitorInfo> vdd_monitors();  // sorted by target id == slot order
    std::optional<MonitorInfo> wait_for_slot(size_t slot, bool want_active, uint32_t timeout_ms);
    std::optional<MonitorInfo> apply_mode(size_t slot, const DisplayModeSpec& mode, Placement placement);
    void detach_unused_locked(size_t keep_slot);
    void idle_locked();

    std::unique_ptr<IVirtualDisplayProvider> provider_;
    bool available_ = false;
    std::optional<MonitorInfo> start_slot_locked(size_t slot, const DisplayModeSpec& mode, Placement placement);
    void remember_mode_locked(const DisplayModeSpec& mode);
    std::vector<DisplayModeSpec> modes_locked() const;

    std::mutex mu_;
    std::map<size_t, Slot> slots_;  // slot index -> owner
    std::vector<DisplayModeSpec> known_modes_;  // most recently used first
};

}  // namespace dm
