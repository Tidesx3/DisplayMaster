#include "setup/vdd_setup.h"

#include <windows.h>
#include <cfgmgr32.h>
#include <newdev.h>
#include <setupapi.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/win.h"

namespace dm::setup {
namespace {

constexpr wchar_t kHardwareId[] = L"Root\\MttVDD";

std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return s;
}

// Calls `fn(set, info)` for every device (present or not) whose hardware IDs include ours.
template <typename Fn>
int for_each_vdd_device(Fn fn, bool present_only = false) {
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | (present_only ? DIGCF_PRESENT : 0));
    if (set == INVALID_HANDLE_VALUE) return 0;
    int count = 0;
    SP_DEVINFO_DATA info{sizeof info};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info); ++i) {
        wchar_t ids[1024] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_HARDWAREID, nullptr, reinterpret_cast<BYTE*>(ids),
                                               sizeof ids - sizeof(wchar_t), nullptr))
            continue;
        for (const wchar_t* id = ids; *id; id += wcslen(id) + 1) {
            if (_wcsicmp(id, kHardwareId) == 0) {
                fn(set, info);
                ++count;
                break;
            }
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return count;
}

bool create_device_node(const std::wstring& inf) {
    GUID class_guid;
    wchar_t class_name[MAX_CLASS_NAME_LEN];
    if (!SetupDiGetINFClassW(inf.c_str(), &class_guid, class_name, MAX_CLASS_NAME_LEN, nullptr)) {
        DM_LOGE("VDD setup: can't read %s (%lu)", to_utf8(inf).c_str(), GetLastError());
        return false;
    }
    HDEVINFO set = SetupDiCreateDeviceInfoList(&class_guid, nullptr);
    if (set == INVALID_HANDLE_VALUE) return false;
    SP_DEVINFO_DATA info{sizeof info};
    bool ok = SetupDiCreateDeviceInfoW(set, class_name, &class_guid, nullptr, nullptr, DICD_GENERATE_ID, &info);
    // REG_MULTI_SZ: the ID followed by an extra terminator.
    std::wstring ids = std::wstring(kHardwareId) + L'\0' + L'\0';
    ok = ok && SetupDiSetDeviceRegistryPropertyW(set, &info, SPDRP_HARDWAREID, reinterpret_cast<const BYTE*>(ids.data()),
                                                 static_cast<DWORD>(ids.size() * sizeof(wchar_t)));
    ok = ok && SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &info);
    if (!ok) DM_LOGE("VDD setup: creating the device failed (%lu)", GetLastError());
    SetupDiDestroyDeviceInfoList(set);
    return ok;
}

}  // namespace

std::filesystem::path vdd_settings_dir() {
    wchar_t path[MAX_PATH] = {};
    DWORD size = sizeof path;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\MikeTheTech\\VirtualDisplayDriver", L"VDDPATH", RRF_RT_REG_SZ,
                     nullptr, path, &size) == ERROR_SUCCESS && path[0])
        return path;
    return L"C:\\VirtualDisplayDriver";
}

int install_vdd(const std::filesystem::path& package_dir) {
    const auto inf = package_dir / L"MttVDD.inf";
    std::error_code ec;
    if (!std::filesystem::exists(inf, ec)) {
        DM_LOGE("VDD setup: %s not found", to_utf8(inf.wstring()).c_str());
        return 2;
    }
    // The driver reads its settings from here; keep an existing configuration.
    const auto settings_dir = vdd_settings_dir();
    std::filesystem::create_directories(settings_dir, ec);
    const auto settings = settings_dir / L"vdd_settings.xml";
    if (!std::filesystem::exists(settings, ec))
        std::filesystem::copy_file(package_dir / L"vdd_settings.xml", settings, ec);

    const int existing = for_each_vdd_device([](HDEVINFO, SP_DEVINFO_DATA&) {});
    if (existing == 0) {
        DM_LOGI("VDD setup: creating device %s", to_utf8(kHardwareId).c_str());
        if (!create_device_node(inf.wstring())) return 3;
    }
    // Installs (or updates) the driver package and binds it to the device.
    BOOL reboot = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, kHardwareId, inf.c_str(), INSTALLFLAG_FORCE, &reboot)) {
        const DWORD err = GetLastError();
        DM_LOGE("VDD setup: driver installation failed (0x%08lX)%s", err,
                err == ERROR_AUTHENTICODE_TRUST_NOT_ESTABLISHED || err == ERROR_CANCELLED
                    ? " - the driver publisher was not trusted"
                    : "");
        return 4;
    }
    DM_LOGI("VDD setup: virtual display driver installed%s", reboot ? " (restart required)" : "");
    return reboot ? 1 : 0;
}

