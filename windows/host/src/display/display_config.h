// Monitor enumeration and topology changes via the CCD API (QueryDisplayConfig /
// SetDisplayConfig) plus ChangeDisplaySettingsEx for modes and placement.
#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dm/input_math.h"

namespace dm {

struct MonitorInfo {
    std::wstring gdi_name;       // "\\.\DISPLAY5" (empty when inactive)
    std::wstring friendly_name;  // EDID monitor name
    std::wstring device_path;    // monitor PnP path, contains the EDID PnP id
    LUID adapter_luid{};
    uint32_t target_id = 0;
    uint32_t source_id = 0;
    RectI rect;                 // desktop coordinates (active only)
    uint32_t refresh_mhz = 0;
    bool active = false;
    bool primary = false;
    bool is_vdd = false;        // our virtual display driver
};

inline bool operator==(const LUID& a, const LUID& b) {
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

// All monitors. With include_inactive, also available-but-detached targets.
std::vector<MonitorInfo> enumerate_monitors(bool include_inactive);

std::optional<MonitorInfo> find_primary_monitor();

// Attach / detach a target to the desktop. Keeps every other path as-is.
bool set_target_active(const LUID& adapter, uint32_t target_id, bool active);

// Set resolution / refresh / desktop position of an active monitor.
// `quiet`: don't log failures (caller retries).
bool set_monitor_mode(const std::wstring& gdi_name, uint32_t width, uint32_t height, uint32_t refresh_hz,
                      std::optional<PointI> position, bool quiet = false);

// Place a new monitor of the given width to the right of all current monitors,
// top-aligned with the primary.
PointI position_right_of_desktop(const std::vector<MonitorInfo>& monitors, const std::wstring& exclude_gdi);

}  // namespace dm
