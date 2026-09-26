// Advertises the host on the LAN as _displaymaster._tcp (DNS-SD over mDNS) so the
// Android app lists nearby PCs without typing an IP. Uses the built-in Windows
// mDNS responder (DnsServiceRegister, Windows 10 1809+).
#pragma once

#include <windows.h>
#include <windns.h>

#include <cstdint>
#include <string>

namespace dm {

class MdnsAdvertiser {
public:
    static constexpr const wchar_t* kServiceType = L"_displaymaster._tcp.local";

    ~MdnsAdvertiser() { stop(); }
    // `ipv4` (dotted): the address to announce; empty lets Windows choose, which can be a
    // Hyper-V / VM adapter the device can't reach.
    bool start(uint16_t port, const std::wstring& pc_name, const std::string& ipv4 = {});
    void stop();
    bool running() const { return instance_ != nullptr; }

private:
    PDNS_SERVICE_INSTANCE instance_ = nullptr;
    DNS_SERVICE_REGISTER_REQUEST request_{};
    std::wstring instance_name_, host_name_;
};

}  // namespace dm