VddState vdd_state() {
    auto state = VddState::NotInstalled;
    for_each_vdd_device(
        [&](HDEVINFO, SP_DEVINFO_DATA& info) {
            ULONG status = 0, problem = 0;
            const bool has_problem = CM_Get_DevNode_Status(&status, &problem, info.DevInst, 0) == CR_SUCCESS &&
                                     (status & DN_HAS_PROBLEM);
            const auto device = !has_problem                ? VddState::Enabled
                                : problem == CM_PROB_DISABLED ? VddState::Disabled
                                                              : VddState::Failed;
            // One working device is enough; otherwise report the most "on" state.
            if (state == VddState::NotInstalled || device == VddState::Enabled ||
                (device == VddState::Failed && state == VddState::Disabled))
                state = device;
        },
        true);
    return state;
}

bool set_vdd_enabled(bool on) {
    bool ok = false;
    for_each_vdd_device(
        [&](HDEVINFO set, SP_DEVINFO_DATA& info) {
            SP_PROPCHANGE_PARAMS params{};
            params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
            params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
            params.StateChange = on ? DICS_ENABLE : DICS_DISABLE;
            params.Scope = DICS_FLAG_GLOBAL;
            if (SetupDiSetClassInstallParamsW(set, &info, &params.ClassInstallHeader, sizeof params) &&
                SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &info))
                ok = true;
            else
                DM_LOGW("VDD: %s the device failed (%lu)", on ? "enabling" : "disabling", GetLastError());
        },
        true);
    return ok;
}

bool restart_vdd() {
    bool ok = false;
    for_each_vdd_device(
        [&](HDEVINFO set, SP_DEVINFO_DATA& info) {
            SP_PROPCHANGE_PARAMS params{};
            params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
            params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
            params.StateChange = DICS_PROPCHANGE;  // stop + start
            params.Scope = DICS_FLAG_CONFIGSPECIFIC;
            if (SetupDiSetClassInstallParamsW(set, &info, &params.ClassInstallHeader, sizeof params) &&
                SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &info))
                ok = true;
            else
                DM_LOGW("VDD: restarting the device failed (%lu)", GetLastError());
        },
        true);
    // A device that stays failed after a restart usually recovers from off/on.
    if (ok && vdd_state() == VddState::Failed) ok = set_vdd_enabled(false) && set_vdd_enabled(true);
    return ok;
}

bool vdd_installed_by_us() {
    DWORD value = 0, size = sizeof value;
    return RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\DisplayMaster", L"InstalledVdd", RRF_RT_REG_DWORD, nullptr,
                        &value, &size) == ERROR_SUCCESS &&
           value == 1;
}

int uninstall_vdd() {
    int removed = 0;
    for_each_vdd_device([&](HDEVINFO set, SP_DEVINFO_DATA& info) {
        BOOL reboot = FALSE;
        if (DiUninstallDevice(nullptr, set, &info, 0, &reboot)) ++removed;
    });
    // Remove the driver package from the driver store (oemNN.inf that mentions our hardware ID).
    wchar_t windir[MAX_PATH];
    GetWindowsDirectoryW(windir, MAX_PATH);
    const std::filesystem::path infdir = std::filesystem::path(windir) / L"INF";
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(infdir, ec)) {
        const auto name = lower(entry.path().filename().wstring());
        if (name.rfind(L"oem", 0) != 0 || entry.path().extension() != L".inf") continue;
        std::ifstream f(entry.path(), std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        std::string raw = ss.str();
        // INFs may be UTF-16: drop the zero bytes before searching.
        raw.erase(std::remove(raw.begin(), raw.end(), '\0'), raw.end());
        std::transform(raw.begin(), raw.end(), raw.begin(), [](char c) { return static_cast<char>(::tolower(static_cast<unsigned char>(c))); });
        if (raw.find("root\\mttvdd") == std::string::npos) continue;
        f.close();
        if (SetupUninstallOEMInfW(entry.path().filename().c_str(), SUOI_FORCEDELETE, nullptr))
            DM_LOGI("VDD setup: removed driver package %s", to_utf8(entry.path().filename().wstring()).c_str());
    }
    DM_LOGI("VDD setup: removed %d virtual display device(s)", removed);
    return 0;
}

}  // namespace dm::setup
