#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/config.h"
#include "display/virtual_display.h"
#include "ipc/control_server.h"
#include "session/session.h"
#include "session/approval.h"
#include "session/identity.h"
#include "transport/adb.h"
#include "transport/mdns.h"
#include "transport/connection.h"

namespace dm {

class Host {
public:
    explicit Host(const HostOptions& opts);
    ~Host();
    bool start();
    void stop();
    // Drops finished sessions; call periodically.
    void reap();
    size_t session_count();

    // Control API (JSON in, JSON out); see docs/control-api.md.
    std::string handle_control(const std::string& request);

    // Called (on a control thread) when the UI asks the engine to exit.
    std::function<void()> on_shutdown_requested;

private:
    bool start_listening();
    // Saved picture settings (host.ini) into opts_; command-line flags win.
    void load_stream_options();
    void update_advertising();
    std::string status_json();

    HostOptions opts_;
    Config config_{host_data_dir(opts_) / L"host.ini"};
    bool allow_wifi_ = false;
    VirtualDisplayManager vdm_;
    TcpServer server_;
    AdbManager adb_;
    ControlServer control_;
    ApprovalBroker approvals_{host_data_dir(opts_) / L"trusted_devices.txt"};
    const noise::KeyPair identity_ = load_or_create_identity(host_data_dir(opts_) / L"identity.key");
    MdnsAdvertiser mdns_;
    std::mutex mu_;
    std::map<uint32_t, std::unique_ptr<Session>> sessions_;
    uint32_t next_id_ = 1;
};

// IPv4 addresses devices on the LAN can reach (adapters with a default gateway, so no
// Hyper-V / VM adapters), best route first. Shown in the UI for Wi-Fi setup.
std::vector<std::string> lan_ipv4_addresses();

}  // namespace dm
