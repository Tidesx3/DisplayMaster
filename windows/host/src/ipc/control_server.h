// Local control API for the tray/settings app: \.\pipe\DisplayMaster.Control,
// message mode, one JSON request -> one JSON response.
#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dm {

class ControlServer {
public:
    using Handler = std::function<std::string(const std::string& request)>;
    static constexpr const wchar_t* kPipeName = L"\\\\.\\pipe\\DisplayMaster.Control";

    ~ControlServer() { stop(); }
    bool start(Handler handler);
    void stop();

private:
    void accept_loop();
    void serve(HANDLE pipe);

    Handler handler_;
    std::thread accept_thread_;
    struct Client {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::mutex clients_mu_;
    std::vector<Client> clients_;
    std::atomic<bool> running_{false};
    PSECURITY_DESCRIPTOR sd_ = nullptr;
};

}  // namespace dm
