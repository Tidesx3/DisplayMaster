#include "core/config.h"

#include <windows.h>
#include <shlobj.h>

#include <fstream>

namespace dm {

std::filesystem::path Config::default_path() {
    PWSTR path = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) dir = path;
    CoTaskMemFree(path);
    return dir / L"DisplayMaster" / L"host.ini";
}

void Config::load() {
    std::ifstream f(file_);
    std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#') continue;
        values_[line.substr(0, eq)] = line.substr(eq + 1);
    }
}

void Config::save() const {
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    std::ofstream f(file_, std::ios::trunc);
    f << "# DisplayMaster host settings (edited by the DisplayMaster app)\n";
    for (const auto& [k, v] : values_) f << k << '=' << v << '\n';
}

bool Config::get_bool(const std::string& key, bool fallback) const {
    std::lock_guard lock(mu_);
    auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second == "1" || it->second == "true";
}

float Config::get_float(const std::string& key, float fallback) const {
    std::lock_guard lock(mu_);
    auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    try {
        return std::stof(it->second);
    } catch (...) {
        return fallback;
    }
}

void Config::set_float(const std::string& key, float value) {
    std::lock_guard lock(mu_);
    values_[key] = std::to_string(value);
    save();
}

std::string Config::get_string(const std::string& key, const std::string& fallback) const {
    std::lock_guard lock(mu_);
    auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second;
}

void Config::set_string(const std::string& key, const std::string& value) {
    std::lock_guard lock(mu_);
    values_[key] = value;
    save();
}

void Config::set_bool(const std::string& key, bool value) {
    std::lock_guard lock(mu_);
    values_[key] = value ? "1" : "0";
    save();
}

}  // namespace dm
