#include "session/approval.h"

#include <windows.h>
#include <shlobj.h>

#include <chrono>
#include <fstream>

#include "core/log.h"
#include "core/win.h"
#include "dm/noise.h"

namespace dm {

std::filesystem::path ApprovalBroker::default_store() {
    PWSTR path = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) dir = path;
    CoTaskMemFree(path);
    return dir / L"DisplayMaster" / L"trusted_devices.txt";
}

ApprovalBroker::ApprovalBroker(std::filesystem::path store) : store_(std::move(store)) {
    load();
}

void ApprovalBroker::load() {
    std::ifstream f(store_);
    std::string line;
    while (std::getline(f, line)) {
        const auto tab = line.find('\t');
        if (tab == std::string::npos || tab == 0) continue;
        // Entries from before encrypted pairing hold a device id, not a key: that device pairs again.
        const auto key = line.substr(0, tab);
        if (!noise::key_from_hex(key)) continue;
        trusted_[key] = line.substr(tab + 1);
    }
}

void ApprovalBroker::save() {
    std::error_code ec;
    std::filesystem::create_directories(store_.parent_path(), ec);
    std::ofstream f(store_, std::ios::trunc);
    for (const auto& [id, name] : trusted_) f << id << '\t' << name << '\n';
}

bool ApprovalBroker::is_trusted(const std::string& device_key) {
    std::lock_guard lock(mu_);
    return !device_key.empty() && trusted_.count(device_key);
}

bool ApprovalBroker::request(const Pending& info, uint32_t timeout_ms, const std::function<bool()>& still_connected) {
    std::unique_lock lock(mu_);
    requests_[info.session_id] = Request{info};
    DM_LOGI("Waiting for approval of \"%s\" (%s) from %s, pairing code %s", info.name.c_str(), info.model.c_str(),
            info.address.c_str(), info.code.c_str());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    auto done = [&] {
        const auto& r = requests_[info.session_id];
        return r.decision.has_value() || r.cancelled;
    };
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        cv_.wait_for(lock, std::chrono::milliseconds(500), done);
        if (!done() && !still_connected()) requests_[info.session_id].cancelled = true;
    }
    const bool allowed = requests_[info.session_id].decision.value_or(false);
    requests_.erase(info.session_id);
    return allowed;
}

bool ApprovalBroker::decide(uint32_t session_id, bool allow, bool remember) {
    std::lock_guard lock(mu_);
    auto it = requests_.find(session_id);
    if (it == requests_.end()) return false;
    it->second.decision = allow;
    if (allow && remember && !it->second.info.device_key.empty()) {
        trusted_[it->second.info.device_key] = it->second.info.name;
        save();
    }
    cv_.notify_all();
    return true;
}

void ApprovalBroker::cancel(uint32_t session_id) {
    std::lock_guard lock(mu_);
    auto it = requests_.find(session_id);
    if (it == requests_.end()) return;
    it->second.cancelled = true;
    cv_.notify_all();
}

void ApprovalBroker::trust(const std::string& device_key, const std::string& name) {
    std::lock_guard lock(mu_);
    trusted_[device_key] = name;
    save();
}

std::vector<ApprovalBroker::Pending> ApprovalBroker::pending() {
    std::lock_guard lock(mu_);
    std::vector<Pending> out;
    for (const auto& [id, r] : requests_)
        if (!r.decision && !r.cancelled) out.push_back(r.info);
    return out;
}

std::vector<std::pair<std::string, std::string>> ApprovalBroker::trusted() {
    std::lock_guard lock(mu_);
    return {trusted_.begin(), trusted_.end()};
}

void ApprovalBroker::forget(const std::string& device_key) {
    std::lock_guard lock(mu_);
    if (trusted_.erase(device_key)) save();
}

}  // namespace dm
