#include "display/display_config.h"

#include <algorithm>

#include "core/log.h"
#include "core/win.h"

namespace dm {
namespace {

// EDID PnP id + product code of the MTT Virtual Display Driver ("MTT", 0x1337).
constexpr const wchar_t* kVddEdidId = L"MTT1337";

bool query_config(UINT32 flags, std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
                  std::vector<DISPLAYCONFIG_MODE_INFO>& modes) {
    for (int attempt = 0; attempt < 5; ++attempt) {
        UINT32 np = 0, nm = 0;
        if (GetDisplayConfigBufferSizes(flags, &np, &nm) != ERROR_SUCCESS) return false;
        paths.resize(np);
        modes.resize(nm);
        const LONG r = QueryDisplayConfig(flags, &np, paths.data(), &nm, modes.data(), nullptr);
        if (r == ERROR_INSUFFICIENT_BUFFER) continue;  // topology changed between calls
        if (r != ERROR_SUCCESS) return false;
        paths.resize(np);
        modes.resize(nm);
        return true;
    }
    return false;
}

std::wstring source_gdi_name(const DISPLAYCONFIG_PATH_SOURCE_INFO& src) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
    name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    name.header.size = sizeof name;
    name.header.adapterId = src.adapterId;
    name.header.id = src.id;
    if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS) return {};
    return name.viewGdiDeviceName;
}

void fill_target_names(const DISPLAYCONFIG_PATH_TARGET_INFO& tgt, MonitorInfo& m) {
    DISPLAYCONFIG_TARGET_DEVICE_NAME name{};
    name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    name.header.size = sizeof name;
    name.header.adapterId = tgt.adapterId;
    name.header.id = tgt.id;
    if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS) return;
    m.friendly_name = name.monitorFriendlyDeviceName;
    m.device_path = name.monitorDevicePath;
    std::wstring upper = m.device_path;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::towupper);
    m.is_vdd = upper.find(kVddEdidId) != std::wstring::npos;
}

bool apply_paths(std::vector<DISPLAYCONFIG_PATH_INFO>& paths, std::vector<DISPLAYCONFIG_MODE_INFO>& modes) {
    const LONG r = SetDisplayConfig(static_cast<UINT32>(paths.size()), paths.data(),
                                    static_cast<UINT32>(modes.size()), modes.empty() ? nullptr : modes.data(),
                                    SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES |
                                        SDC_SAVE_TO_DATABASE);
    if (r != ERROR_SUCCESS) DM_LOGE("SetDisplayConfig failed: %ld", r);
    return r == ERROR_SUCCESS;
}

}  // namespace

std::vector<MonitorInfo> enumerate_monitors(bool include_inactive) {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    std::vector<MonitorInfo> out;
    if (!query_config(QDC_ONLY_ACTIVE_PATHS, paths, modes)) return out;

    for (const auto& p : paths) {
        MonitorInfo m;
        m.active = true;
        m.adapter_luid = p.targetInfo.adapterId;
        m.target_id = p.targetInfo.id;
        m.source_id = p.sourceInfo.id;
        m.gdi_name = source_gdi_name(p.sourceInfo);
        fill_target_names(p.targetInfo, m);
        if (p.targetInfo.refreshRate.Denominator)
            m.refresh_mhz = static_cast<uint32_t>(1000ull * p.targetInfo.refreshRate.Numerator /
                                                  p.targetInfo.refreshRate.Denominator);
        const UINT32 mi = p.sourceInfo.modeInfoIdx;
        if (mi < modes.size() && modes[mi].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
            const auto& sm = modes[mi].sourceMode;
            m.rect = {sm.position.x, sm.position.y, static_cast<int32_t>(sm.width), static_cast<int32_t>(sm.height)};
            m.primary = sm.position.x == 0 && sm.position.y == 0;
        }
        out.push_back(std::move(m));
    }

    if (include_inactive) {
        std::vector<DISPLAYCONFIG_PATH_INFO> all;
        std::vector<DISPLAYCONFIG_MODE_INFO> all_modes;
        if (query_config(QDC_ALL_PATHS, all, all_modes)) {
            for (const auto& p : all) {
                if (!p.targetInfo.targetAvailable) continue;
                const bool known = std::any_of(out.begin(), out.end(), [&](const MonitorInfo& m) {
                    return m.adapter_luid == p.targetInfo.adapterId && m.target_id == p.targetInfo.id;
                });
                if (known) continue;
                MonitorInfo m;
                m.adapter_luid = p.targetInfo.adapterId;
                m.target_id = p.targetInfo.id;
                fill_target_names(p.targetInfo, m);
                out.push_back(std::move(m));
            }
        }
    }
    return out;
}

