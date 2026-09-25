// USB via ADB: for every authorized device, set up `adb reverse` so the app can
// reach the host at 127.0.0.1:<port>, and (optionally) launch the app so plugging
// in the cable is all the user has to do.
#pragma once

#include <atomic>
#include <cstdint>
#include <set>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

namespace dm {

class AdbManager {
public:
    ~AdbManager() { stop(); }
    bool start(uint16_t port, bool auto_launch_app);
    void stop();
    const std::wstring& adb_path() const { return adb_; }

    struct DeviceState {
        std::vector<std::string> ready;         // adb reverse set up
        std::vector<std::string> unauthorized;  // waiting for the USB debugging prompt
    };
    DeviceState devices() const;

    static std::wstring find_adb();
    // Runs a command line, returns exit code; stdout+stderr in `output`.
    static int run(const std::wstring& cmdline, std::string& output, uint32_t timeout_ms = 10000);

private:
    void loop();
    void ensure_app_installed(const std::wstring& adb, const std::wstring& serial);

    std::wstring adb_;
    uint16_t port_ = 0;
    bool auto_launch_ = true;
    std::set<std::string> configured_;  // serials with adb reverse set up
    std::set<std::string> warned_;      // unauthorized serials already reported
    std::thread thread_;
    std::atomic<bool> running_{false};
    mutable std::mutex state_mu_;
    DeviceState state_;
};

}  // namespace dm
