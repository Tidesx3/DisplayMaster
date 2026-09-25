// Persistent host settings changed from the UI (key=value lines in
// %LOCALAPPDATA%\DisplayMaster\host.ini). Command-line flags override them.
#pragma once

#include <filesystem>
#include <map>
#include <mutex>
#include <string>

namespace dm {

class Config {
public:
    explicit Config(std::filesystem::path file) : file_(std::move(file)) { load(); }

    bool get_bool(const std::string& key, bool fallback) const;
    void set_bool(const std::string& key, bool value);
    float get_float(const std::string& key, float fallback) const;
    void set_float(const std::string& key, float value);

    static std::filesystem::path default_path();

private:
    void load();
    void save() const;

    std::filesystem::path file_;
    mutable std::mutex mu_;
    std::map<std::string, std::string> values_;
};

}  // namespace dm
