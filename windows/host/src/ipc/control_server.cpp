#include "ipc/control_server.h"

#include <sddl.h>

#include "core/log.h"
#include "core/win.h"

namespace dm {

bool ControlServer::start(Handler handler, std::wstring pipe_name) {
    pipe_name_ = std::move(pipe_name);
    handler_ = std::move(handler);
    // SYSTEM + Administrators: full; interactive users: read/write. The host runs
    // elevated, the UI doesn't.
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)",
                                                              SDDL_REVISION_1, &sd_, nullptr)) {
        DM_LOGE("Control pipe: security descriptor failed (%lu)", GetLastError());
        return false;
    }
    running_ = true;
    accept_thread_ = std::thread([this] { accept_loop(); });
    return true;
}

void ControlServer::stop() {
    if (!running_.exchange(false)) return;
    // Unblock ConnectNamedPipe with a throwaway client.
    HANDLE h = CreateFileW(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (accept_thread_.joinable()) accept_thread_.join();
    std::vector<Client> clients;
    {
        std::lock_guard lock(clients_mu_);
        clients.swap(clients_);
    }
    for (auto& c : clients) {
        CancelSynchronousIo(c.thread.native_handle());  // pending ReadFile
        if (c.thread.joinable()) c.thread.join();
    }
    if (sd_) LocalFree(sd_);
    sd_ = nullptr;
}

void ControlServer::accept_loop() {
    SECURITY_ATTRIBUTES sa{sizeof sa, sd_, FALSE};
    while (running_) {
        HANDLE pipe = CreateNamedPipeW(pipe_name_.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                       PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, &sa);
        if (pipe == INVALID_HANDLE_VALUE) {
            DM_LOGE("Control pipe: CreateNamedPipe failed (%lu)", GetLastError());
            return;
        }
        const bool connected = ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
        if (!running_ || !connected) {
            CloseHandle(pipe);
            continue;
        }
        std::lock_guard lock(clients_mu_);
        // Reap UIs that already disconnected.
        std::erase_if(clients_, [](Client& c) {
            if (!*c.done) return false;
            c.thread.join();
            return true;
        });
        auto done = std::make_shared<std::atomic<bool>>(false);
        clients_.push_back({std::thread([this, pipe, done] {
                                serve(pipe);
                                *done = true;
                            }),
                            done});
    }
}

void ControlServer::serve(HANDLE pipe) {
    UniqueHandle h(pipe);
    std::vector<char> buf(64 * 1024);
    while (running_) {
        DWORD read = 0;
        if (!ReadFile(pipe, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr) || read == 0) break;
        const std::string response = handler_(std::string(buf.data(), read));
        DWORD written = 0;
        if (!WriteFile(pipe, response.data(), static_cast<DWORD>(response.size()), &written, nullptr)) break;
    }
    DisconnectNamedPipe(pipe);
}

}  // namespace dm
