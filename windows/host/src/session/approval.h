// Wi-Fi connections from devices the PC hasn't seen before must be approved by the
// user in the DisplayMaster app (anyone on the network could otherwise inject input).
// Devices are identified by their long-term public key from the encrypted handshake
// (hex), which a device can't fake, unlike an id it reports itself. The user compares
// the pairing code shown on both screens. USB connections are implicitly trusted.
#pragma once

#include <condition_variable>
#include <functional>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace dm {

class ApprovalBroker {
public:
    struct Pending {
        uint32_t session_id = 0;
        std::string device_key, name, model, address;
        std::string code;  // pairing code, shown on the device too
    };

    // `store` holds one "device_key<TAB>name" per line.
    explicit ApprovalBroker(std::filesystem::path store);

    bool is_trusted(const std::string& device_key);

    // Blocks the calling session thread until the user decides, `timeout_ms` passes,
    // or `still_connected` turns false (the device gave up).
    bool request(const Pending& info, uint32_t timeout_ms, const std::function<bool()>& still_connected);
    // From the UI. Returns false if the request no longer exists.
    bool decide(uint32_t session_id, bool allow, bool remember);
    // Session went away while waiting.
    void cancel(uint32_t session_id);
    // Trust without asking (engine test mode).
    void trust(const std::string& device_key, const std::string& name);

    std::vector<Pending> pending();
    std::vector<std::pair<std::string, std::string>> trusted();  // key, name
    void forget(const std::string& device_key);

    static std::filesystem::path default_store();

private:
    void load();
    void save();

    std::filesystem::path store_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::map<std::string, std::string> trusted_;  // device_key -> name
    struct Request {
        Pending info;
        std::optional<bool> decision;
        bool cancelled = false;
    };
    std::map<uint32_t, Request> requests_;
};

}  // namespace dm
