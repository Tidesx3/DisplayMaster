#include "transport/adb.h"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <sstream>
#include <vector>

#include "core/log.h"
#include "core/win.h"

namespace dm {

namespace fs = std::filesystem;

constexpr const char* kAppPackage = "com.displaymaster.client";
constexpr const char* kAppComponent = "com.displaymaster.client/.MainActivity";

std::wstring AdbManager::find_adb() {
    std::vector<fs::path> candidates;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    candidates.push_back(fs::path(exe).parent_path() / L"platform-tools" / L"adb.exe");  // bundled
    for (const wchar_t* var : {L"ANDROID_HOME", L"ANDROID_SDK_ROOT"}) {
        wchar_t buf[MAX_PATH];
        if (GetEnvironmentVariableW(var, buf, MAX_PATH)) candidates.push_back(fs::path(buf) / L"platform-tools" / L"adb.exe");
    }
    wchar_t local[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH))
        candidates.push_back(fs::path(local) / L"Android" / L"Sdk" / L"platform-tools" / L"adb.exe");
    for (const auto& c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec)) return c.wstring();
    }
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH, found, nullptr)) return found;
    return {};
}

int AdbManager::run(const std::wstring& cmdline, std::string& output, uint32_t timeout_ms) {
    output.clear();
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    UniqueHandle read_end(rd), write_end(wr);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = cmdline;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    UniqueHandle proc(pi.hProcess), thread(pi.hThread);
    write_end.reset();  // so ReadFile sees EOF when the child exits

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    char buf[4096];
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail) {
            DWORD n = 0;
            if (ReadFile(rd, buf, std::min<DWORD>(avail, sizeof buf), &n, nullptr) && n) output.append(buf, n);
            continue;
        }
        if (WaitForSingleObject(proc.get(), 20) == WAIT_OBJECT_0) {
            DWORD n = 0;
            while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail &&
                   ReadFile(rd, buf, std::min<DWORD>(avail, sizeof buf), &n, nullptr) && n)
                output.append(buf, n);
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            TerminateProcess(proc.get(), 1);
            return -1;
        }
    }
    DWORD code = 0;
    GetExitCodeProcess(proc.get(), &code);
    return static_cast<int>(code);
}

bool AdbManager::start(uint16_t port, bool auto_launch_app) {
    adb_ = find_adb();
    if (adb_.empty()) {
        DM_LOGW("adb.exe not found - USB (ADB) connections disabled");
        return false;
    }
    DM_LOGI("ADB: %s", to_utf8(adb_).c_str());
    port_ = port;
    auto_launch_ = auto_launch_app;
    running_ = true;
    thread_ = std::thread([this] { loop(); });
    return true;
}

// First plug-in of a device without the app: install the APK bundled next to the engine
// (android\DisplayMaster.apk), so the cable is all that's needed.
void AdbManager::ensure_app_installed(const std::wstring& adb, const std::wstring& serial) {
    std::string out;
    run(adb + L" -s " + serial + L" shell pm path " + to_wide(kAppPackage), out);
    if (out.find("package:") != std::string::npos) return;

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const fs::path apk = fs::path(exe).parent_path() / L"android" / L"DisplayMaster.apk";
    std::error_code ec;
    if (!fs::exists(apk, ec)) {
        DM_LOGW("ADB: %s doesn't have the DisplayMaster app and no APK is bundled", to_utf8(serial).c_str());
        return;
    }
    DM_LOGI("ADB: installing the DisplayMaster app on %s", to_utf8(serial).c_str());
    if (run(adb + L" -s " + serial + L" install -r \"" + apk.wstring() + L"\"", out, 120000) != 0)
        DM_LOGW("ADB: install failed: %s", out.c_str());
}

AdbManager::DeviceState AdbManager::devices() const {
    std::lock_guard lock(state_mu_);
    return state_;
}

void AdbManager::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

void AdbManager::loop() {
    const std::wstring adb = L"\"" + adb_ + L"\"";
    while (running_) {
        std::string out;
        if (run(adb + L" devices", out) == 0) {
            std::set<std::string> online, unauthorized;
            std::istringstream lines(out);
            std::string line;
            while (std::getline(lines, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const auto tab = line.find('\t');
                if (tab == std::string::npos) continue;
                const std::string serial = line.substr(0, tab);
                const std::string state = line.substr(tab + 1);
                if (state == "device") online.insert(serial);
                if (state == "unauthorized") unauthorized.insert(serial);
            }
            for (const auto& serial : unauthorized)
                if (warned_.insert(serial).second)
                    DM_LOGW("ADB: %s is unauthorized - accept the USB debugging prompt on the device", serial.c_str());
            for (const auto& serial : online) {
                if (configured_.count(serial)) continue;
                const std::wstring s = to_wide(serial);
                const std::wstring p = std::to_wstring(port_);
                std::string o;
                if (run(adb + L" -s " + s + L" reverse tcp:" + p + L" tcp:" + p, o) != 0) {
                    DM_LOGW("ADB: reverse failed for %s: %s", serial.c_str(), o.c_str());
                    continue;
                }
                DM_LOGI("ADB: %s ready (reverse tcp:%u)", serial.c_str(), port_);
                ensure_app_installed(adb, s);
                if (auto_launch_)
                    run(adb + L" -s " + s + L" shell am start -n " + to_wide(kAppComponent) +
                            L" --ez com.displaymaster.extra.USB_CONNECT true",
                        o);
                configured_.insert(serial);
            }
            // Forget unplugged devices so re-plugging sets them up again.
            std::erase_if(configured_, [&](const std::string& s) { return !online.count(s); });
            std::erase_if(warned_, [&](const std::string& s) { return !unauthorized.count(s); });
            std::lock_guard lock(state_mu_);
            state_.ready.assign(configured_.begin(), configured_.end());
            state_.unauthorized.assign(unauthorized.begin(), unauthorized.end());
        }
        for (int i = 0; i < 20 && running_; ++i) Sleep(100);
    }
}

}  // namespace dm