std::optional<MonitorInfo> find_primary_monitor() {
    for (auto& m : enumerate_monitors(false))
        if (m.primary) return m;
    return std::nullopt;
}

bool set_target_active(const LUID& adapter, uint32_t target_id, bool active) {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!query_config(QDC_ONLY_ACTIVE_PATHS, paths, modes)) return false;

    auto is_target = [&](const DISPLAYCONFIG_PATH_INFO& p) {
        return p.targetInfo.adapterId == adapter && p.targetInfo.id == target_id;
    };
    const bool currently_active = std::any_of(paths.begin(), paths.end(), is_target);
    if (currently_active == active) return true;

    if (!active) {
        paths.erase(std::remove_if(paths.begin(), paths.end(), is_target), paths.end());
        return apply_paths(paths, modes);
    }

    // Activating: find an inactive path for this target whose source isn't in use.
    std::vector<DISPLAYCONFIG_PATH_INFO> all;
    std::vector<DISPLAYCONFIG_MODE_INFO> all_modes;
    if (!query_config(QDC_ALL_PATHS, all, all_modes)) return false;
    for (const auto& p : all) {
        if (!is_target(p) || !p.targetInfo.targetAvailable) continue;
        const bool source_used = std::any_of(paths.begin(), paths.end(), [&](const DISPLAYCONFIG_PATH_INFO& a) {
            return a.sourceInfo.adapterId == p.sourceInfo.adapterId && a.sourceInfo.id == p.sourceInfo.id;
        });
        if (source_used) continue;
        DISPLAYCONFIG_PATH_INFO np = p;
        np.flags |= DISPLAYCONFIG_PATH_ACTIVE;
        np.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        np.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        paths.push_back(np);
        return apply_paths(paths, modes);
    }
    DM_LOGE("No free source to activate target %u", target_id);
    return false;
}

bool set_monitor_mode(const std::wstring& gdi_name, uint32_t width, uint32_t height, uint32_t refresh_hz,
                      std::optional<PointI> position) {
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    dm.dmPelsWidth = width;
    dm.dmPelsHeight = height;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    if (refresh_hz) {
        dm.dmDisplayFrequency = refresh_hz;
        dm.dmFields |= DM_DISPLAYFREQUENCY;
    }
    if (position) {
        dm.dmPosition = {position->x, position->y};
        dm.dmFields |= DM_POSITION;
    }
    LONG r = ChangeDisplaySettingsExW(gdi_name.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
    if (r != DISP_CHANGE_SUCCESSFUL) {
        DM_LOGE("ChangeDisplaySettingsEx(%s, %ux%u@%u) failed: %ld", to_utf8(gdi_name).c_str(), width, height,
                refresh_hz, r);
        return false;
    }
    r = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    return r == DISP_CHANGE_SUCCESSFUL;
}

PointI position_right_of_desktop(const std::vector<MonitorInfo>& monitors, const std::wstring& exclude_gdi) {
    int32_t right = 0;
    for (const auto& m : monitors) {
        if (!m.active || m.gdi_name == exclude_gdi) continue;
        right = std::max(right, m.rect.x + m.rect.w);
    }
    return {right, 0};
}

}  // namespace dm
