// Installs / removes the Virtual Display Driver (root-enumerated device "Root\MttVDD"),
// the same way `devcon install` does, so the installer needs no extra tools.
#pragma once

#include <filesystem>

namespace dm::setup {

// `package_dir` holds MttVDD.inf / .cat / .dll and a default vdd_settings.xml.
// Requires elevation. Windows may ask once to trust the driver's publisher.
int install_vdd(const std::filesystem::path& package_dir);
int uninstall_vdd();

// Settings folder the driver reads (registry VDDPATH, default C:\VirtualDisplayDriver).
std::filesystem::path vdd_settings_dir();

}  // namespace dm::setup
