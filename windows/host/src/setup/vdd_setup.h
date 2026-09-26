// Installs / removes the Virtual Display Driver (root-enumerated device "Root\MttVDD"),
// the same way `devcon install` does, so the installer needs no extra tools.
#pragma once

#include <filesystem>

namespace dm::setup {

// `package_dir` holds MttVDD.inf / .cat / .dll and a default vdd_settings.xml.
// Requires elevation. Windows may ask once to trust the driver's publisher.
int install_vdd(const std::filesystem::path& package_dir);
int uninstall_vdd();

// Device state. Disabled = no virtual monitors at all (none listed in display settings).
// Failed = switched on, but Windows stopped the driver after it reported a problem (code 43).
enum class VddState { NotInstalled, Disabled, Enabled, Failed };
VddState vdd_state();
// Requires elevation.
bool set_vdd_enabled(bool on);
// Stops and starts the device (clears a failed state). Requires elevation.
bool restart_vdd();
// True if the DisplayMaster installer added the driver (it may be switched off when
// idle); a driver another tool installed is left running.
bool vdd_installed_by_us();

// Settings folder the driver reads (registry VDDPATH, default C:\VirtualDisplayDriver).
std::filesystem::path vdd_settings_dir();

}  // namespace dm::setup
